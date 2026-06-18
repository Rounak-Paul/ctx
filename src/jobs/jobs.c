#include "jobs.h"
#include "../event/event.h"

/* --------------------------------------------------------------------------
 * Internal queue node (intrusive singly-linked list per priority lane)
 * -------------------------------------------------------------------------- */
typedef struct CtxJobNode {
    CtxJob            job;
    struct CtxJobNode *next;
} CtxJobNode;

/* Simple pool allocator for nodes to avoid per-job malloc */
#define JOB_POOL_CAPACITY 4096

typedef struct {
    CtxJobNode  nodes[JOB_POOL_CAPACITY];
    uint32_t    free_list[JOB_POOL_CAPACITY];
    uint32_t    free_count;
#if defined(CTX_PLATFORM_WINDOWS)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t  lock;
#endif
} CtxJobPool;

static CtxJobPool s_pool;

static void pool_init(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    InitializeCriticalSection(&s_pool.lock);
#else
    pthread_mutex_init(&s_pool.lock, NULL);
#endif
    s_pool.free_count = JOB_POOL_CAPACITY;
    for (uint32_t i = 0; i < JOB_POOL_CAPACITY; ++i)
        s_pool.free_list[i] = JOB_POOL_CAPACITY - 1 - i;
}

static CtxJobNode *pool_alloc(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_pool.lock);
#else
    pthread_mutex_lock(&s_pool.lock);
#endif
    CtxJobNode *node = NULL;
    if (s_pool.free_count > 0)
        node = &s_pool.nodes[s_pool.free_list[--s_pool.free_count]];
#if defined(CTX_PLATFORM_WINDOWS)
    LeaveCriticalSection(&s_pool.lock);
#else
    pthread_mutex_unlock(&s_pool.lock);
#endif
    return node;
}

static void pool_free(CtxJobNode *node)
{
    if (!node) return;
    uint32_t idx = (uint32_t)(node - s_pool.nodes);
#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_pool.lock);
    s_pool.free_list[s_pool.free_count++] = idx;
    LeaveCriticalSection(&s_pool.lock);
#else
    pthread_mutex_lock(&s_pool.lock);
    s_pool.free_list[s_pool.free_count++] = idx;
    pthread_mutex_unlock(&s_pool.lock);
#endif
}

static void pool_destroy(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    DeleteCriticalSection(&s_pool.lock);
#else
    pthread_mutex_destroy(&s_pool.lock);
#endif
}

/* --------------------------------------------------------------------------
 * Thread pool state
 * -------------------------------------------------------------------------- */
#define CTX_MAX_THREADS 64

typedef struct {
    /* Priority queues: head/tail per lane */
    CtxJobNode  *head[CTX_JOB_PRIORITY_COUNT];
    CtxJobNode  *tail[CTX_JOB_PRIORITY_COUNT];
    uint32_t     total;         /* total queued across all lanes */
    uint32_t     active;        /* workers currently executing a job */

#if defined(CTX_PLATFORM_WINDOWS)
    CRITICAL_SECTION  mutex;
    CONDITION_VARIABLE work_ready;
    CONDITION_VARIABLE all_idle;
    HANDLE             threads[CTX_MAX_THREADS];
#else
    pthread_mutex_t  mutex;
    pthread_cond_t   work_ready;
    pthread_cond_t   all_idle;
    pthread_t        threads[CTX_MAX_THREADS];
#endif

    uint32_t     thread_count;
    bool         shutdown;
} CtxJobSystem;

static CtxJobSystem s_js;

/* --------------------------------------------------------------------------
 * Worker thread
 * -------------------------------------------------------------------------- */
static CtxJobNode *dequeue_locked(void)
{
    for (int p = 0; p < CTX_JOB_PRIORITY_COUNT; ++p) {
        if (s_js.head[p]) {
            CtxJobNode *node = s_js.head[p];
            s_js.head[p] = node->next;
            if (!s_js.head[p]) s_js.tail[p] = NULL;
            node->next = NULL;
            s_js.total--;
            return node;
        }
    }
    return NULL;
}

#if defined(CTX_PLATFORM_WINDOWS)
static DWORD WINAPI worker_thread(LPVOID arg)
#else
static void *worker_thread(void *arg)
#endif
{
    CTX_UNUSED(arg);

#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_js.mutex);
#else
    pthread_mutex_lock(&s_js.mutex);
#endif

    while (true) {
        while (!s_js.shutdown && s_js.total == 0) {
#if defined(CTX_PLATFORM_WINDOWS)
            SleepConditionVariableCS(&s_js.work_ready, &s_js.mutex, INFINITE);
#else
            pthread_cond_wait(&s_js.work_ready, &s_js.mutex);
#endif
        }

        if (s_js.shutdown && s_js.total == 0) break;

        CtxJobNode *node = dequeue_locked();
        if (!node) continue;

        s_js.active++;
#if defined(CTX_PLATFORM_WINDOWS)
        LeaveCriticalSection(&s_js.mutex);
#else
        pthread_mutex_unlock(&s_js.mutex);
#endif

        node->job.fn(node->job.user_data);
        pool_free(node);

        /* Emit job-complete event (fire-and-forget, no payload needed) */
        ctx_event_emit(CTX_EVENT_JOB_COMPLETE, NULL, 0);

#if defined(CTX_PLATFORM_WINDOWS)
        EnterCriticalSection(&s_js.mutex);
#else
        pthread_mutex_lock(&s_js.mutex);
#endif
        s_js.active--;

        if (s_js.total == 0 && s_js.active == 0) {
#if defined(CTX_PLATFORM_WINDOWS)
            WakeAllConditionVariable(&s_js.all_idle);
#else
            pthread_cond_broadcast(&s_js.all_idle);
#endif
        }
    }

#if defined(CTX_PLATFORM_WINDOWS)
    LeaveCriticalSection(&s_js.mutex);
    return 0;
#else
    pthread_mutex_unlock(&s_js.mutex);
    return NULL;
#endif
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

static uint32_t detect_cpu_count(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (uint32_t)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
#else
    return 1u;
#endif
}

bool ctx_jobs_init(uint32_t thread_count)
{
    memset(&s_js, 0, sizeof(s_js));
    pool_init();

    if (thread_count == 0)
        thread_count = detect_cpu_count();
    if (thread_count > CTX_MAX_THREADS)
        thread_count = CTX_MAX_THREADS;

    s_js.thread_count = thread_count;
    s_js.shutdown     = false;

#if defined(CTX_PLATFORM_WINDOWS)
    InitializeCriticalSection(&s_js.mutex);
    InitializeConditionVariable(&s_js.work_ready);
    InitializeConditionVariable(&s_js.all_idle);
    for (uint32_t i = 0; i < thread_count; ++i) {
        s_js.threads[i] = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (!s_js.threads[i]) return false;
    }
#else
    pthread_mutex_init(&s_js.mutex, NULL);
    pthread_cond_init(&s_js.work_ready, NULL);
    pthread_cond_init(&s_js.all_idle, NULL);
    for (uint32_t i = 0; i < thread_count; ++i) {
        if (pthread_create(&s_js.threads[i], NULL, worker_thread, NULL) != 0)
            return false;
    }
#endif

    return true;
}

void ctx_jobs_shutdown(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_js.mutex);
    s_js.shutdown = true;
    WakeAllConditionVariable(&s_js.work_ready);
    LeaveCriticalSection(&s_js.mutex);

    WaitForMultipleObjects(s_js.thread_count, s_js.threads, TRUE, INFINITE);
    for (uint32_t i = 0; i < s_js.thread_count; ++i)
        CloseHandle(s_js.threads[i]);

    DeleteCriticalSection(&s_js.mutex);
#else
    pthread_mutex_lock(&s_js.mutex);
    s_js.shutdown = true;
    pthread_cond_broadcast(&s_js.work_ready);
    pthread_mutex_unlock(&s_js.mutex);

    for (uint32_t i = 0; i < s_js.thread_count; ++i)
        pthread_join(s_js.threads[i], NULL);

    pthread_mutex_destroy(&s_js.mutex);
    pthread_cond_destroy(&s_js.work_ready);
    pthread_cond_destroy(&s_js.all_idle);
#endif

    pool_destroy();
}

/* --------------------------------------------------------------------------
 * Submit
 * -------------------------------------------------------------------------- */

bool ctx_job_submit(CtxJobFn fn, void *user_data, CtxJobPriority priority)
{
    if (!fn || priority >= CTX_JOB_PRIORITY_COUNT) return false;

    CtxJobNode *node = pool_alloc();
    if (!node) return false;

    node->job.fn        = fn;
    node->job.user_data = user_data;
    node->job.priority  = priority;
    node->next          = NULL;

#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_js.mutex);
#else
    pthread_mutex_lock(&s_js.mutex);
#endif

    int p = (int)priority;
    if (s_js.tail[p]) {
        s_js.tail[p]->next = node;
        s_js.tail[p]       = node;
    } else {
        s_js.head[p] = s_js.tail[p] = node;
    }
    s_js.total++;

#if defined(CTX_PLATFORM_WINDOWS)
    WakeConditionVariable(&s_js.work_ready);
    LeaveCriticalSection(&s_js.mutex);
#else
    pthread_cond_signal(&s_js.work_ready);
    pthread_mutex_unlock(&s_js.mutex);
#endif

    return true;
}

/* --------------------------------------------------------------------------
 * Synchronisation
 * -------------------------------------------------------------------------- */

void ctx_jobs_wait_all(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_js.mutex);
    while (s_js.total > 0 || s_js.active > 0)
        SleepConditionVariableCS(&s_js.all_idle, &s_js.mutex, INFINITE);
    LeaveCriticalSection(&s_js.mutex);
#else
    pthread_mutex_lock(&s_js.mutex);
    while (s_js.total > 0 || s_js.active > 0)
        pthread_cond_wait(&s_js.all_idle, &s_js.mutex);
    pthread_mutex_unlock(&s_js.mutex);
#endif
}

uint32_t ctx_jobs_thread_count(void)
{
    return s_js.thread_count;
}

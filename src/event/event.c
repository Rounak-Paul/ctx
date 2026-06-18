#include "event.h"

/* --------------------------------------------------------------------------
 * Internal types
 * -------------------------------------------------------------------------- */

typedef struct {
    CtxSubscriber   subs[CTX_EVENT_MAX_SUBSCRIBERS];
    uint32_t        count;
#if defined(CTX_PLATFORM_WINDOWS)
    SRWLOCK         lock;
#else
    pthread_rwlock_t lock;
#endif
} CtxEventSlot;

/* We keep a flat table keyed by event ID.
 * IDs above CTX_EVENT_SLOT_COUNT are rejected. */
#define CTX_EVENT_SLOT_COUNT (CTX_EVENT_USER_BASE + 256u)

static CtxEventSlot  s_slots[CTX_EVENT_SLOT_COUNT];
static bool          s_initialised = false;
static uint64_t      s_next_handle = 1; /* monotonically increasing */

/* Global mutex protecting s_next_handle only */
#if defined(CTX_PLATFORM_WINDOWS)
static CRITICAL_SECTION s_handle_lock;
#else
static pthread_mutex_t  s_handle_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static void slot_lock_read(CtxEventSlot *slot)
{
#if defined(CTX_PLATFORM_WINDOWS)
    AcquireSRWLockShared(&slot->lock);
#else
    pthread_rwlock_rdlock(&slot->lock);
#endif
}

static void slot_unlock_read(CtxEventSlot *slot)
{
#if defined(CTX_PLATFORM_WINDOWS)
    ReleaseSRWLockShared(&slot->lock);
#else
    pthread_rwlock_unlock(&slot->lock);
#endif
}

static void slot_lock_write(CtxEventSlot *slot)
{
#if defined(CTX_PLATFORM_WINDOWS)
    AcquireSRWLockExclusive(&slot->lock);
#else
    pthread_rwlock_wrlock(&slot->lock);
#endif
}

static void slot_unlock_write(CtxEventSlot *slot)
{
#if defined(CTX_PLATFORM_WINDOWS)
    ReleaseSRWLockExclusive(&slot->lock);
#else
    pthread_rwlock_unlock(&slot->lock);
#endif
}

static uint64_t next_handle(void)
{
#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_handle_lock);
    uint64_t h = s_next_handle++;
    LeaveCriticalSection(&s_handle_lock);
#else
    pthread_mutex_lock(&s_handle_lock);
    uint64_t h = s_next_handle++;
    pthread_mutex_unlock(&s_handle_lock);
#endif
    return h;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

bool ctx_event_system_init(void)
{
    if (s_initialised) return true;

    memset(s_slots, 0, sizeof(s_slots));

    for (uint32_t i = 0; i < CTX_EVENT_SLOT_COUNT; ++i) {
#if defined(CTX_PLATFORM_WINDOWS)
        InitializeSRWLock(&s_slots[i].lock);
#else
        if (pthread_rwlock_init(&s_slots[i].lock, NULL) != 0) {
            /* Roll back already-initialised locks */
            for (uint32_t j = 0; j < i; ++j)
                pthread_rwlock_destroy(&s_slots[j].lock);
            return false;
        }
#endif
    }

#if defined(CTX_PLATFORM_WINDOWS)
    InitializeCriticalSection(&s_handle_lock);
#endif

    s_initialised = true;
    return true;
}

void ctx_event_system_shutdown(void)
{
    if (!s_initialised) return;

#if defined(CTX_PLATFORM_WINDOWS)
    DeleteCriticalSection(&s_handle_lock);
    for (uint32_t i = 0; i < CTX_EVENT_SLOT_COUNT; ++i)
        /* SRWLocks need no explicit destroy */
        (void)i;
#else
    for (uint32_t i = 0; i < CTX_EVENT_SLOT_COUNT; ++i)
        pthread_rwlock_destroy(&s_slots[i].lock);
#endif

    s_initialised = false;
}

/* --------------------------------------------------------------------------
 * Subscribe / unsubscribe
 * -------------------------------------------------------------------------- */

CtxEventHandle ctx_event_subscribe(CtxEventId id,
                                   CtxEventCallback callback,
                                   void *user_data)
{
    if (!callback || id == CTX_EVENT_INVALID || id >= CTX_EVENT_SLOT_COUNT)
        return CTX_EVENT_HANDLE_INVALID;

    CtxEventSlot *slot = &s_slots[id];
    uint64_t handle = next_handle();

    slot_lock_write(slot);
    if (slot->count >= CTX_EVENT_MAX_SUBSCRIBERS) {
        slot_unlock_write(slot);
        return CTX_EVENT_HANDLE_INVALID;
    }
    slot->subs[slot->count].callback  = callback;
    slot->subs[slot->count].user_data = user_data;
    slot->subs[slot->count].handle    = handle;
    slot->count++;
    slot_unlock_write(slot);

    return handle;
}

void ctx_event_unsubscribe(CtxEventId id, CtxEventHandle handle)
{
    if (handle == CTX_EVENT_HANDLE_INVALID || id >= CTX_EVENT_SLOT_COUNT)
        return;

    CtxEventSlot *slot = &s_slots[id];
    slot_lock_write(slot);

    for (uint32_t i = 0; i < slot->count; ++i) {
        if (slot->subs[i].handle == handle) {
            /* Swap with last to keep array dense */
            slot->subs[i] = slot->subs[slot->count - 1];
            slot->count--;
            break;
        }
    }

    slot_unlock_write(slot);
}

/* --------------------------------------------------------------------------
 * Dispatch
 * -------------------------------------------------------------------------- */

void ctx_event_dispatch(const CtxEvent *event)
{
    if (!event || event->id == CTX_EVENT_INVALID ||
        event->id >= CTX_EVENT_SLOT_COUNT)
        return;

    CtxEventSlot *slot = &s_slots[event->id];

    /* Snapshot subscriber list under read lock, then call outside lock
     * to avoid deadlocks when a callback itself subscribes/dispatches. */
    CtxSubscriber snapshot[CTX_EVENT_MAX_SUBSCRIBERS];
    uint32_t count;

    slot_lock_read(slot);
    count = slot->count;
    memcpy(snapshot, slot->subs, count * sizeof(CtxSubscriber));
    slot_unlock_read(slot);

    for (uint32_t i = 0; i < count; ++i)
        snapshot[i].callback(event, snapshot[i].user_data);
}

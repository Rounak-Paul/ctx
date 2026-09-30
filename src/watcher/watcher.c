#include "watcher.h"
#include "../event/event.h"
#include "../log/log.h"

#if defined(CTX_PLATFORM_MACOS)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#elif defined(CTX_PLATFORM_LINUX)
#include <stdatomic.h>
#endif

/* ============================================================
 * Shared types
 * ============================================================ */

typedef struct {
    CtxWatchHandle  handle;
    char            path[CTX_WATCHER_PATH_MAX];
    bool            recursive;
    bool            active;

    /* Platform-specific descriptor (Linux keeps its own InotifyWatch table) */
#if defined(CTX_PLATFORM_MACOS)
    FSEventStreamRef stream;
    char            real_path[CTX_WATCHER_PATH_MAX]; /* canonical path FSEvents reports */
    size_t          real_len;
    bool            is_dir;
#elif defined(CTX_PLATFORM_WINDOWS)
    HANDLE          dir_handle;
    OVERLAPPED      overlapped;
    uint8_t         buf[65536];
    bool            pending;
#endif
} CtxWatchEntry;

typedef struct {
#if !defined(CTX_PLATFORM_LINUX)
    CtxWatchEntry   entries[CTX_WATCHER_MAX_WATCHES];
    uint32_t        count;
#endif
    uint32_t        next_handle;

#if defined(CTX_PLATFORM_WINDOWS)
    HANDLE           thread;
    HANDLE           stop_event;
    CRITICAL_SECTION lock;
#elif defined(CTX_PLATFORM_MACOS)
    dispatch_queue_t queue;          /* serial queue delivering FSEvents */
    pthread_mutex_t  lock;
#else
    pthread_t        thread;
    int              stop_pipe[2];   /* write [1] to signal shutdown */
    pthread_mutex_t  lock;
#endif

    bool             running;

#if defined(CTX_PLATFORM_LINUX)
    int              inotify_fd;
    atomic_bool      thread_failed;  /* watcher thread exited on an error */
#endif
} CtxWatcher;

static CtxWatcher s_watcher;

/* Optional predicate excluding directories by name; see
   ctx_watcher_set_dir_filter. Set before any watch is added. */
static bool (*s_skip_dir)(const char *name) = NULL;

#if !defined(CTX_PLATFORM_WINDOWS)
/* True when a directory named name should not be watched or reported. */
static bool watcher_skips_dir(const char *name)
{
    return name[0] == '.' || (s_skip_dir && s_skip_dir(name));
}
#endif

void ctx_watcher_set_dir_filter(bool (*skip_dir)(const char *name))
{
    s_skip_dir = skip_dir;
}

/* ============================================================
 * Internal helpers
 * ============================================================ */

/*
 * Dispatches one CtxFileEvent through the event system.
 *
 * kind      Event kind.
 * path      Affected path.
 * old_path  Previous path for renames, or NULL.
 * is_dir    path is (or was) a directory; consumers rescan instead of
 *           updating a single file.
 */
static void emit_file_event(CtxFileEventKind kind,
                            const char *path,
                            const char *old_path,
                            bool is_dir)
{
    CtxFileEvent ev;
    ev.kind = kind;
    ev.is_dir = is_dir;
    strncpy(ev.path, path ? path : "", CTX_WATCHER_PATH_MAX - 1);
    ev.path[CTX_WATCHER_PATH_MAX - 1] = '\0';
    strncpy(ev.old_path, old_path ? old_path : "", CTX_WATCHER_PATH_MAX - 1);
    ev.old_path[CTX_WATCHER_PATH_MAX - 1] = '\0';

    static const CtxEventId kind_to_event[] = {
        CTX_EVENT_FILE_CREATED,
        CTX_EVENT_FILE_MODIFIED,
        CTX_EVENT_FILE_DELETED,
        CTX_EVENT_FILE_RENAMED
    };

    ctx_event_emit(kind_to_event[kind], &ev, sizeof(ev));
}

/* ============================================================
 * POSIX shared: file state dedupe (Linux, macOS)
 *
 * Backends report candidate paths; an event is emitted only when the path's
 * (exists, mtime, size) differs from the last reported state. This absorbs
 * write bursts, repeated/sticky OS flags and metadata-only changes. The map
 * is owned by the single event-delivery context of each backend (watcher
 * thread / serial dispatch queue) and cleared at shutdown after it stops.
 * ============================================================ */
#if !defined(CTX_PLATFORM_WINDOWS)

#include <sys/stat.h>

#if defined(CTX_PLATFORM_MACOS)
#define CTX_STAT_MTIME(st) ((st).st_mtimespec)
#else
#define CTX_STAT_MTIME(st) ((st).st_mtim)
#endif

typedef struct {
    char           *path;
    bool            exists;
    struct timespec mtime;
    off_t           size;
    UT_hash_handle  hh;
} FileState;

static FileState *s_file_states = NULL;

/*
 * Decides whether a file path's current on-disk state differs from the
 * last state reported for it, and records the new state. Uses stat() so
 * symlinked files are treated like the indexer treats them.
 *
 * path      Emitted (caller-form) path.
 * out_kind  Receives CREATED, MODIFIED or DELETED.
 * Returns   true when an event should be emitted.
 */
static bool file_state_changed(const char *path, CtxFileEventKind *out_kind)
{
    struct stat st;
    bool exists = stat(path, &st) == 0 && S_ISREG(st.st_mode);
    struct timespec mtime = exists ? CTX_STAT_MTIME(st) : (struct timespec){0};

    FileState *state = NULL;
    HASH_FIND_STR(s_file_states, path, state);

    if (state && state->exists == exists &&
        (!exists || (state->size == st.st_size &&
                     state->mtime.tv_sec == mtime.tv_sec &&
                     state->mtime.tv_nsec == mtime.tv_nsec)))
        return false;

    if (!state) {
        state = calloc(1, sizeof(*state));
        char *key = state ? strdup(path) : NULL;
        if (!key) {
            free(state);
            *out_kind = exists ? CTX_FILE_EVENT_MODIFIED : CTX_FILE_EVENT_DELETED;
            return true;
        }
        state->path = key;
        HASH_ADD_KEYPTR(hh, s_file_states, state->path, strlen(state->path), state);
        *out_kind = exists ? CTX_FILE_EVENT_CREATED : CTX_FILE_EVENT_DELETED;
    } else {
        *out_kind = !exists ? CTX_FILE_EVENT_DELETED
                  : state->exists ? CTX_FILE_EVENT_MODIFIED
                  : CTX_FILE_EVENT_CREATED;
    }

    state->exists = exists;
    if (exists) {
        state->mtime = mtime;
        state->size = st.st_size;
    }
    return true;
}

/* Frees every recorded file state. Only called once delivery has stopped. */
static void file_states_clear(void)
{
    FileState *state, *tmp;
    HASH_ITER(hh, s_file_states, state, tmp) {
        HASH_DEL(s_file_states, state);
        free(state->path);
        free(state);
    }
}

#endif /* !CTX_PLATFORM_WINDOWS */

/* ============================================================
 * LINUX — inotify
 *
 * One inotify watch per directory (per file for single-file watches), kept
 * in a table keyed by watch descriptor and guarded by s_watcher.lock.
 * Events are batched on the watcher thread until the tree has been quiet
 * for CTX_WATCHER_DEBOUNCE_MS (bounded by CTX_WATCHER_MAX_LATENCY_MS), then
 * files are reported through file_state_changed and directories as
 * is_dir events (consumers rescan, which also prunes removed subtrees).
 * ============================================================ */
#if defined(CTX_PLATFORM_LINUX)

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>

#define INOTIFY_DIR_MASK  (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | \
                           IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |   \
                           IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK)
#define INOTIFY_FILE_MASK (IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF)
#define INOTIFY_FILE_EVENTS (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE | \
                             IN_MOVED_FROM | IN_MOVED_TO)

#define CTX_WATCHER_DEBOUNCE_MS     75
#define CTX_WATCHER_MAX_LATENCY_MS  500

/* One kernel watch. The kernel returns the same wd for an inode that is
   already watched, so the table also breaks symlink/bind-mount cycles. */
typedef struct InotifyWatch {
    int             wd;
    CtxWatchHandle  handle;
    bool            recursive;
    bool            is_root;   /* the path passed to ctx_watcher_add */
    bool            is_dir;
    char           *path;      /* caller-form path */
    UT_hash_handle  hh;
} InotifyWatch;

/* Batched path awaiting the debounce flush; owned by the watcher thread. */
typedef struct {
    char            *path;
    bool             is_dir;
    CtxFileEventKind dir_kind;
    UT_hash_handle   hh;
} InotifyPending;

static InotifyWatch *s_watches = NULL;
static bool s_limit_warned = false;

/* Returns CLOCK_MONOTONIC time in milliseconds. */
static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Removes w from the table, optionally dropping the kernel watch too
   (not needed after IN_IGNORED or once the inotify fd is closed).
   Caller holds lock. */
static void inotify_drop(InotifyWatch *w, bool remove_kernel_watch)
{
    if (remove_kernel_watch)
        inotify_rm_watch(s_watcher.inotify_fd, w->wd);
    HASH_DEL(s_watches, w);
    free(w->path);
    free(w);
}

/*
 * Registers one kernel watch. Caller holds lock.
 *
 * handle     Owning watch handle.
 * path       Caller-form path to watch.
 * is_dir     Watch a directory (children events) rather than a file.
 * recursive  Stored for directories: new subdirectories get watched.
 * is_root    path is the root passed to ctx_watcher_add.
 * Returns    true when a new watch was registered; false when the inode is
 *            already watched or registration failed.
 */
static bool inotify_add_one(CtxWatchHandle handle, const char *path,
                            bool is_dir, bool recursive, bool is_root)
{
    int wd = inotify_add_watch(s_watcher.inotify_fd, path,
                               is_dir ? INOTIFY_DIR_MASK : INOTIFY_FILE_MASK);
    if (wd < 0) {
        if ((errno == ENOSPC || errno == ENOMEM) && !s_limit_warned) {
            s_limit_warned = true;
            CTX_LOG_WARN("inotify watch limit reached at %s; changes below "
                         "unwatched directories are missed until restart "
                         "(raise fs.inotify.max_user_watches)", path);
        }
        return false;
    }

    InotifyWatch *existing = NULL;
    HASH_FIND_INT(s_watches, &wd, existing);
    if (existing) return false;

    InotifyWatch *w = calloc(1, sizeof(*w));
    char *copy = w ? strdup(path) : NULL;
    if (!copy) {
        free(w);
        inotify_rm_watch(s_watcher.inotify_fd, wd);
        return false;
    }
    w->wd = wd;
    w->handle = handle;
    w->recursive = recursive;
    w->is_root = is_root;
    w->is_dir = is_dir;
    w->path = copy;
    HASH_ADD_INT(s_watches, wd, w);
    return true;
}

/* True when the directory entry de inside parent is a directory; resolves
   DT_UNKNOWN (filesystems without d_type) and symlinks with stat(), as the
   indexer does. */
static bool inotify_entry_is_dir(const char *child, const struct dirent *de)
{
    if (de->d_type == DT_DIR) return true;
    if (de->d_type != DT_UNKNOWN && de->d_type != DT_LNK) return false;
    struct stat st;
    return stat(child, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Watches directory path and, when recursive, every non-excluded
 * subdirectory. Already-watched directories are not descended again.
 * Caller holds lock.
 */
static void inotify_add_tree(CtxWatchHandle handle, const char *path,
                             bool recursive, bool is_root)
{
    if (!inotify_add_one(handle, path, true, recursive, is_root) || !recursive)
        return;

    DIR *dir = opendir(path);
    if (!dir) return;
    char *child = malloc(CTX_WATCHER_PATH_MAX);
    if (!child) {
        closedir(dir);
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (watcher_skips_dir(de->d_name)) continue;
        int n = snprintf(child, CTX_WATCHER_PATH_MAX, "%s/%s", path, de->d_name);
        if (n <= 0 || n >= CTX_WATCHER_PATH_MAX) continue;
        if (inotify_entry_is_dir(child, de))
            inotify_add_tree(handle, child, true, false);
    }
    free(child);
    closedir(dir);
}

/* Removes the watches for path and everything below it; used when a
   directory leaves its path (moved away), since a later watch on the same
   inode would otherwise keep the stale path. Caller holds lock. */
static void inotify_drop_subtree(const char *path)
{
    size_t len = strlen(path);
    InotifyWatch *w, *tmp;
    HASH_ITER(hh, s_watches, w, tmp) {
        if (!strncmp(w->path, path, len) && (w->path[len] == '\0' || w->path[len] == '/'))
            inotify_drop(w, true);
    }
}

/* Emits one batched path: directories as is_dir events, files through the
   shared state filter. */
static void inotify_emit(const char *path, bool is_dir, CtxFileEventKind dir_kind)
{
    if (is_dir) {
        emit_file_event(dir_kind, path, NULL, true);
        return;
    }
    CtxFileEventKind kind;
    if (file_state_changed(path, &kind))
        emit_file_event(kind, path, NULL, false);
}

/* Adds path to the batch; a directory event for a path supersedes a file
   event for it. Emits immediately if the batch cannot grow. */
static void inotify_pending_add(InotifyPending **pending, const char *path,
                                bool is_dir, CtxFileEventKind dir_kind)
{
    InotifyPending *p = NULL;
    HASH_FIND_STR(*pending, path, p);
    if (p) {
        if (is_dir) {
            p->is_dir = true;
            p->dir_kind = dir_kind;
        }
        return;
    }
    p = calloc(1, sizeof(*p));
    char *copy = p ? strdup(path) : NULL;
    if (!copy) {
        free(p);
        inotify_emit(path, is_dir, dir_kind);
        return;
    }
    p->path = copy;
    p->is_dir = is_dir;
    p->dir_kind = dir_kind;
    HASH_ADD_KEYPTR(hh, *pending, p->path, strlen(p->path), p);
}

/* Emits (when emit is true) and frees every batched path. */
static void inotify_pending_flush(InotifyPending **pending, bool emit)
{
    InotifyPending *p, *tmp;
    HASH_ITER(hh, *pending, p, tmp) {
        if (emit)
            inotify_emit(p->path, p->is_dir, p->dir_kind);
        HASH_DEL(*pending, p);
        free(p->path);
        free(p);
    }
}

/* Recovers from IN_Q_OVERFLOW: events were lost, so re-register any
   directories missing from the table and request a rescan of every root.
   Caller holds lock. */
static void inotify_recover_overflow(InotifyPending **pending)
{
    CTX_LOG_WARN("inotify event queue overflowed; rescanning watched trees");
    uint32_t roots = 0;
    InotifyWatch *w, *tmp;
    HASH_ITER(hh, s_watches, w, tmp)
        if (w->is_root) roots++;
    if (roots == 0) return;

    InotifyWatch **list = calloc(roots, sizeof(*list));
    if (!list) return;
    uint32_t n = 0;
    HASH_ITER(hh, s_watches, w, tmp)
        if (w->is_root) list[n++] = w;

    for (uint32_t i = 0; i < n; ++i) {
        InotifyWatch *root = list[i];
        if (root->is_dir && root->recursive) {
            /* Descend from the root's children: the root itself is watched,
               and inotify_add_tree stops at already-watched directories. */
            DIR *dir = opendir(root->path);
            char *child = dir ? malloc(CTX_WATCHER_PATH_MAX) : NULL;
            struct dirent *de;
            while (child && (de = readdir(dir))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                if (watcher_skips_dir(de->d_name)) continue;
                int len = snprintf(child, CTX_WATCHER_PATH_MAX, "%s/%s",
                                   root->path, de->d_name);
                if (len <= 0 || len >= CTX_WATCHER_PATH_MAX) continue;
                if (inotify_entry_is_dir(child, de))
                    inotify_add_tree(root->handle, child, true, false);
            }
            free(child);
            if (dir) closedir(dir);
        }
        inotify_pending_add(pending, root->path, root->is_dir, CTX_FILE_EVENT_MODIFIED);
    }
    free(list);
}

/*
 * Applies one inotify event to the watch table and the batch.
 * Caller holds lock.
 */
static void inotify_handle_event(const struct inotify_event *ie,
                                 InotifyPending **pending)
{
    if (ie->mask & IN_Q_OVERFLOW) {
        inotify_recover_overflow(pending);
        return;
    }

    InotifyWatch *w = NULL;
    HASH_FIND_INT(s_watches, &ie->wd, w);
    if (!w) return;

    if (ie->mask & IN_IGNORED) {
        inotify_drop(w, false);
        return;
    }

    char path[CTX_WATCHER_PATH_MAX];

    if (!w->is_dir) {
        /* Single-file watch: the event is about the watched file itself. */
        snprintf(path, sizeof(path), "%s", w->path);
        if (ie->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            /* The inode left the path; re-watch whatever replaced it
               (atomic save). IN_IGNORED for the old wd finds nothing. */
            CtxWatchHandle handle = w->handle;
            bool is_root = w->is_root;
            inotify_drop(w, (ie->mask & IN_MOVE_SELF) != 0);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                inotify_add_one(handle, path, false, false, is_root);
        }
        inotify_pending_add(pending, path, false, CTX_FILE_EVENT_MODIFIED);
        return;
    }

    if (ie->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        /* Non-root directories are handled through their parent's
           IN_DELETE / IN_MOVED_FROM. A root that disappears stops being
           watched; the rescan prunes its files. */
        if (w->is_root) {
            snprintf(path, sizeof(path), "%s", w->path);
            CTX_LOG_WARN("watched root %s was %s; watching stopped", path,
                         (ie->mask & IN_MOVE_SELF) ? "moved" : "deleted");
            if (ie->mask & IN_MOVE_SELF)
                inotify_drop_subtree(path);
            inotify_pending_add(pending, path, true, CTX_FILE_EVENT_DELETED);
        } else if (ie->mask & IN_MOVE_SELF) {
            snprintf(path, sizeof(path), "%s", w->path);
            inotify_drop_subtree(path);
        }
        return;
    }

    if (ie->len == 0 || ie->name[0] == '.') return;
    int n = snprintf(path, sizeof(path), "%s/%s", w->path, ie->name);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;

    if (ie->mask & IN_ISDIR) {
        if (watcher_skips_dir(ie->name)) return;
        if (ie->mask & (IN_CREATE | IN_MOVED_TO)) {
            /* Watch before the rescan runs so nothing created inside after
               this point is missed; the rescan covers what already exists. */
            if (w->recursive)
                inotify_add_tree(w->handle, path, true, false);
            inotify_pending_add(pending, path, true, CTX_FILE_EVENT_CREATED);
        } else if (ie->mask & IN_MOVED_FROM) {
            inotify_drop_subtree(path);
            inotify_pending_add(pending, path, true, CTX_FILE_EVENT_DELETED);
        } else if (ie->mask & IN_DELETE) {
            inotify_pending_add(pending, path, true, CTX_FILE_EVENT_DELETED);
        }
        return;
    }

    if (ie->mask & INOTIFY_FILE_EVENTS)
        inotify_pending_add(pending, path, false, CTX_FILE_EVENT_MODIFIED);
}

static void *watcher_thread_linux(void *arg)
{
    CTX_UNUSED(arg);

    /* 64 KiB holds hundreds of events per read; aligned for inotify_event. */
    static uint8_t buf[65536] __attribute__((aligned(__alignof__(struct inotify_event))));
    struct pollfd fds[2] = {
        { .fd = s_watcher.inotify_fd,  .events = POLLIN },
        { .fd = s_watcher.stop_pipe[0], .events = POLLIN },
    };
    InotifyPending *pending = NULL;
    int64_t batch_start_ms = 0;

    while (true) {
        int timeout_ms = -1;
        if (pending) {
            int64_t elapsed = monotonic_ms() - batch_start_ms;
            if (elapsed >= CTX_WATCHER_MAX_LATENCY_MS) {
                inotify_pending_flush(&pending, true);
                continue;
            }
            int64_t remaining = CTX_WATCHER_MAX_LATENCY_MS - elapsed;
            timeout_ms = (int)(remaining < CTX_WATCHER_DEBOUNCE_MS
                               ? remaining : CTX_WATCHER_DEBOUNCE_MS);
        }

        int ret = poll(fds, 2, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) continue;
            CTX_LOG_ERROR("watcher poll failed: %s", strerror(errno));
            atomic_store(&s_watcher.thread_failed, true);
            break;
        }
        if (ret == 0) {
            inotify_pending_flush(&pending, true);
            continue;
        }
        if (fds[1].revents) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            CTX_LOG_ERROR("watcher inotify descriptor failed");
            atomic_store(&s_watcher.thread_failed, true);
            break;
        }
        if (!(fds[0].revents & POLLIN)) continue;

        /* Drain everything queued; the fd is non-blocking. */
        while (true) {
            ssize_t len = read(s_watcher.inotify_fd, buf, sizeof(buf));
            if (len < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                CTX_LOG_ERROR("watcher read failed: %s", strerror(errno));
                atomic_store(&s_watcher.thread_failed, true);
                goto done;
            }
            if (len == 0) break;

            pthread_mutex_lock(&s_watcher.lock);
            for (ssize_t offset = 0; offset < len;) {
                const struct inotify_event *ie =
                    (const struct inotify_event *)(buf + offset);
                ssize_t size = (ssize_t)sizeof(*ie) + (ssize_t)ie->len;
                if (offset + size > len) break;
                offset += size;

                bool was_empty = pending == NULL;
                inotify_handle_event(ie, &pending);
                if (was_empty && pending)
                    batch_start_ms = monotonic_ms();
            }
            pthread_mutex_unlock(&s_watcher.lock);
        }
    }
done:
    /* Shutting down (or failed): pending changes are dropped; the next
       startup's full scan reconciles them. */
    inotify_pending_flush(&pending, false);
    return NULL;
}

bool ctx_watcher_init(void)
{
    memset(&s_watcher, 0, sizeof(s_watcher));
    s_watches = NULL;
    s_limit_warned = false;

    atomic_init(&s_watcher.thread_failed, false);
    s_watcher.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_watcher.inotify_fd < 0) {
        CTX_LOG_ERROR("inotify_init1 failed: %s", strerror(errno));
        return false;
    }
    if (pipe(s_watcher.stop_pipe) != 0) {
        close(s_watcher.inotify_fd);
        return false;
    }
    fcntl(s_watcher.stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(s_watcher.stop_pipe[1], F_SETFD, FD_CLOEXEC);

    if (pthread_mutex_init(&s_watcher.lock, NULL) != 0)
        goto fail_fds;

    s_watcher.running     = true;
    s_watcher.next_handle = 1;
    if (pthread_create(&s_watcher.thread, NULL, watcher_thread_linux, NULL) != 0) {
        s_watcher.running = false;
        pthread_mutex_destroy(&s_watcher.lock);
        goto fail_fds;
    }
    return true;

fail_fds:
    close(s_watcher.inotify_fd);
    close(s_watcher.stop_pipe[0]);
    close(s_watcher.stop_pipe[1]);
    return false;
}

void ctx_watcher_shutdown(void)
{
    if (!s_watcher.running) return;

    uint8_t byte = 1;
    while (write(s_watcher.stop_pipe[1], &byte, 1) < 0 && errno == EINTR) {}
    pthread_join(s_watcher.thread, NULL);

    pthread_mutex_lock(&s_watcher.lock);
    s_watcher.running = false;
    InotifyWatch *w, *tmp;
    HASH_ITER(hh, s_watches, w, tmp)
        inotify_drop(w, false);
    pthread_mutex_unlock(&s_watcher.lock);

    file_states_clear();
    close(s_watcher.inotify_fd);
    close(s_watcher.stop_pipe[0]);
    close(s_watcher.stop_pipe[1]);
    pthread_mutex_destroy(&s_watcher.lock);
}

CtxWatchHandle ctx_watcher_add(const char *path, bool recursive)
{
    if (!path || !path[0] || !s_watcher.running) return CTX_WATCH_HANDLE_INVALID;

    char root[CTX_WATCHER_PATH_MAX];
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    if (len >= sizeof(root)) return CTX_WATCH_HANDLE_INVALID;
    memcpy(root, path, len);
    root[len] = '\0';

    struct stat st;
    if (stat(root, &st) != 0 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
        return CTX_WATCH_HANDLE_INVALID;

    pthread_mutex_lock(&s_watcher.lock);
    CtxWatchHandle handle = s_watcher.next_handle++;
    unsigned before = HASH_COUNT(s_watches);
    if (S_ISDIR(st.st_mode))
        inotify_add_tree(handle, root, recursive, true);
    else
        inotify_add_one(handle, root, false, false, true);
    bool added = HASH_COUNT(s_watches) > before;
    pthread_mutex_unlock(&s_watcher.lock);

    return added ? handle : CTX_WATCH_HANDLE_INVALID;
}

void ctx_watcher_remove(CtxWatchHandle handle)
{
    if (!handle || !s_watcher.running) return;
    pthread_mutex_lock(&s_watcher.lock);
    InotifyWatch *w, *tmp;
    HASH_ITER(hh, s_watches, w, tmp)
        if (w->handle == handle)
            inotify_drop(w, true);
    pthread_mutex_unlock(&s_watcher.lock);
}

/* ============================================================
 * MACOS — FSEvents
 *
 * One FSEventStream per watch handle, delivered on a serial dispatch queue.
 * File-level events are coalesced by FSEvents over CTX_FSEVENTS_LATENCY_S;
 * each batch is filtered by per-path (mtime, size) state so metadata-only
 * changes (atime, xattr, Spotlight) and sticky historical flags do not
 * produce duplicate updates. Directory creation/removal/rename and dropped
 * or rescan-required events are reported as directory events, which
 * consumers resolve with a full rescan.
 * ============================================================ */
#elif defined(CTX_PLATFORM_MACOS)

#include <fcntl.h>
#include <sys/stat.h>

#define CTX_FSEVENTS_LATENCY_S 0.075

#define CTX_FSEVENTS_CONTENT_FLAGS (kFSEventStreamEventFlagItemCreated  | \
                                    kFSEventStreamEventFlagItemRemoved  | \
                                    kFSEventStreamEventFlagItemRenamed  | \
                                    kFSEventStreamEventFlagItemModified)

#define CTX_FSEVENTS_RESCAN_FLAGS  (kFSEventStreamEventFlagMustScanSubDirs | \
                                    kFSEventStreamEventFlagUserDropped    | \
                                    kFSEventStreamEventFlagKernelDropped  | \
                                    kFSEventStreamEventFlagRootChanged    | \
                                    kFSEventStreamEventFlagMount          | \
                                    kFSEventStreamEventFlagUnmount)

/* Snapshot of a watch entry taken under the lock for use by the callback. */
typedef struct {
    char   path[CTX_WATCHER_PATH_MAX];
    char   real_path[CTX_WATCHER_PATH_MAX];
    size_t real_len;
    bool   is_dir;
    bool   recursive;
} WatchSnapshot;

/* Returns the index of the entry for handle, or -1. Caller holds lock. */
static int fsevents_find_entry(CtxWatchHandle handle)
{
    for (uint32_t i = 0; i < s_watcher.count; i++)
        if (s_watcher.entries[i].active && s_watcher.entries[i].handle == handle)
            return (int)i;
    return -1;
}

/* Stops, invalidates and releases a stream. Must not be called with the
   lock held: invalidation may wait for an in-flight callback that is
   blocked on the lock. */
static void fsevents_release_stream(FSEventStreamRef stream)
{
    if (!stream) return;
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
}

/* Queue drain barrier for dispatch_sync_f. */
static void fsevents_queue_barrier(void *context)
{
    CTX_UNUSED(context);
}

/*
 * Maps a canonical FSEvents path onto the watched path as the caller gave
 * it, so emitted paths match the indexer's stored paths.
 *
 * snap      Watch the event belongs to.
 * reported  Canonical path reported by FSEvents.
 * out       Receives the mapped path (CTX_WATCHER_PATH_MAX bytes).
 * Returns   false when the path is outside the watch, below a directory
 *           excluded by watcher_skips_dir, hidden ('.'-prefixed), beyond
 *           depth 1 of a non-recursive directory watch, or too long.
 */
static bool fsevents_map_path(const WatchSnapshot *snap, const char *reported,
                              char *out)
{
    if (strncmp(reported, snap->real_path, snap->real_len) != 0) return false;
    const char *suffix = reported + snap->real_len;
    if (suffix[0] != '\0' && suffix[0] != '/') return false;
    if (!snap->is_dir && suffix[0] != '\0') return false;

    /* suffix is "" or "/a/b/name": every component but the last is a
       directory; the last may be a file or directory (callers check the
       latter with watcher_skips_dir on dir events). */
    const char *component = suffix;
    while (*component == '/') {
        const char *name = component + 1;
        const char *next = strchr(name, '/');
        if (name[0] == '.') return false;
        if (next) {
            char dir_name[NAME_MAX + 1];
            size_t len = (size_t)(next - name);
            if (len == 0 || len > NAME_MAX) return false;
            memcpy(dir_name, name, len);
            dir_name[len] = '\0';
            if (!snap->recursive || watcher_skips_dir(dir_name)) return false;
        }
        component = next ? next : name + strlen(name);
    }

    const char *prefix = strcmp(snap->path, "/") == 0 ? "" : snap->path;
    int n = snprintf(out, CTX_WATCHER_PATH_MAX, "%s%s", prefix, suffix);
    return n > 0 && (size_t)n < CTX_WATCHER_PATH_MAX;
}

/* FSEventStreamCallback: translates one coalesced batch into file events.
   info carries the watch handle; the entry is looked up under the lock
   because entries are compacted on removal. */
static void fsevents_callback(ConstFSEventStreamRef stream, void *info,
                              size_t count, void *event_paths,
                              const FSEventStreamEventFlags flags[],
                              const FSEventStreamEventId ids[])
{
    CTX_UNUSED(stream);
    CTX_UNUSED(ids);

    WatchSnapshot *snap = malloc(sizeof(*snap));
    char *mapped = malloc(CTX_WATCHER_PATH_MAX);
    if (!snap || !mapped) {
        free(snap);
        free(mapped);
        return;
    }

    CtxWatchHandle handle = (CtxWatchHandle)(uintptr_t)info;
    pthread_mutex_lock(&s_watcher.lock);
    int index = fsevents_find_entry(handle);
    if (index >= 0) {
        const CtxWatchEntry *entry = &s_watcher.entries[index];
        snprintf(snap->path, sizeof(snap->path), "%s", entry->path);
        snprintf(snap->real_path, sizeof(snap->real_path), "%s", entry->real_path);
        snap->real_len = entry->real_len;
        snap->is_dir = entry->is_dir;
        snap->recursive = entry->recursive;
    }
    pthread_mutex_unlock(&s_watcher.lock);

    char **paths = event_paths;
    for (size_t i = 0; index >= 0 && i < count; ++i) {
        FSEventStreamEventFlags f = flags[i];

        if (f & CTX_FSEVENTS_RESCAN_FLAGS) {
            if (snap->is_dir)
                emit_file_event(CTX_FILE_EVENT_MODIFIED, snap->path, NULL, true);
            else if (file_state_changed(snap->path, &(CtxFileEventKind){0}))
                emit_file_event(CTX_FILE_EVENT_MODIFIED, snap->path, NULL, false);
            continue;
        }
        if (!(f & CTX_FSEVENTS_CONTENT_FLAGS)) continue;
        if (!fsevents_map_path(snap, paths[i], mapped)) continue;

        if (f & kFSEventStreamEventFlagItemIsDir) {
            const char *base = strrchr(mapped, '/');
            if (base && watcher_skips_dir(base + 1)) continue;
            if (!(f & (kFSEventStreamEventFlagItemCreated |
                       kFSEventStreamEventFlagItemRemoved |
                       kFSEventStreamEventFlagItemRenamed)))
                continue;
            struct stat st;
            bool exists = stat(mapped, &st) == 0 && S_ISDIR(st.st_mode);
            emit_file_event(exists ? CTX_FILE_EVENT_CREATED : CTX_FILE_EVENT_DELETED,
                            mapped, NULL, true);
            continue;
        }

        CtxFileEventKind kind;
        if (file_state_changed(mapped, &kind))
            emit_file_event(kind, mapped, NULL, false);
    }

    free(snap);
    free(mapped);
}

bool ctx_watcher_init(void)
{
    memset(&s_watcher, 0, sizeof(s_watcher));

    s_watcher.queue = dispatch_queue_create("ctx.watcher.fsevents",
                                            DISPATCH_QUEUE_SERIAL);
    if (!s_watcher.queue) return false;
    if (pthread_mutex_init(&s_watcher.lock, NULL) != 0) {
        dispatch_release(s_watcher.queue);
        s_watcher.queue = NULL;
        return false;
    }

    s_watcher.running     = true;
    s_watcher.next_handle = 1;
    return true;
}

void ctx_watcher_shutdown(void)
{
    if (!s_watcher.running) return;

    pthread_mutex_lock(&s_watcher.lock);
    s_watcher.running = false;
    FSEventStreamRef streams[CTX_WATCHER_MAX_WATCHES];
    uint32_t stream_count = 0;
    for (uint32_t i = 0; i < s_watcher.count; ++i)
        if (s_watcher.entries[i].stream)
            streams[stream_count++] = s_watcher.entries[i].stream;
    s_watcher.count = 0;
    pthread_mutex_unlock(&s_watcher.lock);

    for (uint32_t i = 0; i < stream_count; ++i)
        fsevents_release_stream(streams[i]);

    dispatch_sync_f(s_watcher.queue, NULL, fsevents_queue_barrier);
    dispatch_release(s_watcher.queue);
    s_watcher.queue = NULL;
    file_states_clear();
    pthread_mutex_destroy(&s_watcher.lock);
}

CtxWatchHandle ctx_watcher_add(const char *path, bool recursive)
{
    if (!path || !path[0] || !s_watcher.running) return CTX_WATCH_HANDLE_INVALID;

    CtxWatchEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.recursive = recursive;

    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    if (len >= sizeof(entry.path)) return CTX_WATCH_HANDLE_INVALID;
    memcpy(entry.path, path, len);
    entry.path[len] = '\0';

    /* F_GETPATH yields the canonical path (symlinks resolved, on-disk case)
       that FSEvents reports, unlike realpath() on case-insensitive volumes. */
    int fd = open(entry.path, O_RDONLY | O_EVTONLY | O_CLOEXEC);
    if (fd < 0) return CTX_WATCH_HANDLE_INVALID;
    struct stat st;
    char canonical[MAXPATHLEN];
    bool ok = fstat(fd, &st) == 0 && fcntl(fd, F_GETPATH, canonical) != -1;
    close(fd);
    if (!ok || strlen(canonical) >= sizeof(entry.real_path))
        return CTX_WATCH_HANDLE_INVALID;

    entry.is_dir = S_ISDIR(st.st_mode);
    snprintf(entry.real_path, sizeof(entry.real_path), "%s", canonical);
    entry.real_len = strlen(entry.real_path);
    if (entry.real_len == 1) entry.real_len = 0; /* "/": every path matches */

    /* Files are watched through their parent directory and filtered to the
       exact path in fsevents_map_path. */
    char stream_root[MAXPATHLEN];
    snprintf(stream_root, sizeof(stream_root), "%s", entry.real_path);
    if (!entry.is_dir) {
        char *slash = strrchr(stream_root, '/');
        if (!slash) return CTX_WATCH_HANDLE_INVALID;
        slash[slash == stream_root ? 1 : 0] = '\0';
    }

    pthread_mutex_lock(&s_watcher.lock);
    if (s_watcher.count >= CTX_WATCHER_MAX_WATCHES) {
        pthread_mutex_unlock(&s_watcher.lock);
        return CTX_WATCH_HANDLE_INVALID;
    }
    entry.handle = s_watcher.next_handle++;
    pthread_mutex_unlock(&s_watcher.lock);

    CFStringRef root = CFStringCreateWithFileSystemRepresentation(NULL, stream_root);
    CFArrayRef roots = root ? CFArrayCreate(NULL, (const void **)&root, 1,
                                            &kCFTypeArrayCallBacks) : NULL;
    FSEventStreamContext context = {
        .version = 0,
        .info    = (void *)(uintptr_t)entry.handle,
    };
    FSEventStreamRef stream = roots ? FSEventStreamCreate(
        NULL, fsevents_callback, &context, roots,
        kFSEventStreamEventIdSinceNow, CTX_FSEVENTS_LATENCY_S,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot) : NULL;
    if (roots) CFRelease(roots);
    if (root) CFRelease(root);
    if (!stream) return CTX_WATCH_HANDLE_INVALID;

    entry.stream = stream;
    entry.active = true;

    /* Publish before starting so the first callback finds the entry. */
    pthread_mutex_lock(&s_watcher.lock);
    bool published = s_watcher.running && s_watcher.count < CTX_WATCHER_MAX_WATCHES;
    if (published)
        s_watcher.entries[s_watcher.count++] = entry;
    pthread_mutex_unlock(&s_watcher.lock);
    if (!published) {
        FSEventStreamRelease(stream);
        return CTX_WATCH_HANDLE_INVALID;
    }

    FSEventStreamSetDispatchQueue(stream, s_watcher.queue);
    if (!FSEventStreamStart(stream)) {
        pthread_mutex_lock(&s_watcher.lock);
        int index = fsevents_find_entry(entry.handle);
        if (index >= 0)
            s_watcher.entries[index] = s_watcher.entries[--s_watcher.count];
        pthread_mutex_unlock(&s_watcher.lock);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        return CTX_WATCH_HANDLE_INVALID;
    }
    return entry.handle;
}

void ctx_watcher_remove(CtxWatchHandle handle)
{
    if (!handle || !s_watcher.running) return;

    pthread_mutex_lock(&s_watcher.lock);
    FSEventStreamRef stream = NULL;
    int index = fsevents_find_entry(handle);
    if (index >= 0) {
        stream = s_watcher.entries[index].stream;
        s_watcher.entries[index] = s_watcher.entries[--s_watcher.count];
    }
    pthread_mutex_unlock(&s_watcher.lock);

    fsevents_release_stream(stream);
}

/* ============================================================
 * WINDOWS — ReadDirectoryChangesW
 * ============================================================ */
#elif defined(CTX_PLATFORM_WINDOWS)

static DWORD WINAPI watcher_thread_windows(LPVOID arg)
{
    CTX_UNUSED(arg);

    /* Collect all active OVERLAPPED handles + stop event */
    while (true) {
        EnterCriticalSection(&s_watcher.lock);
        uint32_t count = s_watcher.count;

        /* Build wait list: stop_event first, then per-entry events */
        HANDLE wait_handles[CTX_WATCHER_MAX_WATCHES + 1];
        uint32_t nhandles = 0;
        wait_handles[nhandles++] = s_watcher.stop_event;

        for (uint32_t i = 0; i < count; ++i) {
            CtxWatchEntry *e = &s_watcher.entries[i];
            if (e->active && !e->pending && e->dir_handle != INVALID_HANDLE_VALUE) {
                DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME  |
                               FILE_NOTIFY_CHANGE_DIR_NAME   |
                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_SIZE;
                ReadDirectoryChangesW(e->dir_handle, e->buf, sizeof(e->buf),
                                      e->recursive,
                                      filter,
                                      NULL, &e->overlapped, NULL);
                e->pending = true;
                wait_handles[nhandles++] = e->overlapped.hEvent;
            }
        }
        LeaveCriticalSection(&s_watcher.lock);

        DWORD idx = WaitForMultipleObjects(nhandles, wait_handles, FALSE, INFINITE);
        if (idx == WAIT_FAILED || idx == WAIT_OBJECT_0) break; /* stop */

        uint32_t entry_idx = idx - WAIT_OBJECT_0 - 1;

        EnterCriticalSection(&s_watcher.lock);
        if (entry_idx >= s_watcher.count) {
            LeaveCriticalSection(&s_watcher.lock);
            continue;
        }

        CtxWatchEntry *e = &s_watcher.entries[entry_idx];
        e->pending = false;

        DWORD bytes_returned = 0;
        if (!GetOverlappedResult(e->dir_handle, &e->overlapped,
                                 &bytes_returned, FALSE) || !bytes_returned) {
            LeaveCriticalSection(&s_watcher.lock);
            continue;
        }

        FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)e->buf;
        char prev_name[CTX_WATCHER_PATH_MAX] = {0};

        while (true) {
            char name_utf8[CTX_WATCHER_PATH_MAX];
            WideCharToMultiByte(CP_UTF8, 0,
                                fni->FileName,
                                (int)(fni->FileNameLength / sizeof(WCHAR)),
                                name_utf8, sizeof(name_utf8) - 1, NULL, NULL);
            name_utf8[CTX_WATCHER_PATH_MAX - 1] = '\0';

            char full_path[CTX_WATCHER_PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s\\%s", e->path, name_utf8);

            switch (fni->Action) {
            case FILE_ACTION_ADDED:
                emit_file_event(CTX_FILE_EVENT_CREATED, full_path, NULL, false);
                break;
            case FILE_ACTION_REMOVED:
                emit_file_event(CTX_FILE_EVENT_DELETED, full_path, NULL, false);
                break;
            case FILE_ACTION_MODIFIED:
                emit_file_event(CTX_FILE_EVENT_MODIFIED, full_path, NULL, false);
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                strncpy(prev_name, full_path, sizeof(prev_name) - 1);
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                emit_file_event(CTX_FILE_EVENT_RENAMED, full_path, prev_name, false);
                prev_name[0] = '\0';
                break;
            default:
                break;
            }

            if (!fni->NextEntryOffset) break;
            fni = (FILE_NOTIFY_INFORMATION *)((uint8_t *)fni + fni->NextEntryOffset);
        }

        ResetEvent(e->overlapped.hEvent);
        LeaveCriticalSection(&s_watcher.lock);
    }
    return 0;
}

bool ctx_watcher_init(void)
{
    memset(&s_watcher, 0, sizeof(s_watcher));
    InitializeCriticalSection(&s_watcher.lock);

    s_watcher.stop_event  = CreateEvent(NULL, TRUE, FALSE, NULL);
    s_watcher.running     = true;
    s_watcher.next_handle = 1;

    s_watcher.thread = CreateThread(NULL, 0, watcher_thread_windows, NULL, 0, NULL);
    return s_watcher.thread != NULL;
}

void ctx_watcher_shutdown(void)
{
    if (!s_watcher.running) return;

    SetEvent(s_watcher.stop_event);
    WaitForSingleObject(s_watcher.thread, INFINITE);
    CloseHandle(s_watcher.thread);
    CloseHandle(s_watcher.stop_event);

    for (uint32_t i = 0; i < s_watcher.count; ++i) {
        CtxWatchEntry *e = &s_watcher.entries[i];
        if (e->dir_handle != INVALID_HANDLE_VALUE) {
            CancelIo(e->dir_handle);
            CloseHandle(e->overlapped.hEvent);
            CloseHandle(e->dir_handle);
        }
    }

    DeleteCriticalSection(&s_watcher.lock);
    s_watcher.running = false;
}

CtxWatchHandle ctx_watcher_add(const char *path, bool recursive)
{
    if (!path || !s_watcher.running) return CTX_WATCH_HANDLE_INVALID;

    EnterCriticalSection(&s_watcher.lock);

    if (s_watcher.count >= CTX_WATCHER_MAX_WATCHES) {
        LeaveCriticalSection(&s_watcher.lock);
        return CTX_WATCH_HANDLE_INVALID;
    }

    CtxWatchEntry *entry = &s_watcher.entries[s_watcher.count++];
    memset(entry, 0, sizeof(*entry));

    entry->handle     = s_watcher.next_handle++;
    entry->recursive  = recursive;
    entry->active     = true;
    strncpy(entry->path, path, CTX_WATCHER_PATH_MAX - 1);

    entry->dir_handle = CreateFileA(
        path,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);

    if (entry->dir_handle == INVALID_HANDLE_VALUE) {
        s_watcher.count--;
        LeaveCriticalSection(&s_watcher.lock);
        return CTX_WATCH_HANDLE_INVALID;
    }

    entry->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    LeaveCriticalSection(&s_watcher.lock);
    return entry->handle;
}

void ctx_watcher_remove(CtxWatchHandle handle)
{
    if (!handle) return;
    EnterCriticalSection(&s_watcher.lock);
    for (uint32_t i = 0; i < s_watcher.count; ++i) {
        if (s_watcher.entries[i].handle == handle) {
            CtxWatchEntry *e = &s_watcher.entries[i];
            CancelIo(e->dir_handle);
            CloseHandle(e->overlapped.hEvent);
            CloseHandle(e->dir_handle);
            s_watcher.entries[i] = s_watcher.entries[--s_watcher.count];
            break;
        }
    }
    LeaveCriticalSection(&s_watcher.lock);
}

#else
#   error "Unsupported platform — define CTX_PLATFORM_LINUX, CTX_PLATFORM_MACOS, or CTX_PLATFORM_WINDOWS"
#endif

bool ctx_watcher_is_running(void)
{
#if defined(CTX_PLATFORM_LINUX)
    return s_watcher.running && !atomic_load(&s_watcher.thread_failed);
#else
    return s_watcher.running;
#endif
}

uint32_t ctx_watcher_active_count(void)
{
    if (!s_watcher.running) return 0;

#if defined(CTX_PLATFORM_LINUX)
    pthread_mutex_lock(&s_watcher.lock);
    uint32_t watches = (uint32_t)HASH_COUNT(s_watches);
    pthread_mutex_unlock(&s_watcher.lock);
    return watches;
#else

#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_watcher.lock);
#else
    pthread_mutex_lock(&s_watcher.lock);
#endif

    uint32_t active = 0;
    for (uint32_t i = 0; i < s_watcher.count; ++i)
        if (s_watcher.entries[i].active)
            active++;

#if defined(CTX_PLATFORM_WINDOWS)
    LeaveCriticalSection(&s_watcher.lock);
#else
    pthread_mutex_unlock(&s_watcher.lock);
#endif

    return active;
#endif
}

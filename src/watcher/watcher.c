#include "watcher.h"
#include "../event/event.h"

/* ============================================================
 * Shared types
 * ============================================================ */

typedef struct {
    CtxWatchHandle  handle;
    char            path[CTX_WATCHER_PATH_MAX];
    bool            recursive;
    bool            active;

    /* Platform-specific descriptor */
#if defined(CTX_PLATFORM_LINUX)
    int             wd;     /* inotify watch descriptor */
#elif defined(CTX_PLATFORM_MACOS)
    int             fd;     /* kqueue-registered file descriptor */
#elif defined(CTX_PLATFORM_WINDOWS)
    HANDLE          dir_handle;
    OVERLAPPED      overlapped;
    uint8_t         buf[65536];
    bool            pending;
#endif
} CtxWatchEntry;

typedef struct {
    CtxWatchEntry   entries[CTX_WATCHER_MAX_WATCHES];
    uint32_t        count;
    uint32_t        next_handle;

#if defined(CTX_PLATFORM_WINDOWS)
    HANDLE           thread;
    HANDLE           stop_event;
    CRITICAL_SECTION lock;
#else
    pthread_t        thread;
    int              stop_pipe[2];   /* write [1] to signal shutdown */
    pthread_mutex_t  lock;
#endif

    bool             running;

#if defined(CTX_PLATFORM_LINUX)
    int              inotify_fd;
#elif defined(CTX_PLATFORM_MACOS)
    int              kq;
#endif
} CtxWatcher;

static CtxWatcher s_watcher;

/* ============================================================
 * Internal helpers
 * ============================================================ */

static void emit_file_event(CtxFileEventKind kind,
                            const char *path,
                            const char *old_path)
{
    CtxFileEvent ev;
    ev.kind = kind;
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
 * LINUX — inotify
 * ============================================================ */
#if defined(CTX_PLATFORM_LINUX)

#include <dirent.h>
#include <sys/inotify.h>

#define INOTIFY_FLAGS (IN_CREATE | IN_MODIFY | IN_DELETE | \
                       IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE)

static CtxWatchEntry *find_by_wd(int wd)
{
    for (uint32_t i = 0; i < s_watcher.count; ++i)
        if (s_watcher.entries[i].active && s_watcher.entries[i].wd == wd)
            return &s_watcher.entries[i];
    return NULL;
}

static void add_inotify_watch_recursive(CtxWatchEntry *entry, const char *path)
{
    int wd = inotify_add_watch(s_watcher.inotify_fd, path, INOTIFY_FLAGS);
    if (wd < 0) return;
    if (entry->wd < 0) entry->wd = wd; /* store primary wd */

    if (!entry->recursive) return;

    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        if (de->d_type == DT_DIR) {
            char sub[CTX_WATCHER_PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            add_inotify_watch_recursive(entry, sub);
        }
    }
    closedir(dir);
}

static void *watcher_thread_linux(void *arg)
{
    CTX_UNUSED(arg);

    uint8_t  buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    struct pollfd fds[2];
    fds[0].fd      = s_watcher.inotify_fd;
    fds[0].events  = POLLIN;
    fds[1].fd      = s_watcher.stop_pipe[0];
    fds[1].events  = POLLIN;

    /* Pending rename: track IN_MOVED_FROM until we see IN_MOVED_TO */
    char    rename_from[CTX_WATCHER_PATH_MAX] = {0};
    uint32_t rename_cookie = 0;

    while (true) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) break;

        /* Stop signal */
        if (fds[1].revents & POLLIN) break;

        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t len = read(s_watcher.inotify_fd, buf, sizeof(buf));
        if (len < 0) continue;

        ssize_t offset = 0;
        while (offset < len) {
            struct inotify_event *ie =
                (struct inotify_event *)(buf + offset);
            offset += (ssize_t)(sizeof(struct inotify_event) + ie->len);

            CtxWatchEntry *entry = find_by_wd(ie->wd);
            if (!entry) continue;

            char full_path[CTX_WATCHER_PATH_MAX];
            if (ie->len > 0) {
                int needed = snprintf(full_path, sizeof(full_path), "%s/%s",
                                      entry->path, ie->name);
                if (needed < 0 || (size_t)needed >= sizeof(full_path))
                    continue; /* path too long — skip */
            } else {
                strncpy(full_path, entry->path, sizeof(full_path) - 1);
                full_path[sizeof(full_path) - 1] = '\0';
            }

            if (ie->mask & IN_CREATE)
                emit_file_event(CTX_FILE_EVENT_CREATED, full_path, NULL);
            else if (ie->mask & (IN_MODIFY | IN_CLOSE_WRITE))
                emit_file_event(CTX_FILE_EVENT_MODIFIED, full_path, NULL);
            else if (ie->mask & IN_DELETE)
                emit_file_event(CTX_FILE_EVENT_DELETED, full_path, NULL);
            else if (ie->mask & IN_MOVED_FROM) {
                strncpy(rename_from, full_path, sizeof(rename_from) - 1);
                rename_cookie = ie->cookie;
            } else if (ie->mask & IN_MOVED_TO) {
                if (ie->cookie == rename_cookie && rename_from[0])
                    emit_file_event(CTX_FILE_EVENT_RENAMED, full_path, rename_from);
                else
                    emit_file_event(CTX_FILE_EVENT_CREATED, full_path, NULL);
                rename_from[0]  = '\0';
                rename_cookie   = 0;
            }
        }
    }
    return NULL;
}

bool ctx_watcher_init(void)
{
    memset(&s_watcher, 0, sizeof(s_watcher));

    s_watcher.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_watcher.inotify_fd < 0) return false;

    if (pipe(s_watcher.stop_pipe) != 0) {
        close(s_watcher.inotify_fd);
        return false;
    }

    pthread_mutex_init(&s_watcher.lock, NULL);

    if (pthread_create(&s_watcher.thread, NULL, watcher_thread_linux, NULL) != 0)
        return false;

    s_watcher.running     = true;
    s_watcher.next_handle = 1;
    return true;
}

void ctx_watcher_shutdown(void)
{
    if (!s_watcher.running) return;

    uint8_t byte = 1;
    write(s_watcher.stop_pipe[1], &byte, 1);
    pthread_join(s_watcher.thread, NULL);
    pthread_mutex_destroy(&s_watcher.lock);
    close(s_watcher.inotify_fd);
    close(s_watcher.stop_pipe[0]);
    close(s_watcher.stop_pipe[1]);
    s_watcher.running = false;
}

CtxWatchHandle ctx_watcher_add(const char *path, bool recursive)
{
    if (!path || !s_watcher.running) return CTX_WATCH_HANDLE_INVALID;

    pthread_mutex_lock(&s_watcher.lock);

    if (s_watcher.count >= CTX_WATCHER_MAX_WATCHES) {
        pthread_mutex_unlock(&s_watcher.lock);
        return CTX_WATCH_HANDLE_INVALID;
    }

    CtxWatchEntry *entry = &s_watcher.entries[s_watcher.count++];
    memset(entry, 0, sizeof(*entry));
    entry->wd        = -1;
    entry->handle    = s_watcher.next_handle++;
    entry->recursive = recursive;
    entry->active    = true;
    strncpy(entry->path, path, CTX_WATCHER_PATH_MAX - 1);

    add_inotify_watch_recursive(entry, path);

    pthread_mutex_unlock(&s_watcher.lock);
    return entry->handle;
}

void ctx_watcher_remove(CtxWatchHandle handle)
{
    if (!handle) return;
    pthread_mutex_lock(&s_watcher.lock);
    for (uint32_t i = 0; i < s_watcher.count; ++i) {
        if (s_watcher.entries[i].handle == handle) {
            inotify_rm_watch(s_watcher.inotify_fd, s_watcher.entries[i].wd);
            s_watcher.entries[i] = s_watcher.entries[--s_watcher.count];
            break;
        }
    }
    pthread_mutex_unlock(&s_watcher.lock);
}

/* ============================================================
 * MACOS — kqueue
 * ============================================================ */
#elif defined(CTX_PLATFORM_MACOS)

#include <dirent.h>
#include <sys/event.h>

#define VNODE_FLAGS (NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME | \
                     NOTE_DELETE | NOTE_EXTEND | NOTE_LINK)

static void add_kqueue_watch(CtxWatchEntry *entry, const char *path)
{
    int fd = open(path, O_RDONLY | O_EVTONLY | O_CLOEXEC);
    if (fd < 0) return;

    struct kevent kev;
    EV_SET(&kev, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           VNODE_FLAGS, 0, entry);
    kevent(s_watcher.kq, &kev, 1, NULL, 0, NULL);

    if (entry->fd < 0) entry->fd = fd;
    else close(fd); /* only store primary fd; recursive fds leak intentionally
                       small — kqueue cleans up on close */

    if (!entry->recursive) return;

    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        if (de->d_type == DT_DIR) {
            char sub[CTX_WATCHER_PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
            add_kqueue_watch(entry, sub);
        }
    }
    closedir(dir);
}

static void *watcher_thread_macos(void *arg)
{
    CTX_UNUSED(arg);

    struct kevent events[32];
    /* Register stop-pipe read end as a EVFILT_READ event */
    struct kevent stop_kev;
    EV_SET(&stop_kev, (uintptr_t)s_watcher.stop_pipe[0],
           EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(s_watcher.kq, &stop_kev, 1, NULL, 0, NULL);

    while (true) {
        int nev = kevent(s_watcher.kq, NULL, 0, events, 32, NULL);
        if (nev < 0) break;

        for (int i = 0; i < nev; ++i) {
            /* Stop signal */
            if ((int)events[i].ident == s_watcher.stop_pipe[0]) goto done;

            CtxWatchEntry *entry = (CtxWatchEntry *)events[i].udata;
            if (!entry) continue;

            uint32_t fflags = (uint32_t)events[i].fflags;
            if (fflags & NOTE_DELETE)
                emit_file_event(CTX_FILE_EVENT_DELETED, entry->path, NULL);
            else if (fflags & NOTE_RENAME)
                emit_file_event(CTX_FILE_EVENT_RENAMED, entry->path, NULL);
            else if (fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB))
                emit_file_event(CTX_FILE_EVENT_MODIFIED, entry->path, NULL);
        }
    }
done:
    return NULL;
}

bool ctx_watcher_init(void)
{
    memset(&s_watcher, 0, sizeof(s_watcher));

    s_watcher.kq = kqueue();
    if (s_watcher.kq < 0) return false;

    if (pipe(s_watcher.stop_pipe) != 0) {
        close(s_watcher.kq);
        return false;
    }

    pthread_mutex_init(&s_watcher.lock, NULL);

    if (pthread_create(&s_watcher.thread, NULL, watcher_thread_macos, NULL) != 0)
        return false;

    s_watcher.running     = true;
    s_watcher.next_handle = 1;
    return true;
}

void ctx_watcher_shutdown(void)
{
    if (!s_watcher.running) return;

    uint8_t byte = 1;
    write(s_watcher.stop_pipe[1], &byte, 1);
    pthread_join(s_watcher.thread, NULL);
    pthread_mutex_destroy(&s_watcher.lock);

    for (uint32_t i = 0; i < s_watcher.count; ++i)
        if (s_watcher.entries[i].fd >= 0)
            close(s_watcher.entries[i].fd);

    close(s_watcher.kq);
    close(s_watcher.stop_pipe[0]);
    close(s_watcher.stop_pipe[1]);
    s_watcher.running = false;
}

CtxWatchHandle ctx_watcher_add(const char *path, bool recursive)
{
    if (!path || !s_watcher.running) return CTX_WATCH_HANDLE_INVALID;

    pthread_mutex_lock(&s_watcher.lock);

    if (s_watcher.count >= CTX_WATCHER_MAX_WATCHES) {
        pthread_mutex_unlock(&s_watcher.lock);
        return CTX_WATCH_HANDLE_INVALID;
    }

    CtxWatchEntry *entry = &s_watcher.entries[s_watcher.count++];
    memset(entry, 0, sizeof(*entry));
    entry->fd        = -1;
    entry->handle    = s_watcher.next_handle++;
    entry->recursive = recursive;
    entry->active    = true;
    strncpy(entry->path, path, CTX_WATCHER_PATH_MAX - 1);

    add_kqueue_watch(entry, path);

    pthread_mutex_unlock(&s_watcher.lock);
    return entry->handle;
}

void ctx_watcher_remove(CtxWatchHandle handle)
{
    if (!handle) return;
    pthread_mutex_lock(&s_watcher.lock);
    for (uint32_t i = 0; i < s_watcher.count; ++i) {
        if (s_watcher.entries[i].handle == handle) {
            if (s_watcher.entries[i].fd >= 0)
                close(s_watcher.entries[i].fd);
            s_watcher.entries[i] = s_watcher.entries[--s_watcher.count];
            break;
        }
    }
    pthread_mutex_unlock(&s_watcher.lock);
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
                emit_file_event(CTX_FILE_EVENT_CREATED, full_path, NULL);
                break;
            case FILE_ACTION_REMOVED:
                emit_file_event(CTX_FILE_EVENT_DELETED, full_path, NULL);
                break;
            case FILE_ACTION_MODIFIED:
                emit_file_event(CTX_FILE_EVENT_MODIFIED, full_path, NULL);
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                strncpy(prev_name, full_path, sizeof(prev_name) - 1);
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                emit_file_event(CTX_FILE_EVENT_RENAMED, full_path, prev_name);
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

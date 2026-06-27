#pragma once

#include "../pch.h"

/* --------------------------------------------------------------------------
 * File watcher
 *
 * Design:
 *   - A single background thread polls the OS watch mechanism.
 *   - Changes are translated to CtxFileEvent and dispatched through the
 *     event system (CTX_EVENT_FILE_*).
 *   - Supports watching individual files or entire directory trees.
 *   - The caller receives a CtxWatchHandle which it can use to stop watching.
 *
 * Event payload:
 *   CtxEvent.data points to a CtxFileEvent (stack-allocated inside the
 *   watcher thread; callbacks must copy what they need before returning).
 * -------------------------------------------------------------------------- */

#define CTX_WATCHER_MAX_WATCHES  256
#define CTX_WATCHER_PATH_MAX     4096

typedef uint32_t CtxWatchHandle;
#define CTX_WATCH_HANDLE_INVALID 0u

typedef enum {
    CTX_FILE_EVENT_CREATED  = 0,
    CTX_FILE_EVENT_MODIFIED = 1,
    CTX_FILE_EVENT_DELETED  = 2,
    CTX_FILE_EVENT_RENAMED  = 3
} CtxFileEventKind;

typedef struct {
    CtxFileEventKind  kind;
    char              path[CTX_WATCHER_PATH_MAX];
    char              old_path[CTX_WATCHER_PATH_MAX]; /* only for RENAMED */
} CtxFileEvent;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
bool ctx_watcher_init(void);
void ctx_watcher_shutdown(void);

/* Returns true while the platform watcher thread is running. */
bool ctx_watcher_is_running(void);

/* Returns the number of active platform watch entries. */
uint32_t ctx_watcher_active_count(void);

/* --------------------------------------------------------------------------
 * Watch / unwatch
 * -------------------------------------------------------------------------- */

/* Watch a path (file or directory).
 * recursive: if true and path is a directory, watch subdirectories too.
 * Returns CTX_WATCH_HANDLE_INVALID on error. */
CtxWatchHandle ctx_watcher_add(const char *path, bool recursive);

void           ctx_watcher_remove(CtxWatchHandle handle);

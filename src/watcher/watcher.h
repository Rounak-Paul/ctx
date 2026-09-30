#pragma once

#include "../pch.h"

/* --------------------------------------------------------------------------
 * File watcher
 *
 * Design:
 *   - Linux: inotify watches per directory on a background thread, events
 *     debounced and deduplicated by file state.
 *   - Windows: a background thread polls ReadDirectoryChangesW.
 *   - macOS: one FSEventStream per watch, delivered on a serial dispatch
 *     queue (no per-file descriptors, so tree size is unbounded).
 *   - Changes are translated to CtxFileEvent and dispatched through the
 *     event system (CTX_EVENT_FILE_*).
 *   - Supports watching individual files or entire directory trees.
 *   - The caller receives a CtxWatchHandle which it can use to stop watching.
 *
 * Event payload:
 *   CtxEvent.data points to a CtxFileEvent (stack-allocated inside the
 *   watcher thread; callbacks must copy what they need before returning).
 * -------------------------------------------------------------------------- */

#define CTX_WATCHER_MAX_WATCHES  256 /* macOS: streams; Windows: directories; Linux: unbounded (kernel limit) */
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
    bool              is_dir; /* path is/was a directory: rescan the tree */
} CtxFileEvent;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
bool ctx_watcher_init(void);
void ctx_watcher_shutdown(void);

/* Returns true while the platform watcher thread is running. */
bool ctx_watcher_is_running(void);

/* Returns the number of active platform watch entries (one per watched
   root on macOS; one per watched directory elsewhere). */
uint32_t ctx_watcher_active_count(void);

/* --------------------------------------------------------------------------
 * Watch / unwatch
 * -------------------------------------------------------------------------- */

/* Excludes directories by name from watching and reporting, in addition to
 * hidden ('.'-prefixed) ones. Call after ctx_watcher_init and before
 * ctx_watcher_add; NULL clears the filter. Not applied on Windows. */
void ctx_watcher_set_dir_filter(bool (*skip_dir)(const char *name));

/* Watch a path (file or directory).
 * recursive: if true and path is a directory, watch subdirectories too.
 * Returns CTX_WATCH_HANDLE_INVALID on error. */
CtxWatchHandle ctx_watcher_add(const char *path, bool recursive);

void           ctx_watcher_remove(CtxWatchHandle handle);

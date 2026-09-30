# File Watcher

## macOS — FSEvents (`src/watcher/watcher.c`)

- One `FSEventStream` per `ctx_watcher_add` (FileEvents | WatchRoot, latency
  75 ms), delivered on a serial dispatch queue. No per-file descriptors, so
  tree size is unbounded (the old kqueue backend capped at 256 fds and
  silently missed most of the full repo).
- Stream context `info` is the watch handle; the callback looks the entry up
  under `s_watcher.lock` (entries are compacted on removal). Streams are
  stopped/invalidated outside the lock; shutdown drains the queue with
  `dispatch_sync_f` before freeing state.
- Path mapping: the watched path is canonicalized with `fcntl(F_GETPATH)`
  (resolves symlinks and on-disk case, unlike `realpath`). Reported paths are
  mapped back onto the caller's path form (e.g. relative `src/...` or a
  symlinked root), which is what the indexer stores.
- Filtering: components starting with `.` are ignored (matches kqueue/indexer
  behaviour: `.git`, `.ctx`, editor temp files). File events require a content
  flag (Created/Removed/Renamed/Modified) and are then deduped by per-path
  `(st_mtimespec, st_size, exists)` state (`FileState` hash, queue-only), which
  absorbs sticky historical flags and metadata-only changes. `touch` alone
  produces no update.
- Directories: Created/Removed/Renamed dir events and rescan conditions
  (MustScanSubDirs, dropped events, RootChanged, mount/unmount) are emitted with
  `CtxFileEvent.is_dir = true`; `main.c` runs a full stat-only rescan, which
  also prunes files under removed/renamed dirs.
- Watching a single file watches its parent and filters to the exact path.

## Consumer (`src/main.c`)

- `FileChangeJob.is_dir || is_dir_path()` -> `request_full_reindex()`
  (coalesced via `s_reindex_pending/again`).
- Per-path job coalescing: `PendingPath` set; events for a path with a queued
  job are dropped; the entry is released when the job starts.

## Linux — inotify (`src/watcher/watcher.c`)

- One watch per directory in a uthash table keyed by wd (`InotifyWatch`,
  guarded by `s_watcher.lock`); no fixed cap — the kernel limit
  (`fs.inotify.max_user_watches`) applies and ENOSPC logs one warning.
- Watcher thread: poll on inotify fd + stop pipe, drains the non-blocking fd,
  batches paths (`InotifyPending`) until 75 ms quiet / 500 ms max, then files
  go through the shared `file_state_changed` filter and directories are
  emitted with `is_dir`. EINTR is retried; fatal errors set `thread_failed`
  so `ctx_watcher_is_running()` reports false.
- Directories: create/moved-in -> watch subtree first, then emit dir event
  (rescan covers files created before the watch). Moved-out/renamed ->
  `inotify_drop_subtree` (rm_watch), since the kernel would otherwise hand
  back the same wd with the stale path. `IN_IGNORED` drops table entries;
  `IN_Q_OVERFLOW` re-registers missing dirs and rescans every root.
- DT_UNKNOWN / symlinked dirs resolved with stat (matches the indexer);
  same-inode re-registration (symlink/bind-mount cycles) is skipped. Known
  limit: a dir reachable via two paths reports events under the first only.
- Single-file watches re-watch the path after DELETE_SELF/MOVE_SELF
  (atomic save).
- Cross-compiled with real glibc/kernel headers (Debian arm64 sysroot,
  `clang --target=aarch64-linux-gnu -Wall -Wextra`): clean. Not yet run on a
  Linux kernel — run `ctest -R watcher_smoke` there.

## Shared rules

- `ctx_watcher_set_dir_filter(ctx_indexer_skips_dir_name)` (wired in
  `main.c`) makes both backends skip the indexer's excluded dirs (build, bin,
  node_modules, dist, target, VCS, hidden).
- `ctx_indexer_update_file` enforces the same eligibility as `collect_files`
  (`path_in_indexed_tree`, size 1 B..10 MiB) and removes files that stop
  qualifying, so incremental updates can never diverge from a full scan.
- `app.c` strips trailing slashes from the project path so indexer, store and
  watcher paths share one root form.

## Windows (not supported; deliberately left as-is, 2026-09-30)

- ctx does not build on Windows: `main.c` (pthread, sigaction, pause),
  `indexer.c` (`opendir`), `api.c` (BSD sockets, pthread) are POSIX-only;
  only some modules (`jobs.c`) have Windows branches.
- The ReadDirectoryChangesW backend is unchanged apart from `is_dir = false`.
  Known defects to fix if Windows becomes a goal: emits `\`-separated paths
  (indexer stores `/`), wrong wait-list-to-entry mapping with >1 watch,
  cross-thread `CancelIo` in `ctx_watcher_remove` (kernel may write into a
  reused buffer), 0-byte (overflow) completions ignored, RDCW failures
  unchecked, no dir filter / burst dedupe, removed dirs indistinguishable from
  files (needs a known-dir set or ReadDirectoryChangesExW attributes).

## Verification

- `tests/watcher_smoke.sh` (ctest `watcher_smoke`): API-level assertions for
  single edit (1 update), 20-write burst (<=2), chmod/read (0), atomic save,
  excluded dirs, nested new dirs, dir move-in/rename/move-out, file
  move-out/delete/truncate, dir removal. Passes on macOS (4 runs).

### Earlier manual runs (scratch project, no-GUI, symlinked relative root)

- touch-after-create/append/truncate/delete/new file/atomic save/20-write burst
  -> 1 update each; xattr/read/.git writes/non-source files -> 0.
- mkdir+file, mv dir into tree, dir rename, rm -rf dir -> rescans produce the
  correct final index (restart reports "Index up to date").
- 1500 files / 500 dirs: edits in dir 250 and 500 detected.

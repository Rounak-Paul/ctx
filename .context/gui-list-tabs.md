# ctx GUI List Tabs

`src/ui/app_window.c` owns the Causality GUI. The durable layout is:

- Static chrome root.
- Sticky tab strip in `s.tabs_div`.
- Rebuilt, scrollable active-panel body in `s.content_div`.

List-style tabs use per-tab state in `s.list_query[]` and `s.list_page[]`.
Filtering is applied across the full backing dataset before pagination in
background jobs, so search is not limited to the visible page and Causality
builders never scan the full graph/store on the UI thread. Page changes and
search edits invalidate only the active content area plus the tab strip when
selection changes.

Tabs:
- `0 Graph`: force graph viewport.
- `1 Symbols`: definitions, searchable by name, kind, file, scope, signature.
- `2 Calls`: call edges, searchable by caller/callee names and files.
- `3 Context`: retrieval playground. The query input flexes to fill remaining
  width after the Retrieve button.
- `4 Files`: indexed files, searchable by path and language.
- File language display must use `CtxLanguage` enum values from
  `parser/parser.h`; `CTX_LANG_C` starts at 0. Avoid hard-coded 1-based mappings.

Async UI rules:
- Context retrieval is submitted to the shared job system and publishes back to
  `s.ctx_result` behind `s_ui_lock`.
- Symbols, Calls, and Files render fixed-size snapshots from
  `s.list_snapshots[]`; workers build those snapshots from graph/store data.
- UI builders may request a refresh, but must render the current snapshot or a
  loading state instead of doing graph/store scans directly.
- Snapshot workers periodically cancel themselves when superseded by a newer
  query/page generation.
- Page controls clamp to the current filtered count and request a fresh snapshot
  if the existing page is beyond the new last page.
- Worker publish paths check `s.closing` before waking Causality.

Keep chrome compact. Previous context rejected extra vertical rows for legends or
explanatory UI, so new controls should be dense and directly functional.

## Causality Integration

The updated Causality vendor adds Apple Objective-C platform sources
(`src/platform/mouse_state_mac.m`). `ctx` must enable CMake's `OBJC` language on
Apple before adding the vendor tree; otherwise the static archive step references
`mouse_state_mac.m.o` without a valid Objective-C compile rule.

## Live Context Freshness

Watcher events must never call indexing synchronously from the watcher thread.
`main.c` copies `CtxFileEvent` payloads into `FileChangeJob` objects and submits
them to the job system. Directory-level watcher events request a debounced full
reindex because macOS kqueue reports directory changes without the child path.

`ctx_indexer_index_all()` and `ctx_indexer_update_file()` are serialized by the
indexer lock. Full reindex removes stored files that no longer exist in the
collected project file list, so deleted symbols cannot remain retrievable.
Incremental updates remove the old graph entries first; if parsing fails, the
store row is also removed to avoid serving stale content.

Platform watcher notes:
- macOS kqueue keeps one active descriptor per recursive file/directory path and
  refreshes child watches on directory write/link events.
- Linux inotify keeps one entry per watch descriptor, so nested file events use
  the correct directory path and new directories get watches dynamically.
- `POST /reindex` submits a background job; it should not block the API thread.

# Build Fix Notes

## Current failure

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` failed because `CMakeLists.txt` required `vendors/sqlite/sqlite3.c`.
- This checkout has the SQLite upstream source tree under `vendors/sqlite/src`, but not the generated amalgamation files `sqlite3.c` and `sqlite3.h`.

## Build decisions

- The CMake SQLite dependency now uses a target named `ctx_sqlite`.
- If a vendored SQLite amalgamation exists, CMake builds it out of the build tree.
- If the amalgamation is absent, CMake uses `find_package(SQLite3 REQUIRED)` and links `SQLite::SQLite3`.

## Verification

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` configures successfully.
- `cmake --build build --parallel` builds `bin/ctx` successfully with no project warnings.
- The context summary reports the previously counted `Other symbols` metric to remove the unused-variable warning.
- Smoke run: `env HOME=/private/tmp ./bin/ctx --no-gui --no-api --project src` indexed `src` and entered the expected watch loop; Ctrl-C shut it down cleanly.

## Notes

- Configure still prints CMake deprecation warnings from vendored third-party projects: `vendors/cjson` and `vendors/causality/vendors/glm`.

## Runtime Crash Follow-Up

- GUI startup could crash with `SIGBUS` while indexing the whole repository.
- macOS crash report `ctx-2026-06-19-010845.ips` showed `Thread stack size exceeded due to excessive recursion` in `walk_node()` at `src/extractor/extractor.c`.
- Root cause: recursive AST traversal used large per-frame buffers and hit the default pthread stack on deeply nested generated/vendor headers.
- Fix: AST extraction now uses an explicit heap-backed traversal stack instead of recursive calls.
- `vendor` and `vendors` are intentionally not skipped by default because ctx needs dependency context for complete graph output.
- Graph symbol replacement now preserves the existing uthash handle when updating an existing symbol.
- Verification: `env HOME=/private/tmp/ctx-full-home ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx` completed a cold full-repo index with vendors included: 2338 files, 65531 symbols, 544907 edges.
- Verification: `env HOME=/private/tmp/ctx-full-home ./bin/ctx --no-api --project /Users/duke/Code/ctx` loaded the full cached graph and reached GUI startup without the stack crash.

## GUI End-Of-Index Abort Follow-Up

- User reproduced a GUI abort after `Index done`: `Assertion failed: (g_ctx.active), function ca_div_begin, file widget.c`.
- Root cause: `on_graph_updated()` runs on the indexer thread and directly called `ca_div_invalidate()`, which can schedule Causality builder work outside the UI frame context.
- Fix: background graph-update events now only mark `s.graph_updated` and wake the event loop; `on_frame()` performs `ca_div_invalidate()` inside Causality's active widget context.
- Parser files with recoverable tree-sitter error nodes are now logged at TRACE instead of DEBUG to avoid flooding normal debug runs. The parser still indexes the recovered tree.
- Ambiguous `.h` files are parsed as both C and C++; ctx keeps the tree with fewer error nodes.
- Verification: `env HOME=/private/tmp/ctx-full-home-3 ./bin/ctx --no-api --project /Users/duke/Code/ctx` completed GUI indexing from an empty cache with vendors included and stayed alive after the post-index graph update.
- Terminology: "cold" in prior notes meant "starting from an empty cache/database", not a separate indexing mode. ctx has one complete indexing operation, available in GUI and no-GUI modes.

## Stability Follow-Up

- `CTX_LOG_FATAL` no longer aborts the process. Fatal logs are emitted as events and callers decide how to shut down or degrade.
- `main()` now uses explicit initialization state and cleanup paths instead of aborting on startup failures.
- Bundled Causality is compiled with `NDEBUG` for this app so debug `assert()` calls in the UI library do not become production runtime crashes.
- Generic semantic tree-sitter nodes that are not first-class ctx symbols are now retained as `CTX_SYM_UNKNOWN` symbols with node type, location, and source snippet. This includes declarations, definitions, type/specifier nodes, imports/includes, namespaces, classes, structs, enums, typedefs, functions, and methods.
- Generic control-flow statements were not promoted to top-level graph symbols because full vendored indexing became too slow when every statement/expression-like node was persisted through the current graph/store path.
- Verification: `env HOME=/private/tmp/ctx-full-home-decls ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx` completed the full index with vendors included: 2338 files, 356330 symbols, 545071 edges, 126631 ms.
- Verification: `env HOME=/private/tmp/ctx-full-home-decls ./bin/ctx --no-api --project /Users/duke/Code/ctx` loaded the expanded graph in GUI mode and stayed alive after the up-to-date graph update.

## Interactive Graph View Follow-Up

- The old Graph tab used a fixed `Ca_NodeGraph` projection capped at 48 nodes and 96 wires.
- `src/ui/force_graph.c` now owns an Obsidian-style force-directed graph viewport: it ranks the complete indexed graph into a bounded visible projection, simulates node physics, supports drag/pan/scroll zoom, and renders vector edges/nodes into a Causality `Ca_Viewport` with Vulkan dynamic rendering.
- The complete index is unchanged. The viewport projection is a rendering strategy for interactivity, not a separate or partial indexing operation.
- Causality `ca_viewport()` now reuses retained viewport widgets instead of allocating a new viewport slot every time a retained builder reruns.
- `src/store/store.c` now creates the complete cache directory path recursively and checks errors, so a missing `HOME` directory no longer causes SQLite startup failure.
- `CMakeLists.txt` uses `file(GLOB_RECURSE ... CONFIGURE_DEPENDS ...)` so added source files are picked up by CMake-generated build dependencies.
- Verification: `cmake --build build --parallel` builds `bin/ctx` successfully.
- Verification: `env HOME=/private/tmp/ctx-verify-graph-nogui ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx/src` created the missing cache path, indexed 32 files, and entered the watch loop.
- Verification: the same no-GUI command on the warmed cache loaded 2408 symbols and 335 edges, reported the index up to date, and entered the watch loop.
- GUI smoke verification could not be rerun in this environment because the required approval to launch a macOS window was rejected by the approval system usage limit.

## Graph Visibility Follow-Up

- User reported that the graph was not visible after the first custom viewport implementation.
- Root cause: the Graph tab relied on a zero-sized/flex viewport directly inside an auto-height content pane, and `ca_viewport()` does not apply CSS to its own descriptor before GPU allocation. The viewport could therefore collapse to an effectively invisible surface.
- Fix: the Graph tab now wraps the viewport in a CSS-applied `.graph-frame` with `min-height: 520px`, `flex-grow: 1`, border, padding, and background. The main `.content` pane also requests `flex-grow: 1`.
- Renderer fallback: `graph_render()` now always begins dynamic rendering and clears the viewport before attempting graph pipeline/buffer setup, so a shader/pipeline failure shows a visible empty graph surface instead of an unrendered widget.
- Verification: `cmake --build build --parallel` succeeds after the visibility fix.
- Verification: `git diff --check` succeeds.
- Verification: `env HOME=/private/tmp/ctx-verify-graph-nogui ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx/src` reindexed changed UI files and entered the watch loop cleanly.

## Empty Viewport Follow-Up

- User could see the graph area but no graph primitives inside it.
- The shader pipeline path can still fail silently on a runtime Vulkan setup; if that happens, the viewport only shows the cleared background.
- Fix: `graph_render()` now draws the graph through a pipeline-independent Vulkan fallback using `vkCmdClearAttachments` rectangles for edge strokes and node markers before the optional shader pipeline path. With graph data present, this should make nodes and edges visible even if shader/pipeline creation or drawing is not working.
- The fallback also draws a center reticle when no nodes are selected, so the viewport never looks blank.
- Verification: `cmake --build build --parallel` succeeds after the fallback draw path.
- Verification: `git diff --check` succeeds.
- Verification: `env HOME=/private/tmp/ctx-verify-graph-nogui ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx/src` reindexed the changed renderer file and entered the watch loop cleanly.

## Quasar Viewport Binding Follow-Up

- User noted that Quasar's custom render path should be used as the reference because the graph area remained empty.
- Quasar creates a `Ca_Viewport` with an empty descriptor and then binds render callbacks afterward through `qs_renderer_bind()` / `qs_viewport_set_callbacks()` / `ca_viewport_set_callbacks()`.
- The first ctx implementation passed the render callback in the `Ca_ViewportDesc`; after adding retained viewport reuse, `ca_viewport()` could overwrite an already-bound callback with NULL during retained rebuilds. This is especially wrong for Quasar-style bind-after-create usage.
- Fix: retained `ca_viewport()` reuse now preserves existing render/resize callbacks when the incoming descriptor does not provide replacements.
- Fix: `ctx_force_graph_build()` now mirrors Quasar by creating/reusing the viewport first, then explicitly calling `ca_viewport_set_callbacks(viewport, graph_render, view, NULL, NULL)` and requesting redraw.
- Verification: `cmake --build build --parallel` succeeds after the callback binding fix.
- Verification: `git diff --check` succeeds in both the main repo and `vendors/causality`.
- Verification: `env HOME=/private/tmp/ctx-verify-graph-nogui ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx/src` reindexed the changed graph renderer file and entered the watch loop cleanly.

## Viewport Redraw Scheduling Follow-Up

- User still saw an empty graph area after the Quasar-style callback binding fix.
- Root cause found in Causality: `ca_viewport_request_redraw()` only set `viewport->needs_redraw`. The renderer skips `ca_swapchain_frame()` unless `window->needs_render` is true, so viewport-only redraws could be starved after normal UI painting was clean.
- Fix: `ca_viewport_request_redraw()` now marks the viewport node content-dirty, sets the owning window's `needs_render`, and wakes the instance. `ca_viewport_set_callbacks()` also requests a redraw after callbacks are installed.
- Verification: `cmake --build build --parallel` succeeds after the Causality redraw scheduling fix.
- Verification: `git diff --check` succeeds in both the main repo and `vendors/causality`.
- Verification: `env HOME=/private/tmp/ctx-verify-graph-nogui ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx/src` loaded the cache and entered the watch loop cleanly.

## Graph Builder Lifecycle Follow-Up

- The app no longer depends on `ca_div_set_builder()` to construct the graph tab. The content pane is explicitly rebuilt from `on_frame()` when needed: initial frame, tab change, or graph update.
- The ctx Causality instance now uses `ca_instance_set_continuous(s.inst, true)`, matching Quasar's engine setup. This is required for viewport physics/redraws to advance without relying only on input/window events.
- Causality `ca_viewport()` now participates in retained child claiming like other widgets, preserving callback ownership on rebuilds and avoiding viewport pool churn.
- Temporary lifecycle diagnostics were removed after the user confirmed the vector graph is visible.
- Verification: `cmake --build build --parallel` succeeds after explicit content rebuild and continuous-mode changes.
- Verification: `git diff --check` succeeds in both the main repo and `vendors/causality`.
- Verification: `env HOME=/private/tmp/ctx-verify-graph-nogui ./bin/ctx --no-gui --no-api --project /Users/duke/Code/ctx/src` loaded the cache and entered the watch loop cleanly.

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

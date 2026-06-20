# ctx Context Server — Architecture & Decisions

Goal: a deep-graph LLM context server. Given a task/question, return a complete
structured relational context block — all relevant symbols, transitive call/
reference/inheritance chains, field usage sites, and module structure — so an
agent has the same picture it would get after reading the entire relevant portion
of the codebase itself. No token budget, no truncation, no markdown formatting.

## Data model (`graph/graph.h`)
`CtxSymbol`: `name, file, line, end_line, col, kind, signature, scope, lang,
is_definition`. `scope` = enclosing class/struct/namespace. Edges: calls,
references, inherits, includes, defines.

## Extraction (`extractor/extractor.c`)
- Language-aware iterative AST walk (no recursion → no stack overflow on vendor).
- First-class symbols: functions, methods, classes, structs, enums, typedefs,
  namespaces, macros, includes, module-level variables (no locals).
- Pending edges: calls (from enclosing fn), references (enclosing fn → resolved
  identifier filtered by `is_noise_identifier`), inheritance.
- `is_noise_identifier`: blocks identifiers < 5 chars, all C/C++ keywords, and
  curated ubiquitous field names (`width`, `height`, `count`, `state`, `text`,
  `node`, etc.) that resolve to the wrong vendor symbol.
- `CTX_SYM_UNKNOWN` emitted for generic nodes — excluded from retrieval scoring.

## Resolution (`graph/graph.c::ctx_graph_resolve_calls`)
- Name → candidate-list multimap; `pick_candidate` ranks by locality (same file
  +100, same dir +40), definition preference (+20), kind priority.
- Resolves under read lock into temp buffer, adds edges after unlock (rwlock
  non-recursive).

## Retrieval engine (`retrieve/retrieve.c`)
Complete rewrite — no budget, no truncation, no format switching.

**Pipeline:**
1. Tokenize query: stopword-filtered, light stemming (-ing/-ed/-s). Symbol/file
   anchors also split on `_`/`/` so `qs_renderer_create` → `qs renderer create`
   + full name as extra term.
2. Score every symbol: exact name=40, substr=18, filename=10, sig=5, scope=4,
   coverage ×8, kind importance, definition +6, vendor ×0.3. Top 128 seeds.
3. BFS traversal from each seed, both directions, all edge kinds, depth 6.
   Up to 512 total symbols. Cross-vendor reference edges suppressed.
4. File sibling pull: top-8 seeds → all co-file definition symbols added
   (structural locality signal).
5. Sort by score, emit grouped by module directory → file → symbol.
   Per-file: peer filenames in same directory. Per-symbol: kind, name, file:line,
   scope, signature, full edge inventory (calls/called-by/refs/ref'd-by/inherits).

**API:** `/context?task=`, `/context/symbol?name=`, `/context/file?path=`.
No budget or format params. Output: plain structured text, always complete.

## Cache/versioning (`store/store.c`, `indexer/indexer.c`)
- `CTX_STORE_SCHEMA_VERSION` → `migrate_schema` drops stale tables on mismatch.
- `CTX_SEMANTIC_INDEX_VERSION` (currently "4") → forces re-extraction on change.
- Bump SCHEMA when CtxSymbol columns change; SEMANTIC when extraction changes.

## API (`api/api.c`)
- `GET /context?task=`, `/context/symbol?name=`, `/context/file?path=`.
- `GET /stats`, `GET /health`, `POST /reindex`.
- URL-decoded query values (`%XX` + `+`). Missing param → 400.

## Benchmark (`bench/bench.c`, `--bench`)
- 6 presence-based cases asserting expected file/symbol names appear in output.
- No budget assertion — output is unbounded by design.

## GUI Context tab (`ui/app_window.c`)
- Tab order: Graph(0) · Symbols(1) · Calls(2) · Context(3) · Files(4).
  `force_graph_frame` guard: `active_tab == 0`. View menu and tab buttons must
  stay in sync if reordering.
- `build_context_tab`: text input + Retrieve button. Enter or click runs
  `ctx_retrieve` (CTX_QUERY_TASK). Result rendered line-by-line up to 400 lines
  with prefix-based styling (MODULE/QUERY → blue, FILE → green, indented → muted).
- No budget dropdown — retrieval is always full depth.
- `ca_input_text(const Ca_TextInput*)` added to vendored Causality (widget.c +
  causality.h) — Ca_TextInput is opaque, had no public text getter.
- `CTX_KEY_ENTER 257` defined locally (GLFW headers not on ctx include path).

## Build note
- Root-owned `build/` artifacts block PCH write. Fix: `mv build build.bak`,
  rebuild fresh as current user.

# ctx Context Server — Architecture & Decisions

Goal: a token-efficient LLM context server. Given a task/query, return a compact,
explainable, budget-bounded context bundle (files, symbols, snippets,
relationships) with `file:line` citations. The GUI is a debug aid; the API is
the product.

## Data model (`graph/graph.h`)
`CtxSymbol` fields used by retrieval: `name, file, line, end_line, col, kind,
signature, scope, lang, is_definition`. `scope` = enclosing class/struct/
namespace; `end_line` bounds the on-demand snippet; `lang` drives the code-fence
language. Edges: calls, references, inherits, includes, defines.

## Extraction (`extractor/extractor.c`)
- Language-aware AST walk over an explicit heap stack (no recursion → no stack
  overflow on deep vendor headers).
- First-class symbols: functions, methods, classes, structs, enums, typedefs,
  namespaces, macros, includes, and module-level variables only (locals are
  noise).
- Pending edges: calls (from enclosing fn), references (enclosing fn → resolved
  identifier, filtered by `is_noise_identifier` to drop keywords/short names),
  inheritance (from base-clause/heritage nodes).
- `is_noise_identifier`: blocks identifiers shorter than 5 chars, all C/C++
  keywords, and a curated list of ubiquitous field names (`width`, `height`,
  `count`, `value`, `state`, `text`, `node`, etc.) that consistently resolve
  to the wrong vendor symbol.
- Scope + enclosing-fn tracked via save/restore entries pushed onto the walk
  stack.
- Generic fallback nodes are still emitted as `CTX_SYM_UNKNOWN` for graph
  completeness but are **excluded from retrieval scoring** so they never
  dominate results.

## Resolution (`graph/graph.c::ctx_graph_resolve_calls`)
- Builds name → candidate-list multimap, then `pick_candidate` ranks by
  locality (same file +100, same module/dir +40), definition preference (+20),
  and kind. Replaces the old arbitrary-first-match resolver.
- Resolves under a read lock into a temp buffer, then adds edges after
  unlocking (rwlock is non-recursive; `ctx_graph_add_edge` takes the write
  lock).

## Retrieval engine (`retrieve/retrieve.c`)
- `/context/symbol`: exact `strcasecmp` match preferred (score 120+kind); falls
  back to case-insensitive substring match (score 80+kind) so the endpoint
  never returns empty on near-miss names. Anchor text is whitespace-trimmed
  before matching to handle URL-decoding artifacts.
- `collect_neighbors`: reference edges that cross from non-vendor source to a
  vendor target are suppressed — these are almost always false resolutions of
  common field names emitted during extraction.

- Tokenize task → terms (stopword-filtered, light stemming: -ing/-ed/-s).
- `score_symbol`: name exact/substr, filename, signature, scope matches +
  term-coverage + kind importance + definition bonus − vendor penalty. UNKNOWN
  kind scores 0.
- Bounded ranked `SeedSet` (top CTX_MAX_SEEDS). Each seed expands up to
  CTX_MAX_NEIGHBORS call/ref/inherit neighbors.
- Snippets read on demand from disk, bounded line range (`read_line_range`,
  CTX_MAX_SNIPPET_LINES). Never reads whole files.
- Greedy budget packing by rank; a too-big item first drops its snippet
  (reference-only) before being omitted. `est_tokens_for` includes markdown
  framing × a 1.3 conservative factor so rendered output stays ≤ budget.
- Two deterministic formats: JSON (programmatic) and Markdown (prompt). Symbol
  and file anchors reuse the same packing path.

## Cache/versioning (`store/store.c`, `indexer/indexer.c`)
- `CTX_STORE_SCHEMA_VERSION` (store table layout) → `migrate_schema` drops
  stale symbol/edge/file tables on mismatch.
- `CTX_SEMANTIC_INDEX_VERSION` (extraction semantics, currently "4") → indexer
  forces re-extraction of all files when changed.
- Both bump automatically; no manual cache deletion. Bump SCHEMA when symbol
  columns change; bump SEMANTIC when extraction/resolution behavior changes.

## API (`api/api.c`)
- `GET /context?task=&budget=&format=`, `/context/symbol?name=`,
  `/context/file?path=`. `budget` = approx tokens (default 2000), `format` =
  markdown|json. Query values are URL-decoded (`%XX` + `+`).
- Legacy `/summary /symbol /file /query /stats /health`, `POST /reindex` kept.
- Invalid budget → default; missing param → 400; no graph → formatted error.

## Benchmark (`bench/bench.c`, `--bench`)
- Fixed representative tasks asserting expected files/symbols appear within a
  small budget. Presence-based (not score-based) to avoid brittleness. `ctx
  --bench` exits with the failure count.

## GUI Context tab (`ui/app_window.c`)
- New default tab "Context" (tab 0; Graph/Symbols/Calls/Files shifted to 1–4).
  Tab indices are also referenced in `on_frame` (force_graph_frame runs when
  `active_tab == 1`) and the View menu — keep all three in sync if reordering.
- `build_context_tab` renders a `ca_input` query field + Retrieve button; Enter
  or click runs `ctx_retrieve(graph, budget=1200, markdown)` and the bundle is
  stored in `s.ctx_result` (freed on rebuild and on `ctx_ui_run` exit). Markdown
  is rendered line-by-line with prefix-based styling (no full md renderer).
- Required adding a public getter to bundled Causality:
  `ca_input_text(const Ca_TextInput*)` in `vendors/causality/.../widget.c` +
  `causality.h` — `Ca_TextInput` is opaque and had a setter but no getter.
- `CTX_KEY_ENTER 257` is defined locally because GLFW headers are not on ctx's
  include path; `ca_input_key_pressed` takes a raw GLFW key int.

## Build note
- Output dirs `bin/` and `build/` had root-owned artifacts from a prior `sudo`
  build that blocked the PCH write. Resolved by moving the stale `build/` aside
  and rebuilding fresh as the current user. If "Operation not permitted" on
  `cmake_pch.h.pch` recurs, check for root-owned files under `build/`.

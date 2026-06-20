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
2. Build per-query indexes (all O(n) single-pass, under read lock):
   - **Adjacency list** (`AdjList`): keyed by symbol id, makes BFS O(degree) per
     hop instead of O(all_edges). Built from `g->edges` in one HASH_ITER pass.
   - **Inverted token index** (`InvIndex`): maps lowercase tokens → matching symbols
     with pre-scored contributions. Tokens come from: name (camelCase + `_` split),
     full lowercased name, filename stem, scope, and leading return-type word from
     signature. Makes scoring O(matching_symbols × terms) not O(all_symbols).
   - **File index** (`FileIndex`): hash-bucketed per-file symbol lists for O(1)
     sibling/peer lookups.
3. Score via inverted index: top 128 seeds selected. Hub bonus: degree × 0.15,
   capped at 12.0 — ensures high-connectivity architectural symbols rank higher.
   Coverage bonus: symbols matching more query terms get +8 per term hit.
4. Prefix matching: terms ≥4 chars also match index tokens that start with the
   term (e.g. "rend" → "render") at 0.5× score — fuzzy without false positives.
5. BFS traversal via adjacency list from each seed, depth 4, up to 128 symbols.
   Incoming reference edges suppressed (high-volume noise). Cross-vendor refs
   suppressed. Score decays 10 points per hop.
6. File sibling pull + include-chain pull for top-8 seeds (structural locality).
7. Sort by score, emit grouped by module directory → file → symbol.

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

## Retrieval engine (`retrieve/retrieve.c`) — output format
Output is optimised for LLM consumption — compact, structured, hierarchical:

```
CODEBASE: /abs/path/to/root
QUERY: how does culling work
TERMS: cull frustum render
SYMBOLS: 47 across 3 modules

MODULES:
  src/render/  18 syms  C  [also: shadow.c, material.c]
  src/math/     8 syms  C  [also: vec.c]

MODULE src/render/
  FILE src/render/culling.c  [8 syms, C]
    fn       ctx_cull_frustum(RenderList*, Frustum*)  :18-67
      calls: frustum_test_sphere(frustum.c:44), ...
      called-by: qs_renderer_submit_renderable(renderer.c:203)
    struct   CullResult  :12
```

Key format rules:
- `CODEBASE:` from `ctx_store_get_meta("root_path")`.
- `MODULES:` compact table — module path, sym count, lang, peer files not in ss.
- `MODULE path/` at column 0; `  FILE fullpath  [N syms, lang]` at indent 2.
- Symbol: `    kind  name(sig)  :line[-endline]` at indent 4.
- Edges: `      label: name:line, cross(file.c:line)` at indent 6.
  Same-file edges show `name:line`; cross-file show `name(basename:line)`.
- No `(reason)` noise. No redundant file paths inside symbol lines.
- Modules ordered by relevance (first appearance in score-sorted list).

## Retrieval engine — scoring
- Inverted index pre-scores: exact name match=40, prefix name=22, filename=10, else=8.
  Plus: `kind_importance()`, definition+6, hub bonus (degree×0.15, cap 12.0), vendor×0.3.
- Coverage bonus: +8 per query term that the symbol matched across all index hits.
- `score_symbol()` still used for FILE/SYMBOL anchor queries (rare, fast enough).
- Tunables: seeds=128, traversal=128, depth=4, min_score=6.0, siblings=8, peers=8.

## Store (`store/store.h`, `store/store.c`)
- Added `CtxFileRecord` struct: path, lang, mtime, size, error_count, sym_count.
- Added `ctx_store_enumerate_files(out, max, g)` — queries `files` table ordered
  by path, fills sym_count from live graph.

## GUI (`ui/app_window.c`)
- Tab order: Graph(0) · Symbols(1) · Calls(2) · Context(3) · Files(4).
- **Files tab**: full per-file table grouped by directory. Columns: file, lang,
  syms, size, errors. Data from `ctx_store_enumerate_files()`. Max 200 rows.
- **Status bar right**: now shows error count in amber when `stats.errors > 0`.
- `ctx-line-h` prefix for DIRECTORY_TREE: added to `ctx_line_style()` already
  via MODULE/QUERY/SYMBOLS prefix match.
- `ca_input_text(const Ca_TextInput*)` in vendored Causality.
- `CTX_KEY_ENTER 257` defined locally.

## Force graph (`ui/force_graph.c`)
- **Dual encoding**: nodes colored by kind (`color_for_kind` restored); module
  membership shown via translucent region overlays drawn first (behind edges+nodes).
- `ModuleRegion`: AABB per unique module palette color, with 22px padding.
  Fill: module color at α=0x28 (≈15%). Border: α=0x60 (≈38%), 1.5px.
  Name label: directory basename at α=0x58, drawn inside top-left of region.
  Regions < 30px wide/tall are skipped (degenerate overlapping positions).
- `color_for_module(file)`: FNV-1a of directory → 10-color palette (index only
  used for region color — not node color).
- `dir_basename(file, out, sz)`: extracts second-to-last path segment.
- Node labels back to plain `n->name`.
- Graph toolbar legend: edge legend + node-kind dot legend + "| region=module" label.
- Vertex budget: `+ CTX_FG_MAX_MODULES * 256u` added to max_vertices.

## Build note
- Root-owned `build/` artifacts block PCH write. Fix: `mv build build.bak`,
  rebuild fresh as current user.

# ctx — LLM Context Server

`ctx` indexes a codebase with tree-sitter, builds a semantic symbol graph
(calls, references, inheritance, includes), and serves **compact, explainable,
budget-aware context bundles** over HTTP. It exists to keep coding agents from
burning tokens scanning large repositories: ask for a task and get back the
relevant files, symbols, snippets, and relationships — within a token budget,
with stable `file:line` citations.

The graph visualizer (GUI) is a debugging aid. The product is the context API.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# Index a project and serve the API (no GUI):
./bin/ctx --no-gui --project /path/to/repo            # API on :8765
./bin/ctx --no-gui --no-api --project /path/to/repo   # index only, then watch
./bin/ctx --bench  --project /path/to/repo            # run retrieval benchmark
```

Flags: `--project <dir>`, `--api-port <n>` (default 8765), `--no-gui`,
`--no-api`, `--bench`. The index is cached in `~/.ctx/<hash>/index.db`; repeated
startups load from cache and only re-extract changed files. Vendored code is
indexed by default.

## Context API

All retrieval endpoints accept `budget` (approx token budget, default 2000) and
`format` (`markdown` | `json`, default `markdown`). Markdown is for direct
prompt inclusion; JSON is for programmatic agents. Output is deterministic and
never dumps whole files — the highest-value slices are packed first and
remaining matches are listed under "omitted".

| Method | Endpoint | Description |
|---|---|---|
| GET | `/context?task=<text>&budget=<n>&format=<fmt>` | Rank context for a natural-language task |
| GET | `/context/symbol?name=<sym>&budget=<n>&format=<fmt>` | Context anchored on a symbol |
| GET | `/context/file?path=<path>&budget=<n>&format=<fmt>` | Context anchored on a file |
| GET | `/stats` | Index counters (files, symbols, edges) |
| GET | `/health` | Liveness probe |
| POST | `/reindex` | Trigger a re-index |

### Example

```sh
curl 'http://127.0.0.1:8765/context?task=where+is+the+API+status+endpoint&budget=1500&format=json'
```

Each selected item carries: `name`, `kind`, `file`, `line`/`end_line`, `scope`,
`signature`, `relevance`, `reason`, a `citation` (`file:line`), an optional
source `snippet` with `snippet_lines`, and `relationships` (callers/callees/
references/inheritance with their own citations).

## Retrieval model

Symbols are scored by a blend of: text match over name/signature/file/scope,
query-term coverage, symbol-kind importance, definition preference, and a vendor
penalty. The top-ranked seeds are expanded with their call/reference/inheritance
neighborhoods, snippets are read on demand from disk (bounded line ranges), and
the result is greedily packed into the token budget — snippets are dropped to a
reference-only entry before a symbol is omitted entirely.

## Architecture

- `parser/` — tree-sitter parsing (C, C++, Python, JS, TS); ambiguous `.h`
  parsed as both C and C++, fewer-error tree kept.
- `extractor/` — language-aware AST walk emitting first-class symbols (functions,
  methods, classes, structs, enums, typedefs, namespaces, macros, includes,
  module-level variables) plus pending call/reference/inheritance edges, with
  enclosing scope tracking. Iterative heap stack — no deep recursion.
- `graph/` — in-memory symbol/edge store (uthash) with rwlock; ranked
  post-index name resolution preferring same-file/module, definitions, and
  symbol kind.
- `store/` — SQLite cache with schema/semantic versioning; stale caches rebuild
  automatically on version change (no manual delete).
- `retrieve/` — the retrieval + budget-packing + JSON/Markdown formatting engine.
- `api/` — minimal HTTP server exposing the endpoints above.
- `indexer/`, `watcher/`, `jobs/`, `event/` — indexing pipeline and live updates.
- `bench/` — built-in retrieval benchmark (`--bench`).
- `ui/` — optional Causality GUI: a **Context** tab that runs the retrieval
  engine on a typed task (type a query, press Enter, see the ranked bundle), plus
  the force-directed dependency graph and Symbols/Calls/Files inspectors.

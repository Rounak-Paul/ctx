# ctx — LLM Context Server

`ctx` indexes a codebase with tree-sitter, builds a semantic symbol graph
(calls, references, inheritance, includes), and serves **compact, explainable
context packets** over HTTP. It exists to keep coding agents from burning tokens
scanning large repositories: ask for a task and get back the relevant files,
symbols, relationships, accounting, and expansion handles with stable
`file:line` citations.

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

Retrieval endpoints accept `detail=compact|standard|full`. Compact/adaptive is
the default. Output is deterministic and avoids whole-file/source dumps unless
the caller explicitly expands a handle or asks for `detail=full`.

| Method | Endpoint | Description |
|---|---|---|
| GET | `/context?task=<text>&detail=<mode>` | Compact context packet for a natural-language task |
| GET | `/context/symbol?name=<sym>&detail=<mode>` | Context packet anchored on a symbol |
| GET | `/context/file?path=<path>&detail=<mode>` | Context packet anchored on a file |
| GET | `/context/expand?handle=<handle>` | Expand a handle returned by a context packet |
| GET | `/stats` | Index counters (files, symbols, edges) |
| GET | `/health` | Liveness probe |
| POST | `/reindex` | Trigger a re-index |

### Example

```sh
curl 'http://127.0.0.1:8765/context?task=where+is+the+API+status+endpoint'
```

The default response is a `CTX_PACKET`: answer map, likely edit targets,
relevant files, symbol cards, omitted expandable handles, and accounting. It is
self-contained for the current request but avoids full source bodies unless the
caller explicitly expands a handle such as `expand:source:<id>`.
`expand:entrypoints:<path>` returns exported/top-level entrypoints for a file.
`expand:file:<path>` returns a compact file symbol map with omitted symbols left
as handles; it is not a whole-file source dump.

Use `detail=full` on the context endpoints for the older complete grouped
module/file/symbol output.

## Retrieval model

Symbols are scored by a blend of: text match over name/signature/file/scope,
query-term coverage, symbol-kind importance, definition preference, graph hub
score, and a vendor penalty. The top-ranked seeds are expanded with their
call/reference/inheritance neighborhoods. Rendering is compact/adaptive by
default: repeated or low-marginal-value items become expansion handles instead
of source dumps. There is no hard context-length cap in the default policy.

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
- `retrieve/` — graph retrieval, compact context-packet rendering, and expansion handles.
- `api/` — minimal HTTP server exposing the endpoints above.
- `indexer/`, `watcher/`, `jobs/`, `event/` — indexing pipeline and live updates.
- `bench/` — built-in retrieval benchmark (`--bench`).
- `ui/` — optional Causality GUI: a **Context** tab that runs the retrieval
  engine on a typed task (type a query, press Enter, see the ranked bundle), plus
  the force-directed dependency graph and Symbols/Calls/Files inspectors.

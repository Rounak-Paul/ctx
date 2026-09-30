# ctx — code navigation server for coding agents

`ctx` indexes a codebase with tree-sitter into a symbol graph (calls,
references, inheritance) and gives coding agents **exact, current answers**
through a small set of tools: ranked search, file outlines, exact symbol
source, callers/callees, and change-impact reports. Every answer is re-checked
against disk before it is returned, so agents can act on it without
re-reading files with grep or whole-file reads — that substitution is where
token savings come from.

Search combines BM25 over symbol names, paths, signatures, and doc comments
with local code embeddings, and re-ranks with a local cross-encoder
(llama.cpp; models are downloaded once, pinned and sha256-verified).

The graph visualizer (GUI) is a debugging aid. The product is the tool API.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# Index a project and serve the API (no GUI):
./bin/ctx --no-gui --project /path/to/repo            # API on :8765
./bin/ctx --no-gui --no-api --project /path/to/repo   # index only, then watch
./bin/ctx --mcp --project /path/to/repo                # MCP stdio server for agents
./bin/ctx --install --project /path/to/repo            # write agent MCP configs
./bin/ctx --bench  --project /path/to/repo            # run search benchmark (lexical floor)
```

Add `--no-models` (or set `CTX_MODELS=0`) to run without the local reranker
and embedder; search then uses BM25 only. Build with `-DCTX_WITH_MODELS=OFF`
to drop the llama.cpp dependency entirely.

Flags: `--project <dir>`, `--api-port <n>` (default 8765), `--no-gui`,
`--no-api`, `--no-models`, `--mcp`, `--install`,
`--clients <all|codex,claude,opencode>`, `--bench`. The index is cached in `~/.ctx/<hash>/index.db`; repeated startups
load from cache and only re-extract changed files. Vendored code is indexed by
default.

## Agent install

Run this once per project after building ctx:

```sh
./bin/ctx --install --project /path/to/repo
```

The installer is project-scoped and idempotent. It writes:

| Client | File |
|---|---|
| Claude Code | `.mcp.json`, `.claude/CLAUDE.md`, `.claude/settings.json`, `.claude/skills/ctx/SKILL.md`, `.claude/rules/ctx.md` |
| Codex | `.codex/config.toml`, `.codex/skills/ctx/SKILL.md`, `AGENTS.md` |
| OpenCode | `opencode.json` |
| Shared instructions | `.ctx/ctx-agent-instructions.md` |

Use `--clients codex`, `--clients claude`, `--clients opencode`, or a comma list
to install only selected clients. Repeat the command after moving or rebuilding
the ctx binary so configs point at the current executable. The generated files
launch ctx as a local stdio MCP server:

```sh
ctx --mcp --project /path/to/repo
```

The generated instructions tell agents to use the ctx tools instead of
grep/glob exploration and whole-file reads, and to treat ctx output as current.

## Tools

The same tools are served over MCP and HTTP (one registry, identical
arguments). Paths may be absolute, root-relative, or a unique suffix; symbols
may be `name`, `Scope::name`, `Scope.name`, or `path:line`.

| Tool | Arguments | Returns |
|---|---|---|
| `search` | `query`, `k`=5, `bodies`=1, `include_vendor`=false | Ranked symbols (`path:line-range`, signature, doc summary), code for the top `bodies` |
| `outline` | `path`, `from_line`, `limit`=300 | A file's symbols: line range, kind, signature (nested) |
| `source` | `symbol` \| `file`+`lines`, `max_lines`=120 | Exact code with line numbers and leading doc comment; other matches listed |
| `callers` | `symbol`, `file`, `depth`=1..3 | Call sites with the calling line's text; indirect callers for depth > 1 |
| `callees` | `symbol`, `file` | Called functions with resolved locations; unresolved/external names |
| `impact` | `symbol`, `file` | Definitions/declarations, call sites, indirect callers, references, subtypes, affected files and tests |
| `status` | — | Index readiness/counts, model states, embedding coverage |

Freshness: before answering, every tool stats the files it answers from and
synchronously re-indexes any that changed (mtime ns + size), independent of
the file watcher. Tools wait for the initial index to finish.

## HTTP API

| Method | Endpoint | Description |
|---|---|---|
| GET | `/tools` | Tool list with JSON input schemas |
| GET | `/tool/<name>?arg=value&…` | Run a tool (text response; HTTP 400 on tool errors) |
| GET | `/context?task=…`, `/context/symbol`, `/context/file`, `/context/expand` | Legacy context packets (used by the GUI Context tab) |
| GET | `/stats`, `/health` | Index counters / liveness |
| POST | `/reindex` | Trigger a re-index |

```sh
curl -G http://127.0.0.1:8765/tool/search --data-urlencode 'query=where are symbols persisted'
curl 'http://127.0.0.1:8765/tool/callers?symbol=ctx_graph_replace_file&depth=2'
```

## MCP

```sh
./bin/ctx --mcp --project /path/to/repo
```

stdio JSON-RPC; replies use the client's framing (newline-delimited JSON per
the MCP spec, or `Content-Length` headers). `initialize` and `tools/list`
answer immediately; tool calls wait for the initial index.

## Search model

1. **Candidates**: BM25 over each symbol's name parts (weighted), scope, file
   stem and directory, signature identifiers, and leading doc comment, with
   light stemming and prefix matching (`defrag` ↔ `defragment`); scores are
   scaled by query-term coverage. In parallel, cosine similarity against
   project-symbol embeddings (jina-embeddings-v2-base-code).
2. **Fusion**: reciprocal-rank fusion of both lists; prototypes merge into
   their definitions.
3. **Rerank**: the top 30 are scored by a cross-encoder
   (jina-reranker-v1-turbo-en) over path + signature + doc comment + body
   head, blended with the fused rank.

Embeddings are computed in the background (event-driven on graph updates and
model readiness) and cached in SQLite by content hash, so unchanged symbols
are never re-embedded. Models live in `~/.ctx/models` (`CTX_MODEL_DIR`);
`CTX_RERANK_MODEL` / `CTX_EMBED_MODEL` point at local GGUF files instead.

## Architecture

- `parser/` — tree-sitter parsing (C, C++, Python, JS, TS, Go, Rust).
- `extractor/` — cursor-based AST walk (no recursion) producing a per-file
  extraction: symbols plus reference sites (calls, references, inheritance)
  with their enclosing symbol and line.
- `graph/` — in-memory symbol graph (uthash, rwlock). Files are replaced
  atomically; reference sites persist and edges are derived, ref-counted, and
  re-resolved when a changed file adds or removes candidate targets. Resolution
  is kind-aware (calls bind to callables) and respects C translation-unit scope.
- `store/` — SQLite cache (files, symbols, sites, embeddings) written per file;
  schema/semantic versions rebuild stale caches automatically.
- `indexer/` — full and incremental indexing, `ensure_fresh` for query-time
  freshness.
- `nav/` — outline, source, callers, callees, impact.
- `search/` — BM25 + embedding candidates, fusion, rerank; background
  embedding worker.
- `model/` — llama.cpp reranker/embedder, pinned model fetch with sha256.
- `tools/` — tool registry shared by `mcp/` and `api/`.
- `retrieve/` — legacy context packets (GUI Context tab, `/context`).
- `watcher/`, `jobs/`, `event/` — live updates and background work.
- `bench/` — built-in search benchmark (`--bench`).
- `ui/` — optional Causality GUI.

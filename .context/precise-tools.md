# Precise Tools Pivot (2026-09-30) — implemented

Why: A/B benchmark (`token-benchmark.md`) showed task→packet retrieval saved no
tokens — agents distrusted fuzzy/stale packets and re-grepped. ctx now returns
exact, fresh, small results that replace grep+read round trips.

## Tools (`tools/tools.c` registry → MCP `mcp/` and HTTP `/tool/<name>`)
search, outline, source, callers, callees, impact, status. Descriptions in the
registry double as agent guidance (sent every turn — keep short). Tools wait
for the initial index (`wait_until_ready`, 120s) and report `isError`.
MCP replies mirror client framing (newline JSON default, Content-Length if sent).
Legacy `get_context`/`expand_context` removed from MCP; `/context*` HTTP and
`retrieve/` remain for the GUI Context tab.

## Graph (graph/graph.c)
- `CtxSymbol` strings (name/signature/scope) packed in one allocation; `file`
  interned in `CtxGraphFile`. Drafts use `CtxSymbolDraft` (fixed buffers).
- `CtxGraphFile`: symbols (sorted by line) + persistent `CtxRefSite`s + `version`.
- Edges derived from sites, ref-counted (`CtxEdgeEntry.refs`).
- `ctx_graph_replace_file(g, path, ex|NULL, resolve)`: one write lock; with
  resolve, re-resolves other files' sites whose target name was defined in the
  old or new content (fixes lost incoming callers after edits).
- `pick_target`: kind-aware (`kind_accepts`: calls→fn/method/macro/class) and
  C translation-unit scope (static fns/macros in .c only bind same-file).
- Site source = same-named symbol in file whose range contains the line
  (deterministic tie-breaks `symbol_tighter`/`symbol_preferred`).

## Extractor
- `ctx_extract_file(path, CtxFileExtract*)`; cursor walk, parent/grandparent
  types tracked in the walk (no `ts_node_parent`). Generic `*@L:C` nodes and
  body-less `struct X` references are no longer emitted (was 84% of symbols).

## Store (schema v3, semantic v7)
files(mtime_ns,size), symbols, sites, embeddings(key=fnv(model|text)).
`ctx_store_commit_files` = per-file DELETE+INSERT in one txn (fixed ghost
symbols); load streams ORDER BY file per file then `resolve_all`.

## Indexer
mtime ns + size staleness; `ctx_indexer_update_file` returns changed;
`ctx_indexer_ensure_fresh(paths)` used by nav/search before answering.

## Search (search/search.c)
Per-file incremental doc index synced by `CtxGraphFile.version`. BM25 over name
parts (x2), full name, scope, file stem/dir, signature, doc comment (project
files); stemming (s/ing/ed/e), prefix expansion (0.5), coverage² scaling,
exact-name bonus. Embedding cosine (project docs) → RRF → rerank top 30
(2:1 blend with fused rank). Prototypes merge into definitions.
Embedding worker: event-driven (GRAPH_UPDATED, MODEL_READY), batches of 32,
cache in memory + SQLite, prune after pass. Embed text 1200B, rerank 2000B.

## Model (model/model.c, fetch.c)
llama.cpp b11259 vendored (`CTX_WITH_MODELS`, Metal). Batch 1024 tokens,
8 seqs, 512 tokens/seq (encoder memory ∝ ubatch²). Fetch via `curl` spawn,
commit-pinned URL, size+sha256 check, atomic rename. `--no-models`,
`CTX_MODELS=0`, `CTX_MODEL_DIR`, `CTX_RERANK_MODEL`, `CTX_EMBED_MODEL`.

## Measured (M-series Mac)
- ctx repo w/ vendors (4.3k files): 117k symbols, cold ~100s, warm 5s.
- llama.cpp (1.8k files, 628k LOC): 48.7k symbols, cold index 21s,
  131MB RSS without models, ~710MB with models; embed ~30–95 syms/s (once).

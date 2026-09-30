#pragma once
#include "../pch.h"
#include "../graph/graph.h"

#define CTX_EVENT_GRAPH_UPDATED (CTX_EVENT_USER_BASE + 10u)

typedef struct {
    uint32_t files;
    uint32_t symbols;
    uint32_t edges;
    uint32_t errors;
    int64_t  duration_ms;
} CtxGraphStats;

typedef struct {
    uint32_t total;     /* total files to process */
    uint32_t done;      /* files processed so far */
    bool     running;   /* true while indexing is in progress */
} CtxIndexProgress;

/* Snapshot of index readiness and graph freshness.
 * progress: current index progress counters.
 * cache_loaded: true after persisted graph data has been loaded.
 * ready: true after the initial index pass completes and no mutation is active.
 * graph_generation: increments whenever the graph update event is emitted.
 * last_update_unix_ms: Unix timestamp in milliseconds for the last graph update. */
typedef struct {
    CtxIndexProgress progress;
    bool             cache_loaded;
    bool             ready;
    uint64_t         graph_generation;
    int64_t          last_update_unix_ms;
} CtxIndexStatus;

bool      ctx_indexer_init(const char *root_path);
void      ctx_indexer_shutdown(void);
void      ctx_indexer_index_all(void);   /* blocks until complete */
/* Re-indexes one file when its on-disk state (mtime ns, size) differs from the
   store, or removes it when it no longer exists or no longer qualifies
   (skipped directory, hidden, empty, > 10 MiB). Directory paths trigger a
   full rescan. Returns true when the graph changed. */
bool      ctx_indexer_update_file(const char *path);
/* Synchronously re-indexes every stale path in paths[0..count). Used by query
   tools before answering so results never reflect outdated files.
   Returns the number of files updated. */
uint32_t  ctx_indexer_ensure_fresh(const char *const *paths, uint32_t count);
/* Absolute project root the indexer was initialised with. */
const char *ctx_indexer_root(void);
/* Returns true when directories named `name` are excluded from indexing
   (hidden, build output, dependency and VCS directories). */
bool      ctx_indexer_skips_dir_name(const char *name);
CtxGraph *ctx_indexer_get_graph(void);
void      ctx_indexer_get_stats(CtxGraphStats *out);
void      ctx_indexer_get_progress(CtxIndexProgress *out); /* Writes a thread-safe progress snapshot to out. */
void      ctx_indexer_get_status(CtxIndexStatus *out);     /* Writes a thread-safe readiness snapshot to out. */

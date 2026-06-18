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

bool      ctx_indexer_init(const char *root_path);
void      ctx_indexer_shutdown(void);
void      ctx_indexer_index_all(void);   /* blocks until complete */
void      ctx_indexer_update_file(const char *path);
CtxGraph *ctx_indexer_get_graph(void);
void      ctx_indexer_get_stats(CtxGraphStats *out);
void      ctx_indexer_get_progress(CtxIndexProgress *out); /* thread-safe snapshot */

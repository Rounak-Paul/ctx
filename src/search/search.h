#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Ranked symbol search for natural-language or identifier queries.
 *
 * Candidates come from BM25 over symbol names, scopes, paths, and signatures,
 * plus embedding similarity when the embedder is ready; they are fused and,
 * when the reranker is ready, re-scored by the cross-encoder against the
 * symbol's code. Results are refreshed against disk before rendering.
 */

/*
 * Search request.
 *
 * query           Question or identifiers.
 * k               Results to return (0 → 5, capped at 20).
 * bodies          Results that include a code excerpt (0 = none).
 * include_vendor  Also search third-party code (vendor/, node_modules/, …).
 */
typedef struct {
    const char *query;
    uint32_t    k;
    uint32_t    bodies;
    bool        include_vendor;
} CtxSearchRequest;

/* Runs a search; returns a heap string (never NULL) the caller frees. */
char *ctx_search(CtxGraph *g, const CtxSearchRequest *req);

/*
 * Starts the background embedding worker, which keeps project-symbol vectors
 * current as the graph changes and models become ready (event-driven).
 */
void ctx_search_start(CtxGraph *g);

/* Stops the embedding worker and releases the search index. */
void ctx_search_stop(void);

/*
 * Embedding coverage of project symbols.
 *
 * embedded  Receives symbols with a vector.
 * total     Receives searchable project symbols.
 */
void ctx_search_embedding_progress(uint32_t *embedded, uint32_t *total);

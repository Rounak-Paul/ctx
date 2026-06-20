#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Context retrieval engine.
 *
 * Given a natural-language query or an explicit symbol/file anchor, performs
 * deep graph traversal across the indexed codebase and returns a structured
 * relational context block. Output captures: matched symbols with full
 * signatures, transitive call/reference/inheritance chains, field usage sites
 * (write vs read), co-located symbols in the same file and directory module,
 * and structural hierarchy. No token budget, no truncation, no markdown
 * formatting — the goal is to give an agent the same picture it would have
 * after reading the entire relevant portion of the codebase itself.
 */

typedef enum {
    CTX_QUERY_TASK = 0,  /* natural-language task/question */
    CTX_QUERY_SYMBOL,    /* anchored on a specific symbol name */
    CTX_QUERY_FILE       /* anchored on a file path */
} CtxQueryKind;

typedef struct {
    CtxQueryKind kind;
    const char  *text;   /* task text, symbol name, or file path — must be non-NULL */
} CtxRetrieveRequest;

/*
 * Run retrieval and return a heap-allocated, NUL-terminated structured context
 * block. Never returns NULL — errors produce a valid error message string.
 * Caller must free().
 *
 * g    Live graph (may be NULL or empty — handled gracefully).
 * req  Retrieval request.
 */
char *ctx_retrieve(CtxGraph *g, const CtxRetrieveRequest *req);

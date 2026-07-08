#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Context retrieval engine.
 *
 * Given a natural-language query or an explicit symbol/file anchor, performs
 * broad graph retrieval and returns a structured context packet. The default
 * renderer favours symbol cards, relation summaries, and expansion handles over
 * full source dumps so agents get enough current context without paying to
 * reread the repository. Callers can request full detail explicitly.
 */

typedef enum {
    CTX_QUERY_TASK = 0,  /* natural-language task/question */
    CTX_QUERY_SYMBOL,    /* anchored on a specific symbol name */
    CTX_QUERY_FILE       /* anchored on a file path */
} CtxQueryKind;

typedef enum {
    CTX_RETRIEVE_DETAIL_COMPACT = 0,
    CTX_RETRIEVE_DETAIL_STANDARD,
    CTX_RETRIEVE_DETAIL_FULL
} CtxRetrieveDetail;

typedef struct {
    CtxQueryKind       kind;
    CtxRetrieveDetail  detail;
    const char        *text;   /* task text, symbol name, or file path — must be non-NULL */
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

/*
 * Expands a handle returned by ctx_retrieve().
 *
 * Supported handles:
 *   expand:symbol:<id>
 *   expand:file:<path>
 *   expand:entrypoints:<path>
 *   expand:source:<id>
 *   expand:callers:<id>
 *   expand:callees:<id>
 *   expand:refs:<id>
 *
 * g       Live graph (may be NULL or empty — handled gracefully).
 * handle  Expansion handle string.
 */
char *ctx_expand_context(CtxGraph *g, const char *handle);

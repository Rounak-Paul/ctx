#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Context retrieval engine.
 *
 * Accepts a natural-language task/query or an explicit symbol/file anchor and
 * returns a compact, ranked, budget-bounded context bundle suitable for direct
 * inclusion in an LLM prompt. The bundle is rendered to either JSON (for
 * programmatic agents) or Markdown (for prompt inclusion). Output is
 * deterministic and never dumps whole files — it packs the highest-value source
 * slices first and reports what was omitted when the budget is exceeded.
 */

typedef enum { CTX_FMT_MARKDOWN = 0, CTX_FMT_JSON } CtxFormat;

typedef enum {
    CTX_QUERY_TASK = 0,   /* natural-language task text */
    CTX_QUERY_SYMBOL,     /* anchored on a symbol name  */
    CTX_QUERY_FILE        /* anchored on a file path    */
} CtxQueryKind;

typedef struct {
    CtxQueryKind kind;
    const char  *text;        /* task text, symbol name, or file path */
    uint32_t     budget;      /* token budget (approx); 0 → default    */
    CtxFormat    format;
} CtxRetrieveRequest;

/*
 * Runs retrieval and returns a heap-allocated, NUL-terminated response string
 * in the requested format. Never returns NULL: on any error it returns a valid
 * formatted error payload. Caller must free().
 *
 * g    Live graph (may be NULL or empty — handled gracefully).
 * req  Retrieval request. req->text must be non-NULL.
 */
char *ctx_retrieve(CtxGraph *g, const CtxRetrieveRequest *req);

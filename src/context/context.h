#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/* All returned strings are heap-allocated; caller must free(). */

/* Full markdown context for a single symbol (signature + callers + callees). */
char *ctx_context_for_symbol(CtxGraph *g, const char *name);

/* All symbols defined in a file, as markdown. */
char *ctx_context_for_file(CtxGraph *g, const char *file_path);

/* High-level codebase summary (counts, top symbols, language breakdown). */
char *ctx_context_summary(CtxGraph *g, const char *root_path);

/* Fuzzy-ish search: symbols whose names contain `query` (case-insensitive). */
char *ctx_context_query(CtxGraph *g, const char *query, uint32_t max_results);

#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Precise navigation tools. Every call refreshes the files it answers from
 * (re-indexing any that changed on disk) and reads code from disk, so output
 * always matches the working tree. Results cite root-relative path:line.
 * All functions return a heap string (never NULL) the caller must free().
 */

/*
 * Symbol map of one file: kind, line range, and signature per symbol.
 *
 * path       Absolute, root-relative, or unique-suffix path.
 * from_line  First line to list (1 = start of file).
 * limit      Maximum symbols to list (0 → default 300).
 */
char *ctx_nav_outline(CtxGraph *g, const char *path, uint32_t from_line, uint32_t limit);

/*
 * Source request.
 *
 * symbol     Symbol name, "Scope::name", "Scope.name", or "path:line"; may be
 *            NULL when file + lines select a raw range.
 * file       Optional path restricting symbol lookup, or the file for lines.
 * lines      Optional "start-end" range (1-based, inclusive).
 * max_lines  Body line cap (0 → default 200).
 */
typedef struct {
    const char *symbol;
    const char *file;
    const char *lines;
    uint32_t    max_lines;
} CtxNavSourceRequest;

/* Exact code of the best-matching definition (with its leading doc comment)
 * or of a file line range, with line numbers. Other matches are listed. */
char *ctx_nav_source(CtxGraph *g, const CtxNavSourceRequest *req);

/*
 * Call sites of a symbol with the calling line's text.
 *
 * symbol  Target symbol (same forms as CtxNavSourceRequest.symbol).
 * file    Optional path restricting the target.
 * depth   1 = direct callers; 2–3 also lists transitive callers.
 */
char *ctx_nav_callers(CtxGraph *g, const char *symbol, const char *file, uint32_t depth);

/* Functions called by a symbol, with call lines and resolved locations. */
char *ctx_nav_callees(CtxGraph *g, const char *symbol, const char *file);

/*
 * Change-impact report: definitions and declarations, direct call sites,
 * transitive callers (depth 3), non-call references, subtypes, affected
 * files, and affected test files.
 */
char *ctx_nav_impact(CtxGraph *g, const char *symbol, const char *file);

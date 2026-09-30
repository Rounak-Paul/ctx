#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Source text of one file with a line index, read from disk at query time so
 * returned code always matches the working tree.
 */
typedef struct {
    char     *text;
    size_t    len;
    uint32_t *line_starts;   /* byte offset of each line, 0-based index */
    uint32_t  line_count;
} CtxSource;

/*
 * Loads a file and indexes its lines.
 * Returns false when the file cannot be read or exceeds 64 MiB.
 */
bool ctx_source_open(const char *path, CtxSource *out);

/* Releases a loaded source and zeroes it. */
void ctx_source_close(CtxSource *src);

/*
 * Returns a pointer to 1-based line n (not NUL-terminated) and its length
 * without the trailing newline, or NULL when n is out of range.
 */
const char *ctx_source_line(const CtxSource *src, uint32_t n, uint32_t *len);

/*
 * Writes path relative to root (no leading "./") when it lies under root,
 * otherwise the path unchanged.
 */
void ctx_path_display(const char *root, const char *path, char *out, size_t out_size);

/*
 * Resolves a user-supplied path (absolute, root-relative, or a unique suffix
 * such as "indexer/indexer.c") to an indexed absolute path.
 *
 * g         Graph; caller must hold at least the read lock.
 * root      Project root.
 * query     User path.
 * out       Receives the absolute path.
 * Returns false when nothing matches; ambiguous suffixes pick the shortest path.
 */
bool ctx_path_resolve_locked(CtxGraph *g, const char *root, const char *query,
                             char *out, size_t out_size);

/* True for third-party code: vendor(s)/, third_party/, external/, node_modules/, deps/. */
bool ctx_path_is_vendor(const char *root, const char *path);

/* Growable heap text buffer used to render tool output. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CtxBuf;

/* Appends printf-formatted text. Returns false on allocation failure (buffer
 * keeps its previous content). */
bool  ctx_buf_printf(CtxBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool  ctx_buf_append(CtxBuf *b, const char *text, size_t len);
/* Returns the NUL-terminated buffer (never NULL) and resets b; caller frees. */
char *ctx_buf_take(CtxBuf *b);

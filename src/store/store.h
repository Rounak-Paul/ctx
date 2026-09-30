#pragma once
#include "../pch.h"
#include "../graph/graph.h"

bool ctx_store_open(const char *db_path);
void ctx_store_close(void);

/* Build db path: ~/.ctx/<hex(fnv64(root_path))>/index.db */
bool ctx_store_build_path(const char *root_path, char *out, size_t out_len);

/*
 * Loads every persisted file into the graph and resolves all reference sites.
 * Returns false when the store is not open.
 */
bool ctx_store_load_graph(CtxGraph *g);

/*
 * File metadata written alongside the file's graph content.
 *
 * path         Absolute file path.
 * mtime_ns     Modification time in nanoseconds since the epoch.
 * size         File size in bytes.
 * lang         CtxLanguage of the file.
 * error_count  Extraction errors recorded for the file.
 */
typedef struct {
    const char *path;
    int64_t     mtime_ns;
    int64_t     size;
    int         lang;
    int         error_count;
} CtxStoreFileState;

/*
 * Persists files in one transaction: upserts each metadata row and replaces
 * the file's symbols and reference sites with the graph's current content.
 * Files absent from the graph are persisted with no symbols.
 */
bool ctx_store_commit_files(CtxGraph *g, const CtxStoreFileState *files, uint32_t count);

/* Deletes metadata, symbols, and sites of the given files in one transaction. */
bool ctx_store_remove_files(const char *const *paths, uint32_t count);

/*
 * Reads the stored modification time (ns) and size of a file.
 * Returns false when the file is not in the store.
 */
bool ctx_store_file_state(const char *path, int64_t *mtime_ns, int64_t *size);

bool ctx_store_set_meta(const char *key, const char *value);
bool ctx_store_get_meta(const char *key, char *buf, size_t buflen);

bool ctx_store_increment_stat(const char *key, int64_t delta);
bool ctx_store_get_stat(const char *key, int64_t *out);

/*
 * Per-file metadata record returned by ctx_store_enumerate_files.
 *
 * path         Absolute path to the source file.
 * lang         Language ID matching CtxLanguage values.
 * mtime_ns     Last modification time in nanoseconds.
 * size         File size in bytes.
 * error_count  Parse/extraction errors recorded for this file.
 * sym_count    Number of definitions in the live graph for this file.
 */
typedef struct {
    char    path[4096];
    int     lang;
    int64_t mtime_ns;
    int64_t size;
    int     error_count;
    int     sym_count;
} CtxFileRecord;

/*
 * Enumerates all files from the store ordered by path.
 *
 * out  Caller-allocated array of CtxFileRecord.
 * max  Capacity of out[].
 * g    Live graph used to fill sym_count; may be NULL.
 * Returns the number of records written.
 */
uint32_t ctx_store_enumerate_files(CtxFileRecord *out, uint32_t max, CtxGraph *g);

/*
 * Callback for ctx_store_embedding_load_all.
 *
 * key   Content key the vector was stored under.
 * vec   dim floats; valid only during the call.
 * user  Caller context.
 */
typedef void (*CtxEmbeddingVisitor)(uint64_t key, const float *vec, uint32_t dim, void *user);

/* Streams every cached embedding whose dimension equals dim. */
bool ctx_store_embedding_load_all(uint32_t dim, CtxEmbeddingVisitor visit, void *user);

/* Upserts count vectors (count * dim floats, row-major) in one transaction. */
bool ctx_store_embedding_put(const uint64_t *keys, const float *vecs, uint32_t count, uint32_t dim);

/* Deletes cached embeddings whose key is not in live_keys. */
bool ctx_store_embedding_retain(const uint64_t *live_keys, uint32_t count);

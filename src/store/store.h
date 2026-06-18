#pragma once
#include "../pch.h"
#include "../graph/graph.h"

bool ctx_store_open(const char *db_path);
void ctx_store_close(void);

bool ctx_store_save_graph(CtxGraph *g);
bool ctx_store_load_graph(CtxGraph *g);

bool ctx_store_file_mtime(const char *path, int64_t *mtime_out);
bool ctx_store_upsert_file(const char *path, int64_t mtime, int64_t size,
                           const char *hash, int lang, int error_count);
bool ctx_store_remove_file(const char *path);

bool ctx_store_set_meta(const char *key, const char *value);
bool ctx_store_get_meta(const char *key, char *buf, size_t buflen);

bool ctx_store_increment_stat(const char *key, int64_t delta);
bool ctx_store_get_stat(const char *key, int64_t *out);

/* Build db path: ~/.ctx/<hex(fnv64(root_path))>/index.db */
bool ctx_store_build_path(const char *root_path, char *out, size_t out_len);

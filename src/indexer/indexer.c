#include "indexer.h"
#include "../store/store.h"
#include "../extractor/extractor.h"
#include "../parser/parser.h"
#include "../event/event.h"
#include "../jobs/jobs.h"
#include "../log/log.h"
#include "../stats/stats.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static CtxGraph     *s_graph    = NULL;
static char          s_root[4096] = {0};
static CtxGraphStats s_stats    = {0};

#define CTX_SEMANTIC_INDEX_VERSION "3"

/* Progress — written by indexer thread, read by UI/CLI */
static volatile uint32_t s_prog_total   = 0;
static volatile uint32_t s_prog_done    = 0;
static volatile bool     s_prog_running = false;

static const char *s_skip_dirs[] = {
    "node_modules", ".git", "build", "bin", "__pycache__",
    ".cache", "dist", "target", ".svn", ".hg", NULL
};

static bool should_skip_dir(const char *name) {
    if (name[0] == '.') return true;
    for (int i = 0; s_skip_dirs[i]; i++)
        if (!strcmp(name, s_skip_dirs[i])) return true;
    return false;
}

static bool is_source_ext(const char *name) {
    return ctx_lang_from_path(name) != CTX_LANG_UNKNOWN;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct FileList { char **paths; uint32_t count; uint32_t cap; } FileList;

static void fl_push(FileList *fl, const char *p) {
    if (fl->count >= fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 1024;
        fl->paths = (char **)realloc(fl->paths, fl->cap * sizeof(char *));
    }
    fl->paths[fl->count++] = strdup(p);
}

static void fl_free(FileList *fl) {
    for (uint32_t i = 0; i < fl->count; i++) free(fl->paths[i]);
    free(fl->paths);
    fl->paths = NULL; fl->count = fl->cap = 0;
}

static void collect_files(const char *dir, FileList *fl) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(de->d_name)) collect_files(full, fl);
        } else if (S_ISREG(st.st_mode) && is_source_ext(de->d_name)) {
            if (st.st_size > 0 && st.st_size <= 10 * 1024 * 1024)
                fl_push(fl, full);
        }
    }
    closedir(d);
}

static void emit_graph_updated(void) {
    CtxGraphStats gs = {0};
    gs.symbols = ctx_graph_symbol_count(s_graph);
    gs.edges   = ctx_graph_edge_count(s_graph);
    ctx_event_emit(CTX_EVENT_GRAPH_UPDATED, &gs, sizeof(gs));
}

bool ctx_indexer_init(const char *root_path) {
    if (!root_path) return false;
    strncpy(s_root, root_path, sizeof(s_root) - 1);

    s_graph = ctx_graph_create();
    if (!s_graph) return false;

    char db_path[1024];
    if (!ctx_store_build_path(root_path, db_path, sizeof(db_path))) return false;
    if (!ctx_store_open(db_path)) return false;

    ctx_store_load_graph(s_graph);
    CTX_LOG_INFO("Loaded %u symbols, %u edges from cache",
                 ctx_graph_symbol_count(s_graph), ctx_graph_edge_count(s_graph));
    return true;
}

void ctx_indexer_shutdown(void) {
    if (s_graph) {
        ctx_store_close();
        ctx_graph_destroy(s_graph);
        s_graph = NULL;
    }
}

void ctx_indexer_index_all(void) {
    if (!s_graph) return;
    int64_t t0 = now_ms();

    FileList fl = {0};
    collect_files(s_root, &fl);

    char index_version[32] = {0};
    bool force_reindex = !ctx_store_get_meta("semantic_index_version",
                                             index_version, sizeof(index_version)) ||
                         strcmp(index_version, CTX_SEMANTIC_INDEX_VERSION) != 0;
    if (force_reindex) {
        CTX_LOG_INFO("Semantic index version changed; rebuilding symbol graph");
    }

    /* Identify stale files */
    FileList stale = {0};
    for (uint32_t i = 0; i < fl.count; i++) {
        struct stat st;
        if (stat(fl.paths[i], &st) != 0) continue;
        int64_t stored_mtime = 0;
        bool known = ctx_store_file_mtime(fl.paths[i], &stored_mtime);
        if (!force_reindex && known && stored_mtime == (int64_t)st.st_mtime) continue;
        fl_push(&stale, fl.paths[i]);
    }

    s_prog_total   = stale.count;
    s_prog_done    = 0;
    s_prog_running = (stale.count > 0);

    if (stale.count == 0) {
        CTX_LOG_INFO("Index up to date: %u files, %u symbols, %u edges",
                     fl.count, ctx_graph_symbol_count(s_graph), ctx_graph_edge_count(s_graph));
        s_stats.files       = fl.count;
        s_stats.symbols     = ctx_graph_symbol_count(s_graph);
        s_stats.edges       = ctx_graph_edge_count(s_graph);
        s_stats.duration_ms = now_ms() - t0;
        fl_free(&fl);
        fl_free(&stale);
        emit_graph_updated();
        return;
    }

    CTX_LOG_INFO("Indexing %u changed files (of %u total)…", stale.count, fl.count);

    uint32_t errors = 0;
    for (uint32_t i = 0; i < stale.count; i++) {
        const char *path = stale.paths[i];
        struct stat st;
        if (stat(path, &st) != 0) continue;

        ctx_graph_remove_file(s_graph, path);
        if (ctx_extract_file(s_graph, path)) {
            if (ctx_lang_from_path(path) != CTX_LANG_UNKNOWN) {
                ctx_store_upsert_file(path, (int64_t)st.st_mtime,
                                      (int64_t)st.st_size, NULL,
                                      (int)ctx_lang_from_path(path), 0);
            }
        } else {
            errors++;
        }

        /* Atomic-ish increment — single writer, readers just sample */
        s_prog_done = i + 1;
    }

    ctx_jobs_wait_all();

    uint32_t resolved = ctx_graph_resolve_calls(s_graph);

    int64_t dur = now_ms() - t0;
    s_stats.files       = fl.count;
    s_stats.symbols     = ctx_graph_symbol_count(s_graph);
    s_stats.edges       = ctx_graph_edge_count(s_graph);
    s_stats.errors      = errors;
    s_stats.duration_ms = dur;

    CTX_LOG_INFO("Index done: %u symbols, %u edges, %u semantic edges resolved in %"PRId64"ms",
                 s_stats.symbols, s_stats.edges, resolved, dur);
    ctx_stats_record_index(stale.count, s_stats.symbols, (double)dur);

    ctx_store_save_graph(s_graph);

    char ts[64];
    time_t now_t = time(NULL);
    struct tm *tm_info = localtime(&now_t);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_info);
    ctx_store_set_meta("last_indexed", ts);
    ctx_store_set_meta("root_path",    s_root);
    ctx_store_set_meta("semantic_index_version", CTX_SEMANTIC_INDEX_VERSION);
    ctx_store_increment_stat("files_indexed",       (int64_t)stale.count);
    ctx_store_increment_stat("symbols_found",        (int64_t)s_stats.symbols);
    ctx_store_increment_stat("errors_encountered",   (int64_t)errors);
    ctx_store_increment_stat("last_index_duration_ms", dur);

    s_prog_running = false;

    fl_free(&fl);
    fl_free(&stale);
    emit_graph_updated();
}

void ctx_indexer_update_file(const char *path) {
    if (!s_graph || !path) return;
    if (ctx_lang_from_path(path) == CTX_LANG_UNKNOWN) return;

    CTX_LOG_DEBUG("Incremental update: %s", path);
    ctx_graph_remove_file(s_graph, path);

    struct stat st;
    if (stat(path, &st) != 0) {
        ctx_store_remove_file(path);
        emit_graph_updated();
        return;
    }

    if (ctx_extract_file(s_graph, path)) {
        ctx_graph_resolve_calls(s_graph);
        ctx_store_upsert_file(path, (int64_t)st.st_mtime,
                              (int64_t)st.st_size, NULL,
                              (int)ctx_lang_from_path(path), 0);
        ctx_store_save_graph(s_graph);
    }
    emit_graph_updated();
}

CtxGraph *ctx_indexer_get_graph(void) { return s_graph; }

void ctx_indexer_get_stats(CtxGraphStats *out) {
    if (out) *out = s_stats;
}

void ctx_indexer_get_progress(CtxIndexProgress *out) {
    if (!out) return;
    out->total   = s_prog_total;
    out->done    = s_prog_done;
    out->running = s_prog_running;
}

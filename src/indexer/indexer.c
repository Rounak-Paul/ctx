#include "indexer.h"
#include "../store/store.h"
#include "../extractor/extractor.h"
#include "../parser/parser.h"
#include "../event/event.h"
#include "../log/log.h"
#include "../stats/stats.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static CtxGraph     *s_graph    = NULL;
static char          s_root[4096] = {0};
static CtxGraphStats s_stats    = {0};
static CtxIndexStatus s_status  = {0};

#define CTX_SEMANTIC_INDEX_VERSION "8"

#if defined(CTX_PLATFORM_WINDOWS)
static CRITICAL_SECTION s_index_lock;
static CRITICAL_SECTION s_status_lock;
static void index_lock(void) { EnterCriticalSection(&s_index_lock); }
static void index_unlock(void) { LeaveCriticalSection(&s_index_lock); }
static void status_lock(void) { EnterCriticalSection(&s_status_lock); }
static void status_unlock(void) { LeaveCriticalSection(&s_status_lock); }
#else
static pthread_mutex_t s_index_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_status_lock = PTHREAD_MUTEX_INITIALIZER;
static void index_lock(void) { pthread_mutex_lock(&s_index_lock); }
static void index_unlock(void) { pthread_mutex_unlock(&s_index_lock); }
static void status_lock(void) { pthread_mutex_lock(&s_status_lock); }
static void status_unlock(void) { pthread_mutex_unlock(&s_status_lock); }
#endif

/* Progress — written by indexer thread, read by UI/CLI */
static volatile uint32_t s_prog_total   = 0;
static volatile uint32_t s_prog_done    = 0;
static volatile bool     s_prog_running = false;

static const char *s_skip_dirs[] = {
    "node_modules", ".git", "build", "bin", "__pycache__",
    ".cache", "dist", "target", ".svn", ".hg", NULL
};

bool ctx_indexer_skips_dir_name(const char *name) {
    if (!name || name[0] == '.') return true;
    for (int i = 0; s_skip_dirs[i]; i++)
        if (!strcmp(name, s_skip_dirs[i])) return true;
    return false;
}

/* True when path lies under s_root and no directory component below the
   root is excluded by ctx_indexer_skips_dir_name — the rule collect_files
   applies while walking. */
static bool path_in_indexed_tree(const char *path) {
    size_t root_len = strlen(s_root);
    if (strncmp(path, s_root, root_len) != 0 || path[root_len] != '/')
        return false;

    const char *component = path + root_len + 1;
    const char *slash;
    while ((slash = strchr(component, '/')) != NULL) {
        char name[256];
        size_t len = (size_t)(slash - component);
        if (len == 0 || len >= sizeof(name)) return false;
        memcpy(name, component, len);
        name[len] = '\0';
        if (ctx_indexer_skips_dir_name(name)) return false;
        component = slash + 1;
    }
    return component[0] != '.';
}

/* Files outside (0, 10 MiB] are not indexed. */
static bool file_size_indexable(off_t size) {
    return size > 0 && size <= 10 * 1024 * 1024;
}

static bool is_source_ext(const char *name) {
    return ctx_lang_from_path(name) != CTX_LANG_UNKNOWN;
}

static int64_t stat_mtime_ns(const struct stat *st) {
#if defined(CTX_PLATFORM_MACOS)
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#elif defined(CTX_PLATFORM_LINUX)
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#else
    return (int64_t)st->st_mtime * 1000000000LL;
#endif
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t unix_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void status_set_progress(uint32_t total, uint32_t done, bool running) {
    s_prog_total = total;
    s_prog_done = done;
    s_prog_running = running;

    status_lock();
    s_status.progress.total = total;
    s_status.progress.done = done;
    s_status.progress.running = running;
    s_status.ready = !running;
    status_unlock();
}

typedef struct PathEntry {
    char          *path;
    struct stat    st;
    UT_hash_handle hh;
} PathEntry;

typedef struct {
    PathEntry  *set;     /* by path */
    PathEntry **items;   /* discovery order */
    uint32_t    count;
    uint32_t    cap;
} FileList;

static void fl_push(FileList *fl, const char *path, const struct stat *st) {
    PathEntry *e = NULL;
    HASH_FIND_STR(fl->set, path, e);
    if (e) return;
    if (fl->count >= fl->cap) {
        uint32_t cap = fl->cap ? fl->cap * 2 : 1024;
        PathEntry **next = (PathEntry **)realloc(fl->items, cap * sizeof(PathEntry *));
        if (!next) return;
        fl->items = next;
        fl->cap = cap;
    }
    e = (PathEntry *)calloc(1, sizeof(PathEntry));
    if (!e) return;
    e->path = strdup(path);
    if (!e->path) { free(e); return; }
    e->st = *st;
    HASH_ADD_KEYPTR(hh, fl->set, e->path, strlen(e->path), e);
    fl->items[fl->count++] = e;
}

static bool fl_contains(const FileList *fl, const char *path) {
    PathEntry *e = NULL;
    HASH_FIND_STR(fl->set, path, e);
    return e != NULL;
}

static void fl_free(FileList *fl) {
    HASH_CLEAR(hh, fl->set);
    for (uint32_t i = 0; i < fl->count; i++) {
        free(fl->items[i]->path);
        free(fl->items[i]);
    }
    free(fl->items);
    memset(fl, 0, sizeof(*fl));
}

static void collect_files(const char *dir, FileList *fl) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!ctx_indexer_skips_dir_name(de->d_name)) collect_files(full, fl);
        } else if (S_ISREG(st.st_mode) && is_source_ext(de->d_name) &&
                   file_size_indexable(st.st_size)) {
            fl_push(fl, full, &st);
        }
    }
    closedir(d);
}

static void emit_graph_updated(void) {
    CtxGraphStats gs = {0};
    gs.files   = ctx_graph_file_count(s_graph);
    gs.symbols = ctx_graph_symbol_count(s_graph);
    gs.edges   = ctx_graph_edge_count(s_graph);
    status_lock();
    s_stats.files = gs.files;
    s_stats.symbols = gs.symbols;
    s_stats.edges = gs.edges;
    s_status.graph_generation++;
    s_status.last_update_unix_ms = unix_ms();
    s_status.cache_loaded = s_graph != NULL;
    s_status.ready = !s_status.progress.running;
    status_unlock();
    ctx_event_emit(CTX_EVENT_GRAPH_UPDATED, &gs, sizeof(gs));
}

/*
 * Extracts one file into the graph and fills its store state.
 * Returns false when extraction failed; the file is then kept in the graph
 * with no content and recorded with an error so it is retried only after it
 * changes on disk.
 */
static bool extract_into_graph(const char *path, const struct stat *st, bool resolve,
                               CtxStoreFileState *state) {
    CtxFileExtract ex;
    bool ok = ctx_extract_file(path, &ex);
    if (!ok) memset(&ex, 0, sizeof(ex));
    ctx_graph_replace_file(s_graph, path, &ex, resolve);
    *state = (CtxStoreFileState){
        .path = path,
        .mtime_ns = stat_mtime_ns(st),
        .size = (int64_t)st->st_size,
        .lang = (int)ctx_lang_from_path(path),
        .error_count = ok ? 0 : 1,
    };
    return ok;
}

bool ctx_indexer_init(const char *root_path) {
    if (!root_path) return false;
#if defined(CTX_PLATFORM_WINDOWS)
    InitializeCriticalSection(&s_index_lock);
    InitializeCriticalSection(&s_status_lock);
#endif
    strncpy(s_root, root_path, sizeof(s_root) - 1);

    s_graph = ctx_graph_create();
    if (!s_graph) return false;

    char db_path[1024];
    if (!ctx_store_build_path(root_path, db_path, sizeof(db_path))) return false;
    if (!ctx_store_open(db_path)) return false;

    ctx_store_load_graph(s_graph);
    uint32_t symbols = ctx_graph_symbol_count(s_graph);
    status_lock();
    s_status.cache_loaded = true;
    s_status.ready = false;
    s_status.graph_generation = symbols ? 1 : 0;
    s_status.last_update_unix_ms = 0;
    status_unlock();
    return true;
}

void ctx_indexer_shutdown(void) {
    if (s_graph) {
        ctx_store_close();
        ctx_graph_destroy(s_graph);
        s_graph = NULL;
    }
#if defined(CTX_PLATFORM_WINDOWS)
    DeleteCriticalSection(&s_status_lock);
    DeleteCriticalSection(&s_index_lock);
#endif
}

void ctx_indexer_index_all(void) {
    if (!s_graph) return;
    index_lock();
    int64_t t0 = now_ms();

    FileList fl = {0};
    collect_files(s_root, &fl);

    char index_version[32] = {0};
    bool force_reindex = !ctx_store_get_meta("semantic_index_version",
                                             index_version, sizeof(index_version)) ||
                         strcmp(index_version, CTX_SEMANTIC_INDEX_VERSION) != 0;
    if (force_reindex) CTX_LOG_INFO("Semantic index version changed; rebuilding symbol graph");

    PathEntry **stale = (PathEntry **)malloc((fl.count ? fl.count : 1) * sizeof(PathEntry *));
    uint32_t stale_count = 0;
    for (uint32_t i = 0; stale && i < fl.count; i++) {
        int64_t mtime_ns = 0, size = 0;
        bool known = ctx_store_file_state(fl.items[i]->path, &mtime_ns, &size);
        if (!force_reindex && known && mtime_ns == stat_mtime_ns(&fl.items[i]->st) &&
            size == (int64_t)fl.items[i]->st.st_size)
            continue;
        stale[stale_count++] = fl.items[i];
    }

    uint32_t stored_cap = ctx_graph_file_count(s_graph) + 1024u;
    CtxFileRecord *stored = NULL;
    uint32_t stored_count = 0;
    for (;;) {
        CtxFileRecord *next = (CtxFileRecord *)realloc(stored, stored_cap * sizeof(CtxFileRecord));
        if (!next) { stored_count = 0; break; }
        stored = next;
        stored_count = ctx_store_enumerate_files(stored, stored_cap, NULL);
        if (stored_count < stored_cap) break;
        stored_cap *= 2;
    }
    const char **removed = (const char **)malloc((stored_count ? stored_count : 1) * sizeof(char *));
    uint32_t removed_count = 0;
    for (uint32_t i = 0; removed && i < stored_count; i++) {
        if (fl_contains(&fl, stored[i].path)) continue;
        ctx_graph_replace_file(s_graph, stored[i].path, NULL, false);
        removed[removed_count++] = stored[i].path;
    }

    status_set_progress(stale_count, 0, true);
    if (stale_count) CTX_LOG_INFO("Indexing %u changed files (of %u total)…", stale_count, fl.count);

    CtxStoreFileState *states = (CtxStoreFileState *)calloc(stale_count ? stale_count : 1,
                                                            sizeof(CtxStoreFileState));
    uint32_t state_count = 0;
    uint32_t errors = 0;
    for (uint32_t i = 0; states && i < stale_count; i++) {
        if (!extract_into_graph(stale[i]->path, &stale[i]->st, false, &states[state_count++]))
            errors++;
        status_set_progress(stale_count, i + 1, true);
    }

    bool changed = stale_count > 0 || removed_count > 0;
    uint32_t edges = changed ? ctx_graph_resolve_all(s_graph) : ctx_graph_edge_count(s_graph);
    if (changed) {
        ctx_store_remove_files(removed, removed_count);
        ctx_store_commit_files(s_graph, states, state_count);
    }

    int64_t dur = now_ms() - t0;
    CtxGraphStats stats = {
        .files = fl.count,
        .symbols = ctx_graph_symbol_count(s_graph),
        .edges = edges,
        .errors = errors,
        .duration_ms = dur,
    };
    status_lock();
    s_stats = stats;
    status_unlock();

    if (changed) {
        CTX_LOG_INFO("Index done: %u files, %u symbols, %u edges in %"PRId64"ms",
                     stats.files, stats.symbols, stats.edges, dur);
        ctx_stats_record_index(stale_count, stats.symbols, (double)dur);
        char ts[64];
        time_t now_t = time(NULL);
        struct tm tm_info;
        localtime_r(&now_t, &tm_info);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_info);
        ctx_store_set_meta("last_indexed", ts);
        ctx_store_increment_stat("files_indexed",          (int64_t)stale_count);
        ctx_store_increment_stat("errors_encountered",     (int64_t)errors);
        ctx_store_increment_stat("last_index_duration_ms", dur);
    } else {
        CTX_LOG_INFO("Index up to date: %u files, %u symbols, %u edges",
                     stats.files, stats.symbols, stats.edges);
    }
    ctx_store_set_meta("root_path", s_root);
    ctx_store_set_meta("semantic_index_version", CTX_SEMANTIC_INDEX_VERSION);

    free(states);
    free(removed);
    free(stored);
    free(stale);
    fl_free(&fl);

    status_set_progress(stale_count, stale_count, false);
    emit_graph_updated();
    index_unlock();
}

bool ctx_indexer_update_file(const char *path) {
    if (!s_graph || !path) return false;
    if (ctx_lang_from_path(path) == CTX_LANG_UNKNOWN) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ctx_indexer_index_all();
            return true;
        }
        return false;
    }

    /* Apply the same eligibility rules as a full scan so incremental updates
       never index what collect_files would skip (and later prune). A path
       that stops qualifying (deleted, emptied, oversized) is removed. */
    if (!path_in_indexed_tree(path)) return false;

    index_lock();
    struct stat st;
    bool indexable = stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                     file_size_indexable(st.st_size);
    int64_t stored_mtime = 0, stored_size = 0;
    bool known = ctx_store_file_state(path, &stored_mtime, &stored_size);
    bool unchanged = indexable ? known && stored_mtime == stat_mtime_ns(&st) &&
                                 stored_size == (int64_t)st.st_size
                               : !known;
    if (unchanged) {
        index_unlock();
        return false;
    }

    CTX_LOG_DEBUG("Incremental update: %s", path);
    status_set_progress(1, 0, true);
    if (indexable) {
        CtxStoreFileState state;
        extract_into_graph(path, &st, true, &state);
        ctx_store_commit_files(s_graph, &state, 1);
    } else {
        ctx_graph_replace_file(s_graph, path, NULL, true);
        ctx_store_remove_files(&path, 1);
    }
    status_set_progress(1, 1, false);
    emit_graph_updated();
    index_unlock();
    return true;
}

uint32_t ctx_indexer_ensure_fresh(const char *const *paths, uint32_t count) {
    uint32_t updated = 0;
    for (uint32_t i = 0; paths && i < count; i++)
        if (paths[i] && ctx_indexer_update_file(paths[i])) updated++;
    return updated;
}

const char *ctx_indexer_root(void) { return s_root; }

CtxGraph *ctx_indexer_get_graph(void) { return s_graph; }

void ctx_indexer_get_stats(CtxGraphStats *out) {
    if (!out) return;
    status_lock();
    *out = s_stats;
    status_unlock();
}

void ctx_indexer_get_progress(CtxIndexProgress *out) {
    if (!out) return;
    status_lock();
    *out = s_status.progress;
    status_unlock();
}

void ctx_indexer_get_status(CtxIndexStatus *out) {
    if (!out) return;
    status_lock();
    *out = s_status;
    status_unlock();
}

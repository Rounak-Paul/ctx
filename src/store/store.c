#include "store.h"
#include "../log/log.h"

#if defined(CTX_PLATFORM_WINDOWS)
#include <direct.h>
#define ctx_mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#define ctx_mkdir_one(p) mkdir(p, 0755)
#endif

static sqlite3    *s_db   = NULL;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static bool exec_sql(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(s_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        CTX_LOG_ERROR("SQL error: %s", err ? err : "unknown");
        sqlite3_free(err);
        return false;
    }
    return true;
}

/* Bumped whenever the table layout changes. A mismatch drops the cached
 * tables so they are rebuilt with the current columns — no manual delete. */
#define CTX_STORE_SCHEMA_VERSION 3

static void migrate_schema(void) {
    exec_sql("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);");
    sqlite3_stmt *stmt = NULL;
    int stored = 0;
    if (sqlite3_prepare_v2(s_db, "SELECT value FROM meta WHERE key='schema_version';",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            stored = v ? atoi(v) : 0;
        }
        sqlite3_finalize(stmt);
    }
    if (stored == CTX_STORE_SCHEMA_VERSION) return;

    CTX_LOG_INFO("Store schema %d → %d; rebuilding cached tables", stored, CTX_STORE_SCHEMA_VERSION);
    exec_sql("DROP TABLE IF EXISTS symbols; DROP TABLE IF EXISTS edges;"
             "DROP TABLE IF EXISTS sites; DROP TABLE IF EXISTS files;"
             "DROP TABLE IF EXISTS embeddings;"
             "DELETE FROM meta WHERE key='semantic_index_version';");
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%d", CTX_STORE_SCHEMA_VERSION);
    sqlite3_stmt *up = NULL;
    if (sqlite3_prepare_v2(s_db, "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?);",
                           -1, &up, NULL) == SQLITE_OK) {
        sqlite3_bind_text(up, 1, vbuf, -1, SQLITE_STATIC);
        sqlite3_step(up);
        sqlite3_finalize(up);
    }
}

static bool create_schema(void) {
    return exec_sql(
        "CREATE TABLE IF NOT EXISTS meta("
        "  key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS files("
        "  path TEXT PRIMARY KEY, mtime_ns INTEGER, size INTEGER,"
        "  lang INTEGER, error_count INTEGER);"
        "CREATE TABLE IF NOT EXISTS symbols("
        "  id INTEGER PRIMARY KEY, file TEXT NOT NULL, name TEXT,"
        "  line INTEGER, col INTEGER, end_line INTEGER, kind INTEGER,"
        "  signature TEXT, scope TEXT, lang INTEGER, is_definition INTEGER);"
        "CREATE TABLE IF NOT EXISTS sites("
        "  file TEXT NOT NULL, from_name TEXT, from_line INTEGER,"
        "  to_name TEXT NOT NULL, kind INTEGER);"
        "CREATE TABLE IF NOT EXISTS embeddings("
        "  key INTEGER PRIMARY KEY, dim INTEGER, vec BLOB);"
        "CREATE TABLE IF NOT EXISTS stats("
        "  key TEXT PRIMARY KEY, value INTEGER);"
        "CREATE INDEX IF NOT EXISTS idx_sym_file ON symbols(file);"
        "CREATE INDEX IF NOT EXISTS idx_site_file ON sites(file);"
    );
}

/*
 * Creates every directory component in a path if it is missing.
 *
 * path  Directory path to create.
 */
static bool ensure_dir(const char *path) {
    if (!path || !*path) return false;

    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;

    size_t len = strlen(buf);
    while (len > 1 && (buf[len - 1] == '/' || buf[len - 1] == '\\'))
        buf[--len] = '\0';

    for (char *p = buf + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        char sep = *p;
        *p = '\0';
        if (ctx_mkdir_one(buf) != 0 && errno != EEXIST) {
            *p = sep;
            return false;
        }
        *p = sep;
    }

    return ctx_mkdir_one(buf) == 0 || errno == EEXIST;
}

bool ctx_store_build_path(const char *root_path, char *out, size_t out_len) {
    extern uint64_t ctx_fnv64(const char *, size_t);
    uint64_t h = ctx_fnv64(root_path, strlen(root_path));

    const char *home = getenv("HOME");
#if defined(CTX_PLATFORM_WINDOWS)
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = ".";

    char dir1[512], dir2[512];
    snprintf(dir1, sizeof(dir1), "%s/.ctx", home);
    snprintf(dir2, sizeof(dir2), "%s/.ctx/%016" PRIx64, home, h);

    if (!ensure_dir(dir1) || !ensure_dir(dir2)) {
        CTX_LOG_ERROR("Cannot create store directory %s", dir2);
        return false;
    }

    int n = snprintf(out, out_len, "%s/index.db", dir2);
    return (n > 0 && (size_t)n < out_len);
}

bool ctx_store_open(const char *db_path) {
    pthread_mutex_lock(&s_lock);
    if (s_db) { pthread_mutex_unlock(&s_lock); return true; }

    int rc = sqlite3_open(db_path, &s_db);
    if (rc != SQLITE_OK) {
        CTX_LOG_ERROR("Cannot open db %s: %s", db_path, sqlite3_errmsg(s_db));
        sqlite3_close(s_db); s_db = NULL;
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(s_db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    migrate_schema();
    bool ok = create_schema();
    pthread_mutex_unlock(&s_lock);
    CTX_LOG_INFO("Store opened: %s", db_path);
    return ok;
}

void ctx_store_close(void) {
    pthread_mutex_lock(&s_lock);
    if (s_db) { sqlite3_close(s_db); s_db = NULL; }
    pthread_mutex_unlock(&s_lock);
}

/* Finalizes prepared statements and clears their slots. */
static void finalize_all(sqlite3_stmt **stmts, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (stmts[i]) sqlite3_finalize(stmts[i]);
        stmts[i] = NULL;
    }
}

static bool prepare_all(const char *const *sql, sqlite3_stmt **stmts, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sqlite3_prepare_v2(s_db, sql[i], -1, &stmts[i], NULL) != SQLITE_OK) {
            CTX_LOG_ERROR("SQL prepare failed: %s", sqlite3_errmsg(s_db));
            finalize_all(stmts, count);
            return false;
        }
    }
    return true;
}

static bool step_done(sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (rc == SQLITE_DONE) return true;
    CTX_LOG_ERROR("SQL step failed: %s", sqlite3_errmsg(s_db));
    return false;
}

static bool commit_or_rollback(bool ok) {
    if (ok && exec_sql("COMMIT;")) return true;
    exec_sql("ROLLBACK;");
    return false;
}

static void column_copy(sqlite3_stmt *stmt, int col, char *dst, size_t dst_size) {
    const char *v = (const char *)sqlite3_column_text(stmt, col);
    snprintf(dst, dst_size, "%s", v ? v : "");
}

/* Reads a symbol row (file, id, name, …) into a draft. */
static void read_symbol_row(sqlite3_stmt *stmt, CtxSymbolDraft *sym) {
    memset(sym, 0, sizeof(*sym));
    sym->id            = (uint64_t)sqlite3_column_int64(stmt, 1);
    column_copy(stmt, 2, sym->name, sizeof(sym->name));
    sym->line          = (uint32_t)sqlite3_column_int(stmt, 3);
    sym->col           = (uint32_t)sqlite3_column_int(stmt, 4);
    sym->end_line      = (uint32_t)sqlite3_column_int(stmt, 5);
    sym->kind          = (CtxSymbolKind)sqlite3_column_int(stmt, 6);
    column_copy(stmt, 7, sym->signature, sizeof(sym->signature));
    column_copy(stmt, 8, sym->scope, sizeof(sym->scope));
    sym->lang          = (uint8_t)sqlite3_column_int(stmt, 9);
    sym->is_definition = sqlite3_column_int(stmt, 10) != 0;
}

/* Current file of a stepped cursor, NULL once exhausted. */
static const char *cursor_file(sqlite3_stmt *stmt, bool live) {
    return live ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
}

/*
 * Streams symbols and sites (both ordered by file) and installs one file at a
 * time, so peak memory stays at one file's drafts rather than the whole store.
 */
bool ctx_store_load_graph(CtxGraph *g) {
    if (!g) return false;
    pthread_mutex_lock(&s_lock);
    if (!s_db) { pthread_mutex_unlock(&s_lock); return false; }

    sqlite3_stmt *syms = NULL, *sites = NULL;
    bool ok = sqlite3_prepare_v2(s_db,
                  "SELECT file,id,name,line,col,end_line,kind,signature,scope,lang,is_definition"
                  " FROM symbols ORDER BY file;", -1, &syms, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(s_db,
                  "SELECT file,from_name,from_line,to_name,kind FROM sites ORDER BY file;",
                  -1, &sites, NULL) == SQLITE_OK;
    bool syms_live = ok && sqlite3_step(syms) == SQLITE_ROW;
    bool sites_live = ok && sqlite3_step(sites) == SQLITE_ROW;

    char path[4096];
    CtxSymbolDraft draft;
    while (syms_live || sites_live) {
        const char *sf = cursor_file(syms, syms_live);
        const char *tf = cursor_file(sites, sites_live);
        const char *next = !sf ? tf : !tf ? sf : (strcmp(sf, tf) <= 0 ? sf : tf);
        if (!next) break;
        snprintf(path, sizeof(path), "%s", next);

        CtxFileExtract ex = {0};
        while (syms_live && !strcmp(cursor_file(syms, true), path)) {
            read_symbol_row(syms, &draft);
            ctx_file_extract_add_symbol(&ex, &draft);
            syms_live = sqlite3_step(syms) == SQLITE_ROW;
        }
        while (sites_live && !strcmp(cursor_file(sites, true), path)) {
            ctx_file_extract_add_site(&ex,
                (const char *)sqlite3_column_text(sites, 1),
                (uint32_t)sqlite3_column_int(sites, 2),
                (const char *)sqlite3_column_text(sites, 3),
                (CtxEdgeKind)sqlite3_column_int(sites, 4));
            sites_live = sqlite3_step(sites) == SQLITE_ROW;
        }
        ctx_graph_replace_file(g, path, &ex, false);
    }
    sqlite3_finalize(syms);
    sqlite3_finalize(sites);
    pthread_mutex_unlock(&s_lock);

    uint32_t edges = ctx_graph_resolve_all(g);
    CTX_LOG_INFO("Graph loaded from store: %u files, %u symbols, %u edges",
                 ctx_graph_file_count(g), ctx_graph_symbol_count(g), edges);
    return ok;
}

enum { ST_UPSERT_FILE, ST_DEL_SYMS, ST_DEL_SITES, ST_INS_SYM, ST_INS_SITE, ST_COMMIT_COUNT };

static bool write_file_content(sqlite3_stmt **st, const CtxGraphFile *f) {
    bool ok = true;
    for (uint32_t i = 0; ok && i < f->symbol_count; i++) {
        const CtxSymbol *s = f->symbols[i];
        sqlite3_stmt *ins = st[ST_INS_SYM];
        sqlite3_bind_int64(ins, 1, (int64_t)s->id);
        sqlite3_bind_text (ins, 2, f->path, -1, SQLITE_STATIC);
        sqlite3_bind_text (ins, 3, s->name, -1, SQLITE_STATIC);
        sqlite3_bind_int  (ins, 4, (int)s->line);
        sqlite3_bind_int  (ins, 5, (int)s->col);
        sqlite3_bind_int  (ins, 6, (int)s->end_line);
        sqlite3_bind_int  (ins, 7, (int)s->kind);
        sqlite3_bind_text (ins, 8, s->signature, -1, SQLITE_STATIC);
        sqlite3_bind_text (ins, 9, s->scope, -1, SQLITE_STATIC);
        sqlite3_bind_int  (ins, 10, (int)s->lang);
        sqlite3_bind_int  (ins, 11, s->is_definition ? 1 : 0);
        ok = step_done(ins);
    }
    for (uint32_t i = 0; ok && i < f->site_count; i++) {
        const CtxRefSite *site = &f->sites[i];
        sqlite3_stmt *ins = st[ST_INS_SITE];
        sqlite3_bind_text(ins, 1, f->path, -1, SQLITE_STATIC);
        if (site->from_name) sqlite3_bind_text(ins, 2, site->from_name, -1, SQLITE_STATIC);
        else                 sqlite3_bind_null(ins, 2);
        sqlite3_bind_int (ins, 3, (int)site->from_line);
        sqlite3_bind_text(ins, 4, site->to_name, -1, SQLITE_STATIC);
        sqlite3_bind_int (ins, 5, (int)site->kind);
        ok = step_done(ins);
    }
    return ok;
}

bool ctx_store_commit_files(CtxGraph *g, const CtxStoreFileState *files, uint32_t count) {
    if (!g || (!files && count)) return false;
    if (count == 0) return true;
    static const char *const sql[ST_COMMIT_COUNT] = {
        "INSERT OR REPLACE INTO files(path,mtime_ns,size,lang,error_count) VALUES(?,?,?,?,?);",
        "DELETE FROM symbols WHERE file=?;",
        "DELETE FROM sites WHERE file=?;",
        ("INSERT OR REPLACE INTO symbols(id,file,name,line,col,end_line,kind,signature,scope,lang,is_definition)"
         " VALUES(?,?,?,?,?,?,?,?,?,?,?);"),
        "INSERT INTO sites(file,from_name,from_line,to_name,kind) VALUES(?,?,?,?,?);",
    };

    pthread_mutex_lock(&s_lock);
    if (!s_db) { pthread_mutex_unlock(&s_lock); return false; }
    sqlite3_stmt *st[ST_COMMIT_COUNT] = {0};
    if (!prepare_all(sql, st, ST_COMMIT_COUNT) || !exec_sql("BEGIN;")) {
        finalize_all(st, ST_COMMIT_COUNT);
        pthread_mutex_unlock(&s_lock);
        return false;
    }

    bool ok = true;
    ctx_graph_rlock(g);
    for (uint32_t i = 0; ok && i < count; i++) {
        const CtxStoreFileState *fs = &files[i];
        if (!fs->path) continue;
        sqlite3_stmt *up = st[ST_UPSERT_FILE];
        sqlite3_bind_text (up, 1, fs->path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(up, 2, fs->mtime_ns);
        sqlite3_bind_int64(up, 3, fs->size);
        sqlite3_bind_int  (up, 4, fs->lang);
        sqlite3_bind_int  (up, 5, fs->error_count);
        ok = step_done(up);
        for (int d = ST_DEL_SYMS; ok && d <= ST_DEL_SITES; d++) {
            sqlite3_bind_text(st[d], 1, fs->path, -1, SQLITE_STATIC);
            ok = step_done(st[d]);
        }
        const CtxGraphFile *f = ok ? ctx_graph_find_file_locked(g, fs->path) : NULL;
        if (f) ok = write_file_content(st, f);
    }
    ctx_graph_runlock(g);

    finalize_all(st, ST_COMMIT_COUNT);
    ok = commit_or_rollback(ok);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool ctx_store_remove_files(const char *const *paths, uint32_t count) {
    if (!paths && count) return false;
    if (count == 0) return true;
    static const char *const sql[3] = {
        "DELETE FROM files WHERE path=?;",
        "DELETE FROM symbols WHERE file=?;",
        "DELETE FROM sites WHERE file=?;",
    };
    pthread_mutex_lock(&s_lock);
    if (!s_db) { pthread_mutex_unlock(&s_lock); return false; }
    sqlite3_stmt *st[3] = {0};
    if (!prepare_all(sql, st, 3) || !exec_sql("BEGIN;")) {
        finalize_all(st, 3);
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (!paths[i]) continue;
        for (int d = 0; ok && d < 3; d++) {
            sqlite3_bind_text(st[d], 1, paths[i], -1, SQLITE_STATIC);
            ok = step_done(st[d]);
        }
    }
    finalize_all(st, 3);
    ok = commit_or_rollback(ok);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool ctx_store_file_state(const char *path, int64_t *mtime_ns, int64_t *size) {
    if (!path) return false;
    pthread_mutex_lock(&s_lock);
    bool found = false;
    sqlite3_stmt *stmt = NULL;
    if (s_db && sqlite3_prepare_v2(s_db, "SELECT mtime_ns,size FROM files WHERE path=?;",
                                   -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (mtime_ns) *mtime_ns = sqlite3_column_int64(stmt, 0);
            if (size) *size = sqlite3_column_int64(stmt, 1);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return found;
}

bool ctx_store_set_meta(const char *key, const char *value) {
    if (!s_db) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?);", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return true;
}

bool ctx_store_get_meta(const char *key, char *buf, size_t buflen) {
    if (!s_db || !buf) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db, "SELECT value FROM meta WHERE key=?;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) { strncpy(buf, v, buflen - 1); buf[buflen-1] = '\0'; found = true; }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return found;
}

bool ctx_store_increment_stat(const char *key, int64_t delta) {
    if (!s_db) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "INSERT INTO stats(key,value) VALUES(?,?)"
        " ON CONFLICT(key) DO UPDATE SET value=value+excluded.value;",
        -1, &stmt, NULL);
    sqlite3_bind_text (stmt, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, delta);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return true;
}

bool ctx_store_get_stat(const char *key, int64_t *out) {
    if (!s_db || !out) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db, "SELECT value FROM stats WHERE key=?;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) { *out = sqlite3_column_int64(stmt, 0); found = true; }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return found;
}

uint32_t ctx_store_enumerate_files(CtxFileRecord *out, uint32_t max, CtxGraph *g) {
    if (!out || max == 0) return 0;
    pthread_mutex_lock(&s_lock);
    uint32_t n = 0;
    sqlite3_stmt *stmt = NULL;
    if (s_db && sqlite3_prepare_v2(s_db,
            "SELECT path, lang, mtime_ns, size, error_count FROM files ORDER BY path;",
            -1, &stmt, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
            CtxFileRecord *r = &out[n++];
            memset(r, 0, sizeof(*r));
            column_copy(stmt, 0, r->path, sizeof(r->path));
            r->lang        = sqlite3_column_int  (stmt, 1);
            r->mtime_ns    = sqlite3_column_int64(stmt, 2);
            r->size        = sqlite3_column_int64(stmt, 3);
            r->error_count = sqlite3_column_int  (stmt, 4);
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);

    if (g) {
        ctx_graph_rlock(g);
        for (uint32_t i = 0; i < n; i++) {
            const CtxGraphFile *f = ctx_graph_find_file_locked(g, out[i].path);
            for (uint32_t k = 0; f && k < f->symbol_count; k++)
                if (f->symbols[k]->is_definition) out[i].sym_count++;
        }
        ctx_graph_runlock(g);
    }
    return n;
}

bool ctx_store_embedding_load_all(uint32_t dim, CtxEmbeddingVisitor visit, void *user) {
    if (!visit || dim == 0) return false;
    pthread_mutex_lock(&s_lock);
    bool ok = false;
    sqlite3_stmt *stmt = NULL;
    if (s_db && sqlite3_prepare_v2(s_db, "SELECT key, vec FROM embeddings WHERE dim=?;",
                                   -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int)dim);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const void *blob = sqlite3_column_blob(stmt, 1);
            int bytes = sqlite3_column_bytes(stmt, 1);
            if (!blob || bytes != (int)(dim * sizeof(float))) continue;
            visit((uint64_t)sqlite3_column_int64(stmt, 0), (const float *)blob, dim, user);
        }
        ok = true;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool ctx_store_embedding_put(const uint64_t *keys, const float *vecs, uint32_t count, uint32_t dim) {
    if (!keys || !vecs || dim == 0) return false;
    if (count == 0) return true;
    static const char *const sql[1] = {
        "INSERT OR REPLACE INTO embeddings(key,dim,vec) VALUES(?,?,?);",
    };
    pthread_mutex_lock(&s_lock);
    if (!s_db) { pthread_mutex_unlock(&s_lock); return false; }
    sqlite3_stmt *st[1] = {0};
    if (!prepare_all(sql, st, 1) || !exec_sql("BEGIN;")) {
        finalize_all(st, 1);
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        sqlite3_bind_int64(st[0], 1, (int64_t)keys[i]);
        sqlite3_bind_int  (st[0], 2, (int)dim);
        sqlite3_bind_blob (st[0], 3, vecs + (size_t)i * dim, (int)(dim * sizeof(float)), SQLITE_STATIC);
        ok = step_done(st[0]);
    }
    finalize_all(st, 1);
    ok = commit_or_rollback(ok);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool ctx_store_embedding_retain(const uint64_t *live_keys, uint32_t count) {
    if (!live_keys && count) return false;
    static const char *const sql[1] = { "INSERT OR IGNORE INTO temp.live_keys(key) VALUES(?);" };
    pthread_mutex_lock(&s_lock);
    if (!s_db ||
        !exec_sql("CREATE TEMP TABLE IF NOT EXISTS live_keys(key INTEGER PRIMARY KEY);"
                  "DELETE FROM temp.live_keys;")) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    sqlite3_stmt *st[1] = {0};
    if (!prepare_all(sql, st, 1) || !exec_sql("BEGIN;")) {
        finalize_all(st, 1);
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        sqlite3_bind_int64(st[0], 1, (int64_t)live_keys[i]);
        ok = step_done(st[0]);
    }
    finalize_all(st, 1);
    if (ok) ok = exec_sql("DELETE FROM embeddings WHERE key NOT IN (SELECT key FROM temp.live_keys);");
    ok = commit_or_rollback(ok);
    exec_sql("DELETE FROM temp.live_keys;");
    pthread_mutex_unlock(&s_lock);
    return ok;
}

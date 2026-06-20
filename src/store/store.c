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

/* Bumped whenever the symbols/edges table layout changes. A mismatch drops the
 * cached tables so they are rebuilt with the current columns — no manual delete. */
#define CTX_STORE_SCHEMA_VERSION 2

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
    if (stored != CTX_STORE_SCHEMA_VERSION) {
        CTX_LOG_INFO("Store schema %d → %d; rebuilding cached tables", stored, CTX_STORE_SCHEMA_VERSION);
        exec_sql("DROP TABLE IF EXISTS symbols; DROP TABLE IF EXISTS edges; DROP TABLE IF EXISTS files;");
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
}

static bool create_schema(void) {
    return exec_sql(
        "CREATE TABLE IF NOT EXISTS meta("
        "  key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS files("
        "  path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER,"
        "  hash TEXT, lang INTEGER, error_count INTEGER);"
        "CREATE TABLE IF NOT EXISTS symbols("
        "  id INTEGER PRIMARY KEY, name TEXT, file TEXT,"
        "  line INTEGER, col INTEGER, kind INTEGER,"
        "  signature TEXT, is_definition INTEGER,"
        "  end_line INTEGER, scope TEXT, lang INTEGER);"
        "CREATE TABLE IF NOT EXISTS edges("
        "  from_id INTEGER, to_id INTEGER, kind INTEGER,"
        "  PRIMARY KEY(from_id, to_id, kind));"
        "CREATE TABLE IF NOT EXISTS stats("
        "  key TEXT PRIMARY KEY, value INTEGER);"
        "CREATE INDEX IF NOT EXISTS idx_sym_file ON symbols(file);"
        "CREATE INDEX IF NOT EXISTS idx_sym_name ON symbols(name);"
        "CREATE INDEX IF NOT EXISTS idx_edge_from ON edges(from_id);"
        "CREATE INDEX IF NOT EXISTS idx_edge_to   ON edges(to_id);"
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

bool ctx_store_save_graph(CtxGraph *g) {
    if (!g || !s_db) return false;
    pthread_mutex_lock(&s_lock);

    sqlite3_exec(s_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    /* Upsert symbols */
    sqlite3_stmt *sym_stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "INSERT OR REPLACE INTO symbols(id,name,file,line,col,kind,signature,is_definition,end_line,scope,lang)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?);", -1, &sym_stmt, NULL);

    ctx_graph_rlock(g);
    CtxSymbol *s;
    for (s = g->symbols; s != NULL; s = (CtxSymbol *)s->hh.next) {
        sqlite3_bind_int64(sym_stmt, 1, (int64_t)s->id);
        sqlite3_bind_text (sym_stmt, 2, s->name,      -1, SQLITE_STATIC);
        sqlite3_bind_text (sym_stmt, 3, s->file,      -1, SQLITE_STATIC);
        sqlite3_bind_int  (sym_stmt, 4, (int)s->line);
        sqlite3_bind_int  (sym_stmt, 5, (int)s->col);
        sqlite3_bind_int  (sym_stmt, 6, (int)s->kind);
        sqlite3_bind_text (sym_stmt, 7, s->signature, -1, SQLITE_STATIC);
        sqlite3_bind_int  (sym_stmt, 8, s->is_definition ? 1 : 0);
        sqlite3_bind_int  (sym_stmt, 9, (int)s->end_line);
        sqlite3_bind_text (sym_stmt, 10, s->scope,    -1, SQLITE_STATIC);
        sqlite3_bind_int  (sym_stmt, 11, (int)s->lang);
        sqlite3_step(sym_stmt);
        sqlite3_reset(sym_stmt);
    }
    sqlite3_finalize(sym_stmt);

    /* Upsert edges */
    sqlite3_stmt *edge_stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "INSERT OR IGNORE INTO edges(from_id,to_id,kind) VALUES(?,?,?);",
        -1, &edge_stmt, NULL);
    CtxEdgeEntry *e;
    for (e = g->edges; e != NULL; e = (CtxEdgeEntry *)e->hh.next) {
        sqlite3_bind_int64(edge_stmt, 1, (int64_t)e->from_id);
        sqlite3_bind_int64(edge_stmt, 2, (int64_t)e->to_id);
        sqlite3_bind_int  (edge_stmt, 3, (int)e->kind);
        sqlite3_step(edge_stmt);
        sqlite3_reset(edge_stmt);
    }
    ctx_graph_runlock(g);
    sqlite3_finalize(edge_stmt);

    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    pthread_mutex_unlock(&s_lock);
    return true;
}

bool ctx_store_load_graph(CtxGraph *g) {
    if (!g || !s_db) return false;
    pthread_mutex_lock(&s_lock);

    /* Load symbols */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "SELECT id,name,file,line,col,kind,signature,is_definition,end_line,scope,lang FROM symbols;",
        -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CtxSymbol sym = {0};
        sym.id           = (uint64_t)sqlite3_column_int64(stmt, 0);
        const char *nm   = (const char *)sqlite3_column_text(stmt, 1);
        const char *fl   = (const char *)sqlite3_column_text(stmt, 2);
        sym.line         = (uint32_t)sqlite3_column_int(stmt, 3);
        sym.col          = (uint32_t)sqlite3_column_int(stmt, 4);
        sym.kind         = (CtxSymbolKind)sqlite3_column_int(stmt, 5);
        const char *sig  = (const char *)sqlite3_column_text(stmt, 6);
        sym.is_definition = sqlite3_column_int(stmt, 7) != 0;
        sym.end_line     = (uint32_t)sqlite3_column_int(stmt, 8);
        const char *scp  = (const char *)sqlite3_column_text(stmt, 9);
        sym.lang         = (uint8_t)sqlite3_column_int(stmt, 10);
        if (nm) strncpy(sym.name,      nm,  sizeof(sym.name)      - 1);
        if (fl) strncpy(sym.file,      fl,  sizeof(sym.file)      - 1);
        if (sig) strncpy(sym.signature, sig, sizeof(sym.signature) - 1);
        if (scp) strncpy(sym.scope,     scp, sizeof(sym.scope)     - 1);
        ctx_graph_add_symbol(g, &sym);
    }
    sqlite3_finalize(stmt);

    /* Load edges */
    sqlite3_prepare_v2(s_db,
        "SELECT from_id,to_id,kind FROM edges;", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint64_t from = (uint64_t)sqlite3_column_int64(stmt, 0);
        uint64_t to   = (uint64_t)sqlite3_column_int64(stmt, 1);
        CtxEdgeKind k = (CtxEdgeKind)sqlite3_column_int(stmt, 2);
        ctx_graph_add_edge(g, from, to, k);
    }
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&s_lock);
    CTX_LOG_INFO("Graph loaded from store: %u symbols, %u edges",
                 ctx_graph_symbol_count(g), ctx_graph_edge_count(g));
    return true;
}

bool ctx_store_file_mtime(const char *path, int64_t *mtime_out) {
    if (!s_db || !path || !mtime_out) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db, "SELECT mtime FROM files WHERE path=?;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *mtime_out = sqlite3_column_int64(stmt, 0);
        found = true;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return found;
}

bool ctx_store_upsert_file(const char *path, int64_t mtime, int64_t size,
                           const char *hash, int lang, int error_count) {
    if (!s_db) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "INSERT OR REPLACE INTO files(path,mtime,size,hash,lang,error_count)"
        " VALUES(?,?,?,?,?,?);", -1, &stmt, NULL);
    sqlite3_bind_text (stmt, 1, path,  -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int64(stmt, 3, size);
    sqlite3_bind_text (stmt, 4, hash ? hash : "", -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 5, lang);
    sqlite3_bind_int  (stmt, 6, error_count);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);
    return true;
}

bool ctx_store_remove_file(const char *path) {
    if (!s_db) return false;
    pthread_mutex_lock(&s_lock);
    sqlite3_exec(s_db, "BEGIN;", NULL, NULL, NULL);

    char sql[8192];
    /* Remove symbols for file */
    snprintf(sql, sizeof(sql),
             "DELETE FROM edges WHERE from_id IN "
             "(SELECT id FROM symbols WHERE file='%s') OR "
             "to_id IN (SELECT id FROM symbols WHERE file='%s');", path, path);
    sqlite3_exec(s_db, sql, NULL, NULL, NULL);

    snprintf(sql, sizeof(sql), "DELETE FROM symbols WHERE file='%s';", path);
    sqlite3_exec(s_db, sql, NULL, NULL, NULL);

    snprintf(sql, sizeof(sql), "DELETE FROM files WHERE path='%s';", path);
    sqlite3_exec(s_db, sql, NULL, NULL, NULL);

    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, NULL);
    pthread_mutex_unlock(&s_lock);
    return true;
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
    if (!s_db || !out || max == 0) return 0;
    pthread_mutex_lock(&s_lock);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(s_db,
        "SELECT path, lang, mtime, size, error_count FROM files ORDER BY path;",
        -1, &stmt, NULL);

    uint32_t n = 0;
    while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
        CtxFileRecord *r = &out[n];
        memset(r, 0, sizeof(*r));
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        if (p) strncpy(r->path, p, sizeof(r->path) - 1);
        r->lang        = sqlite3_column_int  (stmt, 1);
        r->mtime       = sqlite3_column_int64(stmt, 2);
        r->size        = sqlite3_column_int64(stmt, 3);
        r->error_count = sqlite3_column_int  (stmt, 4);
        r->sym_count   = 0;
        n++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&s_lock);

    /* Fill sym_count from live graph — avoids an expensive JOIN */
    if (g) {
        ctx_graph_rlock(g);
        CtxSymbol *s, *tmp;
        HASH_ITER(hh, g->symbols, s, tmp) {
            if (!s->is_definition) continue;
            for (uint32_t i = 0; i < n; i++) {
                if (!strcmp(out[i].path, s->file)) { out[i].sym_count++; break; }
            }
        }
        ctx_graph_runlock(g);
    }

    return n;
}

#include "graph.h"
#include "../parser/parser.h"

uint64_t ctx_fnv64(const char *data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t ctx_symbol_id(const char *file, const char *name, uint32_t line) {
    char buf[5120];
    int n = snprintf(buf, sizeof(buf), "%s:%s:%u", file ? file : "", name ? name : "", line);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    return ctx_fnv64(buf, (size_t)n);
}

/* ---- extraction buffers ------------------------------------------------ */

bool ctx_file_extract_add_symbol(CtxFileExtract *ex, const CtxSymbolDraft *sym) {
    if (!ex || !sym) return false;
    if (ex->symbol_count >= ex->symbol_cap) {
        uint32_t cap = ex->symbol_cap ? ex->symbol_cap * 2 : 64;
        CtxSymbolDraft *next = (CtxSymbolDraft *)realloc(ex->symbols, cap * sizeof(CtxSymbolDraft));
        if (!next) return false;
        ex->symbols = next;
        ex->symbol_cap = cap;
    }
    ex->symbols[ex->symbol_count++] = *sym;
    return true;
}

bool ctx_file_extract_add_site(CtxFileExtract *ex, const char *from_name,
                               uint32_t from_line, const char *to_name,
                               CtxEdgeKind kind) {
    if (!ex || !to_name || !to_name[0]) return false;
    if (ex->site_count >= ex->site_cap) {
        uint32_t cap = ex->site_cap ? ex->site_cap * 2 : 256;
        CtxRefSite *next = (CtxRefSite *)realloc(ex->sites, cap * sizeof(CtxRefSite));
        if (!next) return false;
        ex->sites = next;
        ex->site_cap = cap;
    }
    char *to = strdup(to_name);
    char *from = (from_name && from_name[0]) ? strdup(from_name) : NULL;
    if (!to || (from_name && from_name[0] && !from)) {
        free(to);
        free(from);
        return false;
    }
    ex->sites[ex->site_count++] = (CtxRefSite){
        .from_name = from, .to_name = to, .from_line = from_line, .kind = kind,
    };
    return true;
}

static void free_sites(CtxRefSite *sites, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        free(sites[i].from_name);
        free(sites[i].to_name);
    }
    free(sites);
}

void ctx_file_extract_free(CtxFileExtract *ex) {
    if (!ex) return;
    free(ex->symbols);
    free_sites(ex->sites, ex->site_count);
    memset(ex, 0, sizeof(*ex));
}

/* ---- lifecycle and locking ---------------------------------------------- */

CtxGraph *ctx_graph_create(void) {
    CtxGraph *g = (CtxGraph *)calloc(1, sizeof(CtxGraph));
    if (!g) return NULL;
#if defined(CTX_PLATFORM_WINDOWS)
    InitializeSRWLock(&g->lock);
#else
    pthread_rwlock_init(&g->lock, NULL);
#endif
    return g;
}

void ctx_graph_destroy(CtxGraph *g) {
    if (!g) return;
    HASH_CLEAR(hh, g->symbols);
    CtxGraphFile *f, *ftmp;
    HASH_ITER(hh, g->files, f, ftmp) {
        HASH_DEL(g->files, f);
        for (uint32_t i = 0; i < f->symbol_count; i++) free(f->symbols[i]);
        free(f->symbols);
        free_sites(f->sites, f->site_count);
        free(f->path);
        free(f);
    }

    CtxEdgeEntry *e, *etmp;
    HASH_ITER(hh, g->edges, e, etmp) { HASH_DEL(g->edges, e); free(e); }

    CtxNameEntry *n, *ntmp;
    HASH_ITER(hh, g->names, n, ntmp) {
        HASH_DEL(g->names, n);
        free(n->symbols);
        free(n->name);
        free(n);
    }
#if !defined(CTX_PLATFORM_WINDOWS)
    pthread_rwlock_destroy(&g->lock);
#endif
    free(g);
}

void ctx_graph_rlock(CtxGraph *g) {
#if defined(CTX_PLATFORM_WINDOWS)
    AcquireSRWLockShared(&g->lock);
#else
    pthread_rwlock_rdlock(&g->lock);
#endif
}
void ctx_graph_runlock(CtxGraph *g) {
#if defined(CTX_PLATFORM_WINDOWS)
    ReleaseSRWLockShared(&g->lock);
#else
    pthread_rwlock_unlock(&g->lock);
#endif
}
void ctx_graph_wlock(CtxGraph *g) {
#if defined(CTX_PLATFORM_WINDOWS)
    AcquireSRWLockExclusive(&g->lock);
#else
    pthread_rwlock_wrlock(&g->lock);
#endif
}
void ctx_graph_wunlock(CtxGraph *g) {
#if defined(CTX_PLATFORM_WINDOWS)
    ReleaseSRWLockExclusive(&g->lock);
#else
    pthread_rwlock_unlock(&g->lock);
#endif
}

/* ---- edges (ref-counted, derived from sites) ------------------------------ */

static uint64_t edge_key(uint64_t from_id, uint64_t to_id, CtxEdgeKind kind) {
    uint64_t key_data[3] = { from_id, to_id, (uint64_t)kind };
    return ctx_fnv64((const char *)key_data, sizeof(key_data));
}

static void edge_acquire(CtxGraph *g, uint64_t from_id, uint64_t to_id, CtxEdgeKind kind) {
    uint64_t key = edge_key(from_id, to_id, kind);
    CtxEdgeEntry *e = NULL;
    HASH_FIND(hh, g->edges, &key, sizeof(uint64_t), e);
    if (e) { e->refs++; return; }
    e = (CtxEdgeEntry *)malloc(sizeof(CtxEdgeEntry));
    if (!e) return;
    e->key = key;
    e->from_id = from_id;
    e->to_id = to_id;
    e->kind = kind;
    e->refs = 1;
    HASH_ADD(hh, g->edges, key, sizeof(uint64_t), e);
}

static void edge_release(CtxGraph *g, uint64_t from_id, uint64_t to_id, CtxEdgeKind kind) {
    uint64_t key = edge_key(from_id, to_id, kind);
    CtxEdgeEntry *e = NULL;
    HASH_FIND(hh, g->edges, &key, sizeof(uint64_t), e);
    if (!e) return;
    if (e->refs > 1) { e->refs--; return; }
    HASH_DEL(g->edges, e);
    free(e);
}

/* ---- name index ------------------------------------------------------------ */

static void name_index_add(CtxGraph *g, CtxSymbol *s) {
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, s->name, n);
    if (!n) {
        n = (CtxNameEntry *)calloc(1, sizeof(CtxNameEntry));
        if (!n) return;
        n->name = strdup(s->name);
        if (!n->name) { free(n); return; }
        HASH_ADD_KEYPTR(hh, g->names, n->name, strlen(n->name), n);
    }
    if (n->count >= n->cap) {
        uint32_t cap = n->cap ? n->cap * 2 : 2;
        CtxSymbol **next = (CtxSymbol **)realloc(n->symbols, cap * sizeof(CtxSymbol *));
        if (!next) return;
        n->symbols = next;
        n->cap = cap;
    }
    n->symbols[n->count++] = s;
}

static void name_index_remove(CtxGraph *g, const CtxSymbol *s) {
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, s->name, n);
    if (!n) return;
    for (uint32_t i = 0; i < n->count; i++) {
        if (n->symbols[i] != s) continue;
        memmove(&n->symbols[i], &n->symbols[i + 1], (n->count - i - 1) * sizeof(CtxSymbol *));
        n->count--;
        break;
    }
    if (n->count == 0) {
        HASH_DEL(g->names, n);
        free(n->symbols);
        free(n->name);
        free(n);
    }
}

/* ---- resolution -------------------------------------------------------------- */

/* Length of the directory part of path (up to, excluding, the last separator). */
static size_t dir_len(const char *path) {
    const char *slash = strrchr(path, '/');
#if defined(CTX_PLATFORM_WINDOWS)
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? (size_t)(slash - path) : 0;
}

static int kind_resolution_rank(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION: case CTX_SYM_METHOD:   return 5;
    case CTX_SYM_CLASS: case CTX_SYM_STRUCT:      return 4;
    case CTX_SYM_TYPEDEF: case CTX_SYM_ENUM:      return 3;
    case CTX_SYM_MACRO: case CTX_SYM_VARIABLE:    return 2;
    case CTX_SYM_NAMESPACE: case CTX_SYM_INCLUDE: return 1;
    default:                                      return 0;
    }
}

static bool is_c_family_header(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && (!strcmp(dot, ".h") || !strcmp(dot, ".hh") || !strcmp(dot, ".hpp") ||
                   !strcmp(dot, ".hxx") || !strcmp(dot, ".inl"));
}

/* Macros and static functions defined in a C/C++ source file are visible only
 * inside that translation unit. */
static bool is_translation_unit_local(const CtxSymbol *s) {
    if ((s->lang != CTX_LANG_C && s->lang != CTX_LANG_CPP) || is_c_family_header(s->file))
        return false;
    return s->kind == CTX_SYM_MACRO || !strncmp(s->signature, "static ", 7) ||
           !strncmp(s->signature, "static\n", 7);
}

/* Whether a symbol can be the target of an edge of the given kind. */
static bool kind_accepts(CtxEdgeKind edge, CtxSymbolKind k) {
    switch (edge) {
    case CTX_EDGE_CALLS:
        return k == CTX_SYM_FUNCTION || k == CTX_SYM_METHOD || k == CTX_SYM_MACRO || k == CTX_SYM_CLASS;
    case CTX_EDGE_INHERITS:
        return k == CTX_SYM_CLASS || k == CTX_SYM_STRUCT || k == CTX_SYM_TYPEDEF;
    default:
        return k != CTX_SYM_INCLUDE && k != CTX_SYM_UNKNOWN;
    }
}

/*
 * Picks the most plausible target among same-named symbols: same file, then
 * same directory, then definitions, then kind rank. Candidates of the wrong
 * kind or private to another translation unit are skipped. Ties keep the
 * lowest id so resolution is deterministic.
 */
static const CtxSymbol *pick_target(const CtxNameEntry *n, const char *from_file, CtxEdgeKind edge) {
    const CtxSymbol *best = NULL;
    int best_score = INT_MIN;
    size_t from_dir = dir_len(from_file);
    for (uint32_t i = 0; i < n->count; i++) {
        const CtxSymbol *s = n->symbols[i];
        if (!kind_accepts(edge, s->kind)) continue;
        bool same_file = s->file == from_file || !strcmp(s->file, from_file);
        if (!same_file && is_translation_unit_local(s)) continue;
        int score = 0;
        if (same_file) score += 100;
        else if (dir_len(s->file) == from_dir && !strncmp(s->file, from_file, from_dir)) score += 40;
        if (s->is_definition) score += 20;
        score += kind_resolution_rank(s->kind);
        if (score > best_score || (score == best_score && best && s->id < best->id)) {
            best_score = score;
            best = s;
        }
    }
    return best;
}

/* Innermost range wins; ties break on later start line, then lower id. */
static bool symbol_tighter(const CtxSymbol *a, const CtxSymbol *b) {
    uint32_t ra = a->end_line - a->line, rb = b->end_line - b->line;
    if (ra != rb) return ra < rb;
    if (a->line != b->line) return a->line > b->line;
    return a->id < b->id;
}

/* Definitions win; ties break on earlier line, then lower id. */
static bool symbol_preferred(const CtxSymbol *a, const CtxSymbol *b) {
    if (a->is_definition != b->is_definition) return a->is_definition;
    if (a->line != b->line) return a->line < b->line;
    return a->id < b->id;
}

/*
 * Finds the enclosing symbol of a site: the same-named symbol in the site's
 * file whose line range contains the site line (innermost wins), else the
 * first same-named definition in that file. File identity is pointer equality
 * because symbol paths are interned in their CtxGraphFile.
 */
static const CtxSymbol *find_site_source(CtxGraph *g, const CtxGraphFile *f,
                                         const CtxRefSite *site) {
    if (!site->from_name) return NULL;
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, site->from_name, n);
    if (!n) return NULL;
    const CtxSymbol *containing = NULL;
    const CtxSymbol *fallback = NULL;
    for (uint32_t i = 0; i < n->count; i++) {
        const CtxSymbol *s = n->symbols[i];
        if (s->file != f->path) continue;
        if (s->line <= site->from_line && site->from_line <= s->end_line) {
            if (!containing || symbol_tighter(s, containing)) containing = s;
        } else if (!fallback || symbol_preferred(s, fallback)) {
            fallback = s;
        }
    }
    return containing ? containing : fallback;
}

/*
 * Resolves one reference site to (source symbol, target symbol) and moves the
 * derived edge reference when the resolution changed.
 *
 * f     File owning the site.
 * site  Site to resolve; res_from/res_to are updated in place.
 */
static void resolve_site(CtxGraph *g, const CtxGraphFile *f, CtxRefSite *site) {
    uint64_t from_id = 0, to_id = 0;
    const CtxSymbol *src = find_site_source(g, f, site);
    if (src) {
        CtxNameEntry *n = NULL;
        HASH_FIND_STR(g->names, site->to_name, n);
        const CtxSymbol *dst = n ? pick_target(n, f->path, site->kind) : NULL;
        if (dst && dst->id != src->id) {
            from_id = src->id;
            to_id = dst->id;
        }
    }
    if (from_id == site->res_from && to_id == site->res_to) return;
    if (site->res_from) edge_release(g, site->res_from, site->res_to, site->kind);
    if (from_id) edge_acquire(g, from_id, to_id, site->kind);
    site->res_from = from_id;
    site->res_to = to_id;
}

/* ---- file replacement ------------------------------------------------------- */

typedef struct NameMark {
    const char    *name;   /* borrowed from a live symbol or extraction draft */
    UT_hash_handle hh;
} NameMark;

static void mark_name(NameMark **set, const char *name) {
    NameMark *m = NULL;
    HASH_FIND_STR(*set, name, m);
    if (m) return;
    m = (NameMark *)malloc(sizeof(NameMark));
    if (!m) return;
    m->name = name;
    HASH_ADD_KEYPTR(hh, *set, m->name, strlen(m->name), m);
}

static void clear_marks(NameMark **set) {
    NameMark *m, *tmp;
    HASH_ITER(hh, *set, m, tmp) { HASH_DEL(*set, m); free(m); }
}

static int symbol_order(const void *a, const void *b) {
    const CtxSymbol *x = *(const CtxSymbol *const *)a;
    const CtxSymbol *y = *(const CtxSymbol *const *)b;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    if (x->col != y->col) return x->col < y->col ? -1 : 1;
    return 0;
}

/* Removes a file's symbols and sites from every index. Names of the removed
 * symbols are copied into *removed_names (heap strings) when non-NULL. */
static void clear_file(CtxGraph *g, CtxGraphFile *f, char ***removed_names,
                       uint32_t *removed_count) {
    for (uint32_t i = 0; i < f->site_count; i++) {
        CtxRefSite *site = &f->sites[i];
        if (site->res_from) edge_release(g, site->res_from, site->res_to, site->kind);
    }
    free_sites(f->sites, f->site_count);
    f->sites = NULL;
    f->site_count = 0;

    if (removed_names && f->symbol_count) {
        *removed_names = (char **)calloc(f->symbol_count, sizeof(char *));
        *removed_count = 0;
    }
    for (uint32_t i = 0; i < f->symbol_count; i++) {
        CtxSymbol *s = f->symbols[i];
        if (removed_names && *removed_names) {
            char *copy = strdup(s->name);
            if (copy) (*removed_names)[(*removed_count)++] = copy;
        }
        name_index_remove(g, s);
        HASH_DEL(g->symbols, s);
        free(s);
    }
    free(f->symbols);
    f->symbols = NULL;
    f->symbol_count = 0;
}

/* Allocates a graph symbol with its strings packed after the struct. */
static CtxSymbol *symbol_from_draft(const CtxSymbolDraft *d, const char *file) {
    size_t name_len = strnlen(d->name, sizeof(d->name));
    size_t sig_len = strnlen(d->signature, sizeof(d->signature));
    size_t scope_len = strnlen(d->scope, sizeof(d->scope));
    CtxSymbol *s = (CtxSymbol *)malloc(sizeof(CtxSymbol) + name_len + sig_len + scope_len + 3);
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    char *tail = (char *)(s + 1);
    memcpy(tail, d->name, name_len);            tail[name_len] = '\0';
    s->name = tail;                              tail += name_len + 1;
    memcpy(tail, d->signature, sig_len);        tail[sig_len] = '\0';
    s->signature = tail;                         tail += sig_len + 1;
    memcpy(tail, d->scope, scope_len);          tail[scope_len] = '\0';
    s->scope = tail;
    s->id = d->id;
    s->file = file;
    s->line = d->line;
    s->col = d->col;
    s->end_line = d->end_line < d->line ? d->line : d->end_line;
    s->kind = d->kind;
    s->lang = d->lang;
    s->is_definition = d->is_definition;
    return s;
}

static void install_extract(CtxGraph *g, CtxGraphFile *f, CtxFileExtract *ex) {
    f->symbols = ex->symbol_count
        ? (CtxSymbol **)malloc(ex->symbol_count * sizeof(CtxSymbol *)) : NULL;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ex->symbol_count && f->symbols; i++) {
        const CtxSymbolDraft *draft = &ex->symbols[i];
        if (!draft->name[0]) continue;
        CtxSymbol *existing = NULL;
        HASH_FIND(hh, g->symbols, &draft->id, sizeof(uint64_t), existing);
        if (existing) continue;
        CtxSymbol *s = symbol_from_draft(draft, f->path);
        if (!s) continue;
        HASH_ADD(hh, g->symbols, id, sizeof(uint64_t), s);
        name_index_add(g, s);
        f->symbols[kept++] = s;
    }
    f->symbol_count = kept;
    if (kept > 1) qsort(f->symbols, kept, sizeof(CtxSymbol *), symbol_order);

    f->sites = ex->sites;
    f->site_count = ex->site_count;
    for (uint32_t i = 0; i < f->site_count; i++) {
        f->sites[i].res_from = 0;
        f->sites[i].res_to = 0;
    }
    ex->sites = NULL;
    ex->site_count = 0;
    ex->site_cap = 0;
    ctx_file_extract_free(ex);
}

void ctx_graph_replace_file(CtxGraph *g, const char *path, CtxFileExtract *ex,
                            bool resolve) {
    if (!g || !path || !path[0]) {
        ctx_file_extract_free(ex);
        return;
    }

    ctx_graph_wlock(g);

    char   **old_names = NULL;
    uint32_t old_count = 0;
    CtxGraphFile *f = NULL;
    HASH_FIND_STR(g->files, path, f);
    if (f) clear_file(g, f, resolve ? &old_names : NULL, &old_count);

    if (!ex) {
        if (f) {
            HASH_DEL(g->files, f);
            free(f->path);
            free(f);
            f = NULL;
        }
    } else {
        if (!f) {
            f = (CtxGraphFile *)calloc(1, sizeof(CtxGraphFile));
            if (f) f->path = strdup(path);
            if (f && !f->path) { free(f); f = NULL; }
            if (f) HASH_ADD_KEYPTR(hh, g->files, f->path, strlen(f->path), f);
        }
        if (f) {
            install_extract(g, f, ex);
            f->version = ++g->version_seq;
        }
        else ctx_file_extract_free(ex);
    }

    if (resolve) {
        NameMark *affected = NULL;
        for (uint32_t i = 0; i < old_count; i++) mark_name(&affected, old_names[i]);
        if (f) {
            for (uint32_t i = 0; i < f->symbol_count; i++) mark_name(&affected, f->symbols[i]->name);
            for (uint32_t i = 0; i < f->site_count; i++) resolve_site(g, f, &f->sites[i]);
        }
        if (affected) {
            CtxGraphFile *other, *otmp;
            HASH_ITER(hh, g->files, other, otmp) {
                if (other == f) continue;
                for (uint32_t i = 0; i < other->site_count; i++) {
                    CtxRefSite *site = &other->sites[i];
                    NameMark *m = NULL;
                    HASH_FIND_STR(affected, site->to_name, m);
                    if (m) resolve_site(g, other, site);
                }
            }
        }
        clear_marks(&affected);
    }

    ctx_graph_wunlock(g);

    for (uint32_t i = 0; i < old_count; i++) free(old_names[i]);
    free(old_names);
}

uint32_t ctx_graph_resolve_all(CtxGraph *g) {
    if (!g) return 0;
    ctx_graph_wlock(g);
    CtxGraphFile *f, *tmp;
    HASH_ITER(hh, g->files, f, tmp) {
        for (uint32_t i = 0; i < f->site_count; i++) resolve_site(g, f, &f->sites[i]);
    }
    uint32_t edges = (uint32_t)HASH_COUNT(g->edges);
    ctx_graph_wunlock(g);
    return edges;
}

/* ---- queries ------------------------------------------------------------------ */

uint32_t ctx_graph_symbol_count(CtxGraph *g) {
    if (!g) return 0;
    ctx_graph_rlock(g);
    uint32_t n = (uint32_t)HASH_COUNT(g->symbols);
    ctx_graph_runlock(g);
    return n;
}

uint32_t ctx_graph_edge_count(CtxGraph *g) {
    if (!g) return 0;
    ctx_graph_rlock(g);
    uint32_t n = (uint32_t)HASH_COUNT(g->edges);
    ctx_graph_runlock(g);
    return n;
}

uint32_t ctx_graph_file_count(CtxGraph *g) {
    if (!g) return 0;
    ctx_graph_rlock(g);
    uint32_t n = (uint32_t)HASH_COUNT(g->files);
    ctx_graph_runlock(g);
    return n;
}

CtxSymbol *ctx_graph_find_by_id_locked(CtxGraph *g, uint64_t id) {
    if (!g) return NULL;
    CtxSymbol *s = NULL;
    HASH_FIND(hh, g->symbols, &id, sizeof(uint64_t), s);
    return s;
}

const CtxGraphFile *ctx_graph_find_file_locked(CtxGraph *g, const char *path) {
    if (!g || !path) return NULL;
    CtxGraphFile *f = NULL;
    HASH_FIND_STR(g->files, path, f);
    return f;
}

const CtxNameEntry *ctx_graph_find_name_locked(CtxGraph *g, const char *name) {
    if (!g || !name) return NULL;
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, name, n);
    return n;
}

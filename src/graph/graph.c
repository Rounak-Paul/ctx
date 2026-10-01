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

/* strdup for optional strings: NULL/"" yield NULL; allocation failure clears *ok. */
static char *dup_optional(const char *s, bool *ok) {
    if (!s || !s[0]) return NULL;
    char *copy = strdup(s);
    if (!copy) *ok = false;
    return copy;
}

static void free_site(CtxRefSite *site) {
    free(site->from_name);
    free(site->to_name);
    free(site->to_scope);
    free(site->recv);
    free(site->arg_types);
}

static void free_sites(CtxRefSite *sites, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free_site(&sites[i]);
    free(sites);
}

static void free_decls(CtxLookupDecl *decls, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        free(decls[i].scope);
        free(decls[i].name);
        free(decls[i].target);
    }
    free(decls);
}

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

bool ctx_file_extract_add_site(CtxFileExtract *ex, const CtxSiteDraft *d) {
    if (!ex || !d || !d->to_name || !d->to_name[0]) return false;
    if (ex->site_count >= ex->site_cap) {
        uint32_t cap = ex->site_cap ? ex->site_cap * 2 : 256;
        CtxRefSite *next = (CtxRefSite *)realloc(ex->sites, cap * sizeof(CtxRefSite));
        if (!next) return false;
        ex->sites = next;
        ex->site_cap = cap;
    }
    bool ok = true;
    CtxRefSite site = {
        .from_name = dup_optional(d->from_name, &ok),
        .to_name   = dup_optional(d->to_name, &ok),
        .to_scope  = dup_optional(d->to_scope, &ok),
        .recv      = dup_optional(d->recv, &ok),
        .arg_types = dup_optional(d->arg_types, &ok),
        .from_line = d->from_line,
        .kind      = d->kind,
        .member    = d->member,
    };
    if (!ok) {
        free_site(&site);
        return false;
    }
    ex->sites[ex->site_count++] = site;
    return true;
}

bool ctx_file_extract_add_decl(CtxFileExtract *ex, CtxDeclKind kind, const char *scope,
                               const char *name, const char *target,
                               uint32_t line, uint32_t end_line, bool local) {
    if (!ex || !target || !target[0]) return false;
    if (kind != CTX_DECL_USING_NAMESPACE && (!name || !name[0])) return false;
    if (ex->decl_count >= ex->decl_cap) {
        uint32_t cap = ex->decl_cap ? ex->decl_cap * 2 : 16;
        CtxLookupDecl *next = (CtxLookupDecl *)realloc(ex->decls, cap * sizeof(CtxLookupDecl));
        if (!next) return false;
        ex->decls = next;
        ex->decl_cap = cap;
    }
    bool ok = true;
    CtxLookupDecl decl = {
        .scope    = strdup(scope ? scope : ""),
        .name     = kind == CTX_DECL_USING_NAMESPACE ? NULL : dup_optional(name, &ok),
        .target   = dup_optional(target, &ok),
        .line     = line,
        .end_line = end_line < line ? line : end_line,
        .kind     = kind,
        .local    = local,
    };
    if (!ok || !decl.scope) {
        free(decl.scope);
        free(decl.name);
        free(decl.target);
        return false;
    }
    ex->decls[ex->decl_count++] = decl;
    return true;
}

void ctx_file_extract_free(CtxFileExtract *ex) {
    if (!ex) return;
    free(ex->symbols);
    free_sites(ex->sites, ex->site_count);
    free_decls(ex->decls, ex->decl_count);
    memset(ex, 0, sizeof(*ex));
}

/* ---- lifecycle and locking ---------------------------------------------- */

CtxGraph *ctx_graph_create(void) {
    CtxGraph *g = (CtxGraph *)calloc(1, sizeof(CtxGraph));
    if (!g) return NULL;
    g->include_epoch = 1;
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
        free_decls(f->decls, f->decl_count);
        free(f->includes);
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
    CtxDeclEntry *d, *dtmp;
    HASH_ITER(hh, g->decls, d, dtmp) {
        HASH_DEL(g->decls, d);
        free(d->decls);
        free(d->files);
        free(d->name);
        free(d);
    }
    CtxBaseEntry *b, *btmp;
    HASH_ITER(hh, g->bases, b, btmp) {
        HASH_DEL(g->bases, b);
        free(b->base_ids);
        free(b);
    }
    CtxBasenameEntry *bn, *bntmp;
    HASH_ITER(hh, g->basenames, bn, bntmp) {
        HASH_DEL(g->basenames, bn);
        free(bn->files);
        free(bn->basename);
        free(bn);
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

/* ---- base-class index (mirrors inheritance edges) ------------------------ */

static void base_link(CtxGraph *g, uint64_t class_id, uint64_t base_id) {
    CtxBaseEntry *b = NULL;
    HASH_FIND(hh, g->bases, &class_id, sizeof(uint64_t), b);
    if (!b) {
        b = (CtxBaseEntry *)calloc(1, sizeof(CtxBaseEntry));
        if (!b) return;
        b->class_id = class_id;
        HASH_ADD(hh, g->bases, class_id, sizeof(uint64_t), b);
    }
    if (b->count >= b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 2;
        uint64_t *next = (uint64_t *)realloc(b->base_ids, cap * sizeof(uint64_t));
        if (!next) return;
        b->base_ids = next;
        b->cap = cap;
    }
    b->base_ids[b->count++] = base_id;
}

static void base_unlink(CtxGraph *g, uint64_t class_id, uint64_t base_id) {
    CtxBaseEntry *b = NULL;
    HASH_FIND(hh, g->bases, &class_id, sizeof(uint64_t), b);
    if (!b) return;
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->base_ids[i] != base_id) continue;
        memmove(&b->base_ids[i], &b->base_ids[i + 1], (b->count - i - 1) * sizeof(uint64_t));
        b->count--;
        break;
    }
    if (b->count == 0) {
        HASH_DEL(g->bases, b);
        free(b->base_ids);
        free(b);
    }
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
    if (kind == CTX_EDGE_INHERITS) base_link(g, from_id, to_id);
}

static void edge_release(CtxGraph *g, uint64_t from_id, uint64_t to_id, CtxEdgeKind kind) {
    uint64_t key = edge_key(from_id, to_id, kind);
    CtxEdgeEntry *e = NULL;
    HASH_FIND(hh, g->edges, &key, sizeof(uint64_t), e);
    if (!e) return;
    if (e->refs > 1) { e->refs--; return; }
    HASH_DEL(g->edges, e);
    free(e);
    if (kind == CTX_EDGE_INHERITS) base_unlink(g, from_id, to_id);
}

/* ---- name, declaration and basename indexes ------------------------------- */

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

/* Declarations indexed graph-wide: named and not function-local. */
static bool decl_is_indexed(const CtxLookupDecl *d) {
    return d->name && !d->local;
}

static void decl_index_add(CtxGraph *g, const CtxGraphFile *f, const CtxLookupDecl *d) {
    CtxDeclEntry *e = NULL;
    HASH_FIND_STR(g->decls, d->name, e);
    if (!e) {
        e = (CtxDeclEntry *)calloc(1, sizeof(CtxDeclEntry));
        if (!e) return;
        e->name = strdup(d->name);
        if (!e->name) { free(e); return; }
        HASH_ADD_KEYPTR(hh, g->decls, e->name, strlen(e->name), e);
    }
    if (e->count >= e->cap) {
        uint32_t cap = e->cap ? e->cap * 2 : 2;
        const CtxLookupDecl **decls = (const CtxLookupDecl **)realloc((void *)e->decls, cap * sizeof(*decls));
        if (!decls) return;
        e->decls = decls;
        const CtxGraphFile **files = (const CtxGraphFile **)realloc((void *)e->files, cap * sizeof(*files));
        if (!files) return;
        e->files = files;
        e->cap = cap;
    }
    e->decls[e->count] = d;
    e->files[e->count] = f;
    e->count++;
}

static void decl_index_remove(CtxGraph *g, const CtxLookupDecl *d) {
    CtxDeclEntry *e = NULL;
    HASH_FIND_STR(g->decls, d->name, e);
    if (!e) return;
    for (uint32_t i = 0; i < e->count; i++) {
        if (e->decls[i] != d) continue;
        memmove((void *)&e->decls[i], &e->decls[i + 1], (e->count - i - 1) * sizeof(*e->decls));
        memmove((void *)&e->files[i], &e->files[i + 1], (e->count - i - 1) * sizeof(*e->files));
        e->count--;
        break;
    }
    if (e->count == 0) {
        HASH_DEL(g->decls, e);
        free((void *)e->decls);
        free((void *)e->files);
        free(e->name);
        free(e);
    }
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
#if defined(CTX_PLATFORM_WINDOWS)
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static void basename_index_add(CtxGraph *g, CtxGraphFile *f) {
    const char *base = path_basename(f->path);
    CtxBasenameEntry *e = NULL;
    HASH_FIND_STR(g->basenames, base, e);
    if (!e) {
        e = (CtxBasenameEntry *)calloc(1, sizeof(CtxBasenameEntry));
        if (!e) return;
        e->basename = strdup(base);
        if (!e->basename) { free(e); return; }
        HASH_ADD_KEYPTR(hh, g->basenames, e->basename, strlen(e->basename), e);
    }
    if (e->count >= e->cap) {
        uint32_t cap = e->cap ? e->cap * 2 : 2;
        CtxGraphFile **next = (CtxGraphFile **)realloc(e->files, cap * sizeof(CtxGraphFile *));
        if (!next) return;
        e->files = next;
        e->cap = cap;
    }
    e->files[e->count++] = f;
}

static void basename_index_remove(CtxGraph *g, const CtxGraphFile *f) {
    CtxBasenameEntry *e = NULL;
    HASH_FIND_STR(g->basenames, path_basename(f->path), e);
    if (!e) return;
    for (uint32_t i = 0; i < e->count; i++) {
        if (e->files[i] != f) continue;
        memmove(&e->files[i], &e->files[i + 1], (e->count - i - 1) * sizeof(CtxGraphFile *));
        e->count--;
        break;
    }
    if (e->count == 0) {
        HASH_DEL(g->basenames, e);
        free(e->files);
        free(e->basename);
        free(e);
    }
}

/* ---- scopes ------------------------------------------------------------------ */

#define CTX_SCOPE_MAX_PARTS  48u
#define CTX_MODULE_MAX_PARTS 12u
#define CTX_SCOPE_OVERFLOW   UINT32_MAX

typedef struct {
    const char *ptr;
    size_t      len;
} ScopePart;

/* A "::" path with its components; parts point into text, so never copy it. */
typedef struct {
    char      text[256];
    ScopePart parts[CTX_SCOPE_MAX_PARTS];
    uint32_t  count;
} ScopePath;

/*
 * Appends the components of a "::"-joined scope to parts[count..], skipping
 * anonymous namespaces. Returns the new count, or CTX_SCOPE_OVERFLOW.
 *
 * scope  Scope string; NULL is treated as the global scope.
 */
static uint32_t scope_append(const char *scope, ScopePart *parts, uint32_t count) {
    static const size_t anon_len = sizeof(CTX_ANONYMOUS_SCOPE) - 1;
    const char *p = scope ? scope : "";
    while (*p) {
        const char *sep = strstr(p, "::");
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        bool anonymous = len == anon_len && !memcmp(p, CTX_ANONYMOUS_SCOPE, anon_len);
        if (len && !anonymous) {
            if (count == CTX_SCOPE_MAX_PARTS) return CTX_SCOPE_OVERFLOW;
            parts[count++] = (ScopePart){ p, len };
        }
        if (!sep) break;
        p = sep + 2;
    }
    return count;
}

static uint32_t scope_split(const char *scope, ScopePart *parts) {
    return scope_append(scope, parts, 0);
}

static bool is_python_path(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && (!strcmp(dot, ".py") || !strcmp(dot, ".pyi"));
}

/*
 * Writes the module components of a Python file: its trailing directories
 * (at most CTX_MODULE_MAX_PARTS - 1) plus the module name; __init__ stands
 * for its package directory. Returns the component count.
 */
static uint32_t module_split(const char *path, ScopePart *parts) {
    const char *end = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!end || (slash && end < slash)) end = path + strlen(path);
    ScopePart rev[CTX_MODULE_MAX_PARTS];
    uint32_t count = 0;
    const char *stop = end;
    while (count < CTX_MODULE_MAX_PARTS && stop > path) {
        const char *start = stop;
        while (start > path && start[-1] != '/' && start[-1] != '\\') start--;
        size_t len = (size_t)(stop - start);
        bool init = count == 0 && len == 8 && !memcmp(start, "__init__", 8);
        if (len && !init) rev[count++] = (ScopePart){ start, len };
        stop = start > path ? start - 1 : path;
    }
    for (uint32_t i = 0; i < count; i++) parts[i] = rev[count - 1 - i];
    return count;
}

/* Effective scope of a symbol: Python module components, then its scope. */
static uint32_t symbol_scope(const CtxSymbol *s, ScopePart *parts) {
    uint32_t count = s->lang == CTX_LANG_PYTHON ? module_split(s->file, parts) : 0;
    return scope_append(s->scope, parts, count);
}

/* Effective path of a class: its effective scope plus its own name. */
static uint32_t class_path(const CtxSymbol *cls, ScopePart *parts) {
    uint32_t count = symbol_scope(cls, parts);
    if (count == CTX_SCOPE_OVERFLOW || count == CTX_SCOPE_MAX_PARTS) return CTX_SCOPE_OVERFLOW;
    parts[count++] = (ScopePart){ cls->name, strlen(cls->name) };
    return count;
}

/* Fills a path from text ("::" or "." separated); false (empty path) when it does not fit. */
static bool scope_path_set(ScopePath *path, const char *text) {
    size_t out = 0;
    bool fits = true;
    for (const char *p = text ? text : ""; *p; p++) {
        size_t need = *p == '.' ? 2 : 1;
        if (out + need >= sizeof(path->text)) { fits = false; break; }
        if (*p == '.') { path->text[out++] = ':'; path->text[out++] = ':'; }
        else path->text[out++] = *p;
    }
    path->text[out] = '\0';
    path->count = scope_split(path->text, path->parts);
    if (!fits || path->count == CTX_SCOPE_OVERFLOW) {
        path->text[0] = '\0';
        path->count = 0;
        return false;
    }
    return true;
}

/* Fills a path with outer::inner. */
static bool scope_path_join(ScopePath *path, const char *outer, const char *inner) {
    char joined[sizeof(path->text)];
    int n = (outer && outer[0] && inner && inner[0])
        ? snprintf(joined, sizeof(joined), "%s::%s", outer, inner)
        : snprintf(joined, sizeof(joined), "%s", outer && outer[0] ? outer : (inner ? inner : ""));
    if (n < 0 || (size_t)n >= sizeof(joined)) {
        path->text[0] = '\0';
        path->count = 0;
        return false;
    }
    return scope_path_set(path, joined);
}

/* Fills a path with the effective scope of a symbol (path->text unused). */
static void scope_path_of_symbol(ScopePath *path, const CtxSymbol *s) {
    path->text[0] = '\0';
    path->count = symbol_scope(s, path->parts);
    if (path->count == CTX_SCOPE_OVERFLOW) path->count = 0;
}

/* Copies the first count components of path into the text of out. */
static bool scope_path_prefix(ScopePath *out, const ScopePart *parts, uint32_t count) {
    size_t len = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (len + parts[i].len + 3 >= sizeof(out->text)) return false;
        if (i) { out->text[len++] = ':'; out->text[len++] = ':'; }
        memcpy(out->text + len, parts[i].ptr, parts[i].len);
        len += parts[i].len;
    }
    out->text[len] = '\0';
    out->count = scope_split(out->text, out->parts);
    return out->count != CTX_SCOPE_OVERFLOW;
}

static bool scope_part_eq(const ScopePart *a, const ScopePart *b) {
    return a->len == b->len && !memcmp(a->ptr, b->ptr, a->len);
}

static void scope_part_copy(const ScopePart *part, char *out, size_t out_size) {
    size_t len = part->len < out_size ? part->len : out_size - 1;
    memcpy(out, part->ptr, len);
    out[len] = '\0';
}

/* True when the first outer_n components of outer lexically enclose inner. */
static bool scope_encloses(const ScopePart *outer, uint32_t outer_n,
                           const ScopePart *inner, uint32_t inner_n) {
    if (outer_n > inner_n) return false;
    for (uint32_t i = 0; i < outer_n; i++)
        if (!scope_part_eq(&outer[i], &inner[i])) return false;
    return true;
}

/* True when tail equals the trailing components of parts. */
static bool scope_ends_with(const ScopePart *parts, uint32_t parts_n,
                            const ScopePart *tail, uint32_t tail_n) {
    if (tail_n > parts_n) return false;
    for (uint32_t i = 0; i < tail_n; i++)
        if (!scope_part_eq(&parts[parts_n - tail_n + i], &tail[i])) return false;
    return true;
}

static bool scope_equal(const ScopePart *a, uint32_t a_n, const ScopePart *b, uint32_t b_n) {
    return a_n == b_n && scope_encloses(a, a_n, b, b_n);
}

bool ctx_symbol_in_scope(const CtxSymbol *s, const char *qualifier) {
    ScopePart parts[CTX_SCOPE_MAX_PARTS];
    ScopePath tail;
    if (!s || !scope_path_set(&tail, qualifier)) return false;
    if (tail.count == 0) return true;
    uint32_t parts_n = symbol_scope(s, parts);
    return parts_n != CTX_SCOPE_OVERFLOW && scope_ends_with(parts, parts_n, tail.parts, tail.count);
}

/* ---- candidate filters ------------------------------------------------------- */

/* Length of the directory part of path (up to, excluding, the last separator). */
static size_t dir_len(const char *path) {
    const char *base = path_basename(path);
    return base == path ? 0 : (size_t)(base - path) - 1;
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

/* Macros, static functions and anonymous-namespace members defined in a C/C++
 * source file are visible only inside that translation unit. */
static bool is_translation_unit_local(const CtxSymbol *s) {
    if ((s->lang != CTX_LANG_C && s->lang != CTX_LANG_CPP) || is_c_family_header(s->file))
        return false;
    return s->kind == CTX_SYM_MACRO || !strncmp(s->signature, "static ", 7) ||
           !strncmp(s->signature, "static\n", 7) || strstr(s->scope, CTX_ANONYMOUS_SCOPE);
}

/* Whether a symbol can be the target of an edge of the given kind. */
static bool kind_accepts(CtxEdgeKind edge, const CtxSymbol *s) {
    CtxSymbolKind k = s->kind;
    switch (edge) {
    case CTX_EDGE_CALLS:
        return k == CTX_SYM_FUNCTION || k == CTX_SYM_METHOD || k == CTX_SYM_MACRO ||
               k == CTX_SYM_CLASS || (k == CTX_SYM_STRUCT && s->lang == CTX_LANG_CPP);
    case CTX_EDGE_INHERITS:
        return k == CTX_SYM_CLASS || k == CTX_SYM_STRUCT || k == CTX_SYM_TYPEDEF;
    default:
        return k != CTX_SYM_INCLUDE && k != CTX_SYM_UNKNOWN;
    }
}

static bool is_class_kind(CtxSymbolKind k) {
    return k == CTX_SYM_CLASS || k == CTX_SYM_STRUCT;
}

static bool is_callable_kind(CtxSymbolKind k) {
    return k == CTX_SYM_FUNCTION || k == CTX_SYM_METHOD;
}

/* True when cand (an effective scope) is exactly the path of class cls. */
static bool scope_is_class(const ScopePart *cand, uint32_t cand_n, const CtxSymbol *cls) {
    ScopePart path[CTX_SCOPE_MAX_PARTS];
    uint32_t path_n = class_path(cls, path);
    return path_n != CTX_SCOPE_OVERFLOW && scope_equal(cand, cand_n, path, path_n);
}

/*
 * Finds the class/struct whose effective path is exactly the given scope (the
 * class a method or field belongs to). Definitions win, then lowest id.
 */
static const CtxSymbol *class_at_path(CtxGraph *g, const ScopePart *path, uint32_t path_n) {
    if (path_n == 0) return NULL;
    char name[256];
    scope_part_copy(&path[path_n - 1], name, sizeof(name));
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, name, n);
    const CtxSymbol *best = NULL;
    for (uint32_t i = 0; n && i < n->count; i++) {
        const CtxSymbol *s = n->symbols[i];
        if (!is_class_kind(s->kind) || !scope_is_class(path, path_n, s)) continue;
        if (!best || (s->is_definition && !best->is_definition) ||
            (s->is_definition == best->is_definition && s->id < best->id))
            best = s;
    }
    return best;
}

/*
 * True when a candidate is a member of a class-like type: a method, or a
 * symbol whose innermost scope component names an indexed class/struct
 * (out-of-line C++ definitions such as Foo::bar).
 */
static bool is_type_member(CtxGraph *g, const CtxSymbol *s, const ScopePart *cand, uint32_t cand_n) {
    if (s->kind == CTX_SYM_METHOD) return true;
    if (cand_n == 0) return false;
    char name[256];
    scope_part_copy(&cand[cand_n - 1], name, sizeof(name));
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, name, n);
    for (uint32_t i = 0; n && i < n->count; i++)
        if (is_class_kind(n->symbols[i]->kind)) return true;
    return false;
}

#define CTX_CLASS_CHAIN_MAX 16u

/*
 * Collects a class followed by its transitive bases (breadth-first, no
 * duplicates). Returns the number of classes written to chain.
 */
static uint32_t class_chain(CtxGraph *g, const CtxSymbol *cls, const CtxSymbol **chain) {
    if (!cls) return 0;
    uint32_t count = 0;
    chain[count++] = cls;
    for (uint32_t i = 0; i < count && count < CTX_CLASS_CHAIN_MAX; i++) {
        CtxBaseEntry *b = NULL;
        HASH_FIND(hh, g->bases, &chain[i]->id, sizeof(uint64_t), b);
        for (uint32_t j = 0; b && j < b->count && count < CTX_CLASS_CHAIN_MAX; j++) {
            CtxSymbol *base = NULL;
            HASH_FIND(hh, g->symbols, &b->base_ids[j], sizeof(uint64_t), base);
            if (!base) continue;
            bool seen = false;
            for (uint32_t k = 0; k < count && !seen; k++) seen = chain[k] == base;
            if (!seen) chain[count++] = base;
        }
    }
    return count;
}

/* ---- includes ------------------------------------------------------------------ */

/* Strips quotes/brackets from an include symbol name ("a/b.h", <a/b.h>). */
static bool include_path(const char *raw, char *out, size_t out_size) {
    size_t len = strlen(raw);
    if (len >= 2 && (raw[0] == '"' || raw[0] == '<')) { raw++; len -= 2; }
    if (len == 0 || len >= out_size) return false;
    memcpy(out, raw, len);
    out[len] = '\0';
    return true;
}

/* True when path names the included file: equal, or ends with "/include". */
static bool path_matches_include(const char *path, const char *inc) {
    size_t plen = strlen(path), ilen = strlen(inc);
    if (plen < ilen || strcmp(path + plen - ilen, inc) != 0) return false;
    return plen == ilen || path[plen - ilen - 1] == '/' || path[plen - ilen - 1] == '\\';
}

#define CTX_DIRECT_INCLUDES_MAX 128u

/*
 * Returns the files a C/C++ file directly #includes, rebuilding the cached
 * list when the file set changed since it was built.
 */
static void file_direct_includes(CtxGraph *g, CtxGraphFile *f) {
    if (f->include_epoch == g->include_epoch) return;
    CtxGraphFile *found[CTX_DIRECT_INCLUDES_MAX];
    uint32_t count = 0;
    for (uint32_t i = 0; i < f->symbol_count && count < CTX_DIRECT_INCLUDES_MAX; i++) {
        const CtxSymbol *s = f->symbols[i];
        char inc[512];
        if (s->kind != CTX_SYM_INCLUDE || (s->lang != CTX_LANG_C && s->lang != CTX_LANG_CPP) ||
            !include_path(s->name, inc, sizeof(inc)))
            continue;
        CtxBasenameEntry *e = NULL;
        HASH_FIND_STR(g->basenames, path_basename(inc), e);
        for (uint32_t j = 0; e && j < e->count && count < CTX_DIRECT_INCLUDES_MAX; j++) {
            CtxGraphFile *h = e->files[j];
            if (h == f || !path_matches_include(h->path, inc)) continue;
            bool seen = false;
            for (uint32_t k = 0; k < count && !seen; k++) seen = found[k] == h;
            if (!seen) found[count++] = h;
        }
    }
    CtxGraphFile **list = count ? (CtxGraphFile **)malloc(count * sizeof(CtxGraphFile *)) : NULL;
    if (count && !list) count = 0;
    if (count) memcpy(list, found, count * sizeof(CtxGraphFile *));
    free(f->includes);
    f->includes = list;
    f->include_count = count;
    f->include_epoch = g->include_epoch;
}

/* ---- per-file lookup context ---------------------------------------------- */

#define CTX_INCLUDE_CLOSURE_MAX 256u
#define CTX_EVAL_DEPTH 3

typedef struct SiteLookup SiteLookup;

#define CTX_STEM_MAX 64u

/*
 * Lookup state shared by every site of one file: the transitive include
 * closure (sorted path pointers, for reachability), the sorted stems of
 * included headers (foo.h makes foo.c reachable), the file's own alias and
 * using declarations, the using-directives of included files, a cache of the
 * last caller's class chain and scratch lookups for nested evaluation.
 * file is temporarily switched while evaluating declarations of other files;
 * home is the file the lookup was built for.
 */
typedef struct {
    CtxGraphFile       *file;
    CtxGraphFile       *home;
    const CtxLookupDecl **aliases;        /* home's alias/using declarations */
    uint32_t            alias_count;
    const CtxLookupDecl **closure_usings; /* using-directives of included files */
    const CtxGraphFile **closure_using_files;
    uint32_t            closure_using_count;
    char              (*stems)[CTX_STEM_MAX];
    uint32_t            stem_count;
    const CtxGraphFile *closure[CTX_INCLUDE_CLOSURE_MAX];
    const char         *closure_paths[CTX_INCLUDE_CLOSURE_MAX];
    uint32_t            closure_count;
    uint64_t            self_src;
    const CtxSymbol    *self_chain[CTX_CLASS_CHAIN_MAX];
    uint32_t            self_count;
    SiteLookup         *scratch;  /* CTX_EVAL_DEPTH + 1 lookups */
} FileLookup;

static int ptr_order(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)*(const void *const *)a, y = (uintptr_t)*(const void *const *)b;
    return x < y ? -1 : x > y;
}

static void file_lookup_free(FileLookup *fl) {
    free(fl->scratch);
    free((void *)fl->aliases);
    free((void *)fl->closure_usings);
    free((void *)fl->closure_using_files);
    free(fl->stems);
    memset(fl, 0, sizeof(*fl));
}

static int stem_order(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* True for declarations that rename or import names (consulted per site). */
static bool is_alias_decl(const CtxLookupDecl *d) {
    return d->kind == CTX_DECL_USING_NAMESPACE || d->kind == CTX_DECL_USING ||
           d->kind == CTX_DECL_NAMESPACE_ALIAS || d->kind == CTX_DECL_TYPE_ALIAS;
}

/* True when path (an interned file path) is in the include closure. */
static bool closure_has_path(const FileLookup *fl, const char *path) {
    return fl->closure_count &&
           bsearch(&path, fl->closure_paths, fl->closure_count, sizeof(const char *), ptr_order) != NULL;
}

/* Writes the basename of path without extension; false when it does not fit. */
static bool path_stem(const char *path, char *out, size_t out_size) {
    const char *base = path_basename(path);
    size_t len = strcspn(base, ".");
    if (len == 0 || len >= out_size) return false;
    memcpy(out, base, len);
    out[len] = '\0';
    return true;
}

/* True when a file's basename stem equals that of some included header. */
static bool closure_has_stem(const FileLookup *fl, const char *path) {
    char stem[CTX_STEM_MAX];
    return fl->stem_count && path_stem(path, stem, sizeof(stem)) &&
           bsearch(stem, fl->stems, fl->stem_count, CTX_STEM_MAX, stem_order) != NULL;
}

/* Number of SiteLookup scratch slots a FileLookup owns. */
#define CTX_SCRATCH_COUNT (CTX_EVAL_DEPTH + 1)

static bool file_lookup_init(CtxGraph *g, CtxGraphFile *f, FileLookup *fl);

/* ---- declarations ------------------------------------------------------------- */

/*
 * True when a declaration of file df applies at a site. Same-file
 * declarations need the site inside their line range. Python declarations
 * (imports) never leave their module; C/C++ using-declarations/directives
 * apply to files that include df; aliases apply graph-wide. The declaration
 * scope must enclose the caller's own scope.
 *
 * raw  Caller scope without Python module components.
 */
static bool decl_visible(const FileLookup *fl, const CtxGraphFile *df, const CtxLookupDecl *d,
                         const ScopePath *raw, uint32_t line) {
    if (df == fl->file) {
        if (line < d->line || line > d->end_line) return false;
    } else {
        if (d->local || is_python_path(df->path)) return false;
        bool directive = d->kind == CTX_DECL_USING || d->kind == CTX_DECL_USING_NAMESPACE;
        if (directive && !closure_has_path(fl, df->path)) return false;
    }
    ScopePart parts[CTX_SCOPE_MAX_PARTS];
    uint32_t n = scope_split(d->scope, parts);
    return n != CTX_SCOPE_OVERFLOW && scope_encloses(parts, n, raw->parts, raw->count);
}

#define DECL_BIT(k) (1u << (k))

/*
 * Finds the visible declaration introducing name: same-file first (local,
 * then innermost scope, then latest line), else graph-wide (innermost scope,
 * reachable through includes). Returns NULL when none.
 *
 * mask  DECL_BIT set of accepted declaration kinds.
 * raw   Caller scope without Python module components.
 * line  Line of the lookup in fl->file.
 */
static const CtxLookupDecl *find_decl(CtxGraph *g, const FileLookup *fl, const char *name,
                                      uint32_t mask, const ScopePath *raw, uint32_t line) {
    const CtxLookupDecl *best = NULL;
    int best_score = INT_MIN;
    uint32_t best_line = 0;
    bool home = fl->file == fl->home;
    uint32_t own_count = home ? fl->alias_count : fl->file->decl_count;
    for (uint32_t i = 0; i < own_count; i++) {
        const CtxLookupDecl *d = home ? fl->aliases[i] : &fl->file->decls[i];
        if (!(mask & DECL_BIT(d->kind)) || !d->name || strcmp(d->name, name) != 0) continue;
        if (!decl_visible(fl, fl->file, d, raw, line)) continue;
        ScopePart parts[CTX_SCOPE_MAX_PARTS];
        int score = 10 * (int)scope_split(d->scope, parts) + (d->local ? 500 : 0);
        if (score > best_score || (score == best_score && d->line > best_line)) {
            best = d;
            best_score = score;
            best_line = d->line;
        }
    }
    if (best) return best;

    CtxDeclEntry *e = NULL;
    HASH_FIND_STR(g->decls, name, e);
    for (uint32_t i = 0; e && i < e->count; i++) {
        const CtxLookupDecl *d = e->decls[i];
        const CtxGraphFile *df = e->files[i];
        if (df == fl->file || !(mask & DECL_BIT(d->kind))) continue;
        if (!decl_visible(fl, df, d, raw, line)) continue;
        ScopePart parts[CTX_SCOPE_MAX_PARTS];
        int score = 10 * (int)scope_split(d->scope, parts) + (closure_has_path(fl, df->path) ? 5 : 0);
        if (score > best_score || (score == best_score && best && strcmp(d->target, best->target) < 0)) {
            best = d;
            best_score = score;
        }
    }
    return best;
}

#define CTX_ALIAS_DEPTH 4

/*
 * Rewrites a path whose first component is a visible alias (namespace alias,
 * type alias/typedef, using-declaration, Python import) into its target,
 * following chains. Returns true when the first component was a declared
 * name (even an identity import such as `import os`).
 */
static bool expand_aliases(CtxGraph *g, const FileLookup *fl, ScopePath *path,
                           const ScopePath *raw, uint32_t line) {
    bool anchored = false;
    uint32_t mask = DECL_BIT(CTX_DECL_NAMESPACE_ALIAS) | DECL_BIT(CTX_DECL_TYPE_ALIAS) |
                    DECL_BIT(CTX_DECL_USING);
    for (int depth = 0; depth < CTX_ALIAS_DEPTH && path->count > 0; depth++) {
        char head[256];
        scope_part_copy(&path->parts[0], head, sizeof(head));
        const CtxLookupDecl *d = find_decl(g, fl, head, mask, raw, line);
        if (!d) break;
        anchored = true;
        ScopePath target;
        if (!scope_path_set(&target, d->target) || target.count == 0) break;
        if (target.count == 1 && scope_part_eq(&target.parts[0], &path->parts[0])) break;
        char rest[256];
        snprintf(rest, sizeof(rest), "%s", path->count > 1 ? path->parts[1].ptr : "");
        ScopePath next;
        if (!scope_path_join(&next, target.text, rest) || !strcmp(next.text, path->text)) break;
        scope_path_set(path, next.text);
    }
    return anchored;
}

/* ---- type evaluation ------------------------------------------------------------ */

/* Caller context for evaluating types and looking names up. */
typedef struct {
    const CtxSymbol *src;   /* symbol the lookup happens in */
    ScopePath        from;  /* its effective scope */
    ScopePath        raw;   /* its scope without Python module components */
    uint32_t         line;
} Caller;

static void caller_init(Caller *c, const CtxSymbol *src, uint32_t line) {
    c->src = src;
    c->line = line;
    scope_path_of_symbol(&c->from, src);
    if (!scope_path_set(&c->raw, src->scope)) c->raw.count = 0;
}

#define CTX_LOOKUP_NS_MAX 8u

/* Everything known about where and how a site looks its target up. */
struct SiteLookup {
    Caller           caller;
    ScopePath        qual;          /* explicit (alias-expanded) qualifier */
    char             name[256];     /* unqualified target name */
    const CtxSymbol *qual_chain[CTX_CLASS_CHAIN_MAX]; /* qualifier class and its bases */
    uint32_t         qual_chain_count;
    const CtxSymbol *recv_chain[CTX_CLASS_CHAIN_MAX]; /* receiver class and its bases */
    uint32_t         recv_chain_count;
    const CtxSymbol **self;         /* caller class and its bases */
    uint32_t         self_count;
    ScopePath        usings[CTX_LOOKUP_NS_MAX];  /* using-directive / star-import namespaces */
    uint32_t         using_count;
    ScopePath        adl[CTX_LOOKUP_NS_MAX];     /* argument-dependent namespaces/classes */
    uint32_t         adl_count;
    const char      *from_file;
    bool             member;
    bool             strict;        /* reject candidates outside the qualifier */
    bool             visible_only;  /* reject candidates no lookup rule reaches */
    bool             include_bias;  /* favour files reachable through includes */
};

/*
 * Resolves a written type path ("ns::Foo", "pkg.mod.Foo", an alias, a
 * typedef) to the class/struct it denotes, as seen from a scope. Returns NULL
 * when the type is not an indexed class.
 *
 * from  Effective scope the type is written in.
 * raw   Same scope without module components (for declaration lookup).
 */
static const CtxSymbol *resolve_type(CtxGraph *g, const FileLookup *fl, const char *written,
                                     const ScopePath *from, const ScopePath *raw, uint32_t line) {
    ScopePath path;
    if (!written || !written[0] || !scope_path_set(&path, written) || path.count == 0) return NULL;
    expand_aliases(g, fl, &path, raw, line);
    if (path.count == 0) return NULL;

    char name[256];
    scope_part_copy(&path.parts[path.count - 1], name, sizeof(name));
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, name, n);

    const CtxSymbol *best = NULL;
    int best_score = INT_MIN;
    uint32_t qual_n = path.count - 1;
    for (uint32_t i = 0; n && i < n->count; i++) {
        const CtxSymbol *s = n->symbols[i];
        if (!is_class_kind(s->kind)) continue;
        if (s->file != fl->file->path && is_translation_unit_local(s)) continue;
        ScopePart cand[CTX_SCOPE_MAX_PARTS];
        uint32_t cand_n = symbol_scope(s, cand);
        if (cand_n == CTX_SCOPE_OVERFLOW || !scope_ends_with(cand, cand_n, path.parts, qual_n)) continue;
        uint32_t outer_n = cand_n - qual_n;
        int score = scope_encloses(cand, outer_n, from->parts, from->count) ? 200 + 10 * (int)outer_n : 0;
        if (s->file == fl->file->path) score += 100;
        else if (closure_has_path(fl, s->file)) score += 60;
        score += s->is_definition ? 20 : 0;
        if (score > best_score || (score == best_score && best && s->id < best->id)) {
            best = s;
            best_score = score;
        }
    }
    return best;
}

/* Class chain of the class the caller is a member of (implicit this). */
static uint32_t self_chain(CtxGraph *g, FileLookup *fl, const Caller *c, const CtxSymbol ***chain) {
    if (fl->self_src != c->src->id) {
        fl->self_src = c->src->id;
        fl->self_count = class_chain(g, class_at_path(g, c->from.parts, c->from.count), fl->self_chain);
    }
    *chain = fl->self_chain;
    return fl->self_count;
}

static const CtxSymbol *eval_type(CtxGraph *g, FileLookup *fl, const Caller *c, const char *expr,
                                  const ScopePath *from, const ScopePath *raw, int depth);

/*
 * Evaluates a type expression recorded in another declaration (a field or
 * a return type) in the file and scope it was written in: lookups such as
 * Python imports then use that file's declarations.
 *
 * context  Symbol the expression belongs to (owning class or function).
 * line     Line of the declaration, for import/alias visibility.
 */
static const CtxSymbol *eval_in_context(CtxGraph *g, FileLookup *fl, const CtxSymbol *context,
                                        const char *expr, const ScopePath *from, const ScopePath *raw,
                                        uint32_t line, int depth) {
    CtxGraphFile *own = fl->file;
    CtxGraphFile *file = own;
    if (context->file != own->path) {
        HASH_FIND_STR(g->files, context->file, file);
        if (!file) return NULL;
    }
    Caller c;
    caller_init(&c, context, line);
    fl->file = file;
    const CtxSymbol *result = eval_type(g, fl, &c, expr, from, raw, depth);
    fl->file = own;
    return result;
}

/*
 * Type of a data member of cls or its bases: the FIELD declaration recorded
 * in the class's own file, evaluated in the owning class's scope.
 */
static const CtxSymbol *field_type(CtxGraph *g, FileLookup *fl, const CtxSymbol *cls,
                                   const char *field, int depth) {
    const CtxSymbol *chain[CTX_CLASS_CHAIN_MAX];
    uint32_t count = class_chain(g, cls, chain);
    CtxDeclEntry *e = NULL;
    HASH_FIND_STR(g->decls, field, e);
    for (uint32_t k = 0; e && k < count; k++) {
        ScopePath owner;
        if (!scope_path_join(&owner, chain[k]->scope, chain[k]->name)) continue;
        for (uint32_t i = 0; i < e->count; i++) {
            const CtxLookupDecl *d = e->decls[i];
            if (d->kind != CTX_DECL_FIELD || e->files[i]->path != chain[k]->file ||
                strcmp(d->scope, owner.text) != 0)
                continue;
            ScopePath from;
            from.text[0] = '\0';
            from.count = class_path(chain[k], from.parts);
            if (from.count == CTX_SCOPE_OVERFLOW) from.count = 0;
            return eval_in_context(g, fl, chain[k], d->target, &from, &owner, d->line, depth + 1);
        }
    }
    return NULL;
}

/* Declared or inferred return type of a function/method (RETURN declarations). */
static const CtxSymbol *return_type(CtxGraph *g, FileLookup *fl, const CtxSymbol *fn, int depth) {
    CtxDeclEntry *e = NULL;
    HASH_FIND_STR(g->decls, fn->name, e);
    const CtxLookupDecl *best = NULL;
    for (uint32_t i = 0; e && i < e->count; i++) {
        const CtxLookupDecl *d = e->decls[i];
        if (d->kind != CTX_DECL_RETURN || strcmp(d->scope, fn->scope) != 0) continue;
        bool same_file = e->files[i]->path == fn->file;
        if (is_python_path(fn->file) && !same_file) continue;
        if (!best || same_file) best = d;
        if (same_file) break;
    }
    if (!best) return NULL;
    ScopePath from, raw;
    scope_path_of_symbol(&from, fn);
    if (!scope_path_set(&raw, fn->scope)) raw.count = 0;
    return eval_in_context(g, fl, fn, best->target, &from, &raw, best->line, depth + 1);
}

/* First method named name declared on cls or its bases. */
static const CtxSymbol *find_method(CtxGraph *g, const CtxSymbol *cls, const char *name) {
    const CtxSymbol *chain[CTX_CLASS_CHAIN_MAX];
    uint32_t count = class_chain(g, cls, chain);
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, name, n);
    for (uint32_t k = 0; n && k < count; k++) {
        for (uint32_t i = 0; i < n->count; i++) {
            const CtxSymbol *m = n->symbols[i];
            ScopePart cand[CTX_SCOPE_MAX_PARTS];
            uint32_t cand_n = symbol_scope(m, cand);
            if (is_callable_kind(m->kind) && cand_n != CTX_SCOPE_OVERFLOW &&
                scope_is_class(cand, cand_n, chain[k]))
                return m;
        }
    }
    return NULL;
}

static bool site_lookup_init(CtxGraph *g, FileLookup *fl, const CtxSymbol *src, const CtxRefSite *site,
                             bool expand, int depth, SiteLookup *lk);
static const CtxSymbol *pick_target_in(CtxGraph *g, const FileLookup *fl, const SiteLookup *lk,
                                       CtxEdgeKind kind);

/* Symbol a call of path resolves to from the caller (constructor or function). */
static const CtxSymbol *callee_of(CtxGraph *g, FileLookup *fl, const Caller *c, const char *path,
                                  int depth) {
    if (depth >= CTX_EVAL_DEPTH) return NULL;
    ScopePath written;
    if (!scope_path_set(&written, path) || written.count == 0) return NULL;
    char name[256], scope[256] = {0};
    scope_part_copy(&written.parts[written.count - 1], name, sizeof(name));
    if (written.count > 1) {
        ScopePath qual;
        if (scope_path_prefix(&qual, written.parts, written.count - 1))
            snprintf(scope, sizeof(scope), "%s", qual.text);
    }
    CtxRefSite site = { .to_name = name, .to_scope = scope[0] ? scope : NULL,
                        .from_line = c->line, .kind = CTX_EDGE_CALLS };
    SiteLookup *lk = &fl->scratch[depth + 1];
    const CtxSymbol *dst = NULL;
    if (site_lookup_init(g, fl, c->src, &site, true, depth + 1, lk))
        dst = pick_target_in(g, fl, lk, CTX_EDGE_CALLS);
    if (!dst && site_lookup_init(g, fl, c->src, &site, false, depth + 1, lk))
        dst = pick_target_in(g, fl, lk, CTX_EDGE_CALLS);
    return dst;
}

/*
 * Evaluates a type expression (see graph.h) to the class it denotes.
 * Returns NULL when any step is unknown or the depth limit is reached.
 *
 * from  Effective scope the expression was written in.
 * raw   Same scope without module components.
 */
static const CtxSymbol *eval_type(CtxGraph *g, FileLookup *fl, const Caller *c, const char *expr,
                                  const ScopePath *from, const ScopePath *raw, int depth) {
    if (!expr || !expr[0] || depth > CTX_EVAL_DEPTH) return NULL;
    char head[256];
    const char *steps = strchr(expr, CTX_TYPE_STEP_SEP);
    size_t head_len = steps ? (size_t)(steps - expr) : strlen(expr);
    if (head_len == 0 || head_len >= sizeof(head)) return NULL;
    memcpy(head, expr + 1, head_len - 1);
    head[head_len - 1] = '\0';

    const CtxSymbol *cur = NULL;
    switch (expr[0]) {
    case 'T':
        cur = resolve_type(g, fl, head, from, raw, c->line);
        break;
    case 'S':
        cur = class_at_path(g, from->parts, from->count);
        break;
    case 'B': {
        const CtxSymbol *chain[CTX_CLASS_CHAIN_MAX];
        cur = class_chain(g, class_at_path(g, from->parts, from->count), chain) > 1 ? chain[1] : NULL;
        break;
    }
    case 'V':
        cur = field_type(g, fl, class_at_path(g, from->parts, from->count), head, depth);
        break;
    case 'C': {
        const CtxSymbol *callee = callee_of(g, fl, c, head, depth);
        if (callee && is_class_kind(callee->kind)) cur = callee;
        else if (callee && is_callable_kind(callee->kind)) cur = return_type(g, fl, callee, depth);
        break;
    }
    default:
        return NULL;
    }

    while (cur && steps) {
        const char *step = steps + 1;
        steps = strchr(step, CTX_TYPE_STEP_SEP);
        size_t len = steps ? (size_t)(steps - step) : strlen(step);
        char name[256];
        if (len < 2 || len >= sizeof(name)) return NULL;
        memcpy(name, step + 1, len - 1);
        name[len - 1] = '\0';
        if (step[0] == 'f') {
            cur = field_type(g, fl, cur, name, depth);
        } else if (step[0] == 'm') {
            const CtxSymbol *m = find_method(g, cur, name);
            cur = m ? return_type(g, fl, m, depth) : NULL;
        } else {
            return NULL;
        }
    }
    return cur;
}

/* ---- site resolution ---------------------------------------------------------- */

static bool file_lookup_init(CtxGraph *g, CtxGraphFile *f, FileLookup *fl) {
    memset(fl, 0, sizeof(*fl));
    fl->file = f;
    fl->home = f;
    fl->scratch = (SiteLookup *)malloc(CTX_SCRATCH_COUNT * sizeof(SiteLookup));
    if (!fl->scratch) return false;

    file_direct_includes(g, f);
    for (uint32_t i = 0; i < f->include_count && fl->closure_count < CTX_INCLUDE_CLOSURE_MAX; i++)
        fl->closure[fl->closure_count++] = f->includes[i];
    for (uint32_t q = 0; q < fl->closure_count; q++) {
        CtxGraphFile *h = (CtxGraphFile *)fl->closure[q];
        file_direct_includes(g, h);
        for (uint32_t i = 0; i < h->include_count && fl->closure_count < CTX_INCLUDE_CLOSURE_MAX; i++) {
            const CtxGraphFile *next = h->includes[i];
            bool seen = next == f;
            for (uint32_t k = 0; k < fl->closure_count && !seen; k++) seen = fl->closure[k] == next;
            if (!seen) fl->closure[fl->closure_count++] = next;
        }
    }
    for (uint32_t i = 0; i < fl->closure_count; i++) fl->closure_paths[i] = fl->closure[i]->path;
    if (fl->closure_count > 1)
        qsort(fl->closure_paths, fl->closure_count, sizeof(const char *), ptr_order);

    uint32_t own_aliases = 0, closure_usings = 0;
    for (uint32_t i = 0; i < f->decl_count; i++) own_aliases += is_alias_decl(&f->decls[i]);
    for (uint32_t c = 0; c < fl->closure_count; c++)
        for (uint32_t i = 0; i < fl->closure[c]->decl_count; i++)
            closure_usings += fl->closure[c]->decls[i].kind == CTX_DECL_USING_NAMESPACE;
    fl->aliases = own_aliases ? (const CtxLookupDecl **)malloc(own_aliases * sizeof(*fl->aliases)) : NULL;
    fl->closure_usings = closure_usings
        ? (const CtxLookupDecl **)malloc(closure_usings * sizeof(*fl->closure_usings)) : NULL;
    fl->closure_using_files = closure_usings
        ? (const CtxGraphFile **)malloc(closure_usings * sizeof(*fl->closure_using_files)) : NULL;
    fl->stems = fl->closure_count ? malloc(fl->closure_count * CTX_STEM_MAX) : NULL;
    if ((own_aliases && !fl->aliases) || (closure_usings && (!fl->closure_usings || !fl->closure_using_files)) ||
        (fl->closure_count && !fl->stems)) {
        file_lookup_free(fl);
        return false;
    }
    for (uint32_t i = 0; i < f->decl_count; i++)
        if (is_alias_decl(&f->decls[i])) fl->aliases[fl->alias_count++] = &f->decls[i];
    for (uint32_t c = 0; c < fl->closure_count; c++) {
        const CtxGraphFile *h = fl->closure[c];
        for (uint32_t i = 0; i < h->decl_count; i++) {
            if (h->decls[i].kind != CTX_DECL_USING_NAMESPACE) continue;
            fl->closure_usings[fl->closure_using_count] = &h->decls[i];
            fl->closure_using_files[fl->closure_using_count++] = h;
        }
        if (path_stem(h->path, fl->stems[fl->stem_count], CTX_STEM_MAX)) fl->stem_count++;
    }
    if (fl->stem_count > 1) qsort(fl->stems, fl->stem_count, CTX_STEM_MAX, stem_order);
    return true;
}

/* Adds the using-directive / star-import namespaces visible at a site. */
static void collect_usings(const FileLookup *fl, SiteLookup *lk) {
    lk->using_count = 0;
    if (fl->file != fl->home) return;
    uint32_t total = fl->alias_count + fl->closure_using_count;
    for (uint32_t i = 0; i < total && lk->using_count < CTX_LOOKUP_NS_MAX; i++) {
        bool own = i < fl->alias_count;
        const CtxLookupDecl *d = own ? fl->aliases[i] : fl->closure_usings[i - fl->alias_count];
        const CtxGraphFile *df = own ? fl->home : fl->closure_using_files[i - fl->alias_count];
        if (d->kind != CTX_DECL_USING_NAMESPACE || !decl_visible(fl, df, d, &lk->caller.raw, lk->caller.line))
            continue;
        if (scope_path_set(&lk->usings[lk->using_count], d->target)) lk->using_count++;
    }
}

/* Adds the namespaces and classes associated with the site's argument types. */
static void collect_adl(CtxGraph *g, FileLookup *fl, const CtxRefSite *site, int depth, SiteLookup *lk) {
    lk->adl_count = 0;
    if (!site->arg_types) return;
    const char *p = site->arg_types;
    while (*p && lk->adl_count + 1 < CTX_LOOKUP_NS_MAX) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char expr[256];
        if (len && len < sizeof(expr)) {
            memcpy(expr, p, len);
            expr[len] = '\0';
            const CtxSymbol *cls = eval_type(g, fl, &lk->caller, expr, &lk->caller.from,
                                             &lk->caller.raw, depth);
            if (cls) {
                ScopePath *ns = &lk->adl[lk->adl_count];
                ns->text[0] = '\0';
                ns->count = symbol_scope(cls, ns->parts);
                if (ns->count != CTX_SCOPE_OVERFLOW && ns->count) lk->adl_count++;
                ScopePath *own = &lk->adl[lk->adl_count];
                own->text[0] = '\0';
                own->count = class_path(cls, own->parts);
                if (own->count != CTX_SCOPE_OVERFLOW) lk->adl_count++;
            } else if (expr[0] == 'T') {
                ScopePath written;
                if (scope_path_set(&written, expr + 1) && written.count > 1 &&
                    scope_path_prefix(&lk->adl[lk->adl_count], written.parts, written.count - 1))
                    lk->adl_count++;
            }
        }
        if (!end) break;
        p = end + 1;
    }
}

/*
 * Prepares the lookup of one site. Returns false when the site cannot be
 * resolved at all (unparseable name).
 *
 * src     Symbol the site belongs to (caller).
 * expand  Apply aliases/using-declarations/imports to the written name.
 * depth   Nesting level of type evaluation (0 for real sites).
 */
static bool site_lookup_init(CtxGraph *g, FileLookup *fl, const CtxSymbol *src, const CtxRefSite *site,
                             bool expand, int depth, SiteLookup *lk) {
    caller_init(&lk->caller, src, site->from_line);
    lk->from_file = src->file;

    ScopePath written;
    if (!scope_path_join(&written, site->to_scope, site->to_name) || written.count == 0) return false;
    bool anchored = expand && expand_aliases(g, fl, &written, &lk->caller.raw, site->from_line);
    scope_part_copy(&written.parts[written.count - 1], lk->name, sizeof(lk->name));
    if (written.count > 1) {
        if (!scope_path_prefix(&lk->qual, written.parts, written.count - 1)) return false;
    } else {
        lk->qual.text[0] = '\0';
        lk->qual.count = 0;
    }

    uint8_t lang = src->lang;
    bool python = lang == CTX_LANG_PYTHON;
    lk->member = site->member && lk->qual.count == 0;
    lk->strict = lk->qual.count > 0 && (lang == CTX_LANG_CPP || (python && anchored));
    lk->visible_only = python && !lk->member && lk->qual.count == 0 && site->kind == CTX_EDGE_CALLS;
    lk->include_bias = lang == CTX_LANG_C || lang == CTX_LANG_CPP;
    lk->qual_chain_count = 0;
    lk->recv_chain_count = 0;
    lk->self = NULL;
    lk->self_count = 0;
    lk->using_count = 0;
    lk->adl_count = 0;
    if (lk->qual.count) {
        const CtxSymbol *cls = resolve_type(g, fl, lk->qual.text, &lk->caller.from, &lk->caller.raw,
                                            site->from_line);
        lk->qual_chain_count = class_chain(g, cls, lk->qual_chain);
    } else if (lk->member) {
        const CtxSymbol *cls = eval_type(g, fl, &lk->caller, site->recv, &lk->caller.from,
                                         &lk->caller.raw, depth);
        lk->recv_chain_count = class_chain(g, cls, lk->recv_chain);
    } else {
        if (lk->caller.from.count) lk->self_count = self_chain(g, fl, &lk->caller, &lk->self);
        collect_usings(fl, lk);
        collect_adl(g, fl, site, depth, lk);
    }
    return true;
}

/* Position of cand in a class chain, or -1. */
static int chain_index(const ScopePart *cand, uint32_t cand_n, const CtxSymbol *const *chain, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (scope_is_class(cand, cand_n, chain[i])) return (int)i;
    return -1;
}

/*
 * Scores how well a candidate's effective scope fits a site, approximating
 * the language's name lookup. Returns -1 when the candidate is excluded.
 *   qualified    qualifier names the candidate scope (or a base of the
 *                qualifying class); strict sites reject anything else.
 *   member       receiver class and its bases; a resolved receiver in C/C++
 *                excludes everything else, and C has no member functions.
 *                Unknown receivers prefer any type member.
 *   unqualified  enclosing scopes (innermost first), the caller's bases,
 *                using-directive/star-import namespaces, then ADL ones.
 */
static int scope_score(CtxGraph *g, const SiteLookup *lk, const CtxSymbol *s,
                       const ScopePart *cand, uint32_t cand_n) {
    const ScopePath *from = &lk->caller.from;
    if (lk->qual.count) {
        if (scope_ends_with(cand, cand_n, lk->qual.parts, lk->qual.count)) {
            uint32_t outer_n = cand_n - lk->qual.count;
            bool near = scope_encloses(cand, outer_n, from->parts, from->count);
            return 400 + (near ? 50 + 10 * (int)outer_n : 0);
        }
        int base = chain_index(cand, cand_n, lk->qual_chain, lk->qual_chain_count);
        if (base > 0) return 390 - base;
        return lk->strict ? -1 : 0;
    }

    bool enclosing = scope_encloses(cand, cand_n, from->parts, from->count);
    if (lk->member) {
        uint8_t lang = lk->caller.src->lang;
        if (lang == CTX_LANG_C) return -1;
        if (lk->recv_chain_count) {
            int at = chain_index(cand, cand_n, lk->recv_chain, lk->recv_chain_count);
            if (at >= 0) return 500 - at;
            return lang == CTX_LANG_CPP ? -1 : 0;
        }
        return is_type_member(g, s, cand, cand_n) ? 200 + (enclosing ? 10 * (int)cand_n : 0) : 0;
    }

    if (enclosing) return 200 + 10 * (int)cand_n;
    int base = chain_index(cand, cand_n, lk->self, lk->self_count);
    if (base > 0) return 200 + 10 * (int)from->count - base;
    for (uint32_t i = 0; i < lk->using_count; i++)
        if (scope_ends_with(cand, cand_n, lk->usings[i].parts, lk->usings[i].count)) return 190;
    for (uint32_t i = 0; i < lk->adl_count; i++)
        if (scope_equal(cand, cand_n, lk->adl[i].parts, lk->adl[i].count)) return 180;
    return lk->visible_only ? -1 : 0;
}

/* Locality: same file, then files reachable through includes, then same directory. */
static int locality_score(const SiteLookup *lk, const FileLookup *fl, const char *file) {
    if (file == lk->from_file) return 100;
    if (lk->include_bias && (closure_has_path(fl, file) || closure_has_stem(fl, file))) return 60;
    size_t from_dir = dir_len(lk->from_file);
    if (dir_len(file) == from_dir && !strncmp(file, lk->from_file, from_dir)) return 40;
    return 0;
}

/*
 * Picks the most plausible target for a prepared site. Scope fit (see
 * scope_score) outweighs locality, then definitions, then kind rank.
 * Candidates of the wrong kind or private to another translation unit are
 * skipped. Ties keep the lowest id so resolution is deterministic.
 */
static const CtxSymbol *pick_target_in(CtxGraph *g, const FileLookup *fl, const SiteLookup *lk,
                                       CtxEdgeKind kind) {
    CtxNameEntry *n = NULL;
    HASH_FIND_STR(g->names, lk->name, n);
    const CtxSymbol *best = NULL;
    int best_score = INT_MIN;
    for (uint32_t i = 0; n && i < n->count; i++) {
        const CtxSymbol *s = n->symbols[i];
        if (!kind_accepts(kind, s)) continue;
        if (s->file != lk->from_file && is_translation_unit_local(s)) continue;
        ScopePart cand[CTX_SCOPE_MAX_PARTS];
        uint32_t cand_n = symbol_scope(s, cand);
        if (cand_n == CTX_SCOPE_OVERFLOW) continue;
        int fit = scope_score(g, lk, s, cand, cand_n);
        if (fit < 0) continue;
        int score = fit + locality_score(lk, fl, s->file) + (s->is_definition ? 20 : 0) +
                    kind_resolution_rank(s->kind);
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
 * derived edge reference when the resolution changed. Alias/import expansion
 * is tried first; when it finds nothing the written name is looked up as is.
 *
 * fl    Lookup context of the file owning the site.
 * site  Site to resolve; res_from/res_to are updated in place.
 */
static void resolve_site(CtxGraph *g, FileLookup *fl, CtxRefSite *site) {
    uint64_t from_id = 0, to_id = 0;
    const CtxSymbol *src = find_site_source(g, fl->file, site);
    if (src) {
        SiteLookup *lk = &fl->scratch[0];
        const CtxSymbol *dst = NULL;
        if (site_lookup_init(g, fl, src, site, true, 0, lk)) dst = pick_target_in(g, fl, lk, site->kind);
        if (!dst && site_lookup_init(g, fl, src, site, false, 0, lk)) dst = pick_target_in(g, fl, lk, site->kind);
        if (dst && dst != src) {
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

/*
 * Resolves the sites of one file selected by filter (NULL = all). Callers run
 * the inheritance pass first so base-class member lookup sees fresh bases.
 */
typedef bool (*SiteFilter)(const CtxRefSite *site, const void *arg);

static void resolve_file_sites(CtxGraph *g, CtxGraphFile *f, bool inherits_pass,
                               SiteFilter filter, const void *arg) {
    FileLookup fl;
    bool ready = false;
    for (uint32_t i = 0; i < f->site_count; i++) {
        CtxRefSite *site = &f->sites[i];
        if ((site->kind == CTX_EDGE_INHERITS) != inherits_pass) continue;
        if (filter && !filter(site, arg)) continue;
        if (!ready) {
            if (!file_lookup_init(g, f, &fl)) return;
            ready = true;
        }
        resolve_site(g, &fl, site);
    }
    if (ready) file_lookup_free(&fl);
}

/* ---- file replacement ------------------------------------------------------- */

typedef struct NameMark {
    const char    *name;   /* borrowed from a live symbol, declaration or old-name copy */
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

/*
 * True when any name component of text is in the set. Components are
 * separated by "::", ".", ";" and type-expression step separators; with
 * tagged set, every component after ';' / '|' (and the first) starts with a
 * one-letter type-expression tag that is skipped.
 */
static bool text_mentions(const char *text, NameMark *set, bool tagged) {
    if (!text) return false;
    const char *p = text;
    bool at_tag = tagged;
    while (*p) {
        if (at_tag) p++;
        size_t len = 0;
        while (p[len] && p[len] != ':' && p[len] != '.' && p[len] != ';' && p[len] != CTX_TYPE_STEP_SEP) len++;
        if (len) {
            NameMark *m = NULL;
            HASH_FIND(hh, set, p, len, m);
            if (m) return true;
        }
        p += len;
        at_tag = tagged && (*p == ';' || *p == CTX_TYPE_STEP_SEP);
        while (*p == ':' || *p == '.' || *p == ';' || *p == CTX_TYPE_STEP_SEP) p++;
    }
    return false;
}

/* Site filter: the site's lookup involves a marked name. */
static bool site_mentions(const CtxRefSite *site, const void *arg) {
    NameMark *set = (NameMark *)arg;
    NameMark *m = NULL;
    HASH_FIND_STR(set, site->to_name, m);
    return m || text_mentions(site->to_scope, set, false) || text_mentions(site->recv, set, true) ||
           text_mentions(site->arg_types, set, true);
}

static int symbol_order(const void *a, const void *b) {
    const CtxSymbol *x = *(const CtxSymbol *const *)a;
    const CtxSymbol *y = *(const CtxSymbol *const *)b;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    if (x->col != y->col) return x->col < y->col ? -1 : 1;
    return 0;
}

/* True when a file's declarations can change lookups in files including it. */
static bool has_directives(const CtxGraphFile *f) {
    for (uint32_t i = 0; i < f->decl_count; i++) {
        const CtxLookupDecl *d = &f->decls[i];
        if (!d->local && (d->kind == CTX_DECL_USING || d->kind == CTX_DECL_USING_NAMESPACE)) return true;
    }
    return false;
}

/* Copies name into the removed-names list (heap string). */
static void remember_name(char ***names, uint32_t *count, uint32_t *cap, const char *name) {
    if (*count >= *cap) {
        uint32_t next_cap = *cap ? *cap * 2 : 32;
        char **next = (char **)realloc(*names, next_cap * sizeof(char *));
        if (!next) return;
        *names = next;
        *cap = next_cap;
    }
    char *copy = strdup(name);
    if (copy) (*names)[(*count)++] = copy;
}

/*
 * Removes a file's symbols, sites and declarations from every index. Names
 * of removed symbols and declarations are copied into *removed_names (heap
 * strings) when removed_names is non-NULL.
 */
static void clear_file(CtxGraph *g, CtxGraphFile *f, char ***removed_names,
                       uint32_t *removed_count) {
    uint32_t removed_cap = 0;
    for (uint32_t i = 0; i < f->site_count; i++) {
        CtxRefSite *site = &f->sites[i];
        if (site->res_from) edge_release(g, site->res_from, site->res_to, site->kind);
    }
    free_sites(f->sites, f->site_count);
    f->sites = NULL;
    f->site_count = 0;

    for (uint32_t i = 0; i < f->decl_count; i++) {
        const CtxLookupDecl *d = &f->decls[i];
        if (removed_names && d->name) remember_name(removed_names, removed_count, &removed_cap, d->name);
        if (decl_is_indexed(d)) decl_index_remove(g, d);
    }
    free_decls(f->decls, f->decl_count);
    f->decls = NULL;
    f->decl_count = 0;

    for (uint32_t i = 0; i < f->symbol_count; i++) {
        CtxSymbol *s = f->symbols[i];
        if (removed_names) remember_name(removed_names, removed_count, &removed_cap, s->name);
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
    f->decls = ex->decls;
    f->decl_count = ex->decl_count;
    for (uint32_t i = 0; i < f->decl_count; i++)
        if (decl_is_indexed(&f->decls[i])) decl_index_add(g, f, &f->decls[i]);
    ex->sites = NULL;
    ex->site_count = 0;
    ex->site_cap = 0;
    ex->decls = NULL;
    ex->decl_count = 0;
    ex->decl_cap = 0;
    ctx_file_extract_free(ex);
}

/* True when file f reaches target through its (transitive) #includes. */
static bool file_reaches(CtxGraph *g, CtxGraphFile *f, const CtxGraphFile *target) {
    CtxGraphFile *queue[CTX_INCLUDE_CLOSURE_MAX];
    uint32_t count = 0;
    queue[count++] = f;
    for (uint32_t q = 0; q < count; q++) {
        file_direct_includes(g, queue[q]);
        for (uint32_t i = 0; i < queue[q]->include_count; i++) {
            CtxGraphFile *next = queue[q]->includes[i];
            if (next == target) return true;
            bool seen = false;
            for (uint32_t k = 0; k < count && !seen; k++) seen = queue[k] == next;
            if (!seen && count < CTX_INCLUDE_CLOSURE_MAX) queue[count++] = next;
        }
    }
    return false;
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
    bool     old_directives = false;
    CtxGraphFile *f = NULL;
    HASH_FIND_STR(g->files, path, f);
    if (f) {
        old_directives = has_directives(f);
        clear_file(g, f, resolve ? &old_names : NULL, &old_count);
    }

    if (!ex) {
        if (f) {
            basename_index_remove(g, f);
            HASH_DEL(g->files, f);
            g->include_epoch++;
            free(f->includes);
            free(f->path);
            free(f);
            f = NULL;
        }
    } else {
        if (!f) {
            f = (CtxGraphFile *)calloc(1, sizeof(CtxGraphFile));
            if (f) f->path = strdup(path);
            if (f && !f->path) { free(f); f = NULL; }
            if (f) {
                HASH_ADD_KEYPTR(hh, g->files, f->path, strlen(f->path), f);
                basename_index_add(g, f);
                g->include_epoch++;
            }
        }
        if (f) {
            install_extract(g, f, ex);
            f->include_epoch = 0;
            f->version = ++g->version_seq;
        }
        else ctx_file_extract_free(ex);
    }

    if (resolve) {
        NameMark *affected = NULL;
        for (uint32_t i = 0; i < old_count; i++) mark_name(&affected, old_names[i]);
        bool directives = old_directives || (f && has_directives(f));
        if (f) {
            for (uint32_t i = 0; i < f->symbol_count; i++) mark_name(&affected, f->symbols[i]->name);
            for (uint32_t i = 0; i < f->decl_count; i++)
                if (f->decls[i].name) mark_name(&affected, f->decls[i].name);
        }
        for (int pass = 1; pass >= 0; pass--) {
            if (f) resolve_file_sites(g, f, pass == 1, NULL, NULL);
            if (!affected && !directives) continue;
            CtxGraphFile *other, *otmp;
            HASH_ITER(hh, g->files, other, otmp) {
                if (other == f) continue;
                if (directives && (!f || file_reaches(g, other, f)))
                    resolve_file_sites(g, other, pass == 1, NULL, NULL);
                else if (affected)
                    resolve_file_sites(g, other, pass == 1, site_mentions, affected);
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
    for (int pass = 1; pass >= 0; pass--) {
        CtxGraphFile *f, *tmp;
        HASH_ITER(hh, g->files, f, tmp) resolve_file_sites(g, f, pass == 1, NULL, NULL);
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

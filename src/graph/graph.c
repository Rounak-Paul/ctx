#include "graph.h"
#include "../log/log.h"

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
    return ctx_fnv64(buf, (size_t)(n > 0 ? n : 0));
}

CtxGraph *ctx_graph_create(void) {
    CtxGraph *g = (CtxGraph *)calloc(1, sizeof(CtxGraph));
    if (!g) return NULL;
#if defined(CTX_PLATFORM_WINDOWS)
    InitializeSRWLock(&g->lock);
    InitializeCriticalSection(&g->pending_lock);
#else
    pthread_rwlock_init(&g->lock, NULL);
    pthread_mutex_init(&g->pending_lock, NULL);
#endif
    return g;
}

void ctx_graph_destroy(CtxGraph *g) {
    if (!g) return;
    /* Walk the linked list directly — uthash stores hh.next for traversal */
    CtxSymbol *s = g->symbols;
    while (s) { CtxSymbol *nx = (CtxSymbol *)s->hh.next; free(s); s = nx; }
    g->symbols = NULL;

    CtxEdgeEntry *e = g->edges;
    while (e) { CtxEdgeEntry *nx = (CtxEdgeEntry *)e->hh.next; free(e); e = nx; }
    g->edges = NULL;

    for (uint32_t i = 0; i < g->pending_count; i++) {
        free(g->pending_calls[i].caller_file);
        free(g->pending_calls[i].caller_name);
        free(g->pending_calls[i].callee_name);
    }
    free(g->pending_calls);
#if !defined(CTX_PLATFORM_WINDOWS)
    pthread_rwlock_destroy(&g->lock);
    pthread_mutex_destroy(&g->pending_lock);
#else
    DeleteCriticalSection(&g->pending_lock);
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

void ctx_graph_add_symbol(CtxGraph *g, const CtxSymbol *sym) {
    if (!g || !sym) return;
    ctx_graph_wlock(g);
    CtxSymbol *existing = NULL;
    HASH_FIND(hh, g->symbols, &sym->id, sizeof(uint64_t), existing);
    if (existing) {
        UT_hash_handle hh = existing->hh;
        memcpy(existing, sym, sizeof(CtxSymbol));
        existing->hh = hh;
    } else {
        CtxSymbol *copy = (CtxSymbol *)malloc(sizeof(CtxSymbol));
        if (copy) {
            memcpy(copy, sym, sizeof(CtxSymbol));
            HASH_ADD(hh, g->symbols, id, sizeof(uint64_t), copy);
        }
    }
    ctx_graph_wunlock(g);
}

void ctx_graph_add_edge(CtxGraph *g, uint64_t from_id, uint64_t to_id, CtxEdgeKind kind) {
    if (!g) return;
    uint64_t key_data[3] = { from_id, to_id, (uint64_t)kind };
    uint64_t key = ctx_fnv64((const char *)key_data, sizeof(key_data));

    ctx_graph_wlock(g);
    CtxEdgeEntry *existing = NULL;
    HASH_FIND(hh, g->edges, &key, sizeof(uint64_t), existing);
    if (!existing) {
        CtxEdgeEntry *e = (CtxEdgeEntry *)malloc(sizeof(CtxEdgeEntry));
        if (e) {
            e->key     = key;
            e->from_id = from_id;
            e->to_id   = to_id;
            e->kind    = kind;
            HASH_ADD(hh, g->edges, key, sizeof(uint64_t), e);
        }
    }
    ctx_graph_wunlock(g);
}

void ctx_graph_remove_file(CtxGraph *g, const char *path) {
    if (!g || !path) return;
    ctx_graph_wlock(g);

    /* Collect symbol IDs to remove */
    uint64_t ids_to_remove[65536];
    uint32_t id_count = 0;
    CtxSymbol *s, *stmp;
    HASH_ITER(hh, g->symbols, s, stmp) {
        if (strcmp(s->file, path) == 0) {
            if (id_count < 65536) ids_to_remove[id_count++] = s->id;
            HASH_DEL(g->symbols, s);
            free(s);
        }
    }

    /* Remove edges referencing removed symbols */
    if (id_count > 0) {
        CtxEdgeEntry *e, *etmp;
        HASH_ITER(hh, g->edges, e, etmp) {
            bool remove = false;
            for (uint32_t i = 0; i < id_count; i++) {
                if (e->from_id == ids_to_remove[i] || e->to_id == ids_to_remove[i]) {
                    remove = true; break;
                }
            }
            if (remove) { HASH_DEL(g->edges, e); free(e); }
        }
    }
    ctx_graph_wunlock(g);
}

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

/* Internal version — caller must already hold at least a read lock */
static CtxSymbol *find_by_name_locked(CtxGraph *g, const char *name) {
    CtxSymbol *s, *tmp;
    HASH_ITER(hh, g->symbols, s, tmp) {
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

CtxSymbol *ctx_graph_find_by_name(CtxGraph *g, const char *name) {
    if (!g || !name) return NULL;
    ctx_graph_rlock(g);
    CtxSymbol *found = find_by_name_locked(g, name);
    ctx_graph_runlock(g);
    return found;
}

CtxSymbol *ctx_graph_find_by_id_locked(CtxGraph *g, uint64_t id) {
    if (!g) return NULL;
    CtxSymbol *s = NULL;
    HASH_FIND(hh, g->symbols, &id, sizeof(uint64_t), s);
    return s;
}

void ctx_graph_add_pending_call(CtxGraph *g, const char *caller_file,
                                const char *caller_name, uint32_t caller_line,
                                const char *callee_name) {
    ctx_graph_add_pending_edge(g, caller_file, caller_name, caller_line,
                               callee_name, CTX_EDGE_CALLS);
}

/*
 * Adds an unresolved semantic edge for the post-index resolver.
 *
 * from_file  Source file used as a fallback when the source symbol is unresolved.
 * from_name  Source symbol name. May be NULL for anonymous call sites.
 * from_line  Source line used for fallback source ids.
 * to_name    Target symbol name resolved after all files are indexed.
 * kind       Edge kind to create after resolution.
 */
void ctx_graph_add_pending_edge(CtxGraph *g, const char *from_file,
                                const char *from_name, uint32_t from_line,
                                const char *to_name, CtxEdgeKind kind) {
    if (!g || !to_name || !to_name[0]) return;

#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&g->pending_lock);
#else
    pthread_mutex_lock(&g->pending_lock);
#endif

    if (g->pending_count >= g->pending_cap) {
        uint32_t new_cap = g->pending_cap ? g->pending_cap * 2 : 4096;
        CtxPendingCall *tmp = (CtxPendingCall *)realloc(g->pending_calls,
                              new_cap * sizeof(CtxPendingCall));
        if (tmp) { g->pending_calls = tmp; g->pending_cap = new_cap; }
    }
    if (g->pending_calls && g->pending_count < g->pending_cap) {
        CtxPendingCall *pc = &g->pending_calls[g->pending_count++];
        pc->caller_file = from_file ? strdup(from_file) : NULL;
        pc->caller_name = (from_name && from_name[0]) ? strdup(from_name) : NULL;
        pc->callee_name = strdup(to_name);
        pc->caller_line = from_line;
        pc->kind = kind;
        if (!pc->callee_name) {
            free(pc->caller_file);
            free(pc->caller_name);
            g->pending_count--;
        }
    }

#if defined(CTX_PLATFORM_WINDOWS)
    LeaveCriticalSection(&g->pending_lock);
#else
    pthread_mutex_unlock(&g->pending_lock);
#endif
}

/*
 * Resolution candidate index: name → linked list of every symbol sharing that
 * name. Resolution ranks candidates by locality (same file/module), definition
 * preference, and kind so a bare name resolves to the most plausible symbol
 * instead of an arbitrary first match.
 */
typedef struct NameCand {
    uint64_t         id;
    const CtxSymbol *sym;        /* borrowed; valid only while graph read-locked */
    struct NameCand *next;
} NameCand;

typedef struct NameEntry {
    char           name[256];
    NameCand      *cands;
    UT_hash_handle hh;
} NameEntry;

/* Module key = directory of the file. Same-module locality is coarser than
 * same-file but still far better than a global match for common names. */
static void module_of(const char *file, char *out, size_t out_sz) {
    out[0] = '\0';
    if (!file) return;
    const char *slash = strrchr(file, '/');
#if defined(CTX_PLATFORM_WINDOWS)
    const char *bslash = strrchr(file, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (!slash) return;
    size_t len = (size_t)(slash - file);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, file, len);
    out[len] = '\0';
}

static int kind_resolution_rank(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION: case CTX_SYM_METHOD: return 5;
    case CTX_SYM_CLASS: case CTX_SYM_STRUCT:    return 4;
    case CTX_SYM_TYPEDEF: case CTX_SYM_ENUM:    return 3;
    case CTX_SYM_MACRO: case CTX_SYM_VARIABLE:  return 2;
    case CTX_SYM_NAMESPACE: case CTX_SYM_INCLUDE: return 1;
    default:                                    return 0;
    }
}

/*
 * Selects the best target symbol for a pending edge among all candidates with
 * a matching name. Higher score wins; ties keep the lexically-first id stable.
 */
static const CtxSymbol *pick_candidate(const NameCand *cands,
                                       const char *from_file,
                                       const char *from_module) {
    const CtxSymbol *best = NULL;
    int best_score = INT_MIN;
    for (const NameCand *c = cands; c; c = c->next) {
        const CtxSymbol *s = c->sym;
        int score = 0;
        if (from_file && !strcmp(s->file, from_file)) score += 100;
        else if (from_module && from_module[0]) {
            char m[4096]; module_of(s->file, m, sizeof(m));
            if (!strcmp(m, from_module)) score += 40;
        }
        if (s->is_definition) score += 20;
        score += kind_resolution_rank(s->kind);
        if (score > best_score ||
            (score == best_score && best && s->id < best->id)) {
            best_score = score;
            best = s;
        }
    }
    return best;
}

uint32_t ctx_graph_resolve_calls(CtxGraph *g) {
    if (!g || !g->pending_count) return 0;

    CTX_LOG_DEBUG("Resolving %u pending semantic edges against %u symbols",
                  g->pending_count, HASH_COUNT(g->symbols));

    /* Build name → candidate-list index for ranked O(1) lookup. */
    NameEntry *name_map = NULL;
    ctx_graph_rlock(g);
    {
        CtxSymbol *s, *tmp;
        HASH_ITER(hh, g->symbols, s, tmp) {
            NameEntry *ne = NULL;
            HASH_FIND_STR(name_map, s->name, ne);
            if (!ne) {
                ne = (NameEntry *)calloc(1, sizeof(NameEntry));
                if (!ne) continue;
                strncpy(ne->name, s->name, sizeof(ne->name) - 1);
                HASH_ADD_STR(name_map, name, ne);
            }
            NameCand *nc = (NameCand *)malloc(sizeof(NameCand));
            if (nc) { nc->id = s->id; nc->sym = s; nc->next = ne->cands; ne->cands = nc; }
        }
    }

    /* Resolve under the read lock into a temp edge buffer, then add edges after
     * releasing it — ctx_graph_add_edge() takes the write lock and the rwlock is
     * not recursive. */
    typedef struct { uint64_t from, to; CtxEdgeKind kind; } ResolvedEdge;
    ResolvedEdge *out_edges = (ResolvedEdge *)malloc(g->pending_count * sizeof(ResolvedEdge));
    uint32_t out_count = 0;
    uint32_t resolved = 0;
    uint32_t missed   = 0;
    for (uint32_t i = 0; i < g->pending_count; i++) {
        CtxPendingCall *pc = &g->pending_calls[i];

        NameEntry *ce = NULL;
        if (pc->callee_name) HASH_FIND_STR(name_map, pc->callee_name, ce);
        if (!ce || !ce->cands) { missed++; }
        else {
            char from_module[4096];
            module_of(pc->caller_file, from_module, sizeof(from_module));
            const CtxSymbol *target =
                pick_candidate(ce->cands, pc->caller_file, from_module);

            uint64_t caller_id;
            if (pc->caller_name) {
                NameEntry *cre = NULL;
                HASH_FIND_STR(name_map, pc->caller_name, cre);
                const CtxSymbol *src = cre && cre->cands
                    ? pick_candidate(cre->cands, pc->caller_file, from_module)
                    : NULL;
                caller_id = src ? src->id
                                : ctx_symbol_id(pc->caller_file ? pc->caller_file : "",
                                                pc->caller_name, pc->caller_line);
            } else {
                caller_id = ctx_symbol_id(pc->caller_file ? pc->caller_file : "",
                                          pc->callee_name, pc->caller_line);
            }
            if (target && caller_id != target->id && out_edges) {
                out_edges[out_count].from = caller_id;
                out_edges[out_count].to   = target->id;
                out_edges[out_count].kind = pc->kind;
                out_count++;
                resolved++;
            }
        }

        free(pc->caller_file);
        free(pc->caller_name);
        free(pc->callee_name);
    }
    ctx_graph_runlock(g);

    for (uint32_t i = 0; i < out_count; i++)
        ctx_graph_add_edge(g, out_edges[i].from, out_edges[i].to, out_edges[i].kind);
    free(out_edges);

    {
        NameEntry *ne, *ntmp;
        HASH_ITER(hh, name_map, ne, ntmp) {
            NameCand *c = ne->cands;
            while (c) { NameCand *nx = c->next; free(c); c = nx; }
            HASH_DEL(name_map, ne);
            free(ne);
        }
    }

    CTX_LOG_DEBUG("Resolve complete: %u edges added, %u unresolved (target not in graph)", resolved, missed);

    free(g->pending_calls);
    g->pending_calls  = NULL;
    g->pending_count  = 0;
    g->pending_cap    = 0;

    return resolved;
}

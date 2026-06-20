#include "retrieve.h"
#include "../log/log.h"
#include <ctype.h>

/* ======================================================================== */
/* tunables                                                                  */
/* ======================================================================== */
#define CTX_MAX_QUERY_TERMS    24
#define CTX_MAX_SEEDS          128   /* top text-scored entry points          */
#define CTX_MAX_TRAVERSAL      512   /* max symbols collected via graph walk  */
#define CTX_TRAVERSAL_DEPTH    6     /* BFS hop limit in each direction       */
#define CTX_MODULE_SIBLINGS    48    /* max co-file symbols pulled per seed   */
#define CTX_DIR_PEERS          16    /* max peer-file names pulled per module */
#define CTX_MIN_SCORE          4.0   /* ignore symbols below this text score  */

/* ======================================================================== */
/* string builder                                                            */
/* ======================================================================== */
typedef struct { char *buf; size_t len; size_t cap; } SB;

static void sb_init(SB *s) {
    s->cap = 65536; s->len = 0;
    s->buf = (char *)malloc(s->cap);
    if (s->buf) s->buf[0] = '\0';
}

static void sb_reserve(SB *s, size_t extra) {
    if (!s->buf) return;
    if (s->len + extra + 1 > s->cap) {
        size_t ncap = (s->len + extra + 1) * 2;
        if (ncap < s->cap + 65536) ncap = s->cap + 65536;
        char *nb = (char *)realloc(s->buf, ncap);
        if (!nb) return;
        s->buf = nb; s->cap = ncap;
    }
}

static void sb_puts(SB *s, const char *str) {
    if (!s->buf || !str) return;
    size_t l = strlen(str);
    sb_reserve(s, l);
    if (s->len + l + 1 <= s->cap) { memcpy(s->buf + s->len, str, l + 1); s->len += l; }
}

static void sb_printf(SB *s, const char *fmt, ...) {
    if (!s->buf) return;
    char tmp[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) sb_puts(s, tmp);
}

/* ======================================================================== */
/* query tokenisation                                                        */
/* ======================================================================== */
typedef struct { char terms[CTX_MAX_QUERY_TERMS][64]; uint32_t count; } QueryTerms;

static bool is_stopword(const char *w) {
    static const char *sw[] = {
        "the","a","an","is","are","of","to","in","on","for","and","or","how",
        "why","does","do","where","what","when","with","this","that","it","as",
        "be","if","has","have","any","all","its","not","but","can","was","were",
        NULL
    };
    for (int i = 0; sw[i]; i++) if (!strcmp(w, sw[i])) return true;
    return false;
}

static void stem(char *w) {
    size_t n = strlen(w);
    if (n > 5 && !strcmp(w + n - 3, "ing")) { w[n-3] = '\0'; return; }
    if (n > 4 && !strcmp(w + n - 2, "ed"))  { w[n-2] = '\0'; return; }
    if (n > 4 && w[n-1] == 's' && w[n-2] != 's') { w[n-1] = '\0'; }
}

static void tokenize(const char *text, QueryTerms *out) {
    out->count = 0;
    if (!text) return;
    char cur[64]; size_t cl = 0;
    for (const char *p = text; ; ++p) {
        unsigned char c = (unsigned char)*p;
        bool wc = isalnum(c) || c == '_';
        if (wc && cl < sizeof(cur) - 1) { cur[cl++] = (char)tolower(c); }
        else if (cl > 0) {
            cur[cl] = '\0';
            stem(cur); cl = strlen(cur);
            if (cl >= 2 && !is_stopword(cur) && out->count < CTX_MAX_QUERY_TERMS) {
                bool dup = false;
                for (uint32_t i = 0; i < out->count; i++)
                    if (!strcmp(out->terms[i], cur)) { dup = true; break; }
                if (!dup) strcpy(out->terms[out->count++], cur);
            }
            cl = 0;
        }
        if (!*p) break;
    }
}

/* ======================================================================== */
/* path helpers                                                              */
/* ======================================================================== */
static const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/*
 * Returns the directory component of a path into out (no trailing slash).
 * out must be at least as large as path.
 */
static void path_dir(const char *path, char *out, size_t out_sz) {
    strncpy(out, path, out_sz - 1);
    out[out_sz - 1] = '\0';
    char *s = strrchr(out, '/');
    if (s) *s = '\0'; else out[0] = '\0';
}

/*
 * Returns the directory component two levels up (module root heuristic).
 * e.g. src/render/pbr.c → src/render
 */
static void path_module(const char *path, char *out, size_t out_sz) {
    path_dir(path, out, out_sz);
}

static bool is_vendor_path(const char *file) {
    return file && (strstr(file, "/vendor/") || strstr(file, "/vendors/") ||
                    strstr(file, "/third_party/") || strstr(file, "/thirdparty/") ||
                    strstr(file, "/external/") || strstr(file, "/deps/"));
}

/* ======================================================================== */
/* case-insensitive substring                                                */
/* ======================================================================== */
static bool ci_substr(const char *hay, const char *needle) {
    size_t hl = strlen(hay), nl = strlen(needle);
    if (!nl || nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        for (; j < nl; j++)
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) break;
        if (j == nl) return true;
    }
    return false;
}

/* ======================================================================== */
/* symbol scoring                                                            */
/* ======================================================================== */
static int kind_importance(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION: case CTX_SYM_METHOD: return 10;
    case CTX_SYM_CLASS: case CTX_SYM_STRUCT:    return 8;
    case CTX_SYM_TYPEDEF: case CTX_SYM_ENUM:    return 6;
    case CTX_SYM_NAMESPACE:                      return 5;
    case CTX_SYM_MACRO: case CTX_SYM_VARIABLE:  return 4;
    case CTX_SYM_INCLUDE:                        return 1;
    default:                                     return 0;
    }
}

static double score_symbol(const CtxSymbol *s, const QueryTerms *q) {
    if (s->kind == CTX_SYM_UNKNOWN) return 0;
    double score = 0;
    uint32_t covered = 0;
    const char *base = path_basename(s->file);

    for (uint32_t i = 0; i < q->count; i++) {
        const char *t = q->terms[i];
        bool hit = false;
        if (!strcasecmp(s->name, t))        { score += 40; hit = true; }
        else if (ci_substr(s->name, t))     { score += 18; hit = true; }
        if (ci_substr(base, t))             { score += 10; hit = true; }
        if (s->signature[0] && ci_substr(s->signature, t)) { score += 5; hit = true; }
        if (s->scope[0] && ci_substr(s->scope, t))         { score += 4; hit = true; }
        if (hit) covered++;
    }
    if (covered == 0) return 0;
    score += covered * 8;
    score += kind_importance(s->kind);
    if (s->is_definition) score += 6;
    if (is_vendor_path(s->file)) score *= 0.3;
    return score;
}

/* ======================================================================== */
/* id set — open-addressed hash for fast dedup during traversal             */
/* ======================================================================== */
#define IDSET_CAP 4096  /* must be power of 2; sized for CTX_MAX_TRAVERSAL */

typedef struct { uint64_t ids[IDSET_CAP]; uint32_t count; } IdSet;

static bool idset_add(IdSet *set, uint64_t id) {
    if (id == 0) return false;
    uint32_t slot = (uint32_t)(id & (IDSET_CAP - 1));
    for (uint32_t i = 0; i < IDSET_CAP; i++) {
        uint32_t s = (slot + i) & (IDSET_CAP - 1);
        if (set->ids[s] == 0) { set->ids[s] = id; set->count++; return true; }
        if (set->ids[s] == id) return false;
    }
    return false; /* full */
}

static bool idset_has(const IdSet *set, uint64_t id) {
    if (id == 0) return false;
    uint32_t slot = (uint32_t)(id & (IDSET_CAP - 1));
    for (uint32_t i = 0; i < IDSET_CAP; i++) {
        uint32_t s = (slot + i) & (IDSET_CAP - 1);
        if (set->ids[s] == 0) return false;
        if (set->ids[s] == id) return true;
    }
    return false;
}

/* ======================================================================== */
/* collected symbol set                                                      */
/* ======================================================================== */
typedef struct {
    const CtxSymbol *sym;
    double           score;
    const char      *reason;
} CollectedSym;

typedef struct {
    CollectedSym items[CTX_MAX_TRAVERSAL];
    uint32_t     count;
    IdSet        seen;
} SymSet;

static bool symset_add(SymSet *ss, const CtxSymbol *sym, double score, const char *reason) {
    if (!sym || ss->count >= CTX_MAX_TRAVERSAL) return false;
    if (!idset_add(&ss->seen, sym->id)) {
        /* already present — upgrade score/reason if better */
        for (uint32_t i = 0; i < ss->count; i++) {
            if (ss->items[i].sym->id == sym->id) {
                if (score > ss->items[i].score) {
                    ss->items[i].score = score; ss->items[i].reason = reason;
                }
                return false;
            }
        }
        return false;
    }
    ss->items[ss->count++] = (CollectedSym){ sym, score, reason };
    return true;
}

/* ======================================================================== */
/* edge relation helpers                                                     */
/* ======================================================================== */
static const char *kind_str(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION:  return "fn";
    case CTX_SYM_METHOD:    return "method";
    case CTX_SYM_CLASS:     return "class";
    case CTX_SYM_STRUCT:    return "struct";
    case CTX_SYM_ENUM:      return "enum";
    case CTX_SYM_TYPEDEF:   return "typedef";
    case CTX_SYM_VARIABLE:  return "var";
    case CTX_SYM_MACRO:     return "macro";
    case CTX_SYM_INCLUDE:   return "include";
    case CTX_SYM_NAMESPACE: return "ns";
    default:                return "node";
    }
}

/* ======================================================================== */
/* BFS graph traversal — both directions, all edge kinds                    */
/* ======================================================================== */
/*
 * Walks the call/reference/inheritance graph outward from seed_id in both
 * directions up to CTX_TRAVERSAL_DEPTH hops. Adds every reachable non-vendor
 * symbol into ss. Caller must hold the graph read lock.
 */
static void traverse_from(CtxGraph *g, uint64_t seed_id, const char *seed_file,
                           SymSet *ss, int max_depth) {
    /* BFS queue of (id, depth) pairs */
    typedef struct { uint64_t id; int depth; } QEntry;
    QEntry *queue = (QEntry *)malloc(CTX_MAX_TRAVERSAL * 2 * sizeof(QEntry));
    if (!queue) return;
    int head = 0, tail = 0;

    IdSet visited = {0};
    idset_add(&visited, seed_id);
    queue[tail++] = (QEntry){ seed_id, 0 };

    bool seed_is_vendor = is_vendor_path(seed_file);

    while (head < tail && ss->count < CTX_MAX_TRAVERSAL) {
        QEntry cur = queue[head++];
        if (cur.depth >= max_depth) continue;

        /* Scan all edges for any that touch cur.id */
        for (CtxEdgeEntry *e = g->edges; e; e = (CtxEdgeEntry *)e->hh.next) {
            uint64_t other = 0;
            bool outgoing = false;
            if (e->from_id == cur.id) { other = e->to_id;   outgoing = true;  }
            else if (e->to_id == cur.id) { other = e->from_id; outgoing = false; }
            else continue;

            const CtxSymbol *os = ctx_graph_find_by_id_locked(g, other);
            if (!os || os->kind == CTX_SYM_UNKNOWN) continue;

            /* Suppress cross-vendor reference noise */
            if (e->kind == CTX_EDGE_REFERENCES && !seed_is_vendor && is_vendor_path(os->file))
                continue;

            const char *reason = outgoing
                ? (e->kind == CTX_EDGE_CALLS ? "callee" :
                   e->kind == CTX_EDGE_INHERITS ? "base" :
                   e->kind == CTX_EDGE_REFERENCES ? "refs" : "related")
                : (e->kind == CTX_EDGE_CALLS ? "caller" :
                   e->kind == CTX_EDGE_INHERITS ? "derived" :
                   e->kind == CTX_EDGE_REFERENCES ? "ref'd by" : "related");

            double sc = 50.0 - cur.depth * 8.0;
            symset_add(ss, os, sc, reason);

            if (!idset_has(&visited, other) && tail < CTX_MAX_TRAVERSAL * 2) {
                idset_add(&visited, other);
                queue[tail++] = (QEntry){ other, cur.depth + 1 };
            }
        }
    }
    free(queue);
}

/* ======================================================================== */
/* module / file sibling collection                                         */
/* ======================================================================== */
/*
 * Adds all definition symbols from the same file as sym into ss.
 * This captures the implicit co-location relationship — functions in the same
 * file are almost always semantically related. Caller holds read lock.
 */
static void collect_file_siblings(CtxGraph *g, const CtxSymbol *sym, SymSet *ss) {
    int added = 0;
    CtxSymbol *s, *tmp;
    HASH_ITER(hh, g->symbols, s, tmp) {
        if (added >= CTX_MODULE_SIBLINGS) break;
        if (s->id == sym->id) continue;
        if (s->kind == CTX_SYM_UNKNOWN || s->kind == CTX_SYM_INCLUDE) continue;
        if (!s->is_definition) continue;
        if (strcmp(s->file, sym->file) != 0) continue;
        if (symset_add(ss, s, 20.0, "co-file")) added++;
    }
}

/*
 * Collects file paths in the same directory as sym->file.
 * Returns count of unique basenames written into out[].
 */
static uint32_t collect_dir_peers(CtxGraph *g, const char *file,
                                  char out[][256], uint32_t max) {
    char dir[4096];
    path_dir(file, dir, sizeof(dir));
    if (!dir[0]) return 0;

    /* Collect unique file basenames in the same directory */
    char seen[CTX_DIR_PEERS][256]; uint32_t seen_count = 0;
    uint32_t found = 0;

    CtxSymbol *s, *tmp;
    HASH_ITER(hh, g->symbols, s, tmp) {
        if (found >= max) break;
        if (strcmp(s->file, file) == 0) continue;
        if (is_vendor_path(s->file)) continue;
        char sdir[4096]; path_dir(s->file, sdir, sizeof(sdir));
        if (strcmp(sdir, dir) != 0) continue;
        const char *base = path_basename(s->file);
        bool dup = false;
        for (uint32_t i = 0; i < seen_count; i++)
            if (!strcmp(seen[i], base)) { dup = true; break; }
        if (!dup && seen_count < CTX_DIR_PEERS) {
            strncpy(seen[seen_count], base, 255);
            strncpy(out[found], base, 255);
            seen_count++; found++;
        }
    }
    return found;
}

/* ======================================================================== */
/* edge inventory for a symbol — all write/read/call sites across the graph */
/* ======================================================================== */
typedef struct {
    const CtxSymbol *sym;
    CtxEdgeKind      kind;
    bool             outgoing;
} EdgeRef;

/*
 * Collects every graph edge touching sym->id, returning all caller/callee/
 * reference sites. Caller holds read lock. Returns count written into out[].
 */
static uint32_t collect_all_edges(CtxGraph *g, uint64_t sym_id,
                                  EdgeRef *out, uint32_t max) {
    uint32_t n = 0;
    for (CtxEdgeEntry *e = g->edges; e && n < max; e = (CtxEdgeEntry *)e->hh.next) {
        uint64_t other = 0; bool outgoing = false;
        if (e->from_id == sym_id) { other = e->to_id;   outgoing = true;  }
        else if (e->to_id == sym_id) { other = e->from_id; outgoing = false; }
        else continue;
        const CtxSymbol *os = ctx_graph_find_by_id_locked(g, other);
        if (!os || os->kind == CTX_SYM_UNKNOWN) continue;
        out[n++] = (EdgeRef){ os, e->kind, outgoing };
    }
    return n;
}

/* ======================================================================== */
/* output formatting — dense structured text, no markdown                   */
/* ======================================================================== */

/* Emit one symbol entry: kind, name, file:line, signature, scope */
static void emit_symbol(SB *sb, const CtxSymbol *s) {
    sb_printf(sb, "[%s] %s  %s:%u",
              kind_str(s->kind), s->name, s->file, s->line);
    if (s->end_line > s->line)
        sb_printf(sb, "-%u", s->end_line);
    if (s->scope[0])
        sb_printf(sb, "  scope:%s", s->scope);
    sb_puts(sb, "\n");
    if (s->signature[0] && s->kind != CTX_SYM_UNKNOWN)
        sb_printf(sb, "  sig: %s\n", s->signature);
}

/* Emit full edge inventory for a symbol */
static void emit_edges(SB *sb, CtxGraph *g, const CtxSymbol *s) {
    EdgeRef refs[256];
    uint32_t n = collect_all_edges(g, s->id, refs, 256);
    if (n == 0) return;

    /* Group edges by kind+direction:
     * outgoing=true  means sym→other (sym is the caller/referrer/base)
     * outgoing=false means other→sym (sym is the callee/target/derived) */
    typedef struct { const char *label; int count; char names[32][128]; } EGroup;
    EGroup groups[8] = {
        { "calls",       0, {{0}} },
        { "called-by",   0, {{0}} },
        { "includes",    0, {{0}} },
        { "included-by", 0, {{0}} },
        { "defines",     0, {{0}} },
        { "refs",        0, {{0}} },
        { "ref'd-by",    0, {{0}} },
        { "inherits",    0, {{0}} },
    };
    /* map (kind, outgoing) → group index */
    int gmap[5][2] = {
        /* CALLS */      { 0, 1 },
        /* INCLUDES */   { 2, 3 },
        /* DEFINES */    { 4, 4 },
        /* REFERENCES */ { 5, 6 },
        /* INHERITS */   { 7, 7 },
    };

    for (uint32_t i = 0; i < n; i++) {
        int k = (int)refs[i].kind;
        if (k < 0 || k > 4) continue;
        int g_idx = gmap[k][refs[i].outgoing ? 0 : 1];
        EGroup *eg = &groups[g_idx];
        if (eg->count < 32) {
            snprintf(eg->names[eg->count++], 128, "%s(%s:%u)",
                     refs[i].sym->name, path_basename(refs[i].sym->file),
                     refs[i].sym->line);
        }
    }

    for (int g = 0; g < 8; g++) {
        if (groups[g].count == 0) continue;
        sb_printf(sb, "  %s: ", groups[g].label);
        for (int i = 0; i < groups[g].count; i++) {
            if (i) sb_puts(sb, ", ");
            sb_puts(sb, groups[g].names[i]);
        }
        sb_puts(sb, "\n");
    }
}

/* ======================================================================== */
/* comparison for qsort — sort collected symbols by score descending        */
/* ======================================================================== */
static int cmp_collected(const void *a, const void *b) {
    const CollectedSym *ca = (const CollectedSym *)a;
    const CollectedSym *cb = (const CollectedSym *)b;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return 0;
}

/* ======================================================================== */
/* driver                                                                   */
/* ======================================================================== */
char *ctx_retrieve(CtxGraph *g, const CtxRetrieveRequest *req) {
    if (!req || !req->text)
        goto err_invalid;
    if (!g) {
        SB sb; sb_init(&sb);
        sb_puts(&sb, "ERROR: no graph available\n");
        return sb.buf;
    }

    /* ---- 1. tokenise query ---- */
    QueryTerms q = {0};
    if (req->kind == CTX_QUERY_TASK) {
        tokenize(req->text, &q);
    } else {
        /* symbol/file anchor: name IS the query — also tokenize on _ separators */
        char tmp[512];
        strncpy(tmp, req->text, sizeof(tmp) - 1);
        /* replace _ and / with space so tokenizer splits compound names */
        for (char *p = tmp; *p; p++) if (*p == '_' || *p == '/') *p = ' ';
        tokenize(tmp, &q);
        /* also add the full name as a term */
        if (q.count < CTX_MAX_QUERY_TERMS) {
            char clean[64]; strncpy(clean, req->text, 63);
            for (char *p = clean; *p; p++) *p = (char)tolower((unsigned char)*p);
            bool dup = false;
            for (uint32_t i = 0; i < q.count; i++)
                if (!strcmp(q.terms[i], clean)) { dup = true; break; }
            if (!dup) strcpy(q.terms[q.count++], clean);
        }
    }
    if (q.count == 0) {
        SB sb; sb_init(&sb);
        sb_puts(&sb, "ERROR: query has no usable terms\n");
        return sb.buf;
    }

    /* ---- 2. score every symbol → ranked seeds ---- */
    typedef struct { const CtxSymbol *sym; double score; } Seed;
    Seed *seeds = (Seed *)malloc(CTX_MAX_SEEDS * sizeof(Seed));
    if (!seeds) goto err_oom;
    uint32_t seed_count = 0;

    ctx_graph_rlock(g);

    {
        CtxSymbol *s, *tmp_s;
        HASH_ITER(hh, g->symbols, s, tmp_s) {
            double sc = 0;
            if (req->kind == CTX_QUERY_FILE) {
                if (strcmp(s->file, req->text) == 0)
                    sc = 200 + kind_importance(s->kind);
            } else if (req->kind == CTX_QUERY_SYMBOL) {
                /* trim and whitespace-clean the anchor */
                char anchor[512]; strncpy(anchor, req->text, sizeof(anchor)-1);
                for (char *p = anchor; *p; p++) if (isspace((unsigned char)*p)) { *p='\0'; break; }
                if (!strcasecmp(s->name, anchor))
                    sc = 200 + kind_importance(s->kind);
                else if (ci_substr(s->name, anchor) || ci_substr(anchor, s->name))
                    sc = 100 + kind_importance(s->kind);
                else
                    sc = score_symbol(s, &q);
            } else {
                sc = score_symbol(s, &q);
            }
            if (sc < CTX_MIN_SCORE) continue;

            if (seed_count < CTX_MAX_SEEDS) {
                seeds[seed_count++] = (Seed){ s, sc };
            } else {
                /* replace lowest if better */
                uint32_t min_i = 0;
                for (uint32_t i = 1; i < seed_count; i++)
                    if (seeds[i].score < seeds[min_i].score) min_i = i;
                if (sc > seeds[min_i].score)
                    seeds[min_i] = (Seed){ s, sc };
            }
        }
    }

    /* ---- 3. build full symbol set via graph traversal + file siblings ---- */
    SymSet *ss = (SymSet *)calloc(1, sizeof(SymSet));
    if (!ss) { ctx_graph_runlock(g); free(seeds); goto err_oom; }

    /* Add seeds themselves */
    for (uint32_t i = 0; i < seed_count; i++)
        symset_add(ss, seeds[i].sym, seeds[i].score, "query match");

    /* BFS from each seed */
    for (uint32_t i = 0; i < seed_count && ss->count < CTX_MAX_TRAVERSAL; i++)
        traverse_from(g, seeds[i].sym->id, seeds[i].sym->file, ss, CTX_TRAVERSAL_DEPTH);

    /* Pull co-file siblings for top seeds (structural locality) */
    uint32_t top = seed_count < 8 ? seed_count : 8;
    for (uint32_t i = 0; i < top && ss->count < CTX_MAX_TRAVERSAL; i++)
        collect_file_siblings(g, seeds[i].sym, ss);

    ctx_graph_runlock(g);
    free(seeds);

    if (ss->count == 0) {
        free(ss);
        SB sb; sb_init(&sb);
        sb_printf(&sb, "ERROR: no symbols found for query: %s\n", req->text);
        return sb.buf;
    }

    /* Sort by score */
    qsort(ss->items, ss->count, sizeof(CollectedSym), cmp_collected);

    /* ---- 4. build output ---- */
    SB sb; sb_init(&sb);

    sb_printf(&sb, "QUERY: %s\n", req->text);
    sb_printf(&sb, "SYMBOLS: %u  QUERY_TERMS:", ss->count);
    for (uint32_t i = 0; i < q.count; i++) sb_printf(&sb, " %s", q.terms[i]);
    sb_puts(&sb, "\n\n");

    /* Group output by module (directory) so structural hierarchy is visible */
    /* First pass: collect unique modules */
    char modules[CTX_MAX_TRAVERSAL][4096];
    uint32_t mod_count = 0;

    ctx_graph_rlock(g);

    for (uint32_t i = 0; i < ss->count; i++) {
        char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
        bool dup = false;
        for (uint32_t m = 0; m < mod_count; m++)
            if (!strcmp(modules[m], mod)) { dup = true; break; }
        if (!dup && mod_count < CTX_MAX_TRAVERSAL)
            strncpy(modules[mod_count++], mod, sizeof(modules[0]) - 1);
    }

    /* Second pass: emit by module */
    for (uint32_t m = 0; m < mod_count; m++) {
        /* Count symbols in this module */
        uint32_t in_mod = 0;
        for (uint32_t i = 0; i < ss->count; i++) {
            char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
            if (!strcmp(mod, modules[m])) in_mod++;
        }
        if (in_mod == 0) continue;

        sb_printf(&sb, "MODULE: %s/\n", modules[m]);

        /* Emit peer files in same directory */
        char peers[CTX_DIR_PEERS][256];
        /* find a representative file for this module */
        const char *mod_file = NULL;
        for (uint32_t i = 0; i < ss->count; i++) {
            char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
            if (!strcmp(mod, modules[m])) { mod_file = ss->items[i].sym->file; break; }
        }
        if (mod_file) {
            uint32_t pc = collect_dir_peers(g, mod_file, peers, CTX_DIR_PEERS);
            if (pc > 0) {
                sb_puts(&sb, "  dir-peers:");
                for (uint32_t p = 0; p < pc; p++) sb_printf(&sb, " %s", peers[p]);
                sb_puts(&sb, "\n");
            }
        }

        /* Group by file within module */
        char files[64][4096]; uint32_t fcount = 0;
        for (uint32_t i = 0; i < ss->count; i++) {
            char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
            if (strcmp(mod, modules[m]) != 0) continue;
            bool dup = false;
            for (uint32_t f = 0; f < fcount; f++)
                if (!strcmp(files[f], ss->items[i].sym->file)) { dup = true; break; }
            if (!dup && fcount < 64)
                strncpy(files[fcount++], ss->items[i].sym->file, sizeof(files[0]) - 1);
        }

        for (uint32_t f = 0; f < fcount; f++) {
            sb_printf(&sb, "  FILE: %s\n", files[f]);
            for (uint32_t i = 0; i < ss->count; i++) {
                if (strcmp(ss->items[i].sym->file, files[f]) != 0) continue;
                const CtxSymbol *s = ss->items[i].sym;
                sb_printf(&sb, "    (%s) ", ss->items[i].reason);
                emit_symbol(&sb, s);
                emit_edges(&sb, g, s);
            }
        }
        sb_puts(&sb, "\n");
    }

    ctx_graph_runlock(g);

    CTX_LOG_DEBUG("retrieve '%s': %u symbols across %u modules",
                  req->text, ss->count, mod_count);
    free(ss);
    return sb.buf;

err_invalid: {
    SB sb; sb_init(&sb);
    sb_puts(&sb, "ERROR: invalid request\n");
    return sb.buf;
}
err_oom: {
    SB sb; sb_init(&sb);
    sb_puts(&sb, "ERROR: out of memory\n");
    return sb.buf;
}
}

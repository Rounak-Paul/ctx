#include "retrieve.h"
#include "../store/store.h"
#include "../log/log.h"
#include <ctype.h>

/* ======================================================================== */
/* tunables                                                                  */
/* ======================================================================== */
#define CTX_MAX_QUERY_TERMS    24
#define CTX_MAX_SEEDS          64    /* top text-scored entry points          */
#define CTX_MAX_TRAVERSAL      96    /* max symbols collected via graph walk  */
#define CTX_TRAVERSAL_DEPTH    4     /* BFS hop limit in each direction       */
#define CTX_MODULE_SIBLINGS    8     /* max co-file symbols pulled per seed   */
#define CTX_DIR_PEERS          8     /* max peer-file names pulled per module */
#define CTX_MIN_SCORE          6.0   /* ignore symbols below this text score  */

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
        /* directory path match: signals the symbol lives in a relevant module */
        if (ci_substr(s->file, t))          { score += 6;  hit = true; }
        if (s->signature[0] && ci_substr(s->signature, t)) { score += 5; hit = true; }
        if (s->scope[0] && ci_substr(s->scope, t))         { score += 4; hit = true; }
        if (hit) covered++;
    }
    if (covered == 0) return 0;
    score += covered * 8;
    score += kind_importance(s->kind);
    if (s->is_definition) score += 6;
    /* scope depth bonus: symbol in a matching scope is more specific */
    if (s->scope[0]) score += 2;
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
/* file index — built once per retrieval call to avoid O(n) scans per seed  */
/* ======================================================================== */

#define FILE_INDEX_BUCKETS 4096u  /* power of 2; >2342 files */

typedef struct FileEntry {
    const char      *file;
    const CtxSymbol **syms;
    uint32_t          sym_count;
    uint32_t          sym_cap;
    struct FileEntry *next;
} FileEntry;

typedef struct {
    FileEntry *buckets[FILE_INDEX_BUCKETS];
    FileEntry *entries;    /* flat allocation pool */
    uint32_t   entry_count;
    uint32_t   entry_cap;
} FileIndex;

static uint32_t fi_hash(const char *file) {
    uint32_t h = 2166136261u;
    for (const char *p = file; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h & (FILE_INDEX_BUCKETS - 1u);
}

/*
 * Builds a file→symbols index over all non-UNKNOWN, non-INCLUDE definition
 * symbols. Must be called under the graph read lock. Caller frees with fi_free().
 */
static FileIndex *fi_build(CtxGraph *g) {
    FileIndex *fi = calloc(1, sizeof(FileIndex));
    if (!fi) return NULL;

    uint32_t total = HASH_COUNT(g->symbols);
    fi->entry_cap = total < 64 ? 64 : total;
    fi->entries = calloc(fi->entry_cap, sizeof(FileEntry));
    if (!fi->entries) { free(fi); return NULL; }

    CtxSymbol *s, *tmp;
    HASH_ITER(hh, g->symbols, s, tmp) {
        if (s->kind == CTX_SYM_UNKNOWN || s->kind == CTX_SYM_INCLUDE) continue;
        if (!s->is_definition) continue;

        uint32_t h = fi_hash(s->file);
        FileEntry *fe = NULL;
        for (FileEntry *e = fi->buckets[h]; e; e = e->next) {
            if (strcmp(e->file, s->file) == 0) { fe = e; break; }
        }
        if (!fe) {
            if (fi->entry_count >= fi->entry_cap) continue; /* shouldn't happen */
            fe = &fi->entries[fi->entry_count++];
            fe->file = s->file;
            fe->sym_cap = 8; fe->sym_count = 0;
            fe->syms = malloc(fe->sym_cap * sizeof(CtxSymbol *));
            if (!fe->syms) continue;
            fe->next = fi->buckets[h];
            fi->buckets[h] = fe;
        }
        if (fe->sym_count >= fe->sym_cap) {
            uint32_t nc = fe->sym_cap * 2u;
            const CtxSymbol **ns = realloc((void *)fe->syms, nc * sizeof(CtxSymbol *));
            if (!ns) continue;
            fe->syms = ns; fe->sym_cap = nc;
        }
        fe->syms[fe->sym_count++] = s;
    }
    return fi;
}

static FileEntry *fi_get(FileIndex *fi, const char *file) {
    if (!fi || !file) return NULL;
    uint32_t h = fi_hash(file);
    for (FileEntry *e = fi->buckets[h]; e; e = e->next)
        if (strcmp(e->file, file) == 0) return e;
    return NULL;
}

static void fi_free(FileIndex *fi) {
    if (!fi) return;
    for (uint32_t i = 0; i < fi->entry_count; i++) free((void *)fi->entries[i].syms);
    free(fi->entries);
    free(fi);
}

/* ======================================================================== */
/* module / file sibling collection                                         */
/* ======================================================================== */

/*
 * Adds definition symbols from the same file as sym into ss using the file
 * index — O(file_syms) instead of O(all_syms). Caller holds read lock.
 */
static void collect_file_siblings(FileIndex *fi, const CtxSymbol *sym, SymSet *ss) {
    FileEntry *fe = fi_get(fi, sym->file);
    if (!fe) return;
    int added = 0;
    for (uint32_t i = 0; i < fe->sym_count && added < CTX_MODULE_SIBLINGS; i++) {
        const CtxSymbol *s = fe->syms[i];
        if (s->id == sym->id) continue;
        if (symset_add(ss, s, 20.0, "co-file")) added++;
    }
}

/*
 * Collects unique file basenames in the same directory as file using the
 * file index. O(unique_files_in_dir) instead of O(all_syms).
 */
static uint32_t collect_dir_peers(FileIndex *fi, const char *file,
                                  char out[][256], uint32_t max) {
    char dir[4096];
    path_dir(file, dir, sizeof(dir));
    if (!dir[0]) return 0;

    char seen[CTX_DIR_PEERS][256]; uint32_t seen_count = 0;
    uint32_t found = 0;

    for (uint32_t i = 0; i < fi->entry_count && found < max; i++) {
        FileEntry *fe = &fi->entries[i];
        if (strcmp(fe->file, file) == 0) continue;
        if (is_vendor_path(fe->file)) continue;
        char sdir[4096]; path_dir(fe->file, sdir, sizeof(sdir));
        if (strcmp(sdir, dir) != 0) continue;
        const char *base = path_basename(fe->file);
        bool dup = false;
        for (uint32_t j = 0; j < seen_count; j++)
            if (!strcmp(seen[j], base)) { dup = true; break; }
        if (!dup && seen_count < CTX_DIR_PEERS) {
            strncpy(seen[seen_count++], base, 255);
            strncpy(out[found++], base, 255);
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
/* file metadata helpers                                                     */
/* ======================================================================== */

static const char *lang_name(int lang) {
    switch (lang) {
    case 1: return "C";
    case 2: return "C++";
    case 3: return "Python";
    case 4: return "JavaScript";
    case 5: return "TypeScript";
    default: return "unknown";
    }
}

/* Count symbols in ss that belong to a given file. */
static uint32_t count_file_syms(const SymSet *ss, const char *file) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ss->count; i++)
        if (!strcmp(ss->items[i].sym->file, file)) n++;
    return n;
}

/*
 * Pulls symbols reachable through include edges from seed files into ss.
 * For each CTX_EDGE_INCLUDES edge where seed is the includer, adds symbols
 * from the included file. This captures the import-chain structural signal:
 * headers and their included types are almost always needed together.
 * Caller holds the graph read lock.
 */
static void collect_include_chain(CtxGraph *g, FileIndex *fi,
                                  const CtxSymbol *seed, SymSet *ss) {
    for (CtxEdgeEntry *e = g->edges; e; e = (CtxEdgeEntry *)e->hh.next) {
        if (e->kind != CTX_EDGE_INCLUDES) continue;
        if (e->from_id != seed->id) continue;
        const CtxSymbol *target = ctx_graph_find_by_id_locked(g, e->to_id);
        if (!target || target->kind == CTX_SYM_UNKNOWN) continue;
        if (is_vendor_path(target->file)) continue;
        FileEntry *fe = fi_get(fi, target->file);
        if (!fe) continue;
        for (uint32_t i = 0; i < fe->sym_count; i++)
            symset_add(ss, fe->syms[i], 15.0, "include-chain");
    }
}

/* ======================================================================== */
/* output formatting — compact structured text optimised for LLM consumption */
/* ======================================================================== */

/*
 * Emits a single symbol line: kind  name(signature)  :line[-endline]
 * Signature is inlined — no separate sig: line.
 * File path is NOT emitted here; the caller already emitted the FILE: header.
 *
 * s  Symbol to emit.
 */
static void emit_symbol(SB *sb, const CtxSymbol *s) {
    sb_printf(sb, "    %-8s %s", kind_str(s->kind), s->name);
    if (s->signature[0] && s->kind != CTX_SYM_UNKNOWN)
        sb_printf(sb, "(%s)", s->signature);
    sb_printf(sb, "  :%u", s->line);
    if (s->end_line > s->line) sb_printf(sb, "-%u", s->end_line);
    sb_puts(sb, "\n");
}

/*
 * Emits the relational edge inventory for a symbol, grouped by direction/kind.
 * Cross-file refs include the file basename so the LLM can locate them;
 * same-file refs show only the name and line.
 *
 * s        Symbol whose edges to emit.
 * cur_file File currently being rendered (used to elide redundant paths).
 */
static void emit_edges(SB *sb, CtxGraph *g, const CtxSymbol *s, const char *cur_file) {
    EdgeRef refs[256];
    uint32_t n = collect_all_edges(g, s->id, refs, 256);
    if (n == 0) return;

    /* ref'd-by omitted — reverse reference edges are high-volume noise at scale.
     * calls/called-by/includes/inherits carry the structural signal that matters. */
#define CTX_EDGE_GROUP_CAP 6
    typedef struct { const char *label; int count; char names[CTX_EDGE_GROUP_CAP][96]; } EGroup;
    EGroup groups[7] = {
        { "calls",       0, {{0}} },
        { "called-by",   0, {{0}} },
        { "includes",    0, {{0}} },
        { "included-by", 0, {{0}} },
        { "defines",     0, {{0}} },
        { "refs",        0, {{0}} },
        { "inherits",    0, {{0}} },
    };
    int gmap[5][2] = {
        { 0, 1 }, /* CALLS:      out=calls,       in=called-by   */
        { 2, 3 }, /* INCLUDES:   out=includes,    in=included-by */
        { 4, 4 }, /* DEFINES:    symmetric                       */
        { 5, 5 }, /* REFERENCES: out=refs only (drop ref'd-by)   */
        { 6, 6 }, /* INHERITS:   symmetric                       */
    };

    for (uint32_t i = 0; i < n; i++) {
        int k = (int)refs[i].kind;
        if (k < 0 || k > 4) continue;
        /* suppress incoming reference edges — too noisy */
        if (k == 3 /* CTX_EDGE_REFERENCES */ && !refs[i].outgoing) continue;
        int gi = gmap[k][refs[i].outgoing ? 0 : 1];
        EGroup *eg = &groups[gi];
        if (eg->count >= CTX_EDGE_GROUP_CAP) continue;
        const char *other_file = refs[i].sym->file;
        if (strcmp(other_file, cur_file) == 0) {
            /* same file — name:line is enough */
            snprintf(eg->names[eg->count++], 96, "%s:%u",
                     refs[i].sym->name, refs[i].sym->line);
        } else {
            /* cross-file — include basename for navigability */
            snprintf(eg->names[eg->count++], 96, "%s(%s:%u)",
                     refs[i].sym->name,
                     path_basename(other_file), refs[i].sym->line);
        }
    }

    for (int gi = 0; gi < 7; gi++) {
        if (groups[gi].count == 0) continue;
        sb_printf(sb, "      %s: ", groups[gi].label);
        for (int i = 0; i < groups[gi].count; i++) {
            if (i) sb_puts(sb, ", ");
            sb_puts(sb, groups[gi].names[i]);
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

    /* Build file index once (under read lock) for O(1) per-file lookups */
    FileIndex *fi = fi_build(g);

    /* Pull co-file siblings for top seeds (structural locality) */
    uint32_t top = seed_count < 8 ? seed_count : 8;
    if (fi) {
        for (uint32_t i = 0; i < top && ss->count < CTX_MAX_TRAVERSAL; i++)
            collect_file_siblings(fi, seeds[i].sym, ss);

        /* Pull include-chain symbols for top seeds (import hierarchy signal) */
        for (uint32_t i = 0; i < top && ss->count < CTX_MAX_TRAVERSAL; i++)
            collect_include_chain(g, fi, seeds[i].sym, ss);
    }

    ctx_graph_runlock(g);
    free(seeds);

    if (ss->count == 0) {
        fi_free(fi);
        free(ss);
        SB sb; sb_init(&sb);
        sb_printf(&sb, "ERROR: no symbols found for query: %s\n", req->text);
        return sb.buf;
    }

    /* Sort by score */
    qsort(ss->items, ss->count, sizeof(CollectedSym), cmp_collected);

    /* ---- 4. collect unique modules (ordered by relevance = first appearance) ---- */
    /* We use heap-allocated arrays since 4096-byte paths × 512 would be 2MB on stack */
    typedef char PathBuf[4096];
    PathBuf *modules = (PathBuf *)malloc(64 * sizeof(PathBuf));
    if (!modules) { free(ss); goto err_oom; }
    uint32_t mod_count = 0;

    ctx_graph_rlock(g);

    for (uint32_t i = 0; i < ss->count; i++) {
        char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
        bool dup = false;
        for (uint32_t m = 0; m < mod_count; m++)
            if (!strcmp(modules[m], mod)) { dup = true; break; }
        if (!dup && mod_count < 64)
            strncpy(modules[mod_count++], mod, sizeof(PathBuf) - 1);
    }

    /* ---- 5. build output ---- */
    SB sb; sb_init(&sb);

    /* CODEBASE header: gives the LLM immediate orientation */
    {
        char root[4096] = {0};
        ctx_store_get_meta("root_path", root, sizeof(root));
        if (root[0]) sb_printf(&sb, "CODEBASE: %s\n", root);
    }
    sb_printf(&sb, "QUERY: %s\n", req->text);
    sb_puts(&sb, "TERMS:");
    for (uint32_t i = 0; i < q.count; i++) sb_printf(&sb, " %s", q.terms[i]);
    sb_printf(&sb, "\nSYMBOLS: %u across %u modules\n\n", ss->count, mod_count);

    /* Module map: compact table giving the LLM a structural overview first */
    sb_puts(&sb, "MODULES:\n");
    for (uint32_t m = 0; m < mod_count; m++) {
        uint32_t in_mod = 0;
        const char *lang_hint = NULL;
        /* Collect peer file basenames for this module */
        char peers[CTX_DIR_PEERS][256]; uint32_t pcount = 0;
        const char *mod_file = NULL;

        for (uint32_t i = 0; i < ss->count; i++) {
            char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
            if (strcmp(mod, modules[m]) != 0) continue;
            in_mod++;
            if (!lang_hint) lang_hint = lang_name(ss->items[i].sym->lang);
            if (!mod_file)  mod_file  = ss->items[i].sym->file;
        }
        if (mod_file && pcount == 0)
            pcount = collect_dir_peers(fi, mod_file, peers, CTX_DIR_PEERS);

        sb_printf(&sb, "  %s/  %u syms  %s", modules[m], in_mod,
                  lang_hint ? lang_hint : "?");
        if (pcount > 0) {
            sb_puts(&sb, "  [also: ");
            for (uint32_t p = 0; p < pcount; p++) {
                if (p) sb_puts(&sb, ", ");
                sb_puts(&sb, peers[p]);
            }
            sb_puts(&sb, "]");
        }
        sb_puts(&sb, "\n");
    }
    sb_puts(&sb, "\n");

    /* Per-module detail */
    for (uint32_t m = 0; m < mod_count; m++) {
        uint32_t in_mod = 0;
        for (uint32_t i = 0; i < ss->count; i++) {
            char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
            if (!strcmp(mod, modules[m])) in_mod++;
        }
        if (in_mod == 0) continue;

        sb_printf(&sb, "MODULE %s/\n", modules[m]);

        /* Collect unique files within this module */
        PathBuf *files = (PathBuf *)malloc(64 * sizeof(PathBuf));
        if (!files) { ctx_graph_runlock(g); free(modules); free(ss); goto err_oom; }
        uint32_t fcount = 0;
        for (uint32_t i = 0; i < ss->count; i++) {
            char mod[4096]; path_module(ss->items[i].sym->file, mod, sizeof(mod));
            if (strcmp(mod, modules[m]) != 0) continue;
            bool dup = false;
            for (uint32_t f = 0; f < fcount; f++)
                if (!strcmp(files[f], ss->items[i].sym->file)) { dup = true; break; }
            if (!dup && fcount < 64)
                strncpy(files[fcount++], ss->items[i].sym->file, sizeof(PathBuf) - 1);
        }

        for (uint32_t f = 0; f < fcount; f++) {
            uint32_t fsyms = count_file_syms(ss, files[f]);
            const char *flang = NULL;
            for (uint32_t i = 0; i < ss->count; i++)
                if (!strcmp(ss->items[i].sym->file, files[f]))
                    { flang = lang_name(ss->items[i].sym->lang); break; }

            sb_printf(&sb, "  FILE %s  [%u syms, %s]\n",
                      files[f], fsyms, flang ? flang : "?");

            for (uint32_t i = 0; i < ss->count; i++) {
                if (strcmp(ss->items[i].sym->file, files[f]) != 0) continue;
                const CtxSymbol *s = ss->items[i].sym;
                emit_symbol(&sb, s);
                emit_edges(&sb, g, s, files[f]);
            }
            sb_puts(&sb, "\n");
        }
        free(files);
    }

    ctx_graph_runlock(g);
    free(modules);

    CTX_LOG_DEBUG("retrieve '%s': %u symbols across %u modules",
                  req->text, ss->count, mod_count);
    fi_free(fi);
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

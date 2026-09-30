#include "search.h"
#include "../nav/source.h"
#include "../indexer/indexer.h"
#include "../model/model.h"
#include "../store/store.h"
#include "../event/event.h"
#include "../log/log.h"

#include <ctype.h>

#define SEARCH_MAX_DOC_TERMS   64u
#define SEARCH_MAX_QUERY_TERMS 16u
#define SEARCH_CANDIDATES      200u
#define SEARCH_FUSED_POOL      40u
#define SEARCH_RERANK_POOL     30u
#define SEARCH_EMBED_BATCH     32u
#define SEARCH_DOC_TEXT_LINES  40u
#define SEARCH_RERANK_TEXT_BYTES 2000u
#define SEARCH_EMBED_TEXT_BYTES  1200u
#define SEARCH_EXCERPT_LINES   20u
#define SEARCH_RRF_K           60.0
#define SEARCH_BM25_K1         1.2
#define SEARCH_BM25_B          0.75

/* ---- index structures --------------------------------------------------------- */

typedef struct Term {
    char          *text;
    uint32_t       id;
    uint32_t       df[2];      /* [0] project docs, [1] vendor docs */
    UT_hash_handle hh;
} Term;

typedef struct VecEntry {
    uint64_t       key;
    bool           live;
    float         *vec;
    UT_hash_handle hh;
} VecEntry;

typedef struct {
    uint64_t     sym_id;
    uint32_t    *terms;        /* term ids with multiplicity; name parts first */
    uint16_t     n_terms;
    uint16_t     n_name_terms;
    uint64_t     emb_key;
    const float *vec;          /* borrowed from a VecEntry */
    bool         emb_failed;
} Doc;

typedef struct DocFile {
    char          *path;
    uint64_t       version;
    bool           vendor;
    bool           seen;
    Doc           *docs;
    uint32_t       count;
    UT_hash_handle hh;
} DocFile;

static struct {
    pthread_mutex_t lock;
    Term           *terms;
    Term          **by_id;
    uint32_t        term_count;
    uint32_t        term_cap;
    DocFile        *files;
    uint64_t        n_docs[2];
    uint64_t        total_len[2];
    VecEntry       *vecs;
    uint32_t        dim;
    bool            cache_loaded;

    CtxGraph       *graph;
    pthread_t       worker;
    bool            worker_started;
    volatile bool   stop;
    pthread_mutex_t wake_lock;
    pthread_cond_t  wake;
    bool            dirty;
    CtxEventHandle  graph_sub;
    CtxEventHandle  model_sub;
} S = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .wake_lock = PTHREAD_MUTEX_INITIALIZER,
    .wake = PTHREAD_COND_INITIALIZER,
};

/* ---- tokenization ----------------------------------------------------------------- */

typedef void (*TokenSink)(const char *token, size_t len, void *user);

static bool is_stopword(const char *t, size_t len) {
    static const char *const words[] = {
        "the", "and", "for", "how", "does", "what", "where", "when", "which", "who", "why",
        "is", "are", "was", "be", "to", "of", "in", "on", "at", "by", "it", "its", "this",
        "that", "with", "from", "into", "an", "or", "as", "do", "we", "our", "can", "should",
        "code", "function", "method", "file", "find", "show", "get", "use", "used", "using",
        "static", "const", "void", "int", "char", "unsigned", "signed", "long", "short",
        "struct", "return", "bool", "uint32", "uint64", "int32", "int64", "size", "self",
        "def", "fn", "pub", "let", "var", "func", "public", "private", "inline", "extern",
        NULL
    };
    for (int i = 0; words[i]; i++)
        if (strlen(words[i]) == len && !strncmp(words[i], t, len)) return true;
    return false;
}

/*
 * Lowercases a token into out and lightly stems it (plural, -ing, -ed, final
 * e) so inflections share a term. Returns the stemmed length, or 0 when the
 * token is a stopword.
 */
static size_t normalize_token(const char *t, size_t len, char *out, size_t out_size) {
    if (len >= out_size) len = out_size - 1;
    for (size_t i = 0; i < len; i++) out[i] = (char)tolower((unsigned char)t[i]);
    out[len] = '\0';
    if (is_stopword(out, len)) return 0;
    if (len > 5 && !strcmp(out + len - 3, "ing")) len -= 3;
    else if (len > 4 && !strcmp(out + len - 2, "ed")) len -= 2;
    else if (len > 3 && out[len - 1] == 's' && out[len - 2] != 's') len -= 1;
    if (len > 3 && out[len - 1] == 'e') len -= 1;
    out[len] = '\0';
    return len;
}

/*
 * Splits text into identifier parts: non-alphanumerics separate words, and
 * camelCase / ACRONYMWord / letter-digit boundaries split within words. Each
 * normalized part of length >= 2 that is not a stopword goes to sink.
 */
static void split_words(const char *text, size_t len, TokenSink sink, void *user) {
    size_t i = 0;
    while (i < len) {
        while (i < len && !isalnum((unsigned char)text[i])) i++;
        size_t start = i;
        while (i < len) {
            unsigned char c = (unsigned char)text[i];
            if (!isalnum(c)) break;
            if (i > start) {
                unsigned char prev = (unsigned char)text[i - 1];
                bool lower_to_upper = islower(prev) && isupper(c);
                bool acronym_end = isupper(prev) && isupper(c) && i + 1 < len &&
                                   islower((unsigned char)text[i + 1]);
                bool alpha_to_digit = isalpha(prev) && isdigit(c);
                if (lower_to_upper || acronym_end || alpha_to_digit) break;
            }
            i++;
        }
        if (i > start) {
            char norm[64];
            size_t n = normalize_token(text + start, i - start, norm, sizeof(norm));
            if (n >= 2) sink(norm, n, user);
        }
    }
}

/* ---- term dictionary (S.lock held) --------------------------------------------------- */

static Term *term_get(const char *text, bool create) {
    Term *t = NULL;
    HASH_FIND_STR(S.terms, text, t);
    if (t || !create) return t;
    if (S.term_count >= S.term_cap) {
        uint32_t cap = S.term_cap ? S.term_cap * 2 : 4096;
        Term **next = (Term **)realloc(S.by_id, cap * sizeof(Term *));
        if (!next) return NULL;
        S.by_id = next;
        S.term_cap = cap;
    }
    t = (Term *)calloc(1, sizeof(Term));
    if (!t) return NULL;
    t->text = strdup(text);
    if (!t->text) { free(t); return NULL; }
    t->id = S.term_count;
    S.by_id[S.term_count++] = t;
    HASH_ADD_KEYPTR(hh, S.terms, t->text, strlen(t->text), t);
    return t;
}

typedef struct {
    uint32_t ids[SEARCH_MAX_DOC_TERMS];
    uint32_t count;
} TermBag;

static void bag_sink(const char *token, size_t len, void *user) {
    CTX_UNUSED(len);
    TermBag *bag = (TermBag *)user;
    if (bag->count >= SEARCH_MAX_DOC_TERMS) return;
    Term *t = term_get(token, true);
    if (t) bag->ids[bag->count++] = t->id;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Calls fn once per distinct term id of a doc. */
static void for_each_distinct(const Doc *d, void (*fn)(uint32_t id, void *user), void *user) {
    uint32_t tmp[SEARCH_MAX_DOC_TERMS];
    memcpy(tmp, d->terms, d->n_terms * sizeof(uint32_t));
    qsort(tmp, d->n_terms, sizeof(uint32_t), cmp_u32);
    for (uint32_t i = 0; i < d->n_terms; i++)
        if (i == 0 || tmp[i] != tmp[i - 1]) fn(tmp[i], user);
}

static void df_inc(uint32_t id, void *user) { S.by_id[id]->df[*(int *)user]++; }
static void df_dec(uint32_t id, void *user) {
    Term *t = S.by_id[id];
    int pool = *(int *)user;
    if (t->df[pool]) t->df[pool]--;
}

static bool comment_line(const char *line, uint32_t len) {
    while (len && isspace((unsigned char)*line)) { line++; len--; }
    if (!len) return false;
    return (len >= 2 && (!strncmp(line, "//", 2) || !strncmp(line, "/*", 2))) ||
           line[0] == '*' || line[0] == '#';
}

static uint32_t doc_comment_start(const CtxSource *src, uint32_t line) {
    uint32_t start = line;
    while (start > 1 && line - start < 12) {
        uint32_t len = 0;
        const char *t = ctx_source_line(src, start - 1, &len);
        if (!t || !comment_line(t, len)) break;
        start--;
    }
    return start;
}

/* ---- documents (S.lock + graph read lock held) ---------------------------------------- */

static bool is_searchable(const CtxSymbol *s) {
    switch (s->kind) {
    case CTX_SYM_FUNCTION: case CTX_SYM_METHOD:
    case CTX_SYM_CLASS: case CTX_SYM_STRUCT: case CTX_SYM_ENUM: case CTX_SYM_TYPEDEF:
    case CTX_SYM_MACRO: case CTX_SYM_NAMESPACE: case CTX_SYM_VARIABLE:
        return true;
    default:
        return false;
    }
}

static void path_parts(const char *path, const char **stem, size_t *stem_len,
                       const char **dir, size_t *dir_len) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    *stem = base;
    *stem_len = dot ? (size_t)(dot - base) : strlen(base);
    *dir = NULL;
    *dir_len = 0;
    if (slash && slash > path) {
        const char *p = slash - 1;
        while (p > path && *p != '/') p--;
        if (*p == '/') p++;
        *dir = p;
        *dir_len = (size_t)(slash - p);
    }
}

static void build_doc(const CtxSymbol *s, const CtxSource *src, Doc *d) {
    TermBag bag = {0};
    split_words(s->name, strlen(s->name), bag_sink, &bag);
    uint32_t name_terms = bag.count;
    for (uint32_t i = 0; i < name_terms && bag.count < SEARCH_MAX_DOC_TERMS; i++)
        bag.ids[bag.count++] = bag.ids[i];
    char lower[256];
    size_t n = strlen(s->name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)s->name[i]);
    lower[n] = '\0';
    if (name_terms > 1 && bag.count < SEARCH_MAX_DOC_TERMS) {
        Term *full = term_get(lower, true);
        if (full) bag.ids[bag.count++] = full->id;
    }
    uint32_t n_name = bag.count;
    split_words(s->scope, strlen(s->scope), bag_sink, &bag);
    const char *stem, *dir;
    size_t stem_len, dir_len;
    path_parts(s->file, &stem, &stem_len, &dir, &dir_len);
    split_words(stem, stem_len, bag_sink, &bag);
    if (dir) split_words(dir, dir_len, bag_sink, &bag);
    split_words(s->signature, strlen(s->signature), bag_sink, &bag);
    if (src) {
        for (uint32_t n = doc_comment_start(src, s->line); n < s->line; n++) {
            uint32_t len = 0;
            const char *t = ctx_source_line(src, n, &len);
            if (t) split_words(t, len, bag_sink, &bag);
        }
    }

    memset(d, 0, sizeof(*d));
    d->sym_id = s->id;
    d->n_name_terms = (uint16_t)n_name;
    d->n_terms = (uint16_t)bag.count;
    d->terms = bag.count ? (uint32_t *)malloc(bag.count * sizeof(uint32_t)) : NULL;
    if (d->terms) memcpy(d->terms, bag.ids, bag.count * sizeof(uint32_t));
    else d->n_terms = d->n_name_terms = 0;
}

static void doc_file_clear(DocFile *df) {
    int pool = df->vendor ? 1 : 0;
    for (uint32_t i = 0; i < df->count; i++) {
        Doc *d = &df->docs[i];
        for_each_distinct(d, df_dec, &pool);
        S.n_docs[pool]--;
        S.total_len[pool] -= d->n_terms;
        free(d->terms);
    }
    free(df->docs);
    df->docs = NULL;
    df->count = 0;
}

static void doc_file_build(DocFile *df, const CtxGraphFile *f) {
    int pool = df->vendor ? 1 : 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < f->symbol_count; i++) n += is_searchable(f->symbols[i]);
    df->docs = n ? (Doc *)calloc(n, sizeof(Doc)) : NULL;
    df->count = 0;
    CtxSource src;
    bool have_src = n && !df->vendor && ctx_source_open(f->path, &src);
    for (uint32_t i = 0; df->docs && i < f->symbol_count; i++) {
        const CtxSymbol *s = f->symbols[i];
        if (!is_searchable(s)) continue;
        Doc *d = &df->docs[df->count++];
        build_doc(s, have_src ? &src : NULL, d);
        for_each_distinct(d, df_inc, &pool);
        S.n_docs[pool]++;
        S.total_len[pool] += d->n_terms;
    }
    if (have_src) ctx_source_close(&src);
    df->version = f->version;
}

/* Brings the document index in line with the graph (per-file versions). */
static void sync_docs_locked(CtxGraph *g) {
    const char *root = ctx_indexer_root();
    DocFile *df, *dtmp;
    HASH_ITER(hh, S.files, df, dtmp) df->seen = false;

    CtxGraphFile *f, *ftmp;
    HASH_ITER(hh, g->files, f, ftmp) {
        HASH_FIND_STR(S.files, f->path, df);
        if (!df) {
            df = (DocFile *)calloc(1, sizeof(DocFile));
            if (!df) continue;
            df->path = strdup(f->path);
            if (!df->path) { free(df); continue; }
            df->vendor = ctx_path_is_vendor(root, f->path);
            HASH_ADD_KEYPTR(hh, S.files, df->path, strlen(df->path), df);
        }
        df->seen = true;
        if (df->version != f->version || (!df->docs && f->symbol_count)) {
            doc_file_clear(df);
            doc_file_build(df, f);
        }
    }

    HASH_ITER(hh, S.files, df, dtmp) {
        if (df->seen) continue;
        HASH_DEL(S.files, df);
        doc_file_clear(df);
        free(df->path);
        free(df);
    }
}

/* ---- ranking ----------------------------------------------------------------------------- */

typedef struct {
    const DocFile *file;
    uint32_t       index;
    double         score;
} Hit;

static int hit_order(const void *a, const void *b) {
    double x = ((const Hit *)a)->score, y = ((const Hit *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

#define SEARCH_MAX_QUERY_IDS   64u
#define SEARCH_PREFIX_PER_WORD 6u

/*
 * Query words and the dictionary terms they match: the exact term (weight 1)
 * and up to SEARCH_PREFIX_PER_WORD prefix relatives such as defrag ↔
 * defragment (weight 0.5). Matches are grouped per query word.
 */
typedef struct {
    char     words[SEARCH_MAX_QUERY_TERMS][64];
    uint32_t word_count;
    uint32_t ids[SEARCH_MAX_QUERY_IDS];
    uint8_t  group[SEARCH_MAX_QUERY_IDS];
    float    weight[SEARCH_MAX_QUERY_IDS];
    uint32_t count;
    double   idf[SEARCH_MAX_QUERY_TERMS];
    uint32_t groups;          /* words that matched at least one term */
    char     lower_query[512];
} QueryTerms;

static void query_sink(const char *token, size_t len, void *user) {
    QueryTerms *q = (QueryTerms *)user;
    if (q->word_count >= SEARCH_MAX_QUERY_TERMS || len >= sizeof(q->words[0])) return;
    for (uint32_t i = 0; i < q->word_count; i++) if (!strcmp(q->words[i], token)) return;
    memcpy(q->words[q->word_count], token, len);
    q->words[q->word_count][len] = '\0';
    q->word_count++;
}

static void query_add_id(QueryTerms *q, uint32_t id, uint8_t group, float weight) {
    if (q->count >= SEARCH_MAX_QUERY_IDS) return;
    for (uint32_t i = 0; i < q->count; i++) if (q->ids[i] == id) return;
    q->ids[q->count] = id;
    q->group[q->count] = group;
    q->weight[q->count] = weight;
    q->count++;
}

/* Common code abbreviations; a query word matches its counterpart at 0.8. */
static const char *const k_abbreviations[][2] = {
    { "attention", "attn" }, { "context", "ctx" }, { "buffer", "buf" }, { "index", "idx" },
    { "initialize", "init" }, { "allocate", "alloc" }, { "parameter", "param" },
    { "argument", "arg" }, { "function", "func" }, { "length", "len" }, { "number", "num" },
    { "string", "str" }, { "message", "msg" }, { "error", "err" }, { "memory", "mem" },
    { "pointer", "ptr" }, { "temporary", "tmp" }, { "source", "src" }, { "destination", "dst" },
    { "request", "req" }, { "response", "resp" }, { "connection", "conn" },
    { "database", "db" }, { "directory", "dir" }, { "command", "cmd" },
    { "environment", "env" }, { "configuration", "config" }, { "config", "cfg" },
    { "implementation", "impl" }, { "utility", "util" }, { "library", "lib" },
    { "position", "pos" }, { "previous", "prev" }, { "current", "cur" },
    { "embedding", "embd" }, { "token", "tok" }, { "sequence", "seq" },
    { "vocabulary", "vocab" }, { "quantize", "quant" }, { "authentication", "auth" },
    { "reference", "ref" }, { "document", "doc" }, { "information", "info" },
    { "statistics", "stats" }, { "synchronize", "sync" }, { "character", "char" },
    { "callback", "cb" }, { "descriptor", "desc" }, { "object", "obj" },
    { "variable", "var" }, { "value", "val" }, { "count", "cnt" }, { "address", "addr" },
    { "database", "store" }, { "persist", "store" }, { "generate", "gen" },
};

/* Adds abbreviation/expansion counterparts of word to group. S.lock held. */
static void add_abbreviations(QueryTerms *q, const char *word, uint8_t group) {
    for (size_t i = 0; i < CTX_ARRAY_LEN(k_abbreviations); i++) {
        for (int side = 0; side < 2; side++) {
            char a[64], b[64];
            size_t al = normalize_token(k_abbreviations[i][side], strlen(k_abbreviations[i][side]), a, sizeof(a));
            if (!al || strcmp(a, word) != 0) continue;
            size_t bl = normalize_token(k_abbreviations[i][1 - side], strlen(k_abbreviations[i][1 - side]), b, sizeof(b));
            if (!bl) continue;
            Term *t = term_get(b, false);
            if (t && (t->df[0] || t->df[1])) query_add_id(q, t->id, group, 0.8f);
        }
    }
}

/* Maps query words to dictionary terms and computes per-word idf. S.lock held. */
static void expand_query_locked(QueryTerms *q, bool include_vendor) {
    double docs = (double)S.n_docs[0] + (include_vendor ? (double)S.n_docs[1] : 0.0);
    uint32_t groups = 0;
    for (uint32_t w = 0; w < q->word_count; w++) {
        const char *word = q->words[w];
        size_t wl = strlen(word);
        uint32_t before = q->count;
        double best_df = -1.0;
        Term *exact = term_get(word, false);
        if (exact) {
            query_add_id(q, exact->id, (uint8_t)groups, 1.0f);
            best_df = (double)exact->df[0] + (include_vendor ? (double)exact->df[1] : 0.0);
        }
        add_abbreviations(q, word, (uint8_t)groups);
        if (best_df < 0 && q->count > before) {
            const Term *t = S.by_id[q->ids[before]];
            best_df = (double)t->df[0] + (include_vendor ? (double)t->df[1] : 0.0);
        }
        uint32_t prefix = 0;
        for (uint32_t t = 0; wl >= 4 && t < S.term_count && prefix < SEARCH_PREFIX_PER_WORD; t++) {
            const Term *term = S.by_id[t];
            size_t tl = strlen(term->text);
            if (tl < 4 || tl == wl) continue;
            size_t shorter = tl < wl ? tl : wl;
            if (strncmp(term->text, word, shorter) != 0) continue;
            double df = (double)term->df[0] + (include_vendor ? (double)term->df[1] : 0.0);
            if (df <= 0) continue;
            query_add_id(q, term->id, (uint8_t)groups, 0.5f);
            if (best_df < 0) best_df = df;
            prefix++;
        }
        if (q->count == before) continue;
        q->idf[groups] = log(1.0 + (docs - best_df + 0.5) / (best_df + 0.5));
        groups++;
    }
    q->groups = groups;
}

/* Names that are clearly identifiers (snake_case, camelCase, digits) rather
 * than plain words that also occur in natural-language questions. */
static bool is_identifier_like(const char *name) {
    bool lower_seen = false;
    for (const char *p = name; *p; p++) {
        if (*p == '_' || isdigit((unsigned char)*p)) return p != name;
        if (islower((unsigned char)*p)) lower_seen = true;
        else if (isupper((unsigned char)*p) && lower_seen) return true;
    }
    return false;
}

/* True when an identifier-like symbol name appears in the query as a whole
 * identifier (case-insensitive; name and lower_query are lowercase). */
static bool query_names_symbol(const char *lower_query, const char *name) {
    size_t n = strlen(name);
    if (n < 3) return false;
    for (const char *p = lower_query; (p = strstr(p, name)) != NULL; p++) {
        bool left = p == lower_query || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        bool right = !(isalnum((unsigned char)p[n]) || p[n] == '_');
        if (left && right) return true;
    }
    return false;
}

typedef struct {
    Hit     *items;
    uint32_t count;
    uint32_t cap;
} HitList;

static void hits_add(HitList *l, const DocFile *f, uint32_t index, double score) {
    if (l->count >= l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 1024;
        Hit *next = (Hit *)realloc(l->items, cap * sizeof(Hit));
        if (!next) return;
        l->items = next;
        l->cap = cap;
    }
    l->items[l->count++] = (Hit){ f, index, score };
}

static void hits_top(HitList *l, uint32_t keep) {
    if (l->count > 1) qsort(l->items, l->count, sizeof(Hit), hit_order);
    if (l->count > keep) l->count = keep;
}

/* BM25 over name/scope/path/signature terms with a name-match bonus. */
static void rank_bm25_locked(CtxGraph *g, const QueryTerms *q, bool include_vendor, HitList *out) {
    double n = (double)S.n_docs[0] + (include_vendor ? (double)S.n_docs[1] : 0.0);
    double len = (double)S.total_len[0] + (include_vendor ? (double)S.total_len[1] : 0.0);
    double avgdl = n > 0 ? len / n : 1.0;
    if (q->groups == 0 || n <= 0) return;

    DocFile *df, *tmp;
    HASH_ITER(hh, S.files, df, tmp) {
        if (df->vendor && !include_vendor) continue;
        for (uint32_t i = 0; i < df->count; i++) {
            const Doc *d = &df->docs[i];
            double tf[SEARCH_MAX_QUERY_TERMS] = {0};
            bool name_hit[SEARCH_MAX_QUERY_TERMS] = {0};
            bool any = false;
            for (uint32_t t = 0; t < d->n_terms; t++) {
                for (uint32_t k = 0; k < q->count; k++) {
                    if (d->terms[t] != q->ids[k]) continue;
                    tf[q->group[k]] += q->weight[k];
                    if (t < d->n_name_terms) name_hit[q->group[k]] = true;
                    any = true;
                }
            }
            if (!any) continue;
            double score = 0.0;
            uint32_t matched = 0;
            for (uint32_t w = 0; w < q->groups; w++) {
                if (tf[w] <= 0.0) continue;
                matched++;
                score += q->idf[w] * tf[w] * (SEARCH_BM25_K1 + 1.0) /
                         (tf[w] + SEARCH_BM25_K1 * (1.0 - SEARCH_BM25_B + SEARCH_BM25_B * d->n_terms / avgdl));
                if (name_hit[w]) score += 0.5;
            }
            double coverage = (double)matched / (double)q->groups;
            score *= coverage * coverage;
            const CtxSymbol *s = ctx_graph_find_by_id_locked(g, d->sym_id);
            if (s) {
                char lower[256];
                size_t ln = strlen(s->name);
                if (ln >= sizeof(lower)) ln = sizeof(lower) - 1;
                for (size_t c = 0; c < ln; c++) lower[c] = (char)tolower((unsigned char)s->name[c]);
                lower[ln] = '\0';
                if (is_identifier_like(s->name) && query_names_symbol(q->lower_query, lower)) score += 12.0;
                if (s->kind == CTX_SYM_FUNCTION || s->kind == CTX_SYM_METHOD) score += 0.3;
            }
            if (df->vendor) score *= 0.5;
            hits_add(out, df, i, score);
        }
    }
    hits_top(out, SEARCH_CANDIDATES);
}

static void rank_embedding_locked(const float *qvec, uint32_t dim, bool include_vendor, HitList *out) {
    DocFile *df, *tmp;
    HASH_ITER(hh, S.files, df, tmp) {
        if (df->vendor && !include_vendor) continue;
        for (uint32_t i = 0; i < df->count; i++) {
            const float *v = df->docs[i].vec;
            if (!v) continue;
            double dot = 0.0;
            for (uint32_t k = 0; k < dim; k++) dot += (double)qvec[k] * v[k];
            hits_add(out, df, i, dot);
        }
    }
    hits_top(out, SEARCH_CANDIDATES);
}

/* ---- candidates -------------------------------------------------------------------------- */

typedef struct {
    uint64_t      sym_id;
    char         *path;
    char          name[256];
    char          signature[512];
    CtxSymbolKind kind;
    uint32_t      line;
    uint32_t      end_line;
    double        fused;
    float         rerank;
    double        final;
} Cand;

static void cands_free(Cand *c, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) free(c[i].path);
    free(c);
}

typedef struct {
    const DocFile *file;
    uint32_t       index;
} FuseKey;

typedef struct FuseEntry {
    FuseKey        key;
    double         score;
    UT_hash_handle hh;
} FuseEntry;

static void fuse_list(FuseEntry **map, const HitList *l) {
    for (uint32_t r = 0; r < l->count; r++) {
        FuseKey key;
        memset(&key, 0, sizeof(key));
        key.file = l->items[r].file;
        key.index = l->items[r].index;
        FuseEntry *e = NULL;
        HASH_FIND(hh, *map, &key, sizeof(FuseKey), e);
        if (!e) {
            e = (FuseEntry *)calloc(1, sizeof(FuseEntry));
            if (!e) continue;
            e->key = key;
            HASH_ADD(hh, *map, key, sizeof(FuseKey), e);
        }
        e->score += 1.0 / (SEARCH_RRF_K + r + 1);
    }
}

static int fuse_order(const void *a, const void *b) {
    double x = (*(FuseEntry *const *)a)->score, y = (*(FuseEntry *const *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* A prototype resolves to its definition (body) when one is indexed. */
static const CtxSymbol *canonical_symbol_locked(CtxGraph *g, const CtxSymbol *s) {
    if (s->is_definition || (s->kind != CTX_SYM_FUNCTION && s->kind != CTX_SYM_METHOD)) return s;
    const CtxNameEntry *n = ctx_graph_find_name_locked(g, s->name);
    const CtxSymbol *best = NULL;
    for (uint32_t i = 0; n && i < n->count; i++) {
        const CtxSymbol *c = n->symbols[i];
        if (!c->is_definition || c->kind != s->kind || c->lang != s->lang) continue;
        if (!best || (c->end_line - c->line) > (best->end_line - best->line)) best = c;
    }
    return best ? best : s;
}

/* Fuses ranked lists (RRF) and copies the top candidates out of the index. */
static uint32_t fuse_candidates_locked(CtxGraph *g, const HitList *bm25, const HitList *emb,
                                       Cand **out) {
    FuseEntry *map = NULL;
    fuse_list(&map, bm25);
    fuse_list(&map, emb);
    uint32_t n = HASH_COUNT(map);
    FuseEntry **arr = n ? (FuseEntry **)malloc(n * sizeof(FuseEntry *)) : NULL;
    uint32_t k = 0;
    FuseEntry *e, *tmp;
    HASH_ITER(hh, map, e, tmp) if (arr) arr[k++] = e;
    if (arr && k > 1) qsort(arr, k, sizeof(FuseEntry *), fuse_order);

    uint32_t keep = k < SEARCH_FUSED_POOL ? k : SEARCH_FUSED_POOL;
    Cand *c = keep ? (Cand *)calloc(keep, sizeof(Cand)) : NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; c && i < keep; i++) {
        const Doc *d = &arr[i]->key.file->docs[arr[i]->key.index];
        const CtxSymbol *s = ctx_graph_find_by_id_locked(g, d->sym_id);
        if (!s) continue;
        s = canonical_symbol_locked(g, s);
        bool duplicate = false;
        for (uint32_t j = 0; j < count && !duplicate; j++) duplicate = c[j].sym_id == s->id;
        if (duplicate) continue;
        Cand *cd = &c[count];
        cd->path = strdup(s->file);
        if (!cd->path) continue;
        cd->sym_id = s->id;
        snprintf(cd->name, sizeof(cd->name), "%s", s->name);
        snprintf(cd->signature, sizeof(cd->signature), "%s", s->signature);
        cd->kind = s->kind;
        cd->line = s->line;
        cd->end_line = s->end_line;
        cd->fused = arr[i]->score;
        count++;
    }
    HASH_ITER(hh, map, e, tmp) { HASH_DEL(map, e); free(e); }
    free(arr);
    *out = c;
    return count;
}

/* ---- rendering helpers ------------------------------------------------------------------- */

static const char *kind_label(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION:  return "fn";
    case CTX_SYM_METHOD:    return "method";
    case CTX_SYM_CLASS:     return "class";
    case CTX_SYM_STRUCT:    return "struct";
    case CTX_SYM_ENUM:      return "enum";
    case CTX_SYM_TYPEDEF:   return "typedef";
    case CTX_SYM_VARIABLE:  return "var";
    case CTX_SYM_MACRO:     return "macro";
    case CTX_SYM_NAMESPACE: return "namespace";
    default:                return "symbol";
    }
}

static void one_line(const char *text, char *out, size_t out_size, size_t max_chars) {
    size_t w = 0;
    bool space = false;
    for (const char *p = text; *p && w + 4 < out_size && w < max_chars; p++) {
        if (*p == '{') break;
        if (isspace((unsigned char)*p)) { space = w > 0; continue; }
        if (space) { out[w++] = ' '; space = false; }
        out[w++] = *p;
    }
    while (w > 0 && (out[w - 1] == ';' || out[w - 1] == ' ')) w--;
    out[w] = '\0';
}

/* First meaningful comment text above a symbol (trimmed), or "". */
static void doc_summary(const CtxSource *src, uint32_t line, char *out, size_t out_size) {
    out[0] = '\0';
    uint32_t start = doc_comment_start(src, line);
    for (uint32_t n = start; n < line; n++) {
        uint32_t len = 0;
        const char *t = ctx_source_line(src, n, &len);
        while (len && (isspace((unsigned char)*t) || *t == '/' || *t == '*' || *t == '#')) { t++; len--; }
        while (len && (isspace((unsigned char)t[len - 1]) || t[len - 1] == '/' || t[len - 1] == '*')) len--;
        if (len < 4) continue;
        if (len > 140) len = 140;
        snprintf(out, out_size, "%.*s", (int)len, t);
        return;
    }
}

/*
 * Text a model sees for a symbol: path, signature, doc comment, and body head,
 * capped at max_bytes.
 */
static char *symbol_text(const char *root, const CtxSource *src, const char *path, const char *name,
                         const char *signature, uint32_t line, uint32_t end_line, size_t max_bytes) {
    CtxBuf b = {0};
    char rel[4096], sig[640];
    ctx_path_display(root, path, rel, sizeof(rel));
    one_line(signature, sig, sizeof(sig), 400);
    ctx_buf_printf(&b, "%s\n%s: %s\n", rel, name, sig);
    if (src) {
        uint32_t from = doc_comment_start(src, line);
        uint32_t to = end_line < from + SEARCH_DOC_TEXT_LINES ? end_line : from + SEARCH_DOC_TEXT_LINES;
        for (uint32_t n = from; n <= to && b.len < max_bytes; n++) {
            uint32_t len = 0;
            const char *t = ctx_source_line(src, n, &len);
            if (!t) break;
            ctx_buf_append(&b, t, len);
            ctx_buf_append(&b, "\n", 1);
        }
    }
    if (b.len > max_bytes) { b.len = max_bytes; b.data[b.len] = '\0'; }
    return ctx_buf_take(&b);
}

/* ---- search -------------------------------------------------------------------------------- */

static int cand_final_order(const void *a, const void *b) {
    double x = ((const Cand *)a)->final, y = ((const Cand *)b)->final;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int cand_rerank_order(const void *a, const void *b) {
    float x = (*(Cand *const *)a)->rerank, y = (*(Cand *const *)b)->rerank;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Rescores candidates with the cross-encoder; blends rerank and fused ranks. */
static bool rerank_candidates(const char *root, const char *query, Cand *c, uint32_t n) {
    uint32_t pool = n < SEARCH_RERANK_POOL ? n : SEARCH_RERANK_POOL;
    if (pool < 2 || ctx_models_state(CTX_MODEL_RERANK, NULL, 0) != CTX_MODEL_READY) return false;
    char **texts = (char **)calloc(pool, sizeof(char *));
    float *scores = (float *)calloc(pool, sizeof(float));
    Cand **order = (Cand **)calloc(pool, sizeof(Cand *));
    bool ok = texts && scores && order;
    for (uint32_t i = 0; ok && i < pool; i++) {
        CtxSource src;
        bool have = ctx_source_open(c[i].path, &src);
        texts[i] = symbol_text(root, have ? &src : NULL, c[i].path, c[i].name, c[i].signature,
                               c[i].line, c[i].end_line, SEARCH_RERANK_TEXT_BYTES);
        if (have) ctx_source_close(&src);
        ok = texts[i] != NULL;
    }
    if (ok) ok = ctx_models_rerank(query, (const char *const *)texts, pool, scores);
    if (ok) {
        for (uint32_t i = 0; i < pool; i++) { c[i].rerank = scores[i]; order[i] = &c[i]; }
        qsort(order, pool, sizeof(Cand *), cand_rerank_order);
        for (uint32_t r = 0; r < pool; r++) order[r]->final += 2.0 / (SEARCH_RRF_K + r + 1);
        for (uint32_t i = 0; i < n; i++) c[i].final += 1.0 / (SEARCH_RRF_K + i + 1);
        for (uint32_t i = pool; i < n; i++) c[i].final -= 1.0;
    }
    for (uint32_t i = 0; texts && i < pool; i++) free(texts[i]);
    free(texts);
    free(scores);
    free(order);
    return ok;
}

static void render_results(CtxBuf *b, const char *root, const char *query, const char *ranking,
                           const Cand *c, uint32_t n, uint32_t k, uint32_t bodies) {
    ctx_buf_printf(b, "search \"%s\" (%s)\n", query, ranking);
    if (n == 0) {
        ctx_buf_printf(b, "no matches; try identifiers or include_vendor=true\n");
        return;
    }
    for (uint32_t i = 0; i < n && i < k; i++) {
        char rel[4096], sig[640], summary[160];
        ctx_path_display(root, c[i].path, rel, sizeof(rel));
        one_line(c[i].signature, sig, sizeof(sig), 200);
        ctx_buf_printf(b, "%u. %s %s  %s:%u", i + 1, kind_label(c[i].kind), c[i].name, rel, c[i].line);
        if (c[i].end_line > c[i].line) ctx_buf_printf(b, "-%u", c[i].end_line);
        ctx_buf_printf(b, "\n   %s\n", sig);

        CtxSource src;
        if (!ctx_source_open(c[i].path, &src)) continue;
        doc_summary(&src, c[i].line, summary, sizeof(summary));
        if (summary[0]) ctx_buf_printf(b, "   // %s\n", summary);
        if (i < bodies && c[i].end_line > c[i].line) {
            uint32_t to = c[i].end_line;
            uint32_t cap = c[i].line + SEARCH_EXCERPT_LINES - 1;
            if (to > cap) to = cap;
            int width = 1;
            for (uint32_t v = to; v >= 10; v /= 10) width++;
            for (uint32_t ln = c[i].line; ln <= to; ln++) {
                uint32_t len = 0;
                const char *t = ctx_source_line(&src, ln, &len);
                if (!t) break;
                ctx_buf_printf(b, "   %*u  %.*s\n", width, ln, (int)len, t);
            }
            if (to < c[i].end_line)
                ctx_buf_printf(b, "   ... %u more lines (source symbol=%s)\n", c[i].end_line - to, c[i].name);
        }
        ctx_source_close(&src);
    }
}

static char *search_once(CtxGraph *g, const CtxSearchRequest *req, bool *refreshed) {
    const char *root = ctx_indexer_root();
    uint32_t k = req->k ? (req->k > 20 ? 20 : req->k) : 5;
    uint32_t bodies = req->bodies > k ? k : req->bodies;

    uint32_t dim = ctx_models_embed_dim();
    float *qvec = NULL;
    if (dim) {
        qvec = (float *)malloc(dim * sizeof(float));
        const char *texts[1] = { req->query };
        if (qvec && !ctx_models_embed(texts, 1, qvec)) { free(qvec); qvec = NULL; }
    }

    QueryTerms q = {0};
    size_t ql = strlen(req->query);
    if (ql >= sizeof(q.lower_query)) ql = sizeof(q.lower_query) - 1;
    for (size_t i = 0; i < ql; i++) q.lower_query[i] = (char)tolower((unsigned char)req->query[i]);
    split_words(req->query, strlen(req->query), query_sink, &q);

    HitList bm25 = {0}, emb = {0};
    Cand *cands = NULL;
    uint32_t n = 0;

    pthread_mutex_lock(&S.lock);
    ctx_graph_rlock(g);
    sync_docs_locked(g);
    expand_query_locked(&q, req->include_vendor);
    rank_bm25_locked(g, &q, req->include_vendor, &bm25);
    if (qvec && S.dim == dim) rank_embedding_locked(qvec, dim, req->include_vendor, &emb);
    n = fuse_candidates_locked(g, &bm25, &emb, &cands);
    ctx_graph_runlock(g);
    pthread_mutex_unlock(&S.lock);
    free(bm25.items);
    free(emb.items);
    free(qvec);

    uint32_t fresh_n = n < k ? n : k;
    if (!*refreshed && fresh_n) {
        const char **paths = (const char **)malloc(fresh_n * sizeof(char *));
        for (uint32_t i = 0; paths && i < fresh_n; i++) paths[i] = cands[i].path;
        uint32_t updated = paths ? ctx_indexer_ensure_fresh(paths, fresh_n) : 0;
        free(paths);
        if (updated) {
            *refreshed = true;
            cands_free(cands, n);
            return NULL;
        }
    }

    for (uint32_t i = 0; i < n; i++) cands[i].final = cands[i].fused;
    bool reranked = rerank_candidates(root, req->query, cands, n);
    for (uint32_t i = 0; i < n; i++) {
        char lower[256];
        size_t ln = strlen(cands[i].name);
        if (ln >= sizeof(lower)) ln = sizeof(lower) - 1;
        for (size_t c = 0; c < ln; c++) lower[c] = (char)tolower((unsigned char)cands[i].name[c]);
        lower[ln] = '\0';
        if (is_identifier_like(cands[i].name) && query_names_symbol(q.lower_query, lower))
            cands[i].final += 10.0;
    }
    if (n > 1) qsort(cands, n, sizeof(Cand), cand_final_order);

    const char *ranking = reranked ? (emb.count ? "bm25+embedding+rerank" : "bm25+rerank")
                                   : (emb.count ? "bm25+embedding" : "bm25");
    CtxBuf b = {0};
    render_results(&b, root, req->query, ranking, cands, n, k, bodies);
    cands_free(cands, n);
    return ctx_buf_take(&b);
}

char *ctx_search(CtxGraph *g, const CtxSearchRequest *req) {
    if (!g || !req || !req->query || !req->query[0]) {
        CtxBuf b = {0};
        ctx_buf_printf(&b, "error: missing query\n");
        return ctx_buf_take(&b);
    }
    bool refreshed = false;
    char *out = search_once(g, req, &refreshed);
    if (!out) out = search_once(g, req, &refreshed);
    return out;
}

/* ---- embedding worker ---------------------------------------------------------------------- */

static void cache_visit(uint64_t key, const float *vec, uint32_t dim, void *user) {
    CTX_UNUSED(user);
    VecEntry *e = NULL;
    HASH_FIND(hh, S.vecs, &key, sizeof(uint64_t), e);
    if (e) return;
    e = (VecEntry *)calloc(1, sizeof(VecEntry));
    if (!e) return;
    e->vec = (float *)malloc(dim * sizeof(float));
    if (!e->vec) { free(e); return; }
    memcpy(e->vec, vec, dim * sizeof(float));
    e->key = key;
    HASH_ADD(hh, S.vecs, key, sizeof(uint64_t), e);
}

typedef struct {
    char    *path;
    uint64_t version;
    uint32_t index;
    uint64_t sym_id;
    char     name[256];
    char     signature[512];
    uint32_t line;
    uint32_t end_line;
    char    *text;
    uint64_t key;
} Pending;

static void pending_free(Pending *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { free(p[i].path); free(p[i].text); }
}

/* Collects up to max project docs lacking vectors. S.lock + graph rlock held. */
static uint32_t collect_missing_locked(CtxGraph *g, Pending *out, uint32_t max) {
    uint32_t n = 0;
    DocFile *df, *tmp;
    HASH_ITER(hh, S.files, df, tmp) {
        if (df->vendor) continue;
        for (uint32_t i = 0; i < df->count && n < max; i++) {
            Doc *d = &df->docs[i];
            if (d->vec || d->emb_failed) continue;
            const CtxSymbol *s = ctx_graph_find_by_id_locked(g, d->sym_id);
            if (!s) { d->emb_failed = true; continue; }
            Pending *p = &out[n];
            memset(p, 0, sizeof(*p));
            p->path = strdup(df->path);
            if (!p->path) continue;
            p->version = df->version;
            p->index = i;
            p->sym_id = d->sym_id;
            snprintf(p->name, sizeof(p->name), "%s", s->name);
            snprintf(p->signature, sizeof(p->signature), "%s", s->signature);
            p->line = s->line;
            p->end_line = s->end_line;
            n++;
        }
        if (n >= max) break;
    }
    return n;
}

/* Attaches a vector to the doc a pending item came from, if it still exists. */
static void attach_locked(const Pending *p, const float *vec, bool failed) {
    DocFile *df = NULL;
    HASH_FIND_STR(S.files, p->path, df);
    if (!df || df->version != p->version || p->index >= df->count) return;
    Doc *d = &df->docs[p->index];
    if (d->sym_id != p->sym_id) return;
    d->emb_key = p->key;
    d->vec = vec;
    d->emb_failed = failed;
}

static VecEntry *cache_put_locked(uint64_t key, const float *vec, uint32_t dim) {
    VecEntry *e = NULL;
    HASH_FIND(hh, S.vecs, &key, sizeof(uint64_t), e);
    if (e) return e;
    e = (VecEntry *)calloc(1, sizeof(VecEntry));
    if (!e) return NULL;
    e->vec = (float *)malloc(dim * sizeof(float));
    if (!e->vec) { free(e); return NULL; }
    memcpy(e->vec, vec, dim * sizeof(float));
    e->key = key;
    HASH_ADD(hh, S.vecs, key, sizeof(uint64_t), e);
    return e;
}

/* Embeds one batch of missing docs. Returns false when nothing was pending. */
static bool embed_batch(CtxGraph *g, uint32_t dim, const char *model_id) {
    Pending pend[SEARCH_EMBED_BATCH];
    pthread_mutex_lock(&S.lock);
    ctx_graph_rlock(g);
    sync_docs_locked(g);
    uint32_t n = collect_missing_locked(g, pend, SEARCH_EMBED_BATCH);
    ctx_graph_runlock(g);
    pthread_mutex_unlock(&S.lock);
    if (n == 0) return false;

    const char *root = ctx_indexer_root();
    const char *texts[SEARCH_EMBED_BATCH];
    uint32_t miss_idx[SEARCH_EMBED_BATCH];
    uint32_t misses = 0;
    for (uint32_t i = 0; i < n; i++) {
        CtxSource src;
        bool have = ctx_source_open(pend[i].path, &src);
        pend[i].text = symbol_text(root, have ? &src : NULL, pend[i].path, pend[i].name,
                                   pend[i].signature, pend[i].line, pend[i].end_line,
                                   SEARCH_EMBED_TEXT_BYTES);
        if (have) ctx_source_close(&src);
        CtxBuf keybuf = {0};
        ctx_buf_printf(&keybuf, "%s\n%s", model_id, pend[i].text ? pend[i].text : "");
        pend[i].key = ctx_fnv64(keybuf.data ? keybuf.data : "", keybuf.len);
        free(keybuf.data);
    }

    pthread_mutex_lock(&S.lock);
    for (uint32_t i = 0; i < n; i++) {
        VecEntry *e = NULL;
        HASH_FIND(hh, S.vecs, &pend[i].key, sizeof(uint64_t), e);
        if (e) { e->live = true; attach_locked(&pend[i], e->vec, false); }
        else if (pend[i].text) { texts[misses] = pend[i].text; miss_idx[misses++] = i; }
        else attach_locked(&pend[i], NULL, true);
    }
    pthread_mutex_unlock(&S.lock);

    if (misses) {
        float *vecs = (float *)malloc((size_t)misses * dim * sizeof(float));
        bool ok = vecs && ctx_models_embed(texts, misses, vecs);
        uint64_t keys[SEARCH_EMBED_BATCH];
        for (uint32_t m = 0; m < misses; m++) keys[m] = pend[miss_idx[m]].key;
        if (ok) ctx_store_embedding_put(keys, vecs, misses, dim);
        pthread_mutex_lock(&S.lock);
        for (uint32_t m = 0; m < misses; m++) {
            VecEntry *e = ok ? cache_put_locked(keys[m], vecs + (size_t)m * dim, dim) : NULL;
            if (e) e->live = true;
            attach_locked(&pend[miss_idx[m]], e ? e->vec : NULL, e == NULL);
        }
        pthread_mutex_unlock(&S.lock);
        free(vecs);
    }
    pending_free(pend, n);
    return true;
}

/* Drops cached vectors no current doc uses, in memory and in the store. */
static void prune_cache(void) {
    pthread_mutex_lock(&S.lock);
    VecEntry *e, *tmp;
    HASH_ITER(hh, S.vecs, e, tmp) e->live = false;
    DocFile *df, *dtmp;
    HASH_ITER(hh, S.files, df, dtmp) {
        for (uint32_t i = 0; i < df->count; i++) {
            if (!df->docs[i].vec) continue;
            HASH_FIND(hh, S.vecs, &df->docs[i].emb_key, sizeof(uint64_t), e);
            if (e) e->live = true;
        }
    }
    uint32_t live = 0;
    HASH_ITER(hh, S.vecs, e, tmp) live += e->live;
    uint64_t *keys = live ? (uint64_t *)malloc(live * sizeof(uint64_t)) : NULL;
    uint32_t k = 0;
    HASH_ITER(hh, S.vecs, e, tmp) {
        if (e->live) { if (keys) keys[k++] = e->key; continue; }
        HASH_DEL(S.vecs, e);
        free(e->vec);
        free(e);
    }
    pthread_mutex_unlock(&S.lock);
    if (keys || live == 0) ctx_store_embedding_retain(keys, k);
    free(keys);
}

static void *worker_main(void *arg) {
    CtxGraph *g = (CtxGraph *)arg;
    bool pruned = false;
    for (;;) {
        pthread_mutex_lock(&S.wake_lock);
        while (!S.dirty && !S.stop) pthread_cond_wait(&S.wake, &S.wake_lock);
        bool stop = S.stop;
        S.dirty = false;
        pthread_mutex_unlock(&S.wake_lock);
        if (stop) break;

        uint32_t dim = ctx_models_embed_dim();
        if (!dim) continue;
        char model_id[256];
        snprintf(model_id, sizeof(model_id), "%s", ctx_models_embed_id());
        if (!S.cache_loaded) {
            pthread_mutex_lock(&S.lock);
            S.dim = dim;
            ctx_store_embedding_load_all(dim, cache_visit, NULL);
            S.cache_loaded = true;
            pthread_mutex_unlock(&S.lock);
        }
        bool worked = false;
        while (!S.stop && embed_batch(g, dim, model_id)) worked = true;
        if (!S.stop && (worked || !pruned)) {
            prune_cache();
            pruned = true;
            uint32_t embedded = 0, total = 0;
            ctx_search_embedding_progress(&embedded, &total);
            CTX_LOG_INFO("Embeddings current: %u/%u project symbols", embedded, total);
        }
    }
    return NULL;
}

static void on_wake_event(const CtxEvent *event, void *user) {
    CTX_UNUSED(event);
    CTX_UNUSED(user);
    pthread_mutex_lock(&S.wake_lock);
    S.dirty = true;
    pthread_cond_signal(&S.wake);
    pthread_mutex_unlock(&S.wake_lock);
}

void ctx_search_start(CtxGraph *g) {
    if (!g || S.worker_started) return;
    S.graph = g;
    S.stop = false;
    S.graph_sub = ctx_event_subscribe(CTX_EVENT_GRAPH_UPDATED, on_wake_event, NULL);
    S.model_sub = ctx_event_subscribe(CTX_EVENT_MODEL_READY, on_wake_event, NULL);
    if (pthread_create(&S.worker, NULL, worker_main, g) != 0) {
        CTX_LOG_WARN("Cannot start embedding worker; search runs without embeddings");
        return;
    }
    S.worker_started = true;
    on_wake_event(NULL, NULL);
}

void ctx_search_stop(void) {
    if (S.graph_sub) ctx_event_unsubscribe(CTX_EVENT_GRAPH_UPDATED, S.graph_sub);
    if (S.model_sub) ctx_event_unsubscribe(CTX_EVENT_MODEL_READY, S.model_sub);
    S.graph_sub = S.model_sub = CTX_EVENT_HANDLE_INVALID;
    if (S.worker_started) {
        pthread_mutex_lock(&S.wake_lock);
        S.stop = true;
        pthread_cond_signal(&S.wake);
        pthread_mutex_unlock(&S.wake_lock);
        pthread_join(S.worker, NULL);
        S.worker_started = false;
    }

    pthread_mutex_lock(&S.lock);
    DocFile *df, *dtmp;
    HASH_ITER(hh, S.files, df, dtmp) {
        HASH_DEL(S.files, df);
        doc_file_clear(df);
        free(df->path);
        free(df);
    }
    Term *t, *ttmp;
    HASH_ITER(hh, S.terms, t, ttmp) { HASH_DEL(S.terms, t); free(t->text); free(t); }
    free(S.by_id);
    S.by_id = NULL;
    S.term_count = S.term_cap = 0;
    VecEntry *e, *etmp;
    HASH_ITER(hh, S.vecs, e, etmp) { HASH_DEL(S.vecs, e); free(e->vec); free(e); }
    S.cache_loaded = false;
    memset(S.n_docs, 0, sizeof(S.n_docs));
    memset(S.total_len, 0, sizeof(S.total_len));
    pthread_mutex_unlock(&S.lock);
}

void ctx_search_embedding_progress(uint32_t *embedded, uint32_t *total) {
    uint32_t e = 0, t = 0;
    pthread_mutex_lock(&S.lock);
    DocFile *df, *tmp;
    HASH_ITER(hh, S.files, df, tmp) {
        if (df->vendor) continue;
        t += df->count;
        for (uint32_t i = 0; i < df->count; i++) e += df->docs[i].vec != NULL;
    }
    pthread_mutex_unlock(&S.lock);
    if (embedded) *embedded = e;
    if (total) *total = t;
}

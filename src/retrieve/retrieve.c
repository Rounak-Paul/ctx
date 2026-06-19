#include "retrieve.h"
#include "../log/log.h"
#include <ctype.h>

/* ---- tunables ---- */
#define CTX_MAX_QUERY_TERMS   24
#define CTX_MAX_SEEDS         64    /* top text/graph matches kept before packing */
#define CTX_MAX_NEIGHBORS     8     /* callers+callees+refs expanded per seed      */
#define CTX_DEFAULT_BUDGET    2000  /* approx tokens when caller passes 0          */
#define CTX_CHARS_PER_TOKEN   4     /* coarse token estimate for budget packing    */
#define CTX_MAX_SNIPPET_LINES 28    /* hard cap on lines read per symbol slice     */

/* ====================================================================== */
/* growable string builder                                                */
/* ====================================================================== */
typedef struct { char *buf; size_t len; size_t cap; } SB;

static void sb_init(SB *s) {
    s->cap = 8192; s->len = 0;
    s->buf = (char *)malloc(s->cap);
    if (s->buf) s->buf[0] = '\0';
}
static void sb_reserve(SB *s, size_t extra) {
    if (!s->buf) return;
    if (s->len + extra + 1 > s->cap) {
        size_t ncap = (s->len + extra + 1) * 2;
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
    char tmp[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_puts(s, tmp);
}
/* Append a JSON-escaped string (no surrounding quotes). */
static void sb_json_escape(SB *s, const char *str) {
    if (!str) return;
    for (const char *p = str; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_puts(s, "\\\""); break;
        case '\\': sb_puts(s, "\\\\"); break;
        case '\n': sb_puts(s, "\\n");  break;
        case '\r': sb_puts(s, "\\r");  break;
        case '\t': sb_puts(s, "\\t");  break;
        default:
            if (c < 0x20) sb_printf(s, "\\u%04x", c);
            else { char b[2] = { (char)c, 0 }; sb_puts(s, b); }
        }
    }
}

static const char *kind_str(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION:  return "function";
    case CTX_SYM_METHOD:    return "method";
    case CTX_SYM_CLASS:     return "class";
    case CTX_SYM_STRUCT:    return "struct";
    case CTX_SYM_ENUM:      return "enum";
    case CTX_SYM_TYPEDEF:   return "typedef";
    case CTX_SYM_VARIABLE:  return "variable";
    case CTX_SYM_MACRO:     return "macro";
    case CTX_SYM_INCLUDE:   return "include";
    case CTX_SYM_NAMESPACE: return "namespace";
    default:                return "symbol";
    }
}

static const char *lang_fence(uint8_t lang) {
    switch (lang) {
    case 0: return "c";
    case 1: return "cpp";
    case 2: return "python";
    case 3: return "javascript";
    case 4: return "typescript";
    default: return "";
    }
}

/* ====================================================================== */
/* query tokenisation                                                     */
/* ====================================================================== */
typedef struct { char terms[CTX_MAX_QUERY_TERMS][64]; uint32_t count; } QueryTerms;

/* Stopwords that carry no retrieval signal in a task description. */
static bool is_stopword(const char *w) {
    static const char *sw[] = {
        "the","a","an","is","are","of","to","in","on","for","and","or","how",
        "why","does","do","where","what","when","with","this","that","it","as",
        "be","fix","add","make","get","set","use","find","show","from","into",
        NULL
    };
    for (int i = 0; sw[i]; i++) if (!strcmp(w, sw[i])) return true;
    return false;
}

static void tokenize(const char *text, QueryTerms *out) {
    out->count = 0;
    if (!text) return;
    char cur[64]; size_t cl = 0;
    for (const char *p = text; ; ++p) {
        unsigned char c = (unsigned char)*p;
        bool word_char = isalnum(c) || c == '_';
        if (word_char && cl < sizeof(cur) - 1) {
            cur[cl++] = (char)tolower(c);
        } else if (cl > 0) {
            cur[cl] = '\0';
            /* Light stemming: drop common inflections so "resolved" matches
             * "resolve", "indexing" matches "index", "files" matches "file". */
            size_t n = strlen(cur);
            if (n > 5 && !strcmp(cur + n - 3, "ing")) cur[n - 3] = '\0';
            else if (n > 4 && !strcmp(cur + n - 2, "ed")) cur[n - 2] = '\0';
            else if (n > 4 && cur[n - 1] == 's' && cur[n - 2] != 's') cur[n - 1] = '\0';
            cl = strlen(cur);
            if (cl >= 2 && !is_stopword(cur) && out->count < CTX_MAX_QUERY_TERMS) {
                /* dedupe */
                bool dup = false;
                for (uint32_t i = 0; i < out->count; i++)
                    if (!strcmp(out->terms[i], cur)) { dup = true; break; }
                if (!dup) { strcpy(out->terms[out->count++], cur); }
            }
            cl = 0;
        }
        if (!*p) break;
    }
}

static bool ci_substr(const char *hay, const char *needle) {
    size_t hl = strlen(hay), nl = strlen(needle);
    if (nl == 0 || nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        for (; j < nl; j++)
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
        if (j == nl) return true;
    }
    return false;
}

/* ====================================================================== */
/* scoring                                                                */
/* ====================================================================== */
static int kind_importance(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION: case CTX_SYM_METHOD: return 10;
    case CTX_SYM_CLASS: case CTX_SYM_STRUCT:    return 8;
    case CTX_SYM_TYPEDEF: case CTX_SYM_ENUM:    return 6;
    case CTX_SYM_NAMESPACE:                      return 5;
    case CTX_SYM_MACRO: case CTX_SYM_VARIABLE:  return 4;
    case CTX_SYM_INCLUDE:                        return 2;
    default:                                     return 0; /* generic/unknown noise */
    }
}

static bool is_vendor_path(const char *file) {
    return file && (ci_substr(file, "/vendor/") || ci_substr(file, "/vendors/"));
}

/*
 * Computes a relevance score for one symbol against the query terms. Combines
 * name/signature/file text match, query-term coverage, symbol-kind importance,
 * definition preference, and a vendor penalty. Pure text scoring — graph
 * proximity is layered on later during seed expansion.
 */
static double score_symbol(const CtxSymbol *s, const QueryTerms *q) {
    /* Generic fallback nodes carry no first-class meaning and only add noise to
     * retrieval — the real named symbol at the same location is indexed too. */
    if (s->kind == CTX_SYM_UNKNOWN) return 0;

    double score = 0;
    uint32_t covered = 0;
    const char *base = strrchr(s->file, '/');
    base = base ? base + 1 : s->file;

    for (uint32_t i = 0; i < q->count; i++) {
        const char *t = q->terms[i];
        bool hit = false;
        if (!strcasecmp(s->name, t)) { score += 30; hit = true; }       /* exact name */
        else if (ci_substr(s->name, t)) { score += 14; hit = true; }    /* name substr */
        if (ci_substr(base, t)) { score += 8; hit = true; }             /* filename */
        if (s->signature[0] && ci_substr(s->signature, t)) { score += 4; hit = true; }
        if (s->scope[0] && ci_substr(s->scope, t)) { score += 3; hit = true; }
        if (hit) covered++;
    }
    if (covered == 0) return 0;

    score += covered * 6;                       /* query-term coverage   */
    score += kind_importance(s->kind);          /* kind importance       */
    if (s->is_definition) score += 5;           /* prefer definitions    */
    if (is_vendor_path(s->file)) score *= 0.45; /* vendor penalty        */
    return score;
}

/* ====================================================================== */
/* ranked seed collection                                                 */
/* ====================================================================== */
typedef struct {
    const CtxSymbol *sym;
    double           score;
    const char      *reason;   /* short why-selected tag */
} Seed;

typedef struct { Seed items[CTX_MAX_SEEDS]; uint32_t count; } SeedSet;

/* Inserts a symbol into the seed set, keeping it sorted high→low and bounded.
 * Deduplicates by symbol id. Returns true if the symbol is now present. */
static bool seeds_offer(SeedSet *set, const CtxSymbol *s, double score, const char *reason) {
    if (score <= 0) return false;
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->items[i].sym->id == s->id) {
            if (score > set->items[i].score) {
                set->items[i].score = score;
                set->items[i].reason = reason;
            }
            return true;
        }
    }
    if (set->count < CTX_MAX_SEEDS) {
        set->items[set->count++] = (Seed){ s, score, reason };
    } else if (score > set->items[set->count - 1].score) {
        set->items[set->count - 1] = (Seed){ s, score, reason };
    } else {
        return false;
    }
    /* bubble the new/updated tail up */
    for (uint32_t i = set->count; i > 1; i--) {
        if (set->items[i - 1].score > set->items[i - 2].score) {
            Seed tmp = set->items[i - 1];
            set->items[i - 1] = set->items[i - 2];
            set->items[i - 2] = tmp;
        }
    }
    return true;
}

/* ====================================================================== */
/* neighbourhood expansion                                                */
/* ====================================================================== */
typedef struct {
    const CtxSymbol *sym;
    CtxEdgeKind      kind;
    bool             outgoing;  /* true: seed→sym; false: sym→seed */
} Neighbor;

/* Collects up to CTX_MAX_NEIGHBORS related symbols for one seed. Caller holds
 * the read lock. Reference edges that cross from non-vendor code into vendor
 * code are suppressed — they are almost always noisy false resolutions of
 * common field names to unrelated vendor symbols. */
static uint32_t collect_neighbors(CtxGraph *g, uint64_t seed_id,
                                  const char *seed_file,
                                  Neighbor *out, uint32_t max) {
    bool seed_is_vendor = is_vendor_path(seed_file);
    uint32_t n = 0;
    for (CtxEdgeEntry *e = g->edges; e && n < max; e = (CtxEdgeEntry *)e->hh.next) {
        uint64_t other = 0; bool outgoing = false;
        if (e->from_id == seed_id) { other = e->to_id; outgoing = true; }
        else if (e->to_id == seed_id) { other = e->from_id; outgoing = false; }
        else continue;
        const CtxSymbol *os = ctx_graph_find_by_id_locked(g, other);
        if (!os) continue;
        /* Drop reference edges that drag non-vendor seeds into vendor files. */
        if (e->kind == CTX_EDGE_REFERENCES && !seed_is_vendor && is_vendor_path(os->file))
            continue;
        bool dup = false;
        for (uint32_t i = 0; i < n; i++) if (out[i].sym->id == os->id) { dup = true; break; }
        if (dup) continue;
        out[n++] = (Neighbor){ os, e->kind, outgoing };
    }
    return n;
}

static const char *edge_label(CtxEdgeKind k, bool outgoing) {
    switch (k) {
    case CTX_EDGE_CALLS:      return outgoing ? "calls" : "called by";
    case CTX_EDGE_REFERENCES: return outgoing ? "references" : "referenced by";
    case CTX_EDGE_INHERITS:   return outgoing ? "inherits" : "base of";
    case CTX_EDGE_INCLUDES:   return "includes";
    case CTX_EDGE_DEFINES:    return "defines";
    default:                  return "related";
    }
}

/* ====================================================================== */
/* on-demand source snippet (read line range from disk)                   */
/* ====================================================================== */
/*
 * Reads lines [start,end] (1-based, inclusive) from a file into out, bounded by
 * both the line cap and a byte cap. Returns the number of lines emitted, or 0
 * on any failure (missing file, read error). Never reads the whole file blindly.
 */
static uint32_t read_line_range(const char *path, uint32_t start, uint32_t end,
                                char *out, size_t out_sz) {
    out[0] = '\0';
    if (!path || start == 0 || end < start) return 0;
    if (end - start + 1 > CTX_MAX_SNIPPET_LINES) end = start + CTX_MAX_SNIPPET_LINES - 1;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t line = 0, emitted = 0;
    size_t pos = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (line + 1 < start) { if (c == '\n') line++; continue; }
        if (line + 1 > end) break;
        if (pos + 2 >= out_sz) break;
        out[pos++] = (char)c;
        if (c == '\n') { line++; emitted++; }
    }
    out[pos] = '\0';
    if (pos > 0 && out[pos - 1] != '\n') emitted++; /* trailing partial line */
    fclose(f);
    return emitted;
}

/* ====================================================================== */
/* packed item model                                                      */
/* ====================================================================== */
typedef struct {
    const CtxSymbol *sym;
    double           score;
    const char      *reason;
    Neighbor         neighbors[CTX_MAX_NEIGHBORS];
    uint32_t         neighbor_count;
    char             snippet[CTX_MAX_SNIPPET_LINES * 200];
    uint32_t         snippet_start;
    uint32_t         snippet_lines;
    uint32_t         est_tokens;
    bool             packed;
} PackItem;

/* Estimates the rendered token cost of an item, including markdown framing
 * (headings, code fences, citation lines) so budget packing reflects the real
 * output size rather than just the raw field lengths. */
static uint32_t est_tokens_for(const PackItem *it) {
    size_t path = strlen(it->sym->file);
    size_t chars = strlen(it->sym->name) + path +
                   strlen(it->sym->signature) + strlen(it->snippet);
    chars += 90 + path;                        /* heading + citation + fences  */
    if (it->snippet[0]) chars += 30 + path;    /* snippet fence + line comment */
    for (uint32_t i = 0; i < it->neighbor_count; i++)
        chars += strlen(it->neighbors[i].sym->name) +
                 strlen(it->neighbors[i].sym->file) + 40; /* relation + citation */
    /* Conservative factor: keep rendered output at or below budget despite
     * markdown framing variance — under-budget is always safe for an agent. */
    return (uint32_t)((chars * 13 / 10) / CTX_CHARS_PER_TOKEN) + 1;
}

/* ====================================================================== */
/* formatting                                                             */
/* ====================================================================== */
static char *format_markdown(const CtxRetrieveRequest *req, PackItem *items,
                             uint32_t count, uint32_t packed, uint32_t omitted,
                             uint32_t used_tokens, uint32_t budget) {
    SB sb; sb_init(&sb);
    sb_printf(&sb, "# Context for: %s\n\n", req->text);
    sb_printf(&sb, "_%u relevant symbols selected", packed);
    if (omitted) sb_printf(&sb, ", %u omitted for budget", omitted);
    sb_printf(&sb, " · ~%u/%u tokens._\n\n", used_tokens, budget);

    if (packed == 0) {
        sb_puts(&sb, "_No relevant symbols found for this query._\n");
        return sb.buf;
    }

    sb_puts(&sb, "## Selected\n\n");
    for (uint32_t i = 0; i < count; i++) {
        PackItem *it = &items[i];
        if (!it->packed) continue;
        const CtxSymbol *s = it->sym;
        sb_printf(&sb, "### %s `%s`  \n", kind_str(s->kind), s->name);
        sb_printf(&sb, "`%s:%u`", s->file, s->line);
        if (s->scope[0]) sb_printf(&sb, " · scope `%s`", s->scope);
        sb_printf(&sb, " · relevance %.0f (%s)\n\n", it->score, it->reason ? it->reason : "match");
        if (s->signature[0] && s->kind != CTX_SYM_UNKNOWN)
            sb_printf(&sb, "```%s\n%s\n```\n", lang_fence(s->lang), s->signature);
        if (it->snippet[0]) {
            sb_printf(&sb, "```%s\n", lang_fence(s->lang));
            sb_printf(&sb, "// %s:%u-%u\n", s->file, it->snippet_start,
                      it->snippet_start + it->snippet_lines - 1);
            sb_puts(&sb, it->snippet);
            if (it->snippet[strlen(it->snippet) - 1] != '\n') sb_puts(&sb, "\n");
            sb_puts(&sb, "```\n");
        }
        if (it->neighbor_count) {
            sb_puts(&sb, "\nRelationships:\n");
            for (uint32_t j = 0; j < it->neighbor_count; j++) {
                Neighbor *nb = &it->neighbors[j];
                sb_printf(&sb, "- %s `%s` (`%s:%u`)\n",
                          edge_label(nb->kind, nb->outgoing), nb->sym->name,
                          nb->sym->file, nb->sym->line);
            }
        }
        sb_puts(&sb, "\n");
    }

    if (omitted) {
        sb_puts(&sb, "## Omitted (budget exceeded)\n\n");
        for (uint32_t i = 0; i < count; i++) {
            if (items[i].packed) continue;
            sb_printf(&sb, "- %s `%s` (`%s:%u`)\n", kind_str(items[i].sym->kind),
                      items[i].sym->name, items[i].sym->file, items[i].sym->line);
        }
    }
    return sb.buf;
}

static char *format_json(const CtxRetrieveRequest *req, PackItem *items,
                         uint32_t count, uint32_t packed, uint32_t omitted,
                         uint32_t used_tokens, uint32_t budget) {
    SB sb; sb_init(&sb);
    sb_puts(&sb, "{\"query\":\"");      sb_json_escape(&sb, req->text); sb_puts(&sb, "\",");
    sb_printf(&sb, "\"budget_tokens\":%u,\"used_tokens\":%u,", budget, used_tokens);
    sb_printf(&sb, "\"selected_count\":%u,\"omitted_count\":%u,", packed, omitted);
    sb_printf(&sb, "\"summary\":\"%u symbols selected within budget\",", packed);

    sb_puts(&sb, "\"selected\":[");
    bool first = true;
    for (uint32_t i = 0; i < count; i++) {
        PackItem *it = &items[i];
        if (!it->packed) continue;
        const CtxSymbol *s = it->sym;
        if (!first) sb_puts(&sb, ",");
        first = false;
        sb_puts(&sb, "{\"name\":\"");      sb_json_escape(&sb, s->name); sb_puts(&sb, "\",");
        sb_printf(&sb, "\"kind\":\"%s\",", kind_str(s->kind));
        sb_puts(&sb, "\"file\":\"");       sb_json_escape(&sb, s->file); sb_puts(&sb, "\",");
        sb_printf(&sb, "\"line\":%u,\"end_line\":%u,", s->line, s->end_line ? s->end_line : s->line);
        sb_puts(&sb, "\"scope\":\"");      sb_json_escape(&sb, s->scope); sb_puts(&sb, "\",");
        sb_puts(&sb, "\"signature\":\"");  sb_json_escape(&sb, s->signature); sb_puts(&sb, "\",");
        sb_printf(&sb, "\"relevance\":%.1f,", it->score);
        sb_puts(&sb, "\"reason\":\"");     sb_json_escape(&sb, it->reason ? it->reason : "match"); sb_puts(&sb, "\",");
        sb_puts(&sb, "\"citation\":\"");
        sb_json_escape(&sb, s->file);
        sb_printf(&sb, ":%u\",", s->line);
        if (it->snippet[0]) {
            sb_printf(&sb, "\"snippet_lines\":\"%u-%u\",", it->snippet_start,
                      it->snippet_start + it->snippet_lines - 1);
            sb_puts(&sb, "\"snippet\":\""); sb_json_escape(&sb, it->snippet); sb_puts(&sb, "\",");
        }
        sb_puts(&sb, "\"relationships\":[");
        for (uint32_t j = 0; j < it->neighbor_count; j++) {
            Neighbor *nb = &it->neighbors[j];
            if (j) sb_puts(&sb, ",");
            sb_printf(&sb, "{\"relation\":\"%s\",\"name\":\"", edge_label(nb->kind, nb->outgoing));
            sb_json_escape(&sb, nb->sym->name);
            sb_puts(&sb, "\",\"citation\":\"");
            sb_json_escape(&sb, nb->sym->file);
            sb_printf(&sb, ":%u\"}", nb->sym->line);
        }
        sb_puts(&sb, "]}");
    }
    sb_puts(&sb, "],");

    sb_puts(&sb, "\"omitted\":[");
    first = true;
    for (uint32_t i = 0; i < count; i++) {
        if (items[i].packed) continue;
        if (!first) sb_puts(&sb, ",");
        first = false;
        sb_puts(&sb, "{\"name\":\""); sb_json_escape(&sb, items[i].sym->name);
        sb_puts(&sb, "\",\"citation\":\"");
        sb_json_escape(&sb, items[i].sym->file);
        sb_printf(&sb, ":%u\"}", items[i].sym->line);
    }
    sb_puts(&sb, "]}");
    return sb.buf;
}

/* ====================================================================== */
/* driver                                                                 */
/* ====================================================================== */
static char *error_payload(CtxFormat fmt, const char *msg) {
    SB sb; sb_init(&sb);
    if (fmt == CTX_FMT_JSON) sb_printf(&sb, "{\"error\":\"%s\",\"selected\":[]}", msg);
    else sb_printf(&sb, "# %s\n", msg);
    return sb.buf;
}

char *ctx_retrieve(CtxGraph *g, const CtxRetrieveRequest *req) {
    if (!req || !req->text) return error_payload(req ? req->format : CTX_FMT_MARKDOWN,
                                                 "invalid request");
    if (!g) return error_payload(req->format, "no graph available");

    uint32_t budget = req->budget ? req->budget : CTX_DEFAULT_BUDGET;
    if (budget > 200000) budget = 200000;  /* keep response memory bounded */

    QueryTerms q = {0};
    if (req->kind == CTX_QUERY_TASK) {
        tokenize(req->text, &q);
    } else {
        /* symbol/file anchor: treat the anchor name as the sole term */
        strncpy(q.terms[0], req->text, sizeof(q.terms[0]) - 1);
        for (char *p = q.terms[0]; *p; ++p) *p = (char)tolower((unsigned char)*p);
        q.count = 1;
    }
    if (q.count == 0) return error_payload(req->format, "query has no usable terms");

    /* Trim whitespace from anchor text for symbol/file queries so that URL
     * decoding artifacts or trailing newlines don't break exact matching. */
    char anchor_buf[512] = {0};
    const char *anchor = req->text;
    if (req->kind == CTX_QUERY_SYMBOL || req->kind == CTX_QUERY_FILE) {
        strncpy(anchor_buf, req->text, sizeof(anchor_buf) - 1);
        char *p = anchor_buf;
        while (*p && isspace((unsigned char)*p)) p++;
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)*(end - 1))) *--end = '\0';
        anchor = p;
    }

    ctx_graph_rlock(g);

    /* 1. text-score every symbol → bounded ranked seed set */
    SeedSet seeds = {0};
    {
        CtxSymbol *s, *tmp;
        HASH_ITER(hh, g->symbols, s, tmp) {
            if (req->kind == CTX_QUERY_FILE) {
                if (strcmp(s->file, anchor) != 0) continue;
                seeds_offer(&seeds, s, 100 + kind_importance(s->kind), "in target file");
                continue;
            }
            if (req->kind == CTX_QUERY_SYMBOL) {
                /* Exact match preferred; fall back to case-insensitive substring
                 * so the endpoint never returns empty for near-miss names. */
                if (!strcasecmp(s->name, anchor)) {
                    seeds_offer(&seeds, s, 120 + kind_importance(s->kind), "exact symbol");
                } else if (ci_substr(s->name, anchor) || ci_substr(anchor, s->name)) {
                    seeds_offer(&seeds, s, 80 + kind_importance(s->kind), "symbol substr");
                }
                continue;
            }
            double sc = score_symbol(s, &q);
            if (sc > 0) seeds_offer(&seeds, s, sc, "text match");
        }
    }

    /* 2. build pack items from seeds, expand neighbourhoods, read snippets */
    uint32_t count = seeds.count;
    PackItem *items = (PackItem *)calloc(count ? count : 1, sizeof(PackItem));
    if (!items) { ctx_graph_runlock(g); return error_payload(req->format, "out of memory"); }

    for (uint32_t i = 0; i < count; i++) {
        items[i].sym    = seeds.items[i].sym;
        items[i].score  = seeds.items[i].score;
        items[i].reason = seeds.items[i].reason;
        items[i].neighbor_count =
            collect_neighbors(g, items[i].sym->id, items[i].sym->file,
                              items[i].neighbors, CTX_MAX_NEIGHBORS);
    }
    ctx_graph_runlock(g);

    /* Snippets are read from disk outside the lock — bounded line ranges only. */
    for (uint32_t i = 0; i < count; i++) {
        const CtxSymbol *s = items[i].sym;
        if (s->kind == CTX_SYM_UNKNOWN || s->kind == CTX_SYM_INCLUDE) continue;
        uint32_t start = s->line;
        uint32_t end   = s->end_line >= s->line ? s->end_line : s->line;
        uint32_t lines = read_line_range(s->file, start, end,
                                         items[i].snippet, sizeof(items[i].snippet));
        if (lines > 0) { items[i].snippet_start = start; items[i].snippet_lines = lines; }
        items[i].est_tokens = est_tokens_for(&items[i]);
    }

    /* 3. budget packing — items are already ranked, pack greedily until full */
    uint32_t used = 0, packed = 0, omitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cost = items[i].est_tokens ? items[i].est_tokens : est_tokens_for(&items[i]);
        if (used + cost <= budget) {
            items[i].packed = true;
            used += cost;
            packed++;
        } else {
            /* drop the snippet and retry as a lightweight reference-only entry */
            items[i].snippet[0] = '\0';
            items[i].snippet_lines = 0;
            uint32_t light = est_tokens_for(&items[i]);
            if (used + light <= budget) {
                items[i].packed = true; used += light; packed++;
            } else { omitted++; }
        }
    }

    char *out = (req->format == CTX_FMT_JSON)
        ? format_json(req, items, count, packed, omitted, used, budget)
        : format_markdown(req, items, count, packed, omitted, used, budget);

    free(items);
    CTX_LOG_DEBUG("retrieve '%s': %u seeds, %u packed, %u omitted, ~%u tokens",
                  req->text, count, packed, omitted, used);
    return out ? out : error_payload(req->format, "formatting failed");
}

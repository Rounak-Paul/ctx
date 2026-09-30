#include "nav.h"
#include "source.h"
#include "../indexer/indexer.h"

#include <ctype.h>
#include <strings.h>

#define NAV_MAX_MATCHES        64u
#define NAV_DEFAULT_OUTLINE    300u
#define NAV_DEFAULT_BODY_LINES 120u
#define NAV_MAX_DOC_LINES      12u
#define NAV_MAX_CALL_SITES     80u
#define NAV_MAX_TRANSITIVE     60u
#define NAV_LINE_TEXT_MAX      160u
#define NAV_SIGNATURE_MAX      200u
#define NAV_MAX_DEPTH          3u

/* ---- small helpers ---------------------------------------------------------- */

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
    case CTX_SYM_INCLUDE:   return "include";
    case CTX_SYM_NAMESPACE: return "namespace";
    default:                return "node";
    }
}

static bool is_navigable(const CtxSymbol *s) {
    return s->kind != CTX_SYM_INCLUDE && s->kind != CTX_SYM_UNKNOWN;
}

static bool is_callable(const CtxSymbol *s) {
    return s->kind == CTX_SYM_FUNCTION || s->kind == CTX_SYM_METHOD || s->kind == CTX_SYM_MACRO;
}

static int kind_rank(CtxSymbolKind k) {
    switch (k) {
    case CTX_SYM_FUNCTION: case CTX_SYM_METHOD: return 5;
    case CTX_SYM_CLASS: case CTX_SYM_STRUCT:    return 4;
    case CTX_SYM_ENUM: case CTX_SYM_TYPEDEF:    return 3;
    case CTX_SYM_MACRO:                         return 2;
    default:                                    return 1;
    }
}

/* Writes a one-line signature: whitespace collapsed, body and trailing
 * punctuation dropped, truncated with an ellipsis. */
static void compact_signature(const char *sig, char *out, size_t out_size) {
    size_t w = 0;
    bool space = false;
    for (const char *p = sig ? sig : ""; *p && w + 4 < out_size; p++) {
        if (*p == '{') break;
        if (isspace((unsigned char)*p)) { space = w > 0; continue; }
        if (space) { out[w++] = ' '; space = false; }
        out[w++] = *p;
    }
    while (w > 0 && (out[w - 1] == ';' || out[w - 1] == ' ')) w--;
    if (w > NAV_SIGNATURE_MAX) {
        w = NAV_SIGNATURE_MAX;
        memcpy(out + w, "...", 3);
        w += 3;
    }
    out[w] = '\0';
}

/* Appends a trimmed, length-capped copy of a source line. */
static void append_line_text(CtxBuf *b, const char *line, uint32_t len) {
    while (len && isspace((unsigned char)*line)) { line++; len--; }
    while (len && isspace((unsigned char)line[len - 1])) len--;
    if (len > NAV_LINE_TEXT_MAX) {
        ctx_buf_append(b, line, NAV_LINE_TEXT_MAX);
        ctx_buf_append(b, "...", 3);
    } else {
        ctx_buf_append(b, line, len);
    }
}

typedef struct {
    char   **items;
    uint32_t count;
    uint32_t cap;
} PathList;

static void path_list_add(PathList *l, const char *path) {
    for (uint32_t i = 0; i < l->count; i++)
        if (!strcmp(l->items[i], path)) return;
    if (l->count >= l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 16;
        char **next = (char **)realloc(l->items, cap * sizeof(char *));
        if (!next) return;
        l->items = next;
        l->cap = cap;
    }
    char *copy = strdup(path);
    if (copy) l->items[l->count++] = copy;
}

static void path_list_free(PathList *l) {
    for (uint32_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    memset(l, 0, sizeof(*l));
}

/* Re-indexes stale files; returns true when anything changed. */
static bool refresh_paths(const PathList *l) {
    return l->count && ctx_indexer_ensure_fresh((const char *const *)l->items, l->count) > 0;
}

/* Sorted id set for membership tests. */
typedef struct {
    uint64_t *ids;
    uint32_t  count;
    uint32_t  cap;
} IdSet;

static void id_set_add(IdSet *s, uint64_t id) {
    if (s->count >= s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 16;
        uint64_t *next = (uint64_t *)realloc(s->ids, cap * sizeof(uint64_t));
        if (!next) return;
        s->ids = next;
        s->cap = cap;
    }
    s->ids[s->count++] = id;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void id_set_seal(IdSet *s) {
    if (s->count < 2) return;
    qsort(s->ids, s->count, sizeof(uint64_t), cmp_u64);
    uint32_t w = 1;
    for (uint32_t i = 1; i < s->count; i++)
        if (s->ids[i] != s->ids[w - 1]) s->ids[w++] = s->ids[i];
    s->count = w;
}

static bool id_set_has(const IdSet *s, uint64_t id) {
    return s->count && bsearch(&id, s->ids, s->count, sizeof(uint64_t), cmp_u64) != NULL;
}

static void id_set_free(IdSet *s) {
    free(s->ids);
    memset(s, 0, sizeof(*s));
}

/* ---- symbol resolution -------------------------------------------------------- */

typedef struct {
    CtxSymbol *items[NAV_MAX_MATCHES];
    uint32_t   count;
    bool       truncated;
    char       error[512];
} Matches;

static int match_score(const char *root, const CtxSymbol *s) {
    int score = kind_rank(s->kind) * 4;
    if (s->is_definition) score += 100;
    if (!ctx_path_is_vendor(root, s->file)) score += 50;
    return score;
}

typedef struct {
    CtxSymbol *sym;
    int        score;
} ScoredMatch;

static int scored_order(const void *a, const void *b) {
    const ScoredMatch *x = (const ScoredMatch *)a, *y = (const ScoredMatch *)b;
    if (x->score != y->score) return y->score - x->score;
    size_t lx = strlen(x->sym->file), ly = strlen(y->sym->file);
    if (lx != ly) return lx < ly ? -1 : 1;
    int c = strcmp(x->sym->file, y->sym->file);
    if (c) return c;
    return x->sym->line < y->sym->line ? -1 : x->sym->line > y->sym->line;
}

static void sort_matches(const char *root, Matches *m) {
    ScoredMatch scored[NAV_MAX_MATCHES];
    for (uint32_t i = 0; i < m->count; i++)
        scored[i] = (ScoredMatch){ m->items[i], match_score(root, m->items[i]) };
    qsort(scored, m->count, sizeof(ScoredMatch), scored_order);
    for (uint32_t i = 0; i < m->count; i++) m->items[i] = scored[i].sym;
}

static bool contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

/* Parses "path:123" into its parts. */
static bool split_path_line(const char *q, char *path, size_t path_size, uint32_t *line) {
    const char *colon = strrchr(q, ':');
    if (!colon || colon == q || !colon[1]) return false;
    for (const char *p = colon + 1; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    size_t len = (size_t)(colon - q);
    if (len >= path_size) return false;
    memcpy(path, q, len);
    path[len] = '\0';
    *line = (uint32_t)strtoul(colon + 1, NULL, 10);
    return *line > 0;
}

/* Splits "Scope::name" / "Scope.name" / "Scope->name"; scope is "" when absent. */
static void split_qualified(const char *q, char *scope, size_t scope_size,
                            char *name, size_t name_size) {
    const char *sep = NULL;
    size_t sep_len = 0;
    for (const char *p = q; *p; p++) {
        if (p[0] == ':' && p[1] == ':') { sep = p; sep_len = 2; p++; }
        else if (p[0] == '-' && p[1] == '>') { sep = p; sep_len = 2; p++; }
        else if (p[0] == '.') { sep = p; sep_len = 1; }
    }
    if (!sep) {
        scope[0] = '\0';
        snprintf(name, name_size, "%s", q);
        return;
    }
    size_t slen = (size_t)(sep - q);
    if (slen >= scope_size) slen = scope_size - 1;
    memcpy(scope, q, slen);
    scope[slen] = '\0';
    snprintf(name, name_size, "%s", sep + sep_len);
}

static const CtxSymbol *innermost_at_line(const CtxGraphFile *f, uint32_t line) {
    const CtxSymbol *best = NULL;
    for (uint32_t i = 0; i < f->symbol_count; i++) {
        const CtxSymbol *s = f->symbols[i];
        if (!is_navigable(s) || s->line > line || s->end_line < line) continue;
        if (!best || (s->end_line - s->line) <= (best->end_line - best->line)) best = s;
    }
    return best;
}

#define NAV_MAX_PARTS 8

/* Splits an identifier into lowercase parts on '_', non-alphanumerics, and
 * camelCase boundaries. Returns the number of parts written. */
static uint32_t name_parts(const char *name, char parts[NAV_MAX_PARTS][32]) {
    uint32_t n = 0;
    size_t len = strlen(name), i = 0;
    while (i < len && n < NAV_MAX_PARTS) {
        while (i < len && !isalnum((unsigned char)name[i])) i++;
        size_t start = i;
        while (i < len && isalnum((unsigned char)name[i])) {
            if (i > start && islower((unsigned char)name[i - 1]) && isupper((unsigned char)name[i])) break;
            i++;
        }
        size_t plen = i - start;
        if (plen < 2) continue;
        if (plen > 31) plen = 31;
        for (size_t k = 0; k < plen; k++) parts[n][k] = (char)tolower((unsigned char)name[start + k]);
        parts[n][plen] = '\0';
        n++;
    }
    return n;
}

typedef struct {
    const char *name;
    int         score;
} Suggestion;

/*
 * Lists up to 6 navigable names most similar to query: names containing it
 * rank first, then names sharing the most identifier parts. Locked.
 */
static void suggest_names_locked(CtxGraph *g, const char *query, char *out, size_t out_size) {
    out[0] = '\0';
    char qparts[NAV_MAX_PARTS][32];
    uint32_t qn = name_parts(query, qparts);
    if (strlen(query) < 3 || qn == 0) return;

    Suggestion best[6] = {0};
    CtxNameEntry *n, *tmp;
    HASH_ITER(hh, g->names, n, tmp) {
        bool navigable = false;
        for (uint32_t i = 0; i < n->count && !navigable; i++) navigable = is_navigable(n->symbols[i]);
        if (!navigable) continue;
        int score = contains_ci(n->name, query) ? 100 : 0;
        char parts[NAV_MAX_PARTS][32];
        uint32_t pn = name_parts(n->name, parts);
        for (uint32_t a = 0; a < qn; a++)
            for (uint32_t b = 0; b < pn; b++)
                if (!strcmp(qparts[a], parts[b])) { score += 10; break; }
        if (score < 10 || (qn > 1 && score < 20)) continue;
        score -= (int)(strlen(n->name) / 8);
        int slot = -1;
        for (int k = 0; k < 6; k++) {
            if (!best[k].name || score > best[k].score) { slot = k; break; }
        }
        if (slot < 0) continue;
        memmove(&best[slot + 1], &best[slot], (size_t)(5 - slot) * sizeof(Suggestion));
        best[slot] = (Suggestion){ n->name, score };
    }
    size_t w = 0;
    for (int k = 0; k < 6 && best[k].name; k++) {
        int wrote = snprintf(out + w, out_size - w, "%s%s", k ? ", " : "", best[k].name);
        if (wrote < 0 || (size_t)wrote >= out_size - w) break;
        w += (size_t)wrote;
    }
}

/*
 * Resolves a symbol query to ranked matches. Caller holds the read lock.
 * On failure m->count is 0 and m->error explains why.
 */
static void resolve_locked(CtxGraph *g, const char *root, const char *symbol,
                           const char *file, Matches *m) {
    memset(m, 0, sizeof(*m));
    char file_abs[4096] = {0};
    if (file && file[0] && !ctx_path_resolve_locked(g, root, file, file_abs, sizeof(file_abs))) {
        snprintf(m->error, sizeof(m->error), "file not indexed: %s", file);
        return;
    }
    if (!symbol || !symbol[0]) {
        snprintf(m->error, sizeof(m->error), "missing symbol");
        return;
    }

    char path_part[4096];
    uint32_t line = 0;
    if (split_path_line(symbol, path_part, sizeof(path_part), &line)) {
        char abs[4096];
        if (ctx_path_resolve_locked(g, root, path_part, abs, sizeof(abs))) {
            const CtxGraphFile *f = ctx_graph_find_file_locked(g, abs);
            const CtxSymbol *s = f ? innermost_at_line(f, line) : NULL;
            if (s) { m->items[m->count++] = (CtxSymbol *)s; return; }
            snprintf(m->error, sizeof(m->error), "no symbol encloses %s", symbol);
            return;
        }
    }

    char scope[256], name[256];
    split_qualified(symbol, scope, sizeof(scope), name, sizeof(name));
    const CtxNameEntry *n = ctx_graph_find_name_locked(g, name);
    for (int pass = 0; pass < 2 && m->count == 0; pass++) {
        bool use_scope = pass == 0 && scope[0];
        if (pass == 1 && !scope[0]) break;
        for (uint32_t i = 0; n && i < n->count; i++) {
            CtxSymbol *s = n->symbols[i];
            if (!is_navigable(s)) continue;
            if (file_abs[0] && strcmp(s->file, file_abs) != 0) continue;
            if (use_scope && strcmp(s->scope, scope) != 0) continue;
            if (m->count < NAV_MAX_MATCHES) m->items[m->count++] = s;
            else m->truncated = true;
        }
    }
    if (m->count == 0) {
        char hints[256];
        suggest_names_locked(g, name, hints, sizeof(hints));
        snprintf(m->error, sizeof(m->error), "symbol not found: %s%s%s", symbol,
                 file_abs[0] ? " in " : "", file_abs[0] ? file : "");
        if (hints[0]) {
            size_t len = strlen(m->error);
            snprintf(m->error + len, sizeof(m->error) - len, " (similar: %s)", hints);
        }
        return;
    }
    sort_matches(root, m);
}

/*
 * Resolves, refreshes the matched files if they changed on disk, and leaves
 * the graph read-locked with final matches on return. Caller must runlock.
 */
static void resolve_fresh(CtxGraph *g, const char *root, const char *symbol,
                          const char *file, Matches *m) {
    for (int attempt = 0; attempt < 2; attempt++) {
        ctx_graph_rlock(g);
        resolve_locked(g, root, symbol, file, m);
        if (attempt == 1) return;
        PathList files = {0};
        for (uint32_t i = 0; i < m->count; i++) path_list_add(&files, m->items[i]->file);
        if (m->count == 0 && file && file[0]) {
            char abs[4096];
            if (ctx_path_resolve_locked(g, root, file, abs, sizeof(abs))) path_list_add(&files, abs);
        }
        ctx_graph_runlock(g);
        bool changed = refresh_paths(&files);
        path_list_free(&files);
        if (!changed) {
            ctx_graph_rlock(g);
            resolve_locked(g, root, symbol, file, m);
            return;
        }
    }
}

static void append_symbol_ref(CtxBuf *b, const char *root, const CtxSymbol *s) {
    char rel[4096];
    ctx_path_display(root, s->file, rel, sizeof(rel));
    if (s->end_line > s->line) ctx_buf_printf(b, "%s:%u-%u", rel, s->line, s->end_line);
    else                       ctx_buf_printf(b, "%s:%u", rel, s->line);
}

static void append_other_matches(CtxBuf *b, const char *root, const Matches *m, uint32_t skip,
                                 const char *label) {
    if (m->count <= skip) return;
    ctx_buf_printf(b, "%s (%u%s):\n", label, m->count - skip, m->truncated ? "+" : "");
    for (uint32_t i = skip; i < m->count && i < skip + 12; i++) {
        char sig[NAV_SIGNATURE_MAX + 8];
        compact_signature(m->items[i]->signature, sig, sizeof(sig));
        ctx_buf_printf(b, "  %s %s  ", kind_label(m->items[i]->kind),
                       m->items[i]->is_definition ? "def" : "decl");
        append_symbol_ref(b, root, m->items[i]);
        ctx_buf_printf(b, "  %s\n", sig);
    }
    if (m->count > skip + 12) ctx_buf_printf(b, "  ... %u more\n", m->count - skip - 12);
}

/* ---- outline ------------------------------------------------------------------- */

char *ctx_nav_outline(CtxGraph *g, const char *path, uint32_t from_line, uint32_t limit) {
    CtxBuf b = {0};
    const char *root = ctx_indexer_root();
    if (!g || !path || !path[0]) {
        ctx_buf_printf(&b, "error: missing path\n");
        return ctx_buf_take(&b);
    }
    if (limit == 0) limit = NAV_DEFAULT_OUTLINE;
    if (from_line == 0) from_line = 1;

    char abs[4096];
    ctx_graph_rlock(g);
    bool found = ctx_path_resolve_locked(g, root, path, abs, sizeof(abs));
    ctx_graph_runlock(g);
    if (!found) {
        ctx_buf_printf(&b, "error: file not indexed: %s\n", path);
        return ctx_buf_take(&b);
    }
    const char *paths[1] = { abs };
    ctx_indexer_ensure_fresh(paths, 1);

    ctx_graph_rlock(g);
    const CtxGraphFile *f = ctx_graph_find_file_locked(g, abs);
    if (!f) {
        ctx_graph_runlock(g);
        ctx_buf_printf(&b, "error: file no longer indexed: %s\n", path);
        return ctx_buf_take(&b);
    }
    char rel[4096];
    ctx_path_display(root, abs, rel, sizeof(rel));
    uint32_t total = 0, last_line = 0;
    for (uint32_t i = 0; i < f->symbol_count; i++) {
        if (is_navigable(f->symbols[i])) total++;
        if (f->symbols[i]->end_line > last_line) last_line = f->symbols[i]->end_line;
    }
    ctx_buf_printf(&b, "%s  (%u symbols)\n", rel, total);

    uint32_t listed = 0, skipped_after = 0;
    uint32_t open_ends[32];
    uint32_t depth = 0;
    for (uint32_t i = 0; i < f->symbol_count; i++) {
        const CtxSymbol *s = f->symbols[i];
        if (!is_navigable(s) || s->line < from_line) continue;
        if (listed >= limit) { skipped_after++; continue; }
        while (depth > 0 && s->line > open_ends[depth - 1]) depth--;
        char sig[NAV_SIGNATURE_MAX + 8];
        compact_signature(s->signature, sig, sizeof(sig));
        char range[32];
        if (s->end_line > s->line) snprintf(range, sizeof(range), "L%u-%u", s->line, s->end_line);
        else                       snprintf(range, sizeof(range), "L%u", s->line);
        ctx_buf_printf(&b, "%*s%-12s %-7s %s\n", (int)(depth * 2), "", range, kind_label(s->kind), sig);
        listed++;
        if (s->end_line > s->line && depth < 32 &&
            (s->kind == CTX_SYM_CLASS || s->kind == CTX_SYM_STRUCT ||
             s->kind == CTX_SYM_NAMESPACE || s->kind == CTX_SYM_ENUM))
            open_ends[depth++] = s->end_line;
    }
    if (skipped_after) {
        uint32_t next_line = 0;
        uint32_t seen = 0;
        for (uint32_t i = 0; i < f->symbol_count && !next_line; i++) {
            const CtxSymbol *s = f->symbols[i];
            if (!is_navigable(s) || s->line < from_line) continue;
            if (seen++ == listed) next_line = s->line;
        }
        ctx_buf_printf(&b, "... %u more symbols; continue with from_line=%u\n", skipped_after, next_line);
    }
    ctx_graph_runlock(g);
    return ctx_buf_take(&b);
}

/* ---- source ---------------------------------------------------------------------- */

static bool is_comment_line(const char *line, uint32_t len) {
    while (len && isspace((unsigned char)*line)) { line++; len--; }
    if (len == 0) return false;
    if (len >= 2 && (!strncmp(line, "//", 2) || !strncmp(line, "/*", 2) || !strncmp(line, "*/", 2)))
        return true;
    if (line[0] == '*' || line[0] == '#') return true;
    return len >= 3 && (!strncmp(line, "\"\"\"", 3) || !strncmp(line, "'''", 3));
}

/* First line of the contiguous comment block directly above line (or line). */
static uint32_t doc_start(const CtxSource *src, uint32_t line) {
    uint32_t start = line;
    while (start > 1 && line - start < NAV_MAX_DOC_LINES) {
        uint32_t len = 0;
        const char *text = ctx_source_line(src, start - 1, &len);
        if (!text || !is_comment_line(text, len)) break;
        start--;
    }
    return start;
}

static void append_numbered_lines(CtxBuf *b, const CtxSource *src, uint32_t from, uint32_t to) {
    int width = 1;
    for (uint32_t v = to; v >= 10; v /= 10) width++;
    for (uint32_t n = from; n <= to; n++) {
        uint32_t len = 0;
        const char *text = ctx_source_line(src, n, &len);
        if (!text) break;
        ctx_buf_printf(b, "%*u  ", width, n);
        ctx_buf_append(b, text, len);
        ctx_buf_append(b, "\n", 1);
    }
}

static bool parse_range(const char *lines, uint32_t *start, uint32_t *end) {
    if (!lines || !lines[0]) return false;
    char *dash = NULL;
    unsigned long a = strtoul(lines, &dash, 10);
    unsigned long b = a;
    if (dash && (*dash == '-' || *dash == ':')) b = strtoul(dash + 1, NULL, 10);
    if (a == 0 || b < a) return false;
    *start = (uint32_t)a;
    *end = (uint32_t)b;
    return true;
}

static char *source_range(CtxGraph *g, const char *root, const char *file, const char *lines,
                          uint32_t max_lines) {
    CtxBuf b = {0};
    uint32_t start = 0, end = 0;
    if (!parse_range(lines, &start, &end)) {
        ctx_buf_printf(&b, "error: lines must be \"start-end\"\n");
        return ctx_buf_take(&b);
    }
    char abs[4096];
    ctx_graph_rlock(g);
    bool found = ctx_path_resolve_locked(g, root, file, abs, sizeof(abs));
    ctx_graph_runlock(g);
    if (!found) {
        if (file[0] == '/') snprintf(abs, sizeof(abs), "%s", file);
        else snprintf(abs, sizeof(abs), "%s/%s", root, file);
    }
    CtxSource src;
    if (!ctx_source_open(abs, &src)) {
        ctx_buf_printf(&b, "error: cannot read %s\n", file);
        return ctx_buf_take(&b);
    }
    if (end > src.line_count) end = src.line_count;
    uint32_t shown_end = end;
    if (end - start + 1 > max_lines) shown_end = start + max_lines - 1;
    char rel[4096];
    ctx_path_display(root, abs, rel, sizeof(rel));
    if (start > src.line_count) {
        ctx_buf_printf(&b, "error: %s has %u lines\n", rel, src.line_count);
    } else {
        ctx_buf_printf(&b, "%s:%u-%u\n", rel, start, shown_end);
        append_numbered_lines(&b, &src, start, shown_end);
        if (shown_end < end)
            ctx_buf_printf(&b, "... %u more lines; continue with lines=%u-%u\n",
                           end - shown_end, shown_end + 1, end);
    }
    ctx_source_close(&src);
    return ctx_buf_take(&b);
}

char *ctx_nav_source(CtxGraph *g, const CtxNavSourceRequest *req) {
    CtxBuf b = {0};
    const char *root = ctx_indexer_root();
    if (!g || !req) {
        ctx_buf_printf(&b, "error: invalid request\n");
        return ctx_buf_take(&b);
    }
    uint32_t max_lines = req->max_lines ? req->max_lines : NAV_DEFAULT_BODY_LINES;
    if ((!req->symbol || !req->symbol[0]) && req->file && req->file[0] && req->lines)
        return source_range(g, root, req->file, req->lines, max_lines);

    Matches m;
    resolve_fresh(g, root, req->symbol, req->file, &m);
    if (m.count == 0) {
        ctx_graph_runlock(g);
        ctx_buf_printf(&b, "error: %s\n", m.error);
        return ctx_buf_take(&b);
    }

    const CtxSymbol *s = m.items[0];
    uint32_t def_index = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        if (m.items[i]->is_definition && m.items[i]->end_line > m.items[i]->line) {
            s = m.items[i];
            def_index = i;
            break;
        }
    }
    char file_copy[4096];
    snprintf(file_copy, sizeof(file_copy), "%s", s->file);
    uint32_t line = s->line, end_line = s->end_line;

    ctx_buf_printf(&b, "%s %s  ", kind_label(s->kind), s->name);
    append_symbol_ref(&b, root, s);
    ctx_buf_append(&b, "\n", 1);

    Matches others = m;
    if (def_index) {
        others.items[def_index] = others.items[0];
        others.items[0] = (CtxSymbol *)s;
    }
    CtxBuf tail = {0};
    append_other_matches(&tail, root, &others, 1, "other matches");
    ctx_graph_runlock(g);

    CtxSource src;
    if (!ctx_source_open(file_copy, &src)) {
        ctx_buf_printf(&b, "error: cannot read source\n");
    } else {
        uint32_t from = doc_start(&src, line);
        uint32_t to = end_line < src.line_count ? end_line : src.line_count;
        uint32_t cap_to = to;
        if (to >= from && to - from + 1 > max_lines) cap_to = from + max_lines - 1;
        append_numbered_lines(&b, &src, from, cap_to);
        if (cap_to < to) {
            char rel[4096];
            ctx_path_display(root, file_copy, rel, sizeof(rel));
            ctx_buf_printf(&b, "... %u more lines; continue with file=%s lines=%u-%u\n",
                           to - cap_to, rel, cap_to + 1, to);
        }
        ctx_source_close(&src);
    }
    if (tail.len) ctx_buf_append(&b, tail.data, tail.len);
    free(tail.data);
    return ctx_buf_take(&b);
}

/* ---- call sites ---------------------------------------------------------------------- */

typedef struct {
    const CtxGraphFile *file;
    const CtxRefSite   *site;
} SiteRef;

typedef struct {
    SiteRef *items;
    uint32_t count;
    uint32_t cap;
} SiteList;

static void site_list_add(SiteList *l, const CtxGraphFile *f, const CtxRefSite *site) {
    if (l->count >= l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 64;
        SiteRef *next = (SiteRef *)realloc(l->items, cap * sizeof(SiteRef));
        if (!next) return;
        l->items = next;
        l->cap = cap;
    }
    l->items[l->count++] = (SiteRef){ f, site };
}

static int site_order(const void *a, const void *b) {
    const SiteRef *x = (const SiteRef *)a, *y = (const SiteRef *)b;
    int c = strcmp(x->file->path, y->file->path);
    if (c) return c;
    return x->site->from_line < y->site->from_line ? -1 : x->site->from_line > y->site->from_line;
}

/* Collects resolved sites of the given kinds that target ids in set. Locked. */
static void collect_sites_locked(CtxGraph *g, const IdSet *targets, bool calls, bool refs,
                                 SiteList *out) {
    CtxGraphFile *f, *tmp;
    HASH_ITER(hh, g->files, f, tmp) {
        for (uint32_t i = 0; i < f->site_count; i++) {
            const CtxRefSite *site = &f->sites[i];
            if (!site->res_from) continue;
            bool kind_ok = (calls && site->kind == CTX_EDGE_CALLS) ||
                           (refs && site->kind == CTX_EDGE_REFERENCES);
            if (kind_ok && id_set_has(targets, site->res_to)) site_list_add(out, f, site);
        }
    }
    if (out->count > 1) qsort(out->items, out->count, sizeof(SiteRef), site_order);
}

/* Target ids: every match plus same-named declarations/definitions of the
 * best match, since resolution may bind a call to a prototype or a body. */
static void build_targets(const Matches *m, CtxGraph *g, IdSet *out) {
    for (uint32_t i = 0; i < m->count; i++) id_set_add(out, m->items[i]->id);
    if (m->count) {
        const CtxNameEntry *n = ctx_graph_find_name_locked(g, m->items[0]->name);
        for (uint32_t i = 0; n && i < n->count; i++)
            if (n->symbols[i]->kind == m->items[0]->kind || is_callable(n->symbols[i]))
                id_set_add(out, n->symbols[i]->id);
    }
    id_set_seal(out);
}

static void append_sites(CtxBuf *b, const char *root, const SiteList *sites, uint32_t cap) {
    CtxSource src = {0};
    const CtxGraphFile *open_file = NULL;
    bool have_src = false;
    for (uint32_t i = 0; i < sites->count && i < cap; i++) {
        const SiteRef *r = &sites->items[i];
        if (r->file != open_file) {
            if (have_src) ctx_source_close(&src);
            have_src = ctx_source_open(r->file->path, &src);
            open_file = r->file;
        }
        char rel[4096];
        ctx_path_display(root, r->file->path, rel, sizeof(rel));
        ctx_buf_printf(b, "  %s:%u  %s  ", rel, r->site->from_line,
                       r->site->from_name ? r->site->from_name : "<file scope>");
        uint32_t len = 0;
        const char *text = have_src ? ctx_source_line(&src, r->site->from_line, &len) : NULL;
        if (text) append_line_text(b, text, len);
        ctx_buf_append(b, "\n", 1);
    }
    if (have_src) ctx_source_close(&src);
    if (sites->count > cap) ctx_buf_printf(b, "  ... %u more sites\n", sites->count - cap);
}

static void site_files(const SiteList *sites, PathList *out) {
    for (uint32_t i = 0; i < sites->count; i++) path_list_add(out, sites->items[i].file->path);
}

static uint32_t distinct_callers(const SiteList *sites) {
    IdSet s = {0};
    for (uint32_t i = 0; i < sites->count; i++) id_set_add(&s, sites->items[i].site->res_from);
    id_set_seal(&s);
    uint32_t n = s.count;
    id_set_free(&s);
    return n;
}

/*
 * Appends transitive callers (levels 2..depth) as "caller ← its callers"
 * lines. Locked.
 */
static void append_transitive(CtxBuf *b, CtxGraph *g, const char *root, const SiteList *direct,
                              uint32_t depth, PathList *test_files) {
    IdSet frontier = {0}, seen = {0};
    for (uint32_t i = 0; i < direct->count; i++) {
        id_set_add(&frontier, direct->items[i].site->res_from);
        id_set_add(&seen, direct->items[i].site->res_from);
    }
    id_set_seal(&frontier);
    id_set_seal(&seen);

    uint32_t printed = 0;
    for (uint32_t level = 2; level <= depth && frontier.count && printed < NAV_MAX_TRANSITIVE; level++) {
        SiteList up = {0};
        collect_sites_locked(g, &frontier, true, false, &up);
        IdSet next = {0};
        if (up.count) ctx_buf_printf(b, "depth %u:\n", level);
        for (uint32_t i = 0; i < up.count && printed < NAV_MAX_TRANSITIVE; i++) {
            const CtxRefSite *site = up.items[i].site;
            if (id_set_has(&seen, site->res_from)) continue;
            const CtxSymbol *caller = ctx_graph_find_by_id_locked(g, site->res_from);
            const CtxSymbol *callee = ctx_graph_find_by_id_locked(g, site->res_to);
            if (!caller) continue;
            id_set_add(&next, caller->id);
            id_set_add(&seen, caller->id);
            id_set_seal(&seen);
            ctx_buf_printf(b, "  %s  ", caller->name);
            append_symbol_ref(b, root, caller);
            ctx_buf_printf(b, "  -> %s\n", callee ? callee->name : site->to_name);
            if (test_files && strstr(caller->file, "test")) path_list_add(test_files, caller->file);
            printed++;
        }
        id_set_seal(&next);
        id_set_free(&frontier);
        frontier = next;
        free(up.items);
    }
    if (printed >= NAV_MAX_TRANSITIVE) ctx_buf_printf(b, "  ... truncated at %u callers\n", printed);
    id_set_free(&frontier);
    id_set_free(&seen);
}

/*
 * Resolves the target, collects its sites, refreshes the files those sites
 * live in, and recollects. Leaves the graph read-locked on return.
 */
static void resolve_with_sites(CtxGraph *g, const char *root, const char *symbol, const char *file,
                               bool refs, Matches *m, IdSet *targets, SiteList *sites) {
    resolve_fresh(g, root, symbol, file, m);
    build_targets(m, g, targets);
    collect_sites_locked(g, targets, true, refs, sites);
    PathList files = {0};
    site_files(sites, &files);
    ctx_graph_runlock(g);
    bool changed = refresh_paths(&files);
    path_list_free(&files);

    ctx_graph_rlock(g);
    if (!changed) return;
    resolve_locked(g, root, symbol, file, m);
    id_set_free(targets);
    free(sites->items);
    memset(sites, 0, sizeof(*sites));
    build_targets(m, g, targets);
    collect_sites_locked(g, targets, true, refs, sites);
}

char *ctx_nav_callers(CtxGraph *g, const char *symbol, const char *file, uint32_t depth) {
    CtxBuf b = {0};
    const char *root = ctx_indexer_root();
    if (!g) return ctx_buf_take(&b);
    if (depth == 0) depth = 1;
    if (depth > NAV_MAX_DEPTH) depth = NAV_MAX_DEPTH;

    Matches m;
    IdSet targets = {0};
    SiteList sites = {0};
    resolve_with_sites(g, root, symbol, file, false, &m, &targets, &sites);
    if (m.count == 0) {
        ctx_buf_printf(&b, "error: %s\n", m.error);
    } else {
        ctx_buf_printf(&b, "callers of %s (", m.items[0]->name);
        append_symbol_ref(&b, root, m.items[0]);
        ctx_buf_printf(&b, "): %u call sites in %u functions\n", sites.count, distinct_callers(&sites));
        append_sites(&b, root, &sites, NAV_MAX_CALL_SITES);
        if (depth > 1) append_transitive(&b, g, root, &sites, depth, NULL);
    }
    ctx_graph_runlock(g);
    id_set_free(&targets);
    free(sites.items);
    return ctx_buf_take(&b);
}

char *ctx_nav_callees(CtxGraph *g, const char *symbol, const char *file) {
    CtxBuf b = {0};
    const char *root = ctx_indexer_root();
    if (!g) return ctx_buf_take(&b);

    Matches m;
    resolve_fresh(g, root, symbol, file, &m);
    if (m.count == 0) {
        ctx_graph_runlock(g);
        ctx_buf_printf(&b, "error: %s\n", m.error);
        return ctx_buf_take(&b);
    }
    const CtxSymbol *s = m.items[0];
    const CtxGraphFile *f = ctx_graph_find_file_locked(g, s->file);
    ctx_buf_printf(&b, "callees of %s (", s->name);
    append_symbol_ref(&b, root, s);
    ctx_buf_printf(&b, ")\n");

    CtxBuf unresolved = {0};
    uint32_t unresolved_count = 0, resolved_count = 0;
    for (uint32_t i = 0; f && i < f->site_count; i++) {
        const CtxRefSite *site = &f->sites[i];
        if (site->kind != CTX_EDGE_CALLS || !site->from_name) continue;
        if (site->from_line < s->line || site->from_line > s->end_line) continue;
        if (strcmp(site->from_name, s->name) != 0) continue;
        const CtxSymbol *target = site->res_to ? ctx_graph_find_by_id_locked(g, site->res_to) : NULL;
        if (!target) {
            if (unresolved_count < 40 && !strstr(unresolved.data ? unresolved.data : "", site->to_name))
                ctx_buf_printf(&unresolved, "%s%s", unresolved_count ? ", " : "", site->to_name);
            unresolved_count++;
            continue;
        }
        char sig[NAV_SIGNATURE_MAX + 8];
        compact_signature(target->signature, sig, sizeof(sig));
        ctx_buf_printf(&b, "  L%u  %s  ", site->from_line, target->name);
        append_symbol_ref(&b, root, target);
        ctx_buf_printf(&b, "  %s\n", sig);
        resolved_count++;
    }
    if (resolved_count == 0) ctx_buf_printf(&b, "  (no calls into indexed code)\n");
    if (unresolved.len) ctx_buf_printf(&b, "external/unresolved: %s\n", unresolved.data);
    free(unresolved.data);
    ctx_graph_runlock(g);
    return ctx_buf_take(&b);
}

char *ctx_nav_impact(CtxGraph *g, const char *symbol, const char *file) {
    CtxBuf b = {0};
    const char *root = ctx_indexer_root();
    if (!g) return ctx_buf_take(&b);

    Matches m;
    IdSet targets = {0};
    SiteList sites = {0};
    resolve_with_sites(g, root, symbol, file, true, &m, &targets, &sites);
    if (m.count == 0) {
        ctx_buf_printf(&b, "error: %s\n", m.error);
        ctx_graph_runlock(g);
        id_set_free(&targets);
        free(sites.items);
        return ctx_buf_take(&b);
    }

    const CtxSymbol *s = m.items[0];
    char sig[NAV_SIGNATURE_MAX + 8];
    compact_signature(s->signature, sig, sizeof(sig));
    ctx_buf_printf(&b, "impact of %s %s  ", kind_label(s->kind), s->name);
    append_symbol_ref(&b, root, s);
    ctx_buf_printf(&b, "\n  %s\n", sig);

    const CtxNameEntry *same = ctx_graph_find_name_locked(g, s->name);
    uint32_t decls = 0;
    for (uint32_t i = 0; same && i < same->count; i++) {
        const CtxSymbol *d = same->symbols[i];
        if (d == s || !is_navigable(d) || !id_set_has(&targets, d->id)) continue;
        if (decls++ == 0) ctx_buf_printf(&b, "also declared/defined:\n");
        if (decls > 10) continue;
        ctx_buf_printf(&b, "  %s ", d->is_definition ? "def " : "decl");
        append_symbol_ref(&b, root, d);
        ctx_buf_append(&b, "\n", 1);
    }

    SiteList calls = {0}, refs = {0};
    for (uint32_t i = 0; i < sites.count; i++) {
        if (sites.items[i].site->kind == CTX_EDGE_CALLS) site_list_add(&calls, sites.items[i].file, sites.items[i].site);
        else site_list_add(&refs, sites.items[i].file, sites.items[i].site);
    }
    ctx_buf_printf(&b, "direct call sites: %u in %u functions\n", calls.count, distinct_callers(&calls));
    append_sites(&b, root, &calls, NAV_MAX_CALL_SITES);

    PathList tests = {0};
    for (uint32_t i = 0; i < sites.count; i++)
        if (strstr(sites.items[i].file->path, "test")) path_list_add(&tests, sites.items[i].file->path);
    if (calls.count) {
        ctx_buf_printf(&b, "transitive callers:\n");
        append_transitive(&b, g, root, &calls, NAV_MAX_DEPTH, &tests);
    }
    if (refs.count) {
        ctx_buf_printf(&b, "non-call references: %u\n", refs.count);
        append_sites(&b, root, &refs, 30);
    }

    uint32_t subtypes = 0;
    CtxGraphFile *f, *tmp;
    HASH_ITER(hh, g->files, f, tmp) {
        for (uint32_t i = 0; i < f->site_count; i++) {
            const CtxRefSite *site = &f->sites[i];
            if (site->kind != CTX_EDGE_INHERITS || !id_set_has(&targets, site->res_to)) continue;
            const CtxSymbol *sub = ctx_graph_find_by_id_locked(g, site->res_from);
            if (!sub) continue;
            if (subtypes++ == 0) ctx_buf_printf(&b, "subtypes:\n");
            if (subtypes > 20) continue;
            ctx_buf_printf(&b, "  %s  ", sub->name);
            append_symbol_ref(&b, root, sub);
            ctx_buf_append(&b, "\n", 1);
        }
    }

    PathList files = {0};
    site_files(&sites, &files);
    ctx_buf_printf(&b, "files affected (%u):", files.count);
    for (uint32_t i = 0; i < files.count && i < 30; i++) {
        char rel[4096];
        ctx_path_display(root, files.items[i], rel, sizeof(rel));
        ctx_buf_printf(&b, "%s%s", i ? ", " : " ", rel);
    }
    ctx_buf_printf(&b, "%s\n", files.count > 30 ? ", ..." : "");
    ctx_buf_printf(&b, "tests touching it:");
    if (tests.count == 0) ctx_buf_printf(&b, " none found in the index\n");
    for (uint32_t i = 0; i < tests.count; i++) {
        char rel[4096];
        ctx_path_display(root, tests.items[i], rel, sizeof(rel));
        ctx_buf_printf(&b, "%s%s", i ? ", " : " ", rel);
    }
    if (tests.count) ctx_buf_append(&b, "\n", 1);

    ctx_graph_runlock(g);
    path_list_free(&files);
    path_list_free(&tests);
    free(calls.items);
    free(refs.items);
    id_set_free(&targets);
    free(sites.items);
    return ctx_buf_take(&b);
}

#include "context.h"
#include "../log/log.h"
#include <ctype.h>

/* ---- simple growable string builder ---- */
typedef struct { char *buf; size_t len; size_t cap; } SB;

static void sb_init(SB *s) { s->buf = (char*)malloc(4096); s->len = 0; s->cap = 4096; s->buf[0]='\0'; }
static void sb_ensure(SB *s, size_t extra) {
    if (s->len + extra + 1 > s->cap) {
        s->cap = (s->len + extra + 1) * 2;
        s->buf = (char*)realloc(s->buf, s->cap);
    }
}
static void sb_append(SB *s, const char *str) {
    size_t l = strlen(str);
    sb_ensure(s, l);
    memcpy(s->buf + s->len, str, l + 1);
    s->len += l;
}
static void sb_appendf(SB *s, const char *fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    sb_append(s, tmp);
}
static char *sb_take(SB *s) { return s->buf; }

/* ---- kind label ---- */
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
    default:                return "unknown";
    }
}

static int ci_contains(const char *haystack, const char *needle) {
    /* case-insensitive substring search */
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++)
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) break;
        if (j == nlen) return 1;
    }
    return 0;
}

char *ctx_context_for_symbol(CtxGraph *g, const char *name) {
    if (!g || !name) return strdup("# Error: invalid arguments\n");

    SB sb; sb_init(&sb);

    ctx_graph_rlock(g);
    CtxSymbol *sym = ctx_graph_find_by_name(g, name);
    if (!sym) {
        ctx_graph_runlock(g);
        sb_appendf(&sb, "# Symbol not found: %s\n", name);
        return sb_take(&sb);
    }

    sb_appendf(&sb, "# %s `%s`\n\n", kind_str(sym->kind), sym->name);
    sb_appendf(&sb, "**File:** `%s`  \n**Line:** %u  \n**Kind:** %s  \n",
               sym->file, sym->line, kind_str(sym->kind));
    if (sym->signature[0])
        sb_appendf(&sb, "\n```c\n%s\n```\n", sym->signature);

    /* Callers: edges where to_id == sym->id */
    sb_append(&sb, "\n## Called by\n");
    bool any_caller = false;
    CtxEdgeEntry *e;
    for (e = g->edges; e; e = (CtxEdgeEntry *)e->hh.next) {
        if (e->to_id == sym->id && e->kind == CTX_EDGE_CALLS) {
            CtxSymbol *caller;
            HASH_FIND(hh, g->symbols, &e->from_id, sizeof(uint64_t), caller);
            if (caller) { sb_appendf(&sb, "- `%s` (%s:%u)\n", caller->name, caller->file, caller->line); any_caller = true; }
        }
    }
    if (!any_caller) sb_append(&sb, "_none found_\n");

    /* Callees: edges where from_id == sym->id */
    sb_append(&sb, "\n## Calls\n");
    bool any_callee = false;
    for (e = g->edges; e; e = (CtxEdgeEntry *)e->hh.next) {
        if (e->from_id == sym->id && e->kind == CTX_EDGE_CALLS) {
            CtxSymbol *callee;
            HASH_FIND(hh, g->symbols, &e->to_id, sizeof(uint64_t), callee);
            if (callee) { sb_appendf(&sb, "- `%s` (%s:%u)\n", callee->name, callee->file, callee->line); any_callee = true; }
        }
    }
    if (!any_callee) sb_append(&sb, "_none found_\n");

    ctx_graph_runlock(g);
    return sb_take(&sb);
}

char *ctx_context_for_file(CtxGraph *g, const char *file_path) {
    if (!g || !file_path) return strdup("# Error: invalid arguments\n");

    SB sb; sb_init(&sb);
    sb_appendf(&sb, "# File: `%s`\n\n", file_path);

    ctx_graph_rlock(g);
    uint32_t count = 0;
    CtxSymbol *s, *stmp;
    HASH_ITER(hh, g->symbols, s, stmp) {
        if (!strcmp(s->file, file_path)) {
            sb_appendf(&sb, "- **%s** `%s` (line %u)", kind_str(s->kind), s->name, s->line);
            if (s->signature[0]) sb_appendf(&sb, "  \n  ```c\n  %s\n  ```", s->signature);
            sb_append(&sb, "\n");
            count++;
        }
    }
    ctx_graph_runlock(g);

    if (!count) sb_append(&sb, "_No symbols found for this file_\n");
    return sb_take(&sb);
}

char *ctx_context_summary(CtxGraph *g, const char *root_path) {
    if (!g) return strdup("# Error: no graph\n");

    SB sb; sb_init(&sb);
    sb_appendf(&sb, "# Codebase Summary\n\n");
    if (root_path) sb_appendf(&sb, "**Root:** `%s`\n\n", root_path);

    ctx_graph_rlock(g);
    uint32_t total    = ctx_graph_symbol_count(g);
    uint32_t edges    = ctx_graph_edge_count(g);
    uint32_t fns = 0, classes = 0, structs = 0, macros = 0, includes = 0, other = 0;
    CtxSymbol *s, *stmp;
    HASH_ITER(hh, g->symbols, s, stmp) {
        switch (s->kind) {
        case CTX_SYM_FUNCTION: case CTX_SYM_METHOD: fns++;      break;
        case CTX_SYM_CLASS:                          classes++;  break;
        case CTX_SYM_STRUCT: case CTX_SYM_ENUM:     structs++;  break;
        case CTX_SYM_MACRO:                          macros++;   break;
        case CTX_SYM_INCLUDE:                        includes++; break;
        default:                                     other++;    break;
        }
    }
    ctx_graph_runlock(g);

    sb_appendf(&sb, "| Metric | Count |\n|---|---|\n");
    sb_appendf(&sb, "| Total symbols | %u |\n", total);
    sb_appendf(&sb, "| Functions/Methods | %u |\n", fns);
    sb_appendf(&sb, "| Classes | %u |\n", classes);
    sb_appendf(&sb, "| Structs/Enums | %u |\n", structs);
    sb_appendf(&sb, "| Macros | %u |\n", macros);
    sb_appendf(&sb, "| Includes | %u |\n", includes);
    sb_appendf(&sb, "| Edges (relationships) | %u |\n", edges);
    return sb_take(&sb);
}

char *ctx_context_query(CtxGraph *g, const char *query, uint32_t max_results) {
    if (!g || !query) return strdup("# Error: invalid arguments\n");
    if (max_results == 0) max_results = 20;

    SB sb; sb_init(&sb);
    sb_appendf(&sb, "# Query: `%s`\n\n", query);

    ctx_graph_rlock(g);
    uint32_t found = 0;
    CtxSymbol *s, *stmp;
    HASH_ITER(hh, g->symbols, s, stmp) {
        if (found >= max_results) break;
        if (ci_contains(s->name, query)) {
            sb_appendf(&sb, "- **%s** `%s`  \n  `%s:%u`\n",
                       kind_str(s->kind), s->name, s->file, s->line);
            found++;
        }
    }
    ctx_graph_runlock(g);

    if (!found) sb_appendf(&sb, "_No symbols matching `%s`_\n", query);
    return sb_take(&sb);
}

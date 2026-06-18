#include "extractor.h"
#include "../parser/parser.h"
#include "../log/log.h"
#include "../jobs/jobs.h"

#include <dirent.h>
#include <sys/stat.h>

/* ---- text extraction helper ---- */
static char *node_text(const char *src, TSNode node, char *buf, size_t buf_sz) {
    if (ts_node_is_null(node)) { buf[0] = '\0'; return buf; }
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    uint32_t len   = end - start;
    if (len >= buf_sz) len = (uint32_t)(buf_sz - 1);
    memcpy(buf, src + start, len);
    buf[len] = '\0';
    /* collapse whitespace */
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n' || buf[i] == '\t') buf[i] = ' ';
    return buf;
}

/* ---- find first child of given type ---- */
static TSNode find_child(TSNode parent, const char *type) {
    if (ts_node_is_null(parent)) { TSNode null = {0}; return null; }
    uint32_t n = ts_node_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        TSNode child = ts_node_child(parent, i);
        if (!ts_node_is_null(child) && !strcmp(ts_node_type(child), type)) return child;
    }
    TSNode null = {0};
    return null;
}

/* ---- symbol kind from node type string ---- */
static CtxSymbolKind sym_kind_for(const char *ntype) {
    if (!strcmp(ntype, "function_definition") || !strcmp(ntype, "function_declaration"))
        return CTX_SYM_FUNCTION;
    if (!strcmp(ntype, "method_definition") || !strcmp(ntype, "method_declaration"))
        return CTX_SYM_METHOD;
    if (!strcmp(ntype, "class_definition") || !strcmp(ntype, "class_declaration") ||
        !strcmp(ntype, "class_specifier"))
        return CTX_SYM_CLASS;
    if (!strcmp(ntype, "struct_specifier")) return CTX_SYM_STRUCT;
    if (!strcmp(ntype, "enum_specifier"))   return CTX_SYM_ENUM;
    if (!strcmp(ntype, "typedef_declaration") || !strcmp(ntype, "type_alias_declaration"))
        return CTX_SYM_TYPEDEF;
    if (!strcmp(ntype, "preproc_def") || !strcmp(ntype, "preproc_function_def"))
        return CTX_SYM_MACRO;
    if (!strcmp(ntype, "preproc_include") || !strcmp(ntype, "import_statement") ||
        !strcmp(ntype, "import_from_statement"))
        return CTX_SYM_INCLUDE;
    if (!strcmp(ntype, "namespace_definition")) return CTX_SYM_NAMESPACE;
    return CTX_SYM_UNKNOWN;
}

static bool str_contains(const char *s, const char *needle) {
    return s && needle && strstr(s, needle) != NULL;
}

static bool should_emit_generic_node(TSNode node, const char *ntype) {
    if (!ntype || !ts_node_is_named(node) || ts_node_is_extra(node)) return false;
    if (!strcmp(ntype, "comment") || !strcmp(ntype, "string") ||
        !strcmp(ntype, "string_literal") || !strcmp(ntype, "ERROR")) {
        return false;
    }
    return str_contains(ntype, "declaration") ||
           str_contains(ntype, "definition") ||
           str_contains(ntype, "specifier") ||
           str_contains(ntype, "import") ||
           str_contains(ntype, "include") ||
           str_contains(ntype, "namespace") ||
           str_contains(ntype, "class") ||
           str_contains(ntype, "struct") ||
           str_contains(ntype, "enum") ||
           str_contains(ntype, "typedef") ||
           str_contains(ntype, "type_alias") ||
           str_contains(ntype, "function") ||
           str_contains(ntype, "method");
}

static void generic_node_name(const char *src, TSNode node, const char *ntype,
                              char *name, size_t name_sz,
                              char *signature, size_t signature_sz) {
    if (!name || name_sz == 0 || !signature || signature_sz == 0) return;

    char text[256];
    node_text(src, node, text, sizeof(text));
    for (size_t i = 0; text[i]; ++i) {
        if ((unsigned char)text[i] < 32) text[i] = ' ';
    }

    uint32_t line = ts_node_start_point(node).row + 1;
    uint32_t col = ts_node_start_point(node).column + 1;
    snprintf(name, name_sz, "%s@%u:%u", ntype ? ntype : "node", line, col);
    snprintf(signature, signature_sz, "%s", text);
}

/* ---- recursive AST walk ---- */
typedef struct {
    CtxGraph   *graph;
    const char *source;
    const char *filepath;
    char        enclosing_fn[256]; /* name of the innermost function being walked */
} WalkCtx;

typedef struct {
    TSNode   node;
    uint32_t depth;
    bool     exit_scope;
    char     restore_fn[256];
} WalkEntry;

typedef struct {
    WalkEntry *items;
    size_t     count;
    size_t     cap;
} WalkStack;

static bool walk_stack_push(WalkStack *stack, WalkEntry entry) {
    if (stack->count >= stack->cap) {
        size_t next_cap = stack->cap ? stack->cap * 2 : 256;
        WalkEntry *next = (WalkEntry *)realloc(stack->items, next_cap * sizeof(WalkEntry));
        if (!next) return false;
        stack->items = next;
        stack->cap = next_cap;
    }
    stack->items[stack->count++] = entry;
    return true;
}

static bool process_node(WalkCtx *ctx, TSNode node, bool *pushed_fn, char pushed_name[256]) {
    *pushed_fn = false;
    pushed_name[0] = '\0';
    const char *ntype = ts_node_type(node);
    char namebuf[256] = {0};
    char sigbuf[512]  = {0};

    /* Skip error nodes but log once */
    if (ts_node_is_error(node)) {
        CTX_LOG_TRACE("Skipping ERROR node in %s at byte %u",
                      ctx->filepath, ts_node_start_byte(node));
        /* Still recurse into children — partial info is better than none */
    }

    CtxSymbolKind kind = sym_kind_for(ntype);
    bool emit_sym = false;

    if (kind == CTX_SYM_FUNCTION || kind == CTX_SYM_METHOD) {
        /* C/C++: function_definition has a declarator child */
        TSNode decl = find_child(node, "function_declarator");
        if (ts_node_is_null(decl)) decl = find_child(node, "declarator");
        TSNode name_node = find_child(decl, "identifier");
        if (ts_node_is_null(name_node)) name_node = find_child(node, "identifier");
        if (!ts_node_is_null(name_node)) {
            node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
            /* signature = trim source of function node up to body */
            TSNode body = find_child(node, "compound_statement");
            if (!ts_node_is_null(body)) {
                uint32_t sig_end = ts_node_start_byte(body);
                uint32_t sig_start = ts_node_start_byte(node);
                uint32_t sig_len = sig_end - sig_start;
                if (sig_len >= sizeof(sigbuf)) sig_len = (uint32_t)(sizeof(sigbuf) - 1);
                memcpy(sigbuf, ctx->source + sig_start, sig_len);
                sigbuf[sig_len] = '\0';
                for (size_t i = 0; sigbuf[i]; i++) if (sigbuf[i]=='\n'||sigbuf[i]=='\t') sigbuf[i]=' ';
            }
            emit_sym = (namebuf[0] != '\0');
        }
        /* Python / JS: name field is directly "name" */
        if (!emit_sym) {
            TSNode name_node2 = find_child(node, "name");
            if (ts_node_is_null(name_node2)) name_node2 = find_child(node, "identifier");
            if (!ts_node_is_null(name_node2)) {
                node_text(ctx->source, name_node2, namebuf, sizeof(namebuf));
                emit_sym = (namebuf[0] != '\0');
            }
        }
    } else if (kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT || kind == CTX_SYM_ENUM) {
        TSNode name_node = find_child(node, "type_identifier");
        if (ts_node_is_null(name_node)) name_node = find_child(node, "identifier");
        if (ts_node_is_null(name_node)) name_node = find_child(node, "name");
        if (!ts_node_is_null(name_node)) {
            node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
            emit_sym = (namebuf[0] != '\0');
        }
    } else if (kind == CTX_SYM_MACRO) {
        TSNode name_node = find_child(node, "identifier");
        if (!ts_node_is_null(name_node)) {
            node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
            emit_sym = (namebuf[0] != '\0');
        }
    } else if (kind == CTX_SYM_INCLUDE) {
        /* include path or module name */
        char pathbuf[512] = {0};
        TSNode path_node = find_child(node, "string_literal");
        if (ts_node_is_null(path_node)) path_node = find_child(node, "system_lib_string");
        if (ts_node_is_null(path_node)) path_node = find_child(node, "dotted_name");
        if (!ts_node_is_null(path_node))
            node_text(ctx->source, path_node, pathbuf, sizeof(pathbuf));
        else
            node_text(ctx->source, node, pathbuf, sizeof(pathbuf));
        strncpy(namebuf, pathbuf, sizeof(namebuf) - 1);
        emit_sym = (namebuf[0] != '\0');
    } else if (!strcmp(ntype, "call_expression") || !strcmp(ntype, "call")) {
        /* Record a pending call — callee may not be in the graph yet during parallel extraction.
         * ctx_graph_resolve_calls() is called post-index to wire edges. */
        TSNode fn_node = find_child(node, "identifier");
        if (ts_node_is_null(fn_node)) fn_node = find_child(node, "field_expression");
        if (!ts_node_is_null(fn_node)) {
            char callee_name[256] = {0};
            node_text(ctx->source, fn_node, callee_name, sizeof(callee_name));
            if (callee_name[0]) {
                uint32_t call_line = ts_node_start_point(node).row + 1;
                ctx_graph_add_pending_call(ctx->graph, ctx->filepath,
                                           ctx->enclosing_fn, call_line, callee_name);
            }
        }
    }

    if (!emit_sym && should_emit_generic_node(node, ntype)) {
        generic_node_name(ctx->source, node, ntype, namebuf, sizeof(namebuf),
                          sigbuf, sizeof(sigbuf));
        emit_sym = (namebuf[0] != '\0');
        kind = CTX_SYM_UNKNOWN;
    }

    if (emit_sym && namebuf[0]) {
        CtxSymbol sym = {0};
        sym.id   = ctx_symbol_id(ctx->filepath, namebuf, ts_node_start_point(node).row + 1);
        strncpy(sym.name,      namebuf,         sizeof(sym.name)      - 1);
        strncpy(sym.file,      ctx->filepath,   sizeof(sym.file)      - 1);
        strncpy(sym.signature, sigbuf[0] ? sigbuf : namebuf, sizeof(sym.signature) - 1);
        sym.line         = ts_node_start_point(node).row + 1;
        sym.col          = ts_node_start_point(node).column + 1;
        sym.kind         = kind;
        sym.is_definition = (!strcmp(ntype, "function_definition") ||
                             !strcmp(ntype, "class_definition")    ||
                             !strcmp(ntype, "struct_specifier")    ||
                             !strcmp(ntype, "enum_specifier"));
        ctx_graph_add_symbol(ctx->graph, &sym);
    }

    /* Track enclosing function for call attribution */
    bool entered_fn = (kind == CTX_SYM_FUNCTION || kind == CTX_SYM_METHOD) && namebuf[0];
    if (entered_fn) {
        strncpy(pushed_name, namebuf, 255);
        pushed_name[255] = '\0';
    }
    *pushed_fn = entered_fn;
    return true;
}

static void walk_tree(WalkCtx *ctx, TSNode root) {
    if (ts_node_is_null(root)) return;

    WalkStack stack = {0};
    if (!walk_stack_push(&stack, (WalkEntry){ .node = root })) {
        CTX_LOG_WARN("Cannot allocate AST walk stack for %s", ctx->filepath);
        return;
    }

    while (stack.count > 0) {
        WalkEntry entry = stack.items[--stack.count];
        if (entry.exit_scope) {
            memcpy(ctx->enclosing_fn, entry.restore_fn, sizeof(ctx->enclosing_fn));
            continue;
        }

        if (ts_node_is_null(entry.node)) continue;
        if (entry.depth > 512) continue;

        bool pushed_fn = false;
        char pushed_name[256];
        if (!process_node(ctx, entry.node, &pushed_fn, pushed_name)) continue;

        if (pushed_fn) {
            WalkEntry exit_entry = { .exit_scope = true };
            memcpy(exit_entry.restore_fn, ctx->enclosing_fn, sizeof(exit_entry.restore_fn));
            if (!walk_stack_push(&stack, exit_entry)) break;
            strncpy(ctx->enclosing_fn, pushed_name, sizeof(ctx->enclosing_fn) - 1);
            ctx->enclosing_fn[sizeof(ctx->enclosing_fn) - 1] = '\0';
        }

        uint32_t n = ts_node_child_count(entry.node);
        for (uint32_t i = n; i > 0; --i) {
            TSNode child = ts_node_child(entry.node, i - 1);
            if (!walk_stack_push(&stack, (WalkEntry){
                    .node = child,
                    .depth = entry.depth + 1,
                })) {
                CTX_LOG_WARN("AST walk stack exhausted while indexing %s", ctx->filepath);
                stack.count = 0;
                break;
            }
        }
    }

    free(stack.items);
}

bool ctx_extract_file(CtxGraph *g, const char *path) {
    if (!g || !path) return false;

    CtxParseResult pr;
    if (!ctx_parser_parse_file(path, &pr)) return false;

    TSNode root = ts_tree_root_node(pr.tree);
    WalkCtx wctx = { .graph = g, .source = pr.source, .filepath = path };
    walk_tree(&wctx, root);

    ctx_parser_free_result(&pr);
    return true;
}

/* ---- skip-list for directory traversal ---- */
static bool should_skip_dir(const char *name) {
    static const char *skip[] = {
        "node_modules", ".git", "build", "bin", "__pycache__",
        ".cache", "dist", "target", ".svn", ".hg", NULL
    };
    if (name[0] == '.') return true; /* hidden */
    for (int i = 0; skip[i]; i++)
        if (!strcmp(name, skip[i])) return true;
    return false;
}

static bool is_source_file(const char *name) {
    return ctx_lang_from_path(name) != CTX_LANG_UNKNOWN;
}

typedef struct { CtxGraph *g; char path[4096]; } ExtractJobData;

static void extract_job_fn(void *ud) {
    ExtractJobData *d = (ExtractJobData *)ud;
    ctx_extract_file(d->g, d->path);
    free(d);
}

void ctx_extract_directory(CtxGraph *g, const char *dir, bool recursive) {
    if (!g || !dir) return;

    DIR *d = opendir(dir);
    if (!d) { CTX_LOG_WARN("Cannot open dir %s: %s", dir, strerror(errno)); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (recursive && !should_skip_dir(de->d_name))
                ctx_extract_directory(g, full, true);
        } else if (S_ISREG(st.st_mode) && is_source_file(de->d_name)) {
            ExtractJobData *jd = (ExtractJobData *)malloc(sizeof(ExtractJobData));
            if (jd) {
                jd->g = g;
                strncpy(jd->path, full, sizeof(jd->path) - 1);
                jd->path[sizeof(jd->path)-1] = '\0';
                ctx_job_submit_normal(extract_job_fn, jd);
            }
        }
    }
    closedir(d);
}

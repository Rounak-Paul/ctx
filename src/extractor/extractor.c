#include "extractor.h"
#include "../parser/parser.h"
#include "../log/log.h"

#include <ctype.h>

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

static bool is_identifier_type(const char *type);

static void rightmost_symbol_name(char *text) {
    if (!text || !text[0]) return;

    char *last = text;
    for (char *p = text; *p; ++p) {
        if (*p == '.' || *p == ':' || *p == '>' || *p == '/') {
            char *next = p + 1;
            while (*next == ':' || *next == '>' || *next == '.' || *next == '/') next++;
            if (*next) last = next;
        }
    }
    if (last != text)
        memmove(text, last, strlen(last) + 1);

    size_t len = strlen(text);
    while (len > 0 && !isalnum((unsigned char)text[len - 1]) && text[len - 1] != '_')
        text[--len] = '\0';
}

static bool symbol_name_from_node(const char *src, TSNode node, char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return false;
    buf[0] = '\0';
    if (ts_node_is_null(node)) return false;

    const char *type = ts_node_type(node);
    if (is_identifier_type(type)) {
        node_text(src, node, buf, buf_sz);
        rightmost_symbol_name(buf);
        return buf[0] != '\0';
    }

    TSNode stack[128];
    uint32_t count = 0;
    stack[count++] = node;
    while (count > 0) {
        TSNode cur = stack[--count];
        if (is_identifier_type(ts_node_type(cur))) {
            node_text(src, cur, buf, buf_sz);
            rightmost_symbol_name(buf);
            return buf[0] != '\0';
        }
        uint32_t n = ts_node_child_count(cur);
        for (uint32_t i = 0; i < n && count < 128; ++i)
            stack[count++] = ts_node_child(cur, i);
    }
    return false;
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

/*
 * Finds the first descendant of a given type in source order.
 *
 * parent  Root node to search below.
 * type    Tree-sitter node type to match.
 * limit   Maximum nodes to visit.
 */
static TSNode find_descendant(TSNode parent, const char *type, uint32_t limit) {
    TSNode null = {0};
    if (ts_node_is_null(parent) || !type || limit == 0) return null;

    TSNode stack[128];
    uint32_t count = 0, visited = 0;
    stack[count++] = parent;
    while (count > 0 && visited++ < limit) {
        TSNode cur = stack[--count];
        if (!ts_node_is_null(cur) && !strcmp(ts_node_type(cur), type))
            return cur;

        uint32_t n = ts_node_child_count(cur);
        for (uint32_t i = n; i > 0 && count < 128; --i)
            stack[count++] = ts_node_child(cur, i - 1);
    }
    return null;
}

static bool type_contains(const char *type, const char *needle) {
    return type && needle && strstr(type, needle) != NULL;
}

static bool is_identifier_type(const char *type) {
    return type && (!strcmp(type, "identifier") ||
                    !strcmp(type, "type_identifier") ||
                    !strcmp(type, "field_identifier") ||
                    !strcmp(type, "property_identifier") ||
                    !strcmp(type, "dotted_name") ||
                    !strcmp(type, "qualified_identifier") ||
                    !strcmp(type, "scoped_identifier") ||
                    !strcmp(type, "package_identifier")); /* Go package-qualified names */
}

static bool is_inheritance_container(const char *type) {
    return type && (type_contains(type, "superclass") ||
                    type_contains(type, "base_class") ||
                    type_contains(type, "base_clause") ||
                    type_contains(type, "extends") ||
                    type_contains(type, "heritage"));
}

static bool is_declaration_context(const char *type) {
    return type && (type_contains(type, "declarator") ||
                    type_contains(type, "declaration") ||
                    type_contains(type, "definition") ||
                    type_contains(type, "specifier") ||
                    type_contains(type, "alias"));
}

/* True when an identifier with the given parent/grandparent types names the
 * thing being declared rather than referencing another symbol. */
static bool is_declaration_name(const char *parent_type, const char *grand_type) {
    return is_declaration_context(parent_type) || is_declaration_context(grand_type);
}

static bool is_call_node(const char *type) {
    return type && (!strcmp(type, "call_expression") || !strcmp(type, "call"));
}

/* True when an identifier with the given parent/grandparent types is the
 * callee of a call (already recorded as a call site). */
static bool is_call_target(const char *parent_type, const char *grand_type) {
    return is_call_node(parent_type) || is_call_node(grand_type);
}

/*
 * Emits inheritance edges from a class-like symbol to base symbols found in
 * language-specific inheritance clauses.
 *
 * ex         Extraction buffer receiving inheritance reference sites.
 * source     Source text backing the tree-sitter nodes.
 * class_name Emitted class/struct symbol name.
 * class_line Emitted class/struct symbol line.
 * node       Class/struct node to inspect.
 */
static void emit_inheritance_edges(CtxFileExtract *ex, const char *source,
                                   const char *class_name,
                                   uint32_t class_line, TSNode node) {
    if (!ex || !source || !class_name || !class_name[0]) return;

    TSNode containers[16];
    uint32_t container_count = 0;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count && container_count < 16; ++i) {
        TSNode child = ts_node_child(node, i);
        if (is_inheritance_container(ts_node_type(child)))
            containers[container_count++] = child;
    }

    for (uint32_t i = 0; i < container_count; ++i) {
        TSNode stack[128];
        uint32_t count = 0;
        stack[count++] = containers[i];
        while (count > 0) {
            TSNode cur = stack[--count];
            const char *ctype = ts_node_type(cur);
            if (is_identifier_type(ctype)) {
                char base_name[256] = {0};
                node_text(source, cur, base_name, sizeof(base_name));
                if (base_name[0] && strcmp(base_name, class_name) != 0) {
                    ctx_file_extract_add_site(ex, class_name, class_line,
                                              base_name, CTX_EDGE_INHERITS);
                }
            }
            uint32_t n = ts_node_child_count(cur);
            for (uint32_t j = n; j > 0 && count < 128; --j)
                stack[count++] = ts_node_child(cur, j - 1);
        }
    }
}

/* C/C++ keywords and common short field names that should never become
 * reference edges — they create dense, meaningless graph noise across the
 * whole index. Identifiers shorter than 5 chars are almost always locals,
 * loop vars, or single-word field names that resolve to the wrong symbol. */
static bool is_noise_identifier(const char *name) {
    if (!name || !name[0]) return false;
    size_t len = strlen(name);
    if (len < 5) return true; /* i, j, n, fd, ok, ptr, buf, len, idx … */
    static const char *noise[] = {
        /* C/C++ keywords */
        "int","char","void","bool","float","double","long","short","unsigned",
        "signed","const","static","struct","class","enum","union","return",
        "true","false","null","NULL","size_t","this","self","auto","new",
        "delete","public","private","protected","virtual","override","template",
        "typename","namespace","using","include","define","ifdef","endif",
        "for","while","switch","case","break","continue","else","sizeof",
        "inline","extern","volatile","register","typedef","goto","default",
        /* ubiquitous short field/member names that resolve to wrong symbols */
        "width","height","depth","count","index","value","error","start",
        "flags","color","style","state","level","group","entry","child",
        "first","last","next","prev","data","text","name","type","node",
        "size","left","right","head","tail","root","file","path","line",
        "kind","mode","rank","code","info","base","list","hash","time",
        NULL
    };
    for (int i = 0; noise[i]; i++) if (!strcmp(name, noise[i])) return true;
    return false;
}

static bool is_variable_decl(const char *ntype) {
    return ntype && (!strcmp(ntype, "lexical_declaration") ||  /* JS const/let */
                     !strcmp(ntype, "variable_declaration"));
}

/*
 * Splits a C++ qualified name in place: "ns::Foo::bar" leaves "bar" in name
 * and writes "Foo" to qualifier. Template arguments in the qualifier are
 * dropped. Unqualified names leave qualifier empty.
 */
static void split_qualified_name(char *name, char *qualifier, size_t qualifier_size) {
    qualifier[0] = '\0';
    char *last = NULL;
    for (char *p = strstr(name, "::"); p; p = strstr(p + 2, "::")) last = p;
    if (!last) return;
    char *prev = NULL;
    for (char *p = strstr(name, "::"); p && p < last; p = strstr(p + 2, "::")) prev = p;
    const char *qstart = prev ? prev + 2 : name;
    size_t qlen = (size_t)(last - qstart);
    const char *angle = memchr(qstart, '<', qlen);
    if (angle) qlen = (size_t)(angle - qstart);
    if (qlen >= qualifier_size) qlen = qualifier_size - 1;
    memcpy(qualifier, qstart, qlen);
    qualifier[qlen] = '\0';
    memmove(name, last + 2, strlen(last + 2) + 1);
}

/* ---- symbol kind from node type string ---- */
static CtxSymbolKind sym_kind_for(const char *ntype) {
    /* C / C++ / Python / JS / TS */
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
    /* Go */
    if (!strcmp(ntype, "function_declaration")) return CTX_SYM_FUNCTION; /* already above */
    if (!strcmp(ntype, "method_declaration"))   return CTX_SYM_METHOD;   /* already above */
    if (!strcmp(ntype, "import_declaration"))   return CTX_SYM_INCLUDE;
    if (!strcmp(ntype, "const_declaration") || !strcmp(ntype, "var_declaration"))
        return CTX_SYM_VARIABLE;
    /* Go type_declaration wraps struct_type / interface_type — handled in process_node */
    if (!strcmp(ntype, "type_declaration"))     return CTX_SYM_TYPEDEF;
    /* Rust */
    if (!strcmp(ntype, "function_item"))        return CTX_SYM_FUNCTION;
    if (!strcmp(ntype, "struct_item"))          return CTX_SYM_STRUCT;
    if (!strcmp(ntype, "enum_item"))            return CTX_SYM_ENUM;
    if (!strcmp(ntype, "trait_item"))           return CTX_SYM_CLASS;
    if (!strcmp(ntype, "type_item"))            return CTX_SYM_TYPEDEF;
    if (!strcmp(ntype, "macro_definition"))     return CTX_SYM_MACRO;
    if (!strcmp(ntype, "use_declaration"))      return CTX_SYM_INCLUDE;
    if (!strcmp(ntype, "impl_item"))            return CTX_SYM_NAMESPACE; /* scope container */
    return CTX_SYM_UNKNOWN;
}

/* True for C/C++ struct/union/class/enum specifiers without a body, which
 * reference a type declared elsewhere instead of defining one. */
static bool is_type_reference(TSNode node, const char *ntype) {
    if (strcmp(ntype, "struct_specifier") && strcmp(ntype, "union_specifier") &&
        strcmp(ntype, "class_specifier") && strcmp(ntype, "enum_specifier"))
        return false;
    return ts_node_is_null(find_child(node, "field_declaration_list")) &&
           ts_node_is_null(find_child(node, "enumerator_list"));
}

/* ---- recursive AST walk ---- */
typedef struct {
    CtxFileExtract *ex;
    const char *source;
    const char *filepath;
    uint8_t     lang;
    const char *parent_type;  /* type of the current node's parent, NULL at root */
    const char *grand_type;   /* type of the grandparent, NULL near the root */
    char        enclosing_fn[256]; /* name of the innermost function being walked */
    char        enclosing_scope[256]; /* nearest class/struct/namespace for scope tagging */
} WalkCtx;

/* Enclosing function/scope saved when the walk enters a node that opens one;
 * restored once the walk leaves that node's subtree. */
typedef struct {
    uint32_t depth;
    char     fn[256];
    char     scope[256];
} ScopeSave;

typedef struct {
    ScopeSave *items;
    size_t     count;
    size_t     cap;
} ScopeStack;

static bool scope_stack_push(ScopeStack *stack, uint32_t depth, const WalkCtx *ctx) {
    if (stack->count >= stack->cap) {
        size_t next_cap = stack->cap ? stack->cap * 2 : 32;
        ScopeSave *next = (ScopeSave *)realloc(stack->items, next_cap * sizeof(ScopeSave));
        if (!next) return false;
        stack->items = next;
        stack->cap = next_cap;
    }
    ScopeSave *save = &stack->items[stack->count++];
    save->depth = depth;
    memcpy(save->fn, ctx->enclosing_fn, sizeof(save->fn));
    memcpy(save->scope, ctx->enclosing_scope, sizeof(save->scope));
    return true;
}

/* Restores every scope opened at a depth >= depth (their subtrees are done). */
static void scope_stack_unwind(ScopeStack *stack, uint32_t depth, WalkCtx *ctx) {
    while (stack->count > 0 && stack->items[stack->count - 1].depth >= depth) {
        ScopeSave *save = &stack->items[--stack->count];
        memcpy(ctx->enclosing_fn, save->fn, sizeof(ctx->enclosing_fn));
        memcpy(ctx->enclosing_scope, save->scope, sizeof(ctx->enclosing_scope));
    }
}

static bool process_node(WalkCtx *ctx, TSNode node, bool *pushed_fn,
                         bool *pushed_scope, char pushed_name[256]) {
    *pushed_fn = false;
    *pushed_scope = false;
    pushed_name[0] = '\0';
    const char *ntype = ts_node_type(node);
    char namebuf[256] = {0};
    char qualifier[256] = {0};
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
        TSNode decl = find_descendant(node, "function_declarator", 64);
        TSNode name_node = {0};
        /* The declarator field names the function itself (identifier,
         * field_identifier, Scope::name, ~Dtor, operator); a descendant
         * search would find parameter names first. */
        if (!ts_node_is_null(decl)) name_node = ts_node_child_by_field_name(decl, "declarator", 10);
        if (ts_node_is_null(decl)) decl = find_child(node, "declarator");
        if (ts_node_is_null(name_node)) name_node = find_descendant(decl, "identifier", 64);
        if (ts_node_is_null(name_node)) name_node = find_child(node, "identifier");
        if (!ts_node_is_null(name_node)) {
            node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
            split_qualified_name(namebuf, qualifier, sizeof(qualifier));
            /* signature = trim source of function node up to body */
            TSNode body = find_child(node, "compound_statement");
            if (ts_node_is_null(body)) body = find_child(node, "block");
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
        /* Python / JS / Go / Rust: name field is directly "name" or "identifier" */
        if (!emit_sym) {
            TSNode name_node2 = find_child(node, "name");
            if (ts_node_is_null(name_node2)) name_node2 = find_child(node, "identifier");
            if (!ts_node_is_null(name_node2)) {
                node_text(ctx->source, name_node2, namebuf, sizeof(namebuf));
                /* Go method receiver → scope */
                if (!strcmp(ntype, "method_declaration")) {
                    TSNode recv = find_child(node, "parameter_list");
                    if (!ts_node_is_null(recv)) {
                        TSNode rtype = find_child(recv, "type_identifier");
                        if (ts_node_is_null(rtype)) rtype = find_child(recv, "pointer_type");
                        if (!ts_node_is_null(rtype)) {
                            char recvbuf[128] = {0};
                            symbol_name_from_node(ctx->source, rtype, recvbuf, sizeof(recvbuf));
                            if (recvbuf[0]) strncpy(sigbuf, recvbuf, sizeof(sigbuf) - 1);
                        }
                    }
                }
                /* Rust function_item: grab signature up to body block */
                if (!strcmp(ntype, "function_item")) {
                    TSNode body = find_child(node, "block");
                    if (!ts_node_is_null(body)) {
                        uint32_t sig_end = ts_node_start_byte(body);
                        uint32_t sig_start = ts_node_start_byte(node);
                        uint32_t sig_len = sig_end - sig_start;
                        if (sig_len >= sizeof(sigbuf)) sig_len = (uint32_t)(sizeof(sigbuf) - 1);
                        memcpy(sigbuf, ctx->source + sig_start, sig_len);
                        sigbuf[sig_len] = '\0';
                        for (size_t i = 0; sigbuf[i]; i++) if (sigbuf[i]=='\n'||sigbuf[i]=='\t') sigbuf[i]=' ';
                    }
                }
                emit_sym = (namebuf[0] != '\0');
            }
        }
    } else if ((kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT || kind == CTX_SYM_ENUM) &&
               is_type_reference(node, ntype)) {
        /* `struct stat st;` names an existing type; it is not a definition. */
    } else if (kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT || kind == CTX_SYM_ENUM) {
        TSNode name_node = find_child(node, "type_identifier");
        if (ts_node_is_null(name_node)) name_node = find_child(node, "identifier");
        if (ts_node_is_null(name_node)) name_node = find_child(node, "name");
        if (!ts_node_is_null(name_node)) {
            node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
            emit_sym = (namebuf[0] != '\0');
        }
    } else if (kind == CTX_SYM_TYPEDEF) {
        /* Go type_declaration: contains a type_spec with the real name and underlying type */
        if (!strcmp(ntype, "type_declaration")) {
            TSNode spec = find_child(node, "type_spec");
            if (!ts_node_is_null(spec)) {
                TSNode name_node = find_child(spec, "type_identifier");
                if (ts_node_is_null(name_node)) name_node = find_child(spec, "identifier");
                if (!ts_node_is_null(name_node)) {
                    node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
                    /* Detect if underlying type is struct or interface → upgrade kind */
                    TSNode underlying = find_child(spec, "struct_type");
                    if (!ts_node_is_null(underlying)) kind = CTX_SYM_STRUCT;
                    else {
                        underlying = find_child(spec, "interface_type");
                        if (!ts_node_is_null(underlying)) kind = CTX_SYM_CLASS;
                    }
                    emit_sym = (namebuf[0] != '\0');
                }
            }
        } else {
            /* Rust type_item */
            TSNode name_node = find_child(node, "type_identifier");
            if (ts_node_is_null(name_node)) name_node = find_child(node, "identifier");
            if (!ts_node_is_null(name_node)) {
                node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
                emit_sym = (namebuf[0] != '\0');
            }
        }
    } else if (kind == CTX_SYM_NAMESPACE) {
        /* Rust impl_item: "impl Foo" or "impl Trait for Foo" — use the type name as scope */
        TSNode type_node = find_child(node, "type_identifier");
        if (ts_node_is_null(type_node)) type_node = find_child(node, "generic_type");
        if (!ts_node_is_null(type_node)) {
            symbol_name_from_node(ctx->source, type_node, namebuf, sizeof(namebuf));
            emit_sym = (namebuf[0] != '\0');
        }
    } else if (kind == CTX_SYM_VARIABLE) {
        /* Go const_declaration / var_declaration */
        TSNode spec = find_child(node, "const_spec");
        if (ts_node_is_null(spec)) spec = find_child(node, "var_spec");
        if (!ts_node_is_null(spec)) {
            TSNode name_node = find_child(spec, "identifier");
            if (!ts_node_is_null(name_node)) {
                node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
                emit_sym = (namebuf[0] != '\0' && !is_noise_identifier(namebuf));
            }
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
        /* Record a call site; the graph resolves it by name once every file is known. */
        TSNode fn_node = find_child(node, "identifier");
        if (ts_node_is_null(fn_node)) fn_node = find_child(node, "field_expression");
        if (!ts_node_is_null(fn_node)) {
            char callee_name[256] = {0};
            symbol_name_from_node(ctx->source, fn_node, callee_name, sizeof(callee_name));
            if (callee_name[0]) {
                uint32_t call_line = ts_node_start_point(node).row + 1;
                ctx_file_extract_add_site(ctx->ex, ctx->enclosing_fn, call_line,
                                          callee_name, CTX_EDGE_CALLS);
            }
        }
    } else if (!ctx->enclosing_fn[0] && is_variable_decl(ntype)) {
        /* Module-level variables/constants only — locals are graph noise. */
        char vname[256] = {0};
        symbol_name_from_node(ctx->source, node, vname, sizeof(vname));
        if (vname[0] && !is_noise_identifier(vname)) {
            strncpy(namebuf, vname, sizeof(namebuf) - 1);
            node_text(ctx->source, node, sigbuf, sizeof(sigbuf));
            kind = CTX_SYM_VARIABLE;
            emit_sym = true;
        }
    }

    if (emit_sym && namebuf[0]) {
        CtxSymbolDraft sym = {0};
        sym.id   = ctx_symbol_id(ctx->filepath, namebuf, ts_node_start_point(node).row + 1);
        strncpy(sym.name,      namebuf,         sizeof(sym.name)      - 1);
        strncpy(sym.signature, sigbuf[0] ? sigbuf : namebuf, sizeof(sym.signature) - 1);
        strncpy(sym.scope, ctx->enclosing_scope[0] ? ctx->enclosing_scope : qualifier,
                sizeof(sym.scope) - 1);
        sym.line         = ts_node_start_point(node).row + 1;
        sym.col          = ts_node_start_point(node).column + 1;
        sym.end_line     = ts_node_end_point(node).row + 1;
        sym.lang         = ctx->lang;
        sym.kind         = kind;
        sym.is_definition = (!strcmp(ntype, "function_definition") ||
                             !strcmp(ntype, "class_definition")    ||
                             !strcmp(ntype, "struct_specifier")    ||
                             !strcmp(ntype, "enum_specifier")      ||
                             /* Go */
                             !strcmp(ntype, "function_declaration") ||
                             !strcmp(ntype, "method_declaration")   ||
                             !strcmp(ntype, "type_declaration")     ||
                             /* Rust */
                             !strcmp(ntype, "function_item")        ||
                             !strcmp(ntype, "struct_item")          ||
                             !strcmp(ntype, "enum_item")            ||
                             !strcmp(ntype, "trait_item")           ||
                             !strcmp(ntype, "impl_item"));
        ctx_file_extract_add_symbol(ctx->ex, &sym);
        if (kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT) {
            emit_inheritance_edges(ctx->ex, ctx->source, namebuf, sym.line, node);
        }
    } else if (ctx->enclosing_fn[0] && is_identifier_type(ntype) &&
               !is_call_target(ctx->parent_type, ctx->grand_type) &&
               !is_declaration_name(ctx->parent_type, ctx->grand_type)) {
        char ref_name[256] = {0};
        symbol_name_from_node(ctx->source, node, ref_name, sizeof(ref_name));
        if (ref_name[0] && !is_noise_identifier(ref_name) &&
            strcmp(ref_name, ctx->enclosing_fn) != 0) {
            uint32_t ref_line = ts_node_start_point(node).row + 1;
            ctx_file_extract_add_site(ctx->ex, ctx->enclosing_fn, ref_line,
                                      ref_name, CTX_EDGE_REFERENCES);
        }
    }

    /* Track enclosing function for call attribution and class/namespace for
     * scope tagging. Both use the same save/restore mechanism in walk_tree. */
    bool entered_fn = (kind == CTX_SYM_FUNCTION || kind == CTX_SYM_METHOD) && namebuf[0];
    bool entered_scope = (kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT ||
                          kind == CTX_SYM_NAMESPACE ||
                          /* Rust impl_item and Go type_declaration act as scope containers */
                          (!strcmp(ntype, "impl_item") && namebuf[0]) ||
                          (!strcmp(ntype, "type_declaration") &&
                           (kind == CTX_SYM_STRUCT || kind == CTX_SYM_CLASS))) && namebuf[0];
    if (entered_fn || entered_scope) {
        strncpy(pushed_name, namebuf, 255);
        pushed_name[255] = '\0';
    }
    *pushed_fn = entered_fn;
    *pushed_scope = entered_scope;
    return true;
}

#define CTX_WALK_MAX_DEPTH 512u

/*
 * Pre-order walk with a tree cursor: O(nodes) regardless of fan-out and no
 * recursion. Subtrees deeper than CTX_WALK_MAX_DEPTH are skipped.
 */
static void walk_tree(WalkCtx *ctx, TSNode root) {
    if (ts_node_is_null(root)) return;

    ScopeStack scopes = {0};
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    const char *types[CTX_WALK_MAX_DEPTH + 2];
    uint32_t depth = 0;
    for (;;) {
        scope_stack_unwind(&scopes, depth, ctx);
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (depth <= CTX_WALK_MAX_DEPTH) types[depth] = ts_node_type(node);
        ctx->parent_type = depth >= 1 && depth - 1 <= CTX_WALK_MAX_DEPTH ? types[depth - 1] : NULL;
        ctx->grand_type = depth >= 2 && depth - 2 <= CTX_WALK_MAX_DEPTH ? types[depth - 2] : NULL;

        bool descend = false;
        bool pushed_fn = false, pushed_scope = false;
        char pushed_name[256];
        if (depth <= CTX_WALK_MAX_DEPTH &&
            process_node(ctx, node, &pushed_fn, &pushed_scope, pushed_name)) {
            descend = true;
            if (pushed_fn || pushed_scope) {
                if (!scope_stack_push(&scopes, depth, ctx)) {
                    CTX_LOG_WARN("Cannot allocate scope stack while indexing %s", ctx->filepath);
                    break;
                }
                if (pushed_fn) {
                    strncpy(ctx->enclosing_fn, pushed_name, sizeof(ctx->enclosing_fn) - 1);
                    ctx->enclosing_fn[sizeof(ctx->enclosing_fn) - 1] = '\0';
                }
                if (pushed_scope) {
                    strncpy(ctx->enclosing_scope, pushed_name, sizeof(ctx->enclosing_scope) - 1);
                    ctx->enclosing_scope[sizeof(ctx->enclosing_scope) - 1] = '\0';
                }
            }
        }

        if (descend && ts_tree_cursor_goto_first_child(&cursor)) {
            depth++;
            continue;
        }
        bool advanced = false;
        for (;;) {
            if (ts_tree_cursor_goto_next_sibling(&cursor)) { advanced = true; break; }
            if (!ts_tree_cursor_goto_parent(&cursor)) break;
            depth--;
        }
        if (!advanced) break;
    }
    scope_stack_unwind(&scopes, 0, ctx);
    ts_tree_cursor_delete(&cursor);
    free(scopes.items);
}

bool ctx_extract_file(const char *path, CtxFileExtract *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));

    CtxParseResult pr;
    if (!ctx_parser_parse_file(path, &pr)) return false;

    TSNode root = ts_tree_root_node(pr.tree);
    WalkCtx wctx = { .ex = out, .source = pr.source, .filepath = path,
                     .lang = (uint8_t)pr.lang };
    walk_tree(&wctx, root);

    ctx_parser_free_result(&pr);
    return true;
}

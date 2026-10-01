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
static bool is_name_node(const char *type);
static void dotted_path(const char *src, TSNode node, char *out, size_t out_size);

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/*
 * Splits a source-level name into its unqualified name and "::" qualifier.
 * Template arguments, parenthesised parts (decltype) and whitespace are
 * dropped, a leading global "::" is ignored, and member access (a.b, a->b)
 * keeps only the member. Operator names are kept verbatim.
 *   "ns::Foo<int>::bar" -> name "bar", qualifier "ns::Foo"
 *   "obj->field.method" -> name "method", qualifier ""
 *
 * text            Name text taken from the source.
 * name            Receives the unqualified name ("" when none).
 * name_size       Capacity of name.
 * qualifier       Receives the "::"-joined qualifier; may be NULL.
 * qualifier_size  Capacity of qualifier.
 */
static void parse_qualified_name(const char *text, char *name, size_t name_size,
                                 char *qualifier, size_t qualifier_size) {
    size_t name_len = 0, qual_len = 0;
    name[0] = '\0';
    if (qualifier && qualifier_size) qualifier[0] = '\0';
    if (!text) return;

    uint32_t depth = 0;
    for (const char *p = text; *p;) {
        char c = *p;
        if (depth > 0) {
            if (c == '<' || c == '(') depth++;
            else if (c == '>' || c == ')') depth--;
            p++;
            continue;
        }
        if (name_len == 0 && !strncmp(p, "operator", 8) && !is_ident_char(p[8])) {
            bool space = false;
            for (; *p && name_len + 1 < name_size; p++) {
                if (isspace((unsigned char)*p)) { space = true; continue; }
                if (space && is_ident_char(*p) && is_ident_char(name[name_len - 1]))
                    name[name_len++] = ' ';
                if (name_len + 1 < name_size) name[name_len++] = *p;
                space = false;
            }
            break;
        }
        if (c == ':' && p[1] == ':') {
            if (name_len && qualifier && qual_len + name_len + 3 < qualifier_size) {
                if (qual_len) { qualifier[qual_len++] = ':'; qualifier[qual_len++] = ':'; }
                memcpy(qualifier + qual_len, name, name_len);
                qual_len += name_len;
                qualifier[qual_len] = '\0';
            }
            name_len = 0;
            p += 2;
            continue;
        }
        if (c == '.' || (c == '-' && p[1] == '>')) {
            name_len = 0;
            qual_len = 0;
            if (qualifier && qualifier_size) qualifier[0] = '\0';
            p += c == '.' ? 1 : 2;
            continue;
        }
        if (c == '<' || c == '(') depth++;
        else if ((is_ident_char(c) || (c == '~' && name_len == 0)) && name_len + 1 < name_size)
            name[name_len++] = c;
        p++;
    }
    name[name_len] = '\0';
}

/*
 * Extracts the symbol name (and qualifier) a node denotes: the node itself
 * when it is a name node, else its first name descendant in source order.
 *
 * src             Source text backing the node.
 * node            Node to inspect.
 * buf             Receives the unqualified name.
 * buf_sz          Capacity of buf.
 * qualifier       Receives the "::" qualifier; may be NULL.
 * qualifier_size  Capacity of qualifier.
 * Returns true when a non-empty name was found.
 */
static bool symbol_name_from_node(const char *src, TSNode node, char *buf, size_t buf_sz,
                                  char *qualifier, size_t qualifier_size) {
    if (!buf || buf_sz == 0) return false;
    buf[0] = '\0';
    if (qualifier && qualifier_size) qualifier[0] = '\0';
    if (ts_node_is_null(node)) return false;

    TSNode stack[128];
    uint32_t count = 0;
    stack[count++] = node;
    while (count > 0) {
        TSNode cur = stack[--count];
        if (is_name_node(ts_node_type(cur))) {
            char text[512];
            node_text(src, cur, text, sizeof(text));
            parse_qualified_name(text, buf, buf_sz, qualifier, qualifier_size);
            return buf[0] != '\0';
        }
        uint32_t n = ts_node_child_count(cur);
        for (uint32_t i = n; i > 0 && count < 128; --i)
            stack[count++] = ts_node_child(cur, i - 1);
    }
    return false;
}

/*
 * Resolves the target named by a call's function expression or a base-class
 * entry. Member access keeps only the member and reports the object
 * expression. C/C++/Rust field access (obj.f, ptr->f) and this/self access
 * in other languages are flagged as member targets.
 *
 * src             Source text backing the node.
 * node            Function/base expression node.
 * name            Receives the unqualified target name.
 * name_size       Capacity of name.
 * qualifier       Receives the explicit "::" qualifier.
 * qualifier_size  Capacity of qualifier.
 * member          Set when the target is reached through an object.
 * object          Receives the object expression of a member access (null
 *                 node otherwise); may be NULL.
 * Returns true when a target name was found.
 */
static bool target_from_node(const char *src, TSNode node, char *name, size_t name_size,
                             char *qualifier, size_t qualifier_size, bool *member, TSNode *object) {
    TSNode null = {0};
    TSNode obj = null;
    *member = false;
    if (object) *object = null;
    if (ts_node_is_null(node)) return false;
    const char *type = ts_node_type(node);
    TSNode field = null;
    if (!strcmp(type, "field_expression")) {
        field = ts_node_child_by_field_name(node, "field", 5);
        obj = ts_node_child_by_field_name(node, "argument", 8);
        *member = !ts_node_is_null(field);
    } else if (!strcmp(type, "member_expression") || !strcmp(type, "attribute")) {
        field = ts_node_child_by_field_name(node, type[0] == 'm' ? "property" : "attribute",
                                            type[0] == 'm' ? 8 : 9);
        obj = ts_node_child_by_field_name(node, "object", 6);
        if (!ts_node_is_null(obj) && !ts_node_is_null(field)) {
            char text[8];
            node_text(src, obj, text, sizeof(text));
            *member = !strcmp(text, "this") || !strcmp(text, "self");
        }
    } else if (!strcmp(type, "selector_expression")) {
        field = ts_node_child_by_field_name(node, "field", 5);
    }
    if (!ts_node_is_null(field)) node = field;
    if (object && *member) *object = obj;
    return symbol_name_from_node(src, node, name, name_size, qualifier, qualifier_size);
}

/* Writes outer::inner (either may be empty) into out. */
static void join_scope(char *out, size_t out_size, const char *outer, const char *inner) {
    if (outer[0] && inner[0]) snprintf(out, out_size, "%s::%s", outer, inner);
    else                      snprintf(out, out_size, "%s", outer[0] ? outer : inner);
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

/* Nodes whose text spells a (possibly qualified or templated) symbol name. */
static bool is_name_node(const char *type) {
    return is_identifier_type(type) || (type && (
               !strcmp(type, "qualified_type_identifier") ||
               !strcmp(type, "qualified_field_identifier") ||
               !strcmp(type, "scoped_type_identifier") ||
               !strcmp(type, "template_function") ||
               !strcmp(type, "template_method") ||
               !strcmp(type, "template_type") ||
               !strcmp(type, "generic_type") ||
               !strcmp(type, "destructor_name") ||
               !strcmp(type, "operator_name")));
}

/* Nodes spelling a "::" path; their own children are parts of that path. */
static bool is_qualified_node(const char *type) {
    return type && (!strcmp(type, "qualified_identifier") ||
                    !strcmp(type, "qualified_type_identifier") ||
                    !strcmp(type, "qualified_field_identifier") ||
                    !strcmp(type, "scoped_identifier") ||
                    !strcmp(type, "scoped_type_identifier"));
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
 * Emits inheritance sites from a class-like symbol to the bases named in its
 * language-specific inheritance clauses. Each base expression (ns::Base<T>,
 * mod.Base) yields one site carrying its explicit qualifier.
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
    TSNode superclasses = ts_node_child_by_field_name(node, "superclasses", 12);
    if (!ts_node_is_null(superclasses)) containers[container_count++] = superclasses;
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
            if (!strcmp(ctype, "keyword_argument")) continue;
            if (is_name_node(ctype) || is_qualified_node(ctype) ||
                !strcmp(ctype, "attribute") || !strcmp(ctype, "member_expression")) {
                char base_name[256], base_scope[256];
                bool member = false;
                bool found;
                if (!strcmp(ctype, "attribute")) {
                    char path[256];
                    dotted_path(source, cur, path, sizeof(path));
                    parse_qualified_name(path, base_name, sizeof(base_name), base_scope, sizeof(base_scope));
                    found = base_name[0] != '\0';
                } else {
                    found = target_from_node(source, cur, base_name, sizeof(base_name),
                                             base_scope, sizeof(base_scope), &member, NULL);
                }
                if (found && strcmp(base_name, class_name) != 0) {
                    CtxSiteDraft site = {
                        .from_name = class_name, .from_line = class_line, .to_name = base_name,
                        .to_scope = base_scope, .kind = CTX_EDGE_INHERITS,
                    };
                    ctx_file_extract_add_site(ex, &site);
                }
                continue;
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
    if (!strcmp(ntype, "typedef_declaration") || !strcmp(ntype, "type_alias_declaration") ||
        !strcmp(ntype, "type_definition") || !strcmp(ntype, "alias_declaration"))
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

/* ---- AST walk ---- */

/* Declared type of a local variable or parameter, in declaration order. */
typedef struct {
    char name[64];
    char type[256];  /* type expression, "" when untyped */
} LocalVar;

typedef struct {
    CtxFileExtract *ex;
    const char *source;
    const char *filepath;
    uint8_t     lang;
    const char *parent_type;  /* type of the current node's parent, NULL at root */
    const char *grand_type;   /* type of the grandparent, NULL near the root */
    TSNode      parent;       /* parent of the current node, null at the root */
    char        enclosing_fn[256]; /* name of the innermost function being walked */
    char        enclosing_scope[256]; /* "::"-joined enclosing namespaces/classes */
    bool        scope_is_type; /* innermost scope is a class/struct/trait/impl body */
    bool        in_method;    /* enclosing function is a member of a class */
    bool        fn_return_known; /* enclosing function's return type is recorded */
    char        fn_scope[256]; /* full scope of the enclosing function (class path for methods) */
    LocalVar   *vars;         /* typed locals/parameters of the enclosing functions */
    uint32_t    var_count;
    uint32_t    var_cap;
} WalkCtx;

/* Function and scope a node opens for its subtree. */
typedef struct {
    bool opens_fn;
    bool opens_scope;
    bool scope_is_type;
    bool fn_in_method;      /* function is a class member (in-class or Class::f) */
    bool fn_return_known;   /* function's return type was recorded */
    char fn[256];           /* function name */
    char fn_scope[256];     /* function's full scope */
    char scope[256];        /* scope path relative to the enclosing scope */
} NodeScope;

/* Enclosing function/scope saved when the walk enters a node that opens one;
 * restored once the walk leaves that node's subtree. */
typedef struct {
    uint32_t depth;
    uint32_t var_count;
    bool     scope_is_type;
    bool     in_method;
    bool     fn_return_known;
    char     fn[256];
    char     fn_scope[256];
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
    save->var_count = ctx->var_count;
    save->scope_is_type = ctx->scope_is_type;
    save->in_method = ctx->in_method;
    save->fn_return_known = ctx->fn_return_known;
    memcpy(save->fn_scope, ctx->fn_scope, sizeof(save->fn_scope));
    memcpy(save->fn, ctx->enclosing_fn, sizeof(save->fn));
    memcpy(save->scope, ctx->enclosing_scope, sizeof(save->scope));
    return true;
}

/* Restores every scope opened at a depth >= depth (their subtrees are done). */
static void scope_stack_unwind(ScopeStack *stack, uint32_t depth, WalkCtx *ctx) {
    while (stack->count > 0 && stack->items[stack->count - 1].depth >= depth) {
        ScopeSave *save = &stack->items[--stack->count];
        ctx->scope_is_type = save->scope_is_type;
        ctx->in_method = save->in_method;
        ctx->fn_return_known = save->fn_return_known;
        memcpy(ctx->fn_scope, save->fn_scope, sizeof(ctx->fn_scope));
        ctx->var_count = save->var_count;
        memcpy(ctx->enclosing_fn, save->fn, sizeof(ctx->enclosing_fn));
        memcpy(ctx->enclosing_scope, save->scope, sizeof(ctx->enclosing_scope));
    }
}

/*
 * Writes the "::"-joined path of a C++ namespace name node (namespace_identifier
 * or nested_namespace_specifier, possibly with inline segments) into out.
 */
static void namespace_path(const char *src, TSNode name_node, char *out, size_t out_size) {
    out[0] = '\0';
    size_t len = 0;
    TSNode stack[64];
    uint32_t count = 0;
    stack[count++] = name_node;
    while (count > 0) {
        TSNode cur = stack[--count];
        if (!strcmp(ts_node_type(cur), "namespace_identifier")) {
            char part[256];
            node_text(src, cur, part, sizeof(part));
            int n = snprintf(out + len, out_size - len, "%s%s", len ? "::" : "", part);
            if (n < 0 || (size_t)n >= out_size - len) return;
            len += (size_t)n;
            continue;
        }
        uint32_t n = ts_node_child_count(cur);
        for (uint32_t i = n; i > 0 && count < 64; --i)
            stack[count++] = ts_node_child(cur, i - 1);
    }
}

/* Copies a node's source (up to body, when present) into a one-line signature. */
static void signature_until(const char *src, TSNode node, TSNode body, char *out, size_t out_size) {
    uint32_t sig_start = ts_node_start_byte(node);
    uint32_t sig_end = ts_node_is_null(body) ? ts_node_end_byte(node) : ts_node_start_byte(body);
    uint32_t sig_len = sig_end > sig_start ? sig_end - sig_start : 0;
    if (sig_len >= out_size) sig_len = (uint32_t)(out_size - 1);
    memcpy(out, src + sig_start, sig_len);
    out[sig_len] = '\0';
    for (size_t i = 0; out[i]; i++) if (out[i] == '\n' || out[i] == '\t') out[i] = ' ';
}

/* ---- type expressions and lookup-declaration tracking ---- */

static bool is_cpp_family(uint8_t lang) {
    return lang == CTX_LANG_C || lang == CTX_LANG_CPP;
}

/* Writes the "::" path a name node spells (template arguments dropped). */
static void node_path(const char *src, TSNode node, char *out, size_t out_size) {
    char name[256], qual[256];
    out[0] = '\0';
    if (symbol_name_from_node(src, node, name, sizeof(name), qual, sizeof(qual)))
        join_scope(out, out_size, qual, name);
}

/*
 * Writes the "::" path of a pure dotted name (a, a.b.c); "" when node is
 * anything else (calls, subscripts, literals).
 */
static void dotted_path(const char *src, TSNode node, char *out, size_t out_size) {
    out[0] = '\0';
    const char *t = ts_node_type(node);
    if (!strcmp(t, "identifier") || !strcmp(t, "dotted_name")) {
        char text[256];
        node_text(src, node, text, sizeof(text));
        size_t len = 0;
        for (const char *p = text; *p && len + 3 < out_size; p++) {
            if (*p == '.') { out[len++] = ':'; out[len++] = ':'; }
            else if (!isspace((unsigned char)*p)) out[len++] = *p;
        }
        out[len] = '\0';
        return;
    }
    if (strcmp(t, "attribute") != 0) return;
    char base[256], attr[128];
    dotted_path(src, ts_node_child_by_field_name(node, "object", 6), base, sizeof(base));
    TSNode name = ts_node_child_by_field_name(node, "attribute", 9);
    if (!base[0] || ts_node_is_null(name)) return;
    node_text(src, name, attr, sizeof(attr));
    snprintf(out, out_size, "%s::%s", base, attr);
}

/* Template argument list of a (possibly qualified) templated name, or null. */
static TSNode template_arguments(TSNode node) {
    TSNode null = {0};
    for (int depth = 0; depth < 8 && !ts_node_is_null(node); depth++) {
        const char *t = ts_node_type(node);
        if (!strcmp(t, "template_type") || !strcmp(t, "template_function") ||
            !strcmp(t, "template_method"))
            return ts_node_child_by_field_name(node, "arguments", 9);
        if (!is_qualified_node(t)) break;
        node = ts_node_child_by_field_name(node, "name", 4);
    }
    return null;
}

/* Wrappers whose -> / * / result yield their first template argument. */
static bool is_pointer_wrapper(const char *path) {
    const char *last = strrchr(path, ':');
    const char *name = last ? last + 1 : path;
    return !strcmp(name, "unique_ptr") || !strcmp(name, "shared_ptr") ||
           !strcmp(name, "weak_ptr") || !strcmp(name, "optional") ||
           !strcmp(name, "make_unique") || !strcmp(name, "make_shared");
}

static void declared_type_path(const char *src, TSNode type, char *out, size_t out_size, int depth);

/* Type path of the first type argument of a template, "" when none. */
static void first_template_type(const char *src, TSNode args, char *out, size_t out_size, int depth) {
    out[0] = '\0';
    if (ts_node_is_null(args) || ts_node_named_child_count(args) == 0) return;
    TSNode arg = ts_node_named_child(args, 0);
    if (!strcmp(ts_node_type(arg), "type_descriptor"))
        arg = ts_node_child_by_field_name(arg, "type", 4);
    declared_type_path(src, arg, out, out_size, depth + 1);
}

/*
 * Writes the class path a C/C++ declared type denotes: the named type, the
 * pointee of a smart pointer/optional, or "" for primitive, auto and unnamed
 * types.
 *
 * type   Type specifier node.
 * depth  Recursion depth through wrapper template arguments.
 */
static void declared_type_path(const char *src, TSNode type, char *out, size_t out_size, int depth) {
    out[0] = '\0';
    if (ts_node_is_null(type) || depth > 3) return;
    const char *t = ts_node_type(type);
    if (!strcmp(t, "primitive_type") || !strcmp(t, "sized_type_specifier") ||
        !strcmp(t, "placeholder_type_specifier") || !strcmp(t, "auto") ||
        !strcmp(t, "decltype"))
        return;
    if (!strcmp(t, "struct_specifier") || !strcmp(t, "class_specifier") ||
        !strcmp(t, "union_specifier") || !strcmp(t, "enum_specifier")) {
        TSNode name = ts_node_child_by_field_name(type, "name", 4);
        if (!ts_node_is_null(name)) node_path(src, name, out, out_size);
        return;
    }
    if (!is_name_node(t) && !is_qualified_node(t)) return;
    node_path(src, type, out, out_size);
    if (is_pointer_wrapper(out)) first_template_type(src, template_arguments(type), out, out_size, depth);
}

/*
 * Writes the class path a Python annotation denotes: Foo, pkg.Foo,
 * Optional[Foo], Foo | None and "Foo" forward references; "" otherwise.
 */
static void python_type_path(const char *src, TSNode type, char *out, size_t out_size, int depth) {
    out[0] = '\0';
    if (ts_node_is_null(type) || depth > 4) return;
    const char *t = ts_node_type(type);
    if (!strcmp(t, "type") || !strcmp(t, "union_type") || !strcmp(t, "binary_operator")) {
        if (ts_node_named_child_count(type))
            python_type_path(src, ts_node_named_child(type, 0), out, out_size, depth + 1);
    } else if (!strcmp(t, "identifier") || !strcmp(t, "attribute") || !strcmp(t, "member_type")) {
        if (!strcmp(t, "member_type")) {
            char text[256];
            node_text(src, type, text, sizeof(text));
            size_t len = 0;
            for (const char *p = text; *p && len + 3 < out_size; p++) {
                if (*p == '.') { out[len++] = ':'; out[len++] = ':'; }
                else if (!isspace((unsigned char)*p)) out[len++] = *p;
            }
            out[len] = '\0';
        } else {
            dotted_path(src, type, out, out_size);
        }
        if (!strcmp(out, "None")) out[0] = '\0';
    } else if (!strcmp(t, "generic_type") || !strcmp(t, "subscript")) {
        TSNode base = !strcmp(t, "subscript") ? ts_node_child_by_field_name(type, "value", 5)
                                              : ts_node_named_child(type, 0);
        python_type_path(src, base, out, out_size, depth + 1);
        if (!strcmp(out, "Optional") || !strcmp(out, "typing::Optional")) {
            TSNode arg = !strcmp(t, "subscript") ? ts_node_child_by_field_name(type, "subscript", 9)
                                                 : ts_node_named_child(type, 1);
            if (!ts_node_is_null(arg) && !strcmp(ts_node_type(arg), "type_parameter") &&
                ts_node_named_child_count(arg))
                arg = ts_node_named_child(arg, 0);
            python_type_path(src, arg, out, out_size, depth + 1);
        }
    } else if (!strcmp(t, "string")) {
        char text[256];
        node_text(src, type, text, sizeof(text));
        size_t len = 0;
        for (const char *p = text; *p && len + 3 < out_size; p++) {
            if (*p == '"' || *p == '\'') continue;
            if (*p == '.') { out[len++] = ':'; out[len++] = ':'; }
            else out[len++] = *p;
        }
        out[len] = '\0';
    }
}

/* Writes "T<path>" into out, or "" when path is empty. */
static void type_expr_of_path(const char *path, char *out, size_t out_size) {
    if (path[0]) snprintf(out, out_size, "T%s", path);
    else out[0] = '\0';
}

/*
 * Name declared by a (possibly nested pointer/reference/array/init)
 * declarator; false for function declarators and unnamed declarators.
 */
static bool declarator_name(const char *src, TSNode decl, char *out, size_t out_size) {
    out[0] = '\0';
    for (int depth = 0; depth < 16 && !ts_node_is_null(decl); depth++) {
        const char *t = ts_node_type(decl);
        if (!strcmp(t, "identifier") || !strcmp(t, "field_identifier") || !strcmp(t, "type_identifier")) {
            node_text(src, decl, out, out_size);
            return out[0] != '\0';
        }
        if (!strcmp(t, "function_declarator") || !strcmp(t, "abstract_function_declarator")) return false;
        TSNode inner = ts_node_child_by_field_name(decl, "declarator", 10);
        if (ts_node_is_null(inner) && ts_node_named_child_count(decl) > 0)
            inner = ts_node_named_child(decl, 0);
        decl = inner;
    }
    return false;
}

/* Function declarator wrapped by a declarator (Foo *f(int)), or null. */
static TSNode function_declarator_of(TSNode decl) {
    TSNode null = {0};
    for (int depth = 0; depth < 8 && !ts_node_is_null(decl); depth++) {
        if (!strcmp(ts_node_type(decl), "function_declarator")) return decl;
        decl = ts_node_child_by_field_name(decl, "declarator", 10);
    }
    return null;
}

/* Remembers the type expression of a local/global variable or parameter. */
static void local_var_add(WalkCtx *ctx, const char *name, const char *type) {
    if (!name[0]) return;
    if (ctx->var_count >= ctx->var_cap) {
        uint32_t cap = ctx->var_cap ? ctx->var_cap * 2 : 32;
        LocalVar *next = (LocalVar *)realloc(ctx->vars, cap * sizeof(LocalVar));
        if (!next) return;
        ctx->vars = next;
        ctx->var_cap = cap;
    }
    LocalVar *v = &ctx->vars[ctx->var_count++];
    snprintf(v->name, sizeof(v->name), "%s", name);
    snprintf(v->type, sizeof(v->type), "%s", type);
}

/* Type expression of the latest visible variable named name ("" when untyped), or NULL. */
static const char *local_var_type(const WalkCtx *ctx, const char *name) {
    for (uint32_t i = ctx->var_count; i > 0; i--)
        if (!strcmp(ctx->vars[i - 1].name, name)) return ctx->vars[i - 1].type;
    return NULL;
}

/* Last line of the block enclosing the current node (where a declaration stays visible). */
static uint32_t enclosing_block_end(const WalkCtx *ctx, TSNode node) {
    TSNode block = ts_node_is_null(ctx->parent) ? node : ctx->parent;
    return ts_node_end_point(block).row + 1;
}

/* True for the receiver names that denote the current object. */
static bool is_self_name(const WalkCtx *ctx, const char *name) {
    if (ctx->lang == CTX_LANG_PYTHON) return !strcmp(name, "self") || !strcmp(name, "cls");
    return !strcmp(name, "this");
}

/* Root identifier of a Python attribute chain (a in a.b.c), or null. */
static TSNode attribute_root(TSNode node) {
    for (int depth = 0; depth < 32 && !ts_node_is_null(node); depth++) {
        if (strcmp(ts_node_type(node), "attribute") != 0) return node;
        node = ts_node_child_by_field_name(node, "object", 6);
    }
    TSNode null = {0};
    return null;
}

/*
 * True when a Python object expression denotes a value (self, a variable,
 * a call result) rather than a module or class path, whose attributes are
 * qualified names instead of members.
 */
static bool python_is_value(const WalkCtx *ctx, TSNode object) {
    TSNode root = attribute_root(object);
    if (ts_node_is_null(root) || strcmp(ts_node_type(root), "identifier") != 0) return true;
    char name[64];
    node_text(ctx->source, root, name, sizeof(name));
    return is_self_name(ctx, name) || local_var_type(ctx, name) != NULL;
}

#define CTX_EXPR_DEPTH 6

static void expr_type(const WalkCtx *ctx, TSNode node, char *out, size_t out_size, int depth);

/* Appends a "|<tag><name>" step to base (a type expression) into out. */
static void expr_step(const char *base, char tag, const char *name, char *out, size_t out_size) {
    int n = snprintf(out, out_size, "%s%c%c%s", base, CTX_TYPE_STEP_SEP, tag, name);
    if (n < 0 || (size_t)n >= out_size || !name[0]) out[0] = '\0';
}

/* Type expression of a call's result (constructor, function, method). */
static void call_result_type(const WalkCtx *ctx, TSNode call, char *out, size_t out_size, int depth) {
    out[0] = '\0';
    TSNode fn = ts_node_child_by_field_name(call, "function", 8);
    if (ts_node_is_null(fn)) return;
    const char *t = ts_node_type(fn);
    char base[256], name[128], path[256];
    if (!strcmp(t, "field_expression") || !strcmp(t, "attribute")) {
        bool python = t[0] == 'a';
        TSNode obj = ts_node_child_by_field_name(fn, python ? "object" : "argument", python ? 6 : 8);
        TSNode member = ts_node_child_by_field_name(fn, python ? "attribute" : "field", python ? 9 : 5);
        if (ts_node_is_null(obj) || ts_node_is_null(member)) return;
        symbol_name_from_node(ctx->source, member, name, sizeof(name), NULL, 0);
        if (python && !python_is_value(ctx, obj)) {
            dotted_path(ctx->source, fn, path, sizeof(path));
            if (path[0]) snprintf(out, out_size, "C%s", path);
            return;
        }
        expr_type(ctx, obj, base, sizeof(base), depth + 1);
        if (base[0]) expr_step(base, 'm', name, out, out_size);
        return;
    }
    if (ctx->lang == CTX_LANG_PYTHON && !strcmp(t, "identifier")) {
        node_text(ctx->source, fn, name, sizeof(name));
        if (!strcmp(name, "super")) { snprintf(out, out_size, "B"); return; }
    }
    if (!is_name_node(t) && !is_qualified_node(t)) return;
    node_path(ctx->source, fn, path, sizeof(path));
    if (is_pointer_wrapper(path)) {
        first_template_type(ctx->source, template_arguments(fn), path, sizeof(path), 0);
        type_expr_of_path(path, out, out_size);
    } else if (path[0]) {
        snprintf(out, out_size, "C%s", path);
    }
}

/*
 * Writes the type expression (see graph.h) of an expression node, or ""
 * when it cannot be typed statically.
 *
 * depth  Recursion depth through member/call chains.
 */
static void expr_type(const WalkCtx *ctx, TSNode node, char *out, size_t out_size, int depth) {
    out[0] = '\0';
    if (ts_node_is_null(node) || depth > CTX_EXPR_DEPTH) return;
    const char *t = ts_node_type(node);
    char base[256], name[128];

    if (!strcmp(t, "this")) {
        snprintf(out, out_size, "S");
    } else if (!strcmp(t, "identifier")) {
        node_text(ctx->source, node, name, sizeof(name));
        const char *local = is_self_name(ctx, name) ? NULL : local_var_type(ctx, name);
        if (is_self_name(ctx, name)) snprintf(out, out_size, "S");
        else if (local) snprintf(out, out_size, "%s", local);
        else if (ctx->lang == CTX_LANG_CPP && ctx->fn_scope[0]) snprintf(out, out_size, "V%s", name);
    } else if (!strcmp(t, "parenthesized_expression") || !strcmp(t, "await")) {
        if (ts_node_named_child_count(node)) expr_type(ctx, ts_node_named_child(node, 0), out, out_size, depth + 1);
    } else if (!strcmp(t, "pointer_expression")) {
        expr_type(ctx, ts_node_child_by_field_name(node, "argument", 8), out, out_size, depth + 1);
    } else if (!strcmp(t, "field_expression") || !strcmp(t, "attribute")) {
        bool python = t[0] == 'a';
        TSNode obj = ts_node_child_by_field_name(node, python ? "object" : "argument", python ? 6 : 8);
        TSNode field = ts_node_child_by_field_name(node, python ? "attribute" : "field", python ? 9 : 5);
        if (ts_node_is_null(field) || (python && !python_is_value(ctx, obj))) return;
        expr_type(ctx, obj, base, sizeof(base), depth + 1);
        node_text(ctx->source, field, name, sizeof(name));
        if (base[0]) expr_step(base, 'f', name, out, out_size);
    } else if (!strcmp(t, "new_expression")) {
        char path[256];
        declared_type_path(ctx->source, ts_node_child_by_field_name(node, "type", 4), path, sizeof(path), 0);
        type_expr_of_path(path, out, out_size);
    } else if (!strcmp(t, "compound_literal_expression")) {
        TSNode type = ts_node_child_by_field_name(node, "type", 4);
        if (!ts_node_is_null(type) && !strcmp(ts_node_type(type), "type_descriptor"))
            type = ts_node_child_by_field_name(type, "type", 4);
        char path[256];
        declared_type_path(ctx->source, type, path, sizeof(path), 0);
        type_expr_of_path(path, out, out_size);
    } else if (is_call_node(t)) {
        call_result_type(ctx, node, out, out_size, depth);
    }
}

/* Adds a RETURN declaration for function name in scope. */
static void record_return(WalkCtx *ctx, const char *scope, const char *name, const char *expr,
                          uint32_t line) {
    if (name[0] && expr[0])
        ctx_file_extract_add_decl(ctx->ex, CTX_DECL_RETURN, scope, name, expr, line, line, false);
}

/*
 * Records typed variables of a C/C++ declaration: locals, parameters and
 * file-scope globals feed receiver/argument typing; data members of classes
 * and structs become CTX_DECL_FIELD lookup declarations; function
 * prototypes and in-class method declarations record their return types.
 */
static void record_declaration(WalkCtx *ctx, TSNode node, const char *ntype) {
    bool in_fn = ctx->enclosing_fn[0] != '\0';
    bool field = !strcmp(ntype, "field_declaration");
    bool variable = !strcmp(ntype, "declaration") || !strcmp(ntype, "parameter_declaration") ||
                    !strcmp(ntype, "optional_parameter_declaration") || !strcmp(ntype, "for_range_loop");
    if (field ? in_fn || !ctx->scope_is_type : !variable) return;

    char declared[256];
    declared_type_path(ctx->source, ts_node_child_by_field_name(node, "type", 4),
                       declared, sizeof(declared), 0);
    uint32_t line = ts_node_start_point(node).row + 1;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        const char *fname = ts_node_field_name_for_child(node, i);
        if (!fname || strcmp(fname, "declarator") != 0) continue;
        TSNode decl = ts_node_child(node, i);
        char name[64], expr[256];

        TSNode fn_decl = function_declarator_of(decl);
        if (!ts_node_is_null(fn_decl)) {
            char fn_name[256], qual[256], scope[256], ret[256];
            node_path(ctx->source, ts_node_child_by_field_name(fn_decl, "declarator", 10),
                      fn_name, sizeof(fn_name));
            char bare[256];
            parse_qualified_name(fn_name, bare, sizeof(bare), qual, sizeof(qual));
            join_scope(scope, sizeof(scope), ctx->enclosing_scope, qual);
            type_expr_of_path(declared, ret, sizeof(ret));
            if (!in_fn) record_return(ctx, scope, bare, ret, line);
            continue;
        }
        if (!declarator_name(ctx->source, decl, name, sizeof(name))) continue;
        type_expr_of_path(declared, expr, sizeof(expr));
        if (!expr[0] && !strcmp(ts_node_type(decl), "init_declarator"))
            expr_type(ctx, ts_node_child_by_field_name(decl, "value", 5), expr, sizeof(expr), 0);
        if (field) {
            if (expr[0])
                ctx_file_extract_add_decl(ctx->ex, CTX_DECL_FIELD, ctx->enclosing_scope, name, expr,
                                          line, enclosing_block_end(ctx, node), false);
        } else if (in_fn || !strcmp(ntype, "declaration")) {
            local_var_add(ctx, name, expr);
        }
    }
}

/*
 * Records C/C++ lookup declarations: using-directives, using-declarations,
 * namespace aliases, alias-declarations and typedefs.
 */
static void record_lookup_decl(WalkCtx *ctx, TSNode node, const char *ntype) {
    uint32_t line = ts_node_start_point(node).row + 1;
    uint32_t end_line = enclosing_block_end(ctx, node);
    bool local = ctx->enclosing_fn[0] != '\0';
    char name[256] = {0}, target[256] = {0};

    if (!strcmp(ntype, "using_declaration")) {
        bool directive = false;
        TSNode path = {0};
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            TSNode child = ts_node_child(node, i);
            const char *t = ts_node_type(child);
            if (!strcmp(t, "namespace")) directive = true;
            else if (!strcmp(t, "enum")) return;
            else if (!strcmp(t, "identifier") || !strcmp(t, "qualified_identifier")) path = child;
        }
        if (ts_node_is_null(path)) return;
        char qual[256];
        if (!symbol_name_from_node(ctx->source, path, name, sizeof(name), qual, sizeof(qual))) return;
        join_scope(target, sizeof(target), qual, name);
        if (directive) {
            ctx_file_extract_add_decl(ctx->ex, CTX_DECL_USING_NAMESPACE, ctx->enclosing_scope,
                                      NULL, target, line, end_line, local);
        } else if (qual[0]) {
            ctx_file_extract_add_decl(ctx->ex, CTX_DECL_USING, ctx->enclosing_scope,
                                      name, target, line, end_line, local);
        }
    } else if (!strcmp(ntype, "namespace_alias_definition")) {
        TSNode alias = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(alias)) return;
        node_text(ctx->source, alias, name, sizeof(name));
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            TSNode child = ts_node_named_child(node, i);
            if (ts_node_start_byte(child) <= ts_node_start_byte(alias)) continue;
            namespace_path(ctx->source, child, target, sizeof(target));
            break;
        }
        ctx_file_extract_add_decl(ctx->ex, CTX_DECL_NAMESPACE_ALIAS, ctx->enclosing_scope,
                                  name, target, line, end_line, local);
    } else if (!strcmp(ntype, "alias_declaration")) {
        TSNode alias = ts_node_child_by_field_name(node, "name", 4);
        TSNode type = ts_node_child_by_field_name(node, "type", 4);
        if (ts_node_is_null(alias) || ts_node_is_null(type)) return;
        node_text(ctx->source, alias, name, sizeof(name));
        if (!strcmp(ts_node_type(type), "type_descriptor")) type = ts_node_child_by_field_name(type, "type", 4);
        declared_type_path(ctx->source, type, target, sizeof(target), 0);
        ctx_file_extract_add_decl(ctx->ex, CTX_DECL_TYPE_ALIAS, ctx->enclosing_scope,
                                  name, target, line, end_line, local);
    } else if (!strcmp(ntype, "type_definition")) {
        declared_type_path(ctx->source, ts_node_child_by_field_name(node, "type", 4),
                           target, sizeof(target), 0);
        if (!target[0]) return;
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            const char *fname = ts_node_field_name_for_child(node, i);
            if (!fname || strcmp(fname, "declarator") != 0) continue;
            if (!declarator_name(ctx->source, ts_node_child(node, i), name, sizeof(name))) continue;
            if (strcmp(name, target) != 0)
                ctx_file_extract_add_decl(ctx->ex, CTX_DECL_TYPE_ALIAS, ctx->enclosing_scope,
                                          name, target, line, end_line, local);
        }
    }
}

/* Converts a Python module reference (a.b, .a.b, ..) to a "::" path without leading dots. */
static void python_module_path(const char *src, TSNode node, char *out, size_t out_size) {
    char text[256];
    node_text(src, node, text, sizeof(text));
    const char *p = text;
    while (*p == '.') p++;
    size_t len = 0;
    for (; *p && len + 3 < out_size; p++) {
        if (*p == '.') { out[len++] = ':'; out[len++] = ':'; }
        else if (!isspace((unsigned char)*p)) out[len++] = *p;
    }
    out[len] = '\0';
}

/*
 * Records the names bound by a Python assignment/loop target (x, (a, b),
 * [a, *rest]) as untyped values so their attributes are member accesses.
 */
static void record_python_targets(WalkCtx *ctx, TSNode target, int depth) {
    if (ts_node_is_null(target) || depth > 8) return;
    const char *t = ts_node_type(target);
    if (!strcmp(t, "identifier")) {
        char name[64];
        node_text(ctx->source, target, name, sizeof(name));
        local_var_add(ctx, name, "");
    } else if (!strcmp(t, "pattern_list") || !strcmp(t, "tuple_pattern") || !strcmp(t, "list_pattern") ||
               !strcmp(t, "tuple") || !strcmp(t, "list") || !strcmp(t, "expression_list") ||
               !strcmp(t, "list_splat_pattern") || !strcmp(t, "as_pattern_target") ||
               !strcmp(t, "parenthesized_expression")) {
        uint32_t n = ts_node_named_child_count(target);
        for (uint32_t i = 0; i < n; i++) record_python_targets(ctx, ts_node_named_child(target, i), depth + 1);
    }
}

static bool is_python_comprehension(const char *t) {
    return !strcmp(t, "list_comprehension") || !strcmp(t, "set_comprehension") ||
           !strcmp(t, "dictionary_comprehension") || !strcmp(t, "generator_expression");
}

/*
 * Records Python name bindings: imports (aliases visible in this module),
 * typed locals/parameters/globals, class attributes and self.attr
 * assignments (fields), and return types inferred from return statements.
 */
static void record_python(WalkCtx *ctx, TSNode node, const char *ntype) {
    uint32_t line = ts_node_start_point(node).row + 1;
    uint32_t end_line = enclosing_block_end(ctx, node);
    bool local = ctx->enclosing_fn[0] != '\0';
    char name[256], target[256], expr[256];

    if (!strcmp(ntype, "import_statement")) {
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            TSNode item = ts_node_named_child(node, i);
            if (!strcmp(ts_node_type(item), "aliased_import")) {
                python_module_path(ctx->source, ts_node_child_by_field_name(item, "name", 4),
                                   target, sizeof(target));
                node_text(ctx->source, ts_node_child_by_field_name(item, "alias", 5), name, sizeof(name));
            } else {
                python_module_path(ctx->source, item, target, sizeof(target));
                size_t head = strcspn(target, ":");
                snprintf(name, sizeof(name), "%.*s", (int)head, target);
                target[head] = '\0';
            }
            ctx_file_extract_add_decl(ctx->ex, CTX_DECL_NAMESPACE_ALIAS, ctx->enclosing_scope,
                                      name, target, line, end_line, local);
        }
    } else if (!strcmp(ntype, "import_from_statement")) {
        char module[256];
        python_module_path(ctx->source, ts_node_child_by_field_name(node, "module_name", 11),
                           module, sizeof(module));
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            TSNode item = ts_node_child(node, i);
            const char *fname = ts_node_field_name_for_child(node, i);
            if (!strcmp(ts_node_type(item), "wildcard_import")) {
                if (module[0])
                    ctx_file_extract_add_decl(ctx->ex, CTX_DECL_USING_NAMESPACE, ctx->enclosing_scope,
                                              NULL, module, line, end_line, local);
                continue;
            }
            if (!fname || strcmp(fname, "name") != 0) continue;
            char imported[256];
            TSNode path = item;
            name[0] = '\0';
            if (!strcmp(ts_node_type(item), "aliased_import")) {
                path = ts_node_child_by_field_name(item, "name", 4);
                node_text(ctx->source, ts_node_child_by_field_name(item, "alias", 5), name, sizeof(name));
            }
            python_module_path(ctx->source, path, imported, sizeof(imported));
            if (!name[0]) {
                const char *last = strrchr(imported, ':');
                snprintf(name, sizeof(name), "%s", last ? last + 1 : imported);
            }
            join_scope(target, sizeof(target), module, imported);
            ctx_file_extract_add_decl(ctx->ex, CTX_DECL_USING, ctx->enclosing_scope,
                                      name, target, line, end_line, local);
        }
    } else if (!strcmp(ntype, "assignment")) {
        TSNode left = ts_node_child_by_field_name(node, "left", 4);
        TSNode type = ts_node_child_by_field_name(node, "type", 4);
        if (ts_node_is_null(left)) return;
        char path[256];
        python_type_path(ctx->source, type, path, sizeof(path), 0);
        type_expr_of_path(path, expr, sizeof(expr));
        if (!expr[0]) expr_type(ctx, ts_node_child_by_field_name(node, "right", 5), expr, sizeof(expr), 0);
        const char *lt = ts_node_type(left);
        if (strcmp(lt, "identifier") != 0 && strcmp(lt, "attribute") != 0) {
            record_python_targets(ctx, left, 0);
        } else if (!strcmp(lt, "identifier")) {
            node_text(ctx->source, left, name, sizeof(name));
            if (!local && ctx->scope_is_type) {
                if (expr[0])
                    ctx_file_extract_add_decl(ctx->ex, CTX_DECL_FIELD, ctx->enclosing_scope, name, expr,
                                              line, end_line, false);
            } else {
                local_var_add(ctx, name, expr);
            }
        } else if (!strcmp(lt, "attribute") && ctx->in_method && expr[0]) {
            TSNode obj = ts_node_child_by_field_name(left, "object", 6);
            char owner[16];
            node_text(ctx->source, obj, owner, sizeof(owner));
            if (strcmp(ts_node_type(obj), "identifier") != 0 || strcmp(owner, "self") != 0) return;
            node_text(ctx->source, ts_node_child_by_field_name(left, "attribute", 9), name, sizeof(name));
            ctx_file_extract_add_decl(ctx->ex, CTX_DECL_FIELD, ctx->enclosing_scope, name, expr,
                                      line, end_line, false);
        }
    } else if (!strcmp(ntype, "typed_parameter") || !strcmp(ntype, "typed_default_parameter") ||
               !strcmp(ntype, "default_parameter")) {
        TSNode id = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(id) && ts_node_named_child_count(node)) id = ts_node_named_child(node, 0);
        if (ts_node_is_null(id) || strcmp(ts_node_type(id), "identifier") != 0) return;
        node_text(ctx->source, id, name, sizeof(name));
        char path[256];
        python_type_path(ctx->source, ts_node_child_by_field_name(node, "type", 4), path, sizeof(path), 0);
        type_expr_of_path(path, expr, sizeof(expr));
        local_var_add(ctx, name, expr);
    } else if (!strcmp(ntype, "identifier") && ctx->parent_type &&
               (!strcmp(ctx->parent_type, "parameters") || !strcmp(ctx->parent_type, "lambda_parameters"))) {
        node_text(ctx->source, node, name, sizeof(name));
        local_var_add(ctx, name, "");
    } else if (!strcmp(ntype, "for_statement")) {
        record_python_targets(ctx, ts_node_child_by_field_name(node, "left", 4), 0);
    } else if (is_python_comprehension(ntype)) {
        /* The body precedes its for-clauses in the tree; bind their targets first. */
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            TSNode clause = ts_node_named_child(node, i);
            if (!strcmp(ts_node_type(clause), "for_in_clause"))
                record_python_targets(ctx, ts_node_child_by_field_name(clause, "left", 4), 0);
        }
    } else if (!strcmp(ntype, "as_pattern")) {
        record_python_targets(ctx, ts_node_child_by_field_name(node, "alias", 5), 0);
    } else if (!strcmp(ntype, "named_expression")) {
        record_python_targets(ctx, ts_node_child_by_field_name(node, "name", 4), 0);
    } else if (!strcmp(ntype, "return_statement") && local && !ctx->fn_return_known) {
        if (!ts_node_named_child_count(node)) return;
        expr_type(ctx, ts_node_named_child(node, 0), expr, sizeof(expr), 0);
        if (!expr[0]) return;
        record_return(ctx, ctx->enclosing_scope, ctx->enclosing_fn, expr, line);
        ctx->fn_return_known = true;
    }
}

#define CTX_ADL_MAX_ARGS 4

/*
 * Collects ';'-separated type expressions of a call's arguments for
 * argument-dependent lookup: typed variables, constructed temporaries,
 * call results and qualified names (whose qualifier names the namespace).
 */
static void argument_types(const WalkCtx *ctx, TSNode call, char *out, size_t out_size) {
    out[0] = '\0';
    TSNode args = ts_node_child_by_field_name(call, "arguments", 9);
    if (ts_node_is_null(args)) return;
    size_t len = 0;
    uint32_t added = 0;
    uint32_t n = ts_node_named_child_count(args);
    for (uint32_t i = 0; i < n && added < CTX_ADL_MAX_ARGS; i++) {
        TSNode arg = ts_node_named_child(args, i);
        char type[256] = {0};
        if (!strcmp(ts_node_type(arg), "qualified_identifier")) {
            char path[256];
            node_path(ctx->source, arg, path, sizeof(path));
            type_expr_of_path(path, type, sizeof(type));
        } else {
            expr_type(ctx, arg, type, sizeof(type), 0);
        }
        if (!type[0] || strchr(type, ';')) continue;
        int w = snprintf(out + len, out_size - len, "%s%s", len ? ";" : "", type);
        if (w < 0 || (size_t)w >= out_size - len) {
            out[len] = '\0';
            return;
        }
        len += (size_t)w;
        added++;
    }
}

/*
 * Classifies the target of a call or member reference.
 *
 * fn        Function expression of a call, or a member-access node.
 * name      Receives the unqualified target name.
 * qual      Receives the explicit qualifier (C++ ns::f, Python module.f).
 * recv      Receives the receiver type expression of a member access.
 * member    Set when the target is reached through an object.
 * Returns true when a target name was found.
 */
static bool classify_target(const WalkCtx *ctx, TSNode fn, char *name, size_t name_size,
                            char *qual, size_t qual_size, char *recv, size_t recv_size, bool *member) {
    name[0] = qual[0] = recv[0] = '\0';
    *member = false;
    if (ts_node_is_null(fn)) return false;
    const char *t = ts_node_type(fn);
    if (!strcmp(t, "field_expression")) {
        TSNode field = ts_node_child_by_field_name(fn, "field", 5);
        if (ts_node_is_null(field)) return false;
        symbol_name_from_node(ctx->source, field, name, name_size, qual, qual_size);
        *member = true;
        expr_type(ctx, ts_node_child_by_field_name(fn, "argument", 8), recv, recv_size, 0);
        return name[0] != '\0';
    }
    if (!strcmp(t, "attribute")) {
        TSNode obj = ts_node_child_by_field_name(fn, "object", 6);
        TSNode attr = ts_node_child_by_field_name(fn, "attribute", 9);
        if (ts_node_is_null(attr)) return false;
        node_text(ctx->source, attr, name, name_size);
        if (python_is_value(ctx, obj)) {
            *member = true;
            expr_type(ctx, obj, recv, recv_size, 0);
        } else {
            dotted_path(ctx->source, obj, qual, qual_size);
        }
        return name[0] != '\0';
    }
    bool object_member = false;
    TSNode object;
    if (!target_from_node(ctx->source, fn, name, name_size, qual, qual_size, &object_member, &object))
        return false;
    if (object_member) {
        *member = true;
        expr_type(ctx, object, recv, recv_size, 0);
    }
    return true;
}

/*
 * Extracts the symbol or reference site a node contributes and reports the
 * function/scope it opens for its subtree.
 *
 * ctx     Walk state (enclosing function/scope, parent types).
 * node    Node being visited.
 * opened  Receives the function/scope the node opens.
 */
static void process_node(WalkCtx *ctx, TSNode node, NodeScope *opened) {
    memset(opened, 0, sizeof(*opened));
    const char *ntype = ts_node_type(node);
    char namebuf[256] = {0};
    char qualifier[256] = {0};
    char sigbuf[512]  = {0};

    if (ts_node_is_error(node)) {
        CTX_LOG_TRACE("Skipping ERROR node in %s at byte %u",
                      ctx->filepath, ts_node_start_byte(node));
    }
    if (is_cpp_family(ctx->lang)) {
        record_lookup_decl(ctx, node, ntype);
        record_declaration(ctx, node, ntype);
    } else if (ctx->lang == CTX_LANG_PYTHON) {
        record_python(ctx, node, ntype);
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
            char text[512];
            node_text(ctx->source, name_node, text, sizeof(text));
            parse_qualified_name(text, namebuf, sizeof(namebuf), qualifier, sizeof(qualifier));
            TSNode body = find_child(node, "compound_statement");
            if (ts_node_is_null(body)) body = find_child(node, "block");
            if (!ts_node_is_null(body)) signature_until(ctx->source, node, body, sigbuf, sizeof(sigbuf));
            emit_sym = (namebuf[0] != '\0');
        }
        /* Python / JS / Go / Rust: name field is directly "name" or "identifier" */
        if (!emit_sym) {
            TSNode name_node2 = find_child(node, "name");
            if (ts_node_is_null(name_node2)) name_node2 = find_child(node, "identifier");
            if (!ts_node_is_null(name_node2)) {
                node_text(ctx->source, name_node2, namebuf, sizeof(namebuf));
                /* Go method receiver → signature */
                if (!strcmp(ntype, "method_declaration")) {
                    TSNode recv = find_child(node, "parameter_list");
                    if (!ts_node_is_null(recv)) {
                        TSNode rtype = find_child(recv, "type_identifier");
                        if (ts_node_is_null(rtype)) rtype = find_child(recv, "pointer_type");
                        if (!ts_node_is_null(rtype)) {
                            char recvbuf[128] = {0};
                            symbol_name_from_node(ctx->source, rtype, recvbuf, sizeof(recvbuf), NULL, 0);
                            if (recvbuf[0]) snprintf(sigbuf, sizeof(sigbuf), "%s", recvbuf);
                        }
                    }
                }
                /* Rust function_item: grab signature up to body block */
                if (!strcmp(ntype, "function_item")) {
                    TSNode body = find_child(node, "block");
                    if (!ts_node_is_null(body)) signature_until(ctx->source, node, body, sigbuf, sizeof(sigbuf));
                }
                emit_sym = (namebuf[0] != '\0');
            }
        }
        if (kind == CTX_SYM_FUNCTION && ctx->scope_is_type && !qualifier[0]) kind = CTX_SYM_METHOD;
        if (emit_sym) {
            char path[256], ret[256];
            if (is_cpp_family(ctx->lang))
                declared_type_path(ctx->source, ts_node_child_by_field_name(node, "type", 4), path, sizeof(path), 0);
            else if (ctx->lang == CTX_LANG_PYTHON)
                python_type_path(ctx->source, ts_node_child_by_field_name(node, "return_type", 11), path, sizeof(path), 0);
            else
                path[0] = '\0';
            type_expr_of_path(path, ret, sizeof(ret));
            join_scope(opened->fn_scope, sizeof(opened->fn_scope), ctx->enclosing_scope, qualifier);
            record_return(ctx, opened->fn_scope, namebuf, ret, ts_node_start_point(node).row + 1);
            opened->fn_return_known = ret[0] != '\0';
            opened->fn_in_method = ctx->scope_is_type || qualifier[0] != '\0';
        }
    } else if ((kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT || kind == CTX_SYM_ENUM) &&
               is_type_reference(node, ntype)) {
        /* `struct stat st;` names an existing type; it is not a definition. */
    } else if (kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT || kind == CTX_SYM_ENUM) {
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(name_node)) name_node = find_child(node, "type_identifier");
        if (ts_node_is_null(name_node)) name_node = find_child(node, "identifier");
        if (!ts_node_is_null(name_node)) {
            symbol_name_from_node(ctx->source, name_node, namebuf, sizeof(namebuf),
                                  qualifier, sizeof(qualifier));
            emit_sym = (namebuf[0] != '\0');
        } else if (ctx->parent_type && !strcmp(ctx->parent_type, "type_definition")) {
            /* typedef struct { ... } Name; names the anonymous struct */
            emit_sym = declarator_name(ctx->source,
                                       ts_node_child_by_field_name(ctx->parent, "declarator", 10),
                                       namebuf, sizeof(namebuf));
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
        } else if (!strcmp(ntype, "type_definition")) {
            TSNode type = ts_node_child_by_field_name(node, "type", 4);
            bool names_anonymous = !ts_node_is_null(type) &&
                                   ts_node_is_null(ts_node_child_by_field_name(type, "name", 4)) &&
                                   !ts_node_is_null(ts_node_child_by_field_name(type, "body", 4));
            TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
            emit_sym = !names_anonymous && declarator_name(ctx->source, decl, namebuf, sizeof(namebuf));
            if (emit_sym) signature_until(ctx->source, node, (TSNode){0}, sigbuf, sizeof(sigbuf));
        } else if (!strcmp(ntype, "alias_declaration")) {
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) {
                node_text(ctx->source, name_node, namebuf, sizeof(namebuf));
                signature_until(ctx->source, node, (TSNode){0}, sigbuf, sizeof(sigbuf));
                emit_sym = (namebuf[0] != '\0');
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
    } else if (kind == CTX_SYM_NAMESPACE && !strcmp(ntype, "namespace_definition")) {
        /* C++ namespace: named ones are symbols; every one opens a scope. */
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(name_node)) {
            opened->opens_scope = true;
            snprintf(opened->scope, sizeof(opened->scope), "%s", CTX_ANONYMOUS_SCOPE);
        } else {
            char path[256];
            namespace_path(ctx->source, name_node, path, sizeof(path));
            parse_qualified_name(path, namebuf, sizeof(namebuf), qualifier, sizeof(qualifier));
            if (namebuf[0]) {
                snprintf(sigbuf, sizeof(sigbuf), "namespace %s", path);
                snprintf(opened->scope, sizeof(opened->scope), "%s", path);
                opened->opens_scope = true;
                emit_sym = true;
            }
        }
    } else if (kind == CTX_SYM_NAMESPACE) {
        /* Rust impl_item: "impl Foo" or "impl Trait for Foo" — the type is the scope */
        TSNode type_node = ts_node_child_by_field_name(node, "type", 4);
        if (ts_node_is_null(type_node)) type_node = find_child(node, "type_identifier");
        if (ts_node_is_null(type_node)) type_node = find_child(node, "generic_type");
        if (!ts_node_is_null(type_node)) {
            symbol_name_from_node(ctx->source, type_node, namebuf, sizeof(namebuf), NULL, 0);
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
        snprintf(namebuf, sizeof(namebuf), "%s", pathbuf);
        emit_sym = (namebuf[0] != '\0');
    } else if (is_call_node(ntype)) {
        /* Record a call site; the graph resolves it by name once every file is known. */
        TSNode fn_node = ts_node_child_by_field_name(node, "function", 8);
        if (ts_node_is_null(fn_node)) fn_node = find_child(node, "identifier");
        if (ts_node_is_null(fn_node)) fn_node = find_child(node, "field_expression");
        char callee_name[256], callee_scope[256], recv[256], args[512] = {0};
        bool member = false;
        if (classify_target(ctx, fn_node, callee_name, sizeof(callee_name),
                            callee_scope, sizeof(callee_scope), recv, sizeof(recv), &member)) {
            if (!member && !callee_scope[0] && ctx->lang == CTX_LANG_CPP)
                argument_types(ctx, node, args, sizeof(args));
            CtxSiteDraft site = {
                .from_name = ctx->enclosing_fn, .from_line = ts_node_start_point(node).row + 1,
                .to_name = callee_name, .to_scope = callee_scope, .kind = CTX_EDGE_CALLS,
                .member = member, .recv = recv, .arg_types = args,
            };
            ctx_file_extract_add_site(ctx->ex, &site);
        }
    } else if (ctx->lang == CTX_LANG_PYTHON && !ctx->enclosing_fn[0] && !strcmp(ntype, "assignment")) {
        /* Module constants and class attributes */
        TSNode left = ts_node_child_by_field_name(node, "left", 4);
        if (!ts_node_is_null(left) && !strcmp(ts_node_type(left), "identifier")) {
            node_text(ctx->source, left, namebuf, sizeof(namebuf));
            node_text(ctx->source, node, sigbuf, sizeof(sigbuf));
            kind = CTX_SYM_VARIABLE;
            emit_sym = namebuf[0] && !is_noise_identifier(namebuf);
            if (!emit_sym) namebuf[0] = '\0';
        }
    } else if (!ctx->enclosing_fn[0] && is_variable_decl(ntype)) {
        /* Module-level variables/constants only — locals are graph noise. */
        char vname[256] = {0};
        symbol_name_from_node(ctx->source, node, vname, sizeof(vname), NULL, 0);
        if (vname[0] && !is_noise_identifier(vname)) {
            snprintf(namebuf, sizeof(namebuf), "%s", vname);
            node_text(ctx->source, node, sigbuf, sizeof(sigbuf));
            kind = CTX_SYM_VARIABLE;
            emit_sym = true;
        }
    }

    if (emit_sym && namebuf[0]) {
        CtxSymbolDraft sym = {0};
        sym.id   = ctx_symbol_id(ctx->filepath, namebuf, ts_node_start_point(node).row + 1);
        snprintf(sym.name, sizeof(sym.name), "%s", namebuf);
        snprintf(sym.signature, sizeof(sym.signature), "%s", sigbuf[0] ? sigbuf : namebuf);
        join_scope(sym.scope, sizeof(sym.scope), ctx->enclosing_scope, qualifier);
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
    } else if (ctx->enclosing_fn[0] &&
               (is_identifier_type(ntype) || !strcmp(ntype, "qualified_type_identifier")) &&
               !is_qualified_node(ctx->parent_type) && !is_qualified_node(ctx->grand_type) &&
               !is_call_target(ctx->parent_type, ctx->grand_type) &&
               !is_declaration_name(ctx->parent_type, ctx->grand_type)) {
        char ref_name[256], ref_scope[256];
        symbol_name_from_node(ctx->source, node, ref_name, sizeof(ref_name),
                              ref_scope, sizeof(ref_scope));
        if (ref_name[0] && !is_noise_identifier(ref_name) &&
            strcmp(ref_name, ctx->enclosing_fn) != 0) {
            bool member = false;
            char recv[256] = {0};
            bool access = ctx->parent_type && (!strcmp(ctx->parent_type, "field_expression") ||
                                               !strcmp(ctx->parent_type, "attribute"));
            if (access) {
                bool python = ctx->parent_type[0] == 'a';
                TSNode parent = ctx->parent;
                bool is_member = ts_node_eq(ts_node_child_by_field_name(parent, python ? "attribute" : "field",
                                                                        python ? 9 : 5), node);
                if (is_member && !classify_target(ctx, parent, ref_name, sizeof(ref_name), ref_scope,
                                                  sizeof(ref_scope), recv, sizeof(recv), &member))
                    return;
            }
            CtxSiteDraft site = {
                .from_name = ctx->enclosing_fn, .from_line = ts_node_start_point(node).row + 1,
                .to_name = ref_name, .to_scope = ref_scope, .kind = CTX_EDGE_REFERENCES,
                .member = member, .recv = recv,
            };
            ctx_file_extract_add_site(ctx->ex, &site);
        }
    }

    if (!namebuf[0]) return;
    if (kind == CTX_SYM_FUNCTION || kind == CTX_SYM_METHOD) {
        opened->opens_fn = true;
        snprintf(opened->fn, sizeof(opened->fn), "%s", namebuf);
    } else if (kind == CTX_SYM_CLASS || kind == CTX_SYM_STRUCT ||
               (kind == CTX_SYM_NAMESPACE && strcmp(ntype, "namespace_definition") != 0)) {
        /* Classes, structs, traits, Rust impls and Go struct/interface types */
        opened->opens_scope = true;
        opened->scope_is_type = true;
        join_scope(opened->scope, sizeof(opened->scope), qualifier, namebuf);
    }
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
    TSNode nodes[CTX_WALK_MAX_DEPTH + 2];
    uint32_t depth = 0;
    for (;;) {
        scope_stack_unwind(&scopes, depth, ctx);
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (depth <= CTX_WALK_MAX_DEPTH) {
            types[depth] = ts_node_type(node);
            nodes[depth] = node;
        }
        ctx->parent = depth >= 1 && depth - 1 <= CTX_WALK_MAX_DEPTH ? nodes[depth - 1] : (TSNode){0};
        ctx->parent_type = depth >= 1 && depth - 1 <= CTX_WALK_MAX_DEPTH ? types[depth - 1] : NULL;
        ctx->grand_type = depth >= 2 && depth - 2 <= CTX_WALK_MAX_DEPTH ? types[depth - 2] : NULL;

        bool descend = depth <= CTX_WALK_MAX_DEPTH;
        if (descend) {
            NodeScope opened;
            process_node(ctx, node, &opened);
            if (opened.opens_fn || opened.opens_scope) {
                if (!scope_stack_push(&scopes, depth, ctx)) {
                    CTX_LOG_WARN("Cannot allocate scope stack while indexing %s", ctx->filepath);
                    break;
                }
                if (opened.opens_fn) {
                    snprintf(ctx->enclosing_fn, sizeof(ctx->enclosing_fn), "%s", opened.fn);
                    snprintf(ctx->fn_scope, sizeof(ctx->fn_scope), "%s", opened.fn_scope);
                    ctx->in_method = opened.fn_in_method;
                    ctx->fn_return_known = opened.fn_return_known;
                    ctx->scope_is_type = false;
                }
                if (opened.opens_scope) {
                    char joined[sizeof(ctx->enclosing_scope)];
                    join_scope(joined, sizeof(joined), ctx->enclosing_scope, opened.scope);
                    memcpy(ctx->enclosing_scope, joined, sizeof(joined));
                    ctx->scope_is_type = opened.scope_is_type;
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
    free(wctx.vars);

    ctx_parser_free_result(&pr);
    return true;
}

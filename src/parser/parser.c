#include "parser.h"
#include "../log/log.h"

static TSParser *s_parsers[CTX_LANG_UNKNOWN];

static uint32_t count_error_nodes(TSNode node) {
    if (ts_node_is_null(node)) return 0;
    uint32_t count = 0;
    if (ts_node_is_error(node) || ts_node_is_missing(node)) count++;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; i++)
        count += count_error_nodes(ts_node_child(node, i));
    return count;
}

bool ctx_parser_init(void) {
    const TSLanguage *langs[CTX_LANG_UNKNOWN] = {
        tree_sitter_c(),
        tree_sitter_cpp(),
        tree_sitter_python(),
        tree_sitter_javascript(),
        tree_sitter_typescript(),
    };
    for (int i = 0; i < CTX_LANG_UNKNOWN; i++) {
        s_parsers[i] = ts_parser_new();
        if (!s_parsers[i]) { CTX_LOG_ERROR("Failed to create parser %d", i); return false; }
        if (!ts_parser_set_language(s_parsers[i], langs[i])) {
            CTX_LOG_ERROR("Failed to set language %d", i);
            return false;
        }
    }
    CTX_LOG_DEBUG("Parser system initialised (5 languages)");
    return true;
}

void ctx_parser_shutdown(void) {
    for (int i = 0; i < CTX_LANG_UNKNOWN; i++) {
        if (s_parsers[i]) { ts_parser_delete(s_parsers[i]); s_parsers[i] = NULL; }
    }
}

CtxLanguage ctx_lang_from_path(const char *path) {
    if (!path) return CTX_LANG_UNKNOWN;
    const char *dot = strrchr(path, '.');
    if (!dot) return CTX_LANG_UNKNOWN;
    dot++;
    if (!strcmp(dot, "c") || !strcmp(dot, "h")) return CTX_LANG_C;
    if (!strcmp(dot, "cpp") || !strcmp(dot, "cc") || !strcmp(dot, "cxx") ||
        !strcmp(dot, "hpp") || !strcmp(dot, "hxx")) return CTX_LANG_CPP;
    if (!strcmp(dot, "py")) return CTX_LANG_PYTHON;
    if (!strcmp(dot, "js") || !strcmp(dot, "mjs") || !strcmp(dot, "jsx")) return CTX_LANG_JS;
    if (!strcmp(dot, "ts") || !strcmp(dot, "tsx")) return CTX_LANG_TS;
    return CTX_LANG_UNKNOWN;
}

static TSTree *parse_source_as(CtxLanguage lang, const char *source, size_t len,
                               uint32_t *error_count, bool *has_errors) {
    if (lang == CTX_LANG_UNKNOWN || !source || len > UINT32_MAX) return NULL;

    TSTree *tree = ts_parser_parse_string(s_parsers[lang], NULL, source, (uint32_t)len);
    if (!tree) return NULL;

    TSNode root = ts_tree_root_node(tree);
    bool errors = ts_node_has_error(root);
    if (has_errors) *has_errors = errors;
    if (error_count) *error_count = errors ? count_error_nodes(root) : 0;
    return tree;
}

static bool is_c_family_header(const char *path) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    return dot && !strcmp(dot, ".h");
}

bool ctx_parser_parse_file(const char *path, CtxParseResult *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    out->lang = ctx_lang_from_path(path);
    if (out->lang == CTX_LANG_UNKNOWN) return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        CTX_LOG_DEBUG("Cannot open %s: %s", path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 10 * 1024 * 1024) {
        fclose(f);
        CTX_LOG_DEBUG("Skipping %s (size %ld)", path, sz);
        return false;
    }

    out->source = (char *)malloc((size_t)sz + 1);
    if (!out->source) { fclose(f); return false; }
    size_t read = fread(out->source, 1, (size_t)sz, f);
    fclose(f);
    out->source[read] = '\0';
    out->source_len = read;

    /* Check for binary content — heuristic: NUL bytes in first 512 bytes */
    size_t check = read < 512 ? read : 512;
    for (size_t i = 0; i < check; i++) {
        if (out->source[i] == '\0') {
            free(out->source);
            out->source = NULL;
            return false;
        }
    }

    if (is_c_family_header(path)) {
        uint32_t c_errors = 0, cpp_errors = 0;
        bool c_has_errors = false, cpp_has_errors = false;
        TSTree *c_tree = parse_source_as(CTX_LANG_C, out->source, read,
                                         &c_errors, &c_has_errors);
        TSTree *cpp_tree = parse_source_as(CTX_LANG_CPP, out->source, read,
                                           &cpp_errors, &cpp_has_errors);

        if (cpp_tree && (!c_tree || cpp_errors < c_errors)) {
            if (c_tree) ts_tree_delete(c_tree);
            out->tree = cpp_tree;
            out->lang = CTX_LANG_CPP;
            out->has_errors = cpp_has_errors;
            out->error_count = cpp_errors;
        } else {
            if (cpp_tree) ts_tree_delete(cpp_tree);
            out->tree = c_tree;
            out->lang = CTX_LANG_C;
            out->has_errors = c_has_errors;
            out->error_count = c_errors;
        }
    } else {
        out->tree = parse_source_as(out->lang, out->source, read,
                                    &out->error_count, &out->has_errors);
    }
    if (!out->tree) {
        free(out->source);
        out->source = NULL;
        return false;
    }

    if (out->has_errors) {
        CTX_LOG_TRACE("Recoverable parse errors in %s: %u error nodes", path, out->error_count);
    }
    return true;
}

void ctx_parser_reparse(CtxParseResult *r, const char *new_source, size_t len) {
    if (!r || !new_source || r->lang == CTX_LANG_UNKNOWN) return;
    TSTree *old = r->tree;
    free(r->source);
    r->source = (char *)malloc(len + 1);
    if (!r->source) { if (old) ts_tree_delete(old); r->tree = NULL; return; }
    memcpy(r->source, new_source, len);
    r->source[len] = '\0';
    r->source_len = len;
    r->tree = ts_parser_parse_string(s_parsers[r->lang], old,
                                     r->source, (uint32_t)len);
    if (old) ts_tree_delete(old);
    if (r->tree) {
        TSNode root = ts_tree_root_node(r->tree);
        r->has_errors = ts_node_has_error(root);
        r->error_count = r->has_errors ? count_error_nodes(root) : 0;
    }
}

void ctx_parser_free_result(CtxParseResult *r) {
    if (!r) return;
    if (r->tree)   { ts_tree_delete(r->tree);  r->tree   = NULL; }
    if (r->source) { free(r->source);           r->source = NULL; }
    r->source_len = 0;
}

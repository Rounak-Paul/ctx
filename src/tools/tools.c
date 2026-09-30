#include "tools.h"
#include "../indexer/indexer.h"
#include "../nav/nav.h"
#include "../nav/source.h"
#include "../search/search.h"
#include "../model/model.h"
#include "../watcher/watcher.h"

#define TOOLS_READY_WAIT_MS 120000

static const CtxToolArg k_search_args[] = {
    { "query", CTX_TOOL_ARG_STRING, true,  "What you are looking for: a question, behaviour, or identifiers" },
    { "k", CTX_TOOL_ARG_INT, false, "Results to return (default 5, max 20)" },
    { "bodies", CTX_TOOL_ARG_INT, false, "How many top results include code (default 1)" },
    { "include_vendor", CTX_TOOL_ARG_BOOL, false, "Also search third-party code (default false)" },
};

static const CtxToolArg k_outline_args[] = {
    { "path", CTX_TOOL_ARG_STRING, true,  "File path (absolute, root-relative, or unique suffix)" },
    { "from_line", CTX_TOOL_ARG_INT, false, "Start listing at this line (paging)" },
    { "limit", CTX_TOOL_ARG_INT, false, "Max symbols (default 300)" },
};

static const CtxToolArg k_source_args[] = {
    { "symbol", CTX_TOOL_ARG_STRING, false, "name, Scope::name, or path:line" },
    { "file", CTX_TOOL_ARG_STRING, false, "Restrict symbol lookup to this file, or the file for lines" },
    { "lines", CTX_TOOL_ARG_STRING, false, "Line range \"start-end\" (with file, no symbol)" },
    { "max_lines", CTX_TOOL_ARG_INT, false, "Line cap (default 120)" },
};

static const CtxToolArg k_callers_args[] = {
    { "symbol", CTX_TOOL_ARG_STRING, true,  "Function name, Scope::name, or path:line" },
    { "file", CTX_TOOL_ARG_STRING, false, "Disambiguate the target by file" },
    { "depth", CTX_TOOL_ARG_INT, false, "1 = direct (default), up to 3 for indirect callers" },
};

static const CtxToolArg k_symbol_args[] = {
    { "symbol", CTX_TOOL_ARG_STRING, true,  "Symbol name, Scope::name, or path:line" },
    { "file", CTX_TOOL_ARG_STRING, false, "Disambiguate the target by file" },
};

static const CtxToolSpec k_tools[] = {
    { "search",
      "Find code by behaviour or identifiers when you don't know where it lives. Returns ranked "
      "symbols as path:line with signatures, and current code for the top matches. Use this "
      "instead of grep/glob exploration.",
      k_search_args, CTX_ARRAY_LEN(k_search_args) },
    { "outline",
      "A file's symbols (kind, line range, signature) without reading the file. Use it to pick "
      "the exact part of a large file you need.",
      k_outline_args, CTX_ARRAY_LEN(k_outline_args) },
    { "source",
      "Exact current code of a symbol with its doc comment, or of a file line range "
      "(file + lines). Use instead of reading whole files.",
      k_source_args, CTX_ARRAY_LEN(k_source_args) },
    { "callers",
      "Every call site of a function with the calling line, resolved through the symbol graph "
      "(not text matches). depth 2-3 adds indirect callers.",
      k_callers_args, CTX_ARRAY_LEN(k_callers_args) },
    { "callees",
      "Functions a symbol calls, with their locations and signatures.",
      k_symbol_args, CTX_ARRAY_LEN(k_symbol_args) },
    { "impact",
      "Before changing a symbol: definitions/declarations, all call sites with code, indirect "
      "callers, references, subtypes, affected files and tests.",
      k_symbol_args, CTX_ARRAY_LEN(k_symbol_args) },
    { "status",
      "Index readiness, counts, and search-model state. Rarely needed: other tools wait for the "
      "index and re-index changed files before answering.",
      NULL, 0 },
};

uint32_t ctx_tools_count(void) { return (uint32_t)CTX_ARRAY_LEN(k_tools); }

const CtxToolSpec *ctx_tools_at(uint32_t i) {
    return i < ctx_tools_count() ? &k_tools[i] : NULL;
}

const CtxToolSpec *ctx_tools_find(const char *name) {
    for (uint32_t i = 0; name && i < ctx_tools_count(); i++)
        if (!strcmp(k_tools[i].name, name)) return &k_tools[i];
    return NULL;
}

cJSON *ctx_tools_input_schema(const CtxToolSpec *spec) {
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(schema, "properties");
    cJSON *required = cJSON_CreateArray();
    for (uint32_t i = 0; spec && i < spec->arg_count; i++) {
        const CtxToolArg *a = &spec->args[i];
        cJSON *p = cJSON_AddObjectToObject(props, a->name);
        cJSON_AddStringToObject(p, "type", a->type == CTX_TOOL_ARG_INT ? "integer"
                                         : a->type == CTX_TOOL_ARG_BOOL ? "boolean" : "string");
        cJSON_AddStringToObject(p, "description", a->description);
        if (a->required) cJSON_AddItemToArray(required, cJSON_CreateString(a->name));
    }
    if (cJSON_GetArraySize(required) > 0) cJSON_AddItemToObject(schema, "required", required);
    else cJSON_Delete(required);
    return schema;
}

/* ---- argument access ---------------------------------------------------------- */

static const char *arg_str(const cJSON *args, const char *name) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    return cJSON_IsString(v) && v->valuestring && v->valuestring[0] ? v->valuestring : NULL;
}

static uint32_t arg_uint(const cJSON *args, const char *name, uint32_t fallback) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= (double)UINT32_MAX)
        return (uint32_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) {
        char *end = NULL;
        unsigned long n = strtoul(v->valuestring, &end, 10);
        if (end && end != v->valuestring && *end == '\0' && n <= UINT32_MAX) return (uint32_t)n;
    }
    return fallback;
}

static bool arg_bool(const cJSON *args, const char *name) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, name) : NULL;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return cJSON_IsString(v) && v->valuestring &&
           (!strcmp(v->valuestring, "true") || !strcmp(v->valuestring, "1"));
}

static char *error_text(const char *fmt, const char *detail) {
    CtxBuf b = {0};
    ctx_buf_printf(&b, fmt, detail);
    return ctx_buf_take(&b);
}

/* Waits for the initial index so answers are complete; false on timeout. */
static bool wait_until_ready(void) {
    for (uint32_t waited = 0; waited < TOOLS_READY_WAIT_MS; waited += 100) {
        CtxIndexStatus st;
        ctx_indexer_get_status(&st);
        if (st.ready) return true;
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

static char *status_text(void) {
    CtxIndexStatus is = {0};
    CtxGraphStats gs = {0};
    ctx_indexer_get_status(&is);
    ctx_indexer_get_stats(&gs);
    uint32_t embedded = 0, total = 0;
    ctx_search_embedding_progress(&embedded, &total);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", is.ready ? "ready" : is.progress.running ? "indexing" : "starting");
    cJSON_AddNumberToObject(root, "progress_done", is.progress.done);
    cJSON_AddNumberToObject(root, "progress_total", is.progress.total);
    cJSON_AddNumberToObject(root, "files", gs.files);
    cJSON_AddNumberToObject(root, "symbols", gs.symbols);
    cJSON_AddNumberToObject(root, "edges", gs.edges);
    cJSON_AddNumberToObject(root, "graph_generation", (double)is.graph_generation);
    cJSON_AddBoolToObject(root, "watcher_running", ctx_watcher_is_running());
    cJSON *models = cJSON_AddObjectToObject(root, "models");
    static const char *const roles[CTX_MODEL_ROLE_COUNT] = { "reranker", "embedder" };
    for (int r = 0; r < CTX_MODEL_ROLE_COUNT; r++) {
        char detail[256];
        CtxModelState st = ctx_models_state((CtxModelRole)r, detail, sizeof(detail));
        cJSON *m = cJSON_AddObjectToObject(models, roles[r]);
        cJSON_AddStringToObject(m, "state", ctx_models_state_name(st));
        if (detail[0]) cJSON_AddStringToObject(m, "detail", detail);
    }
    cJSON_AddNumberToObject(root, "embedded_symbols", embedded);
    cJSON_AddNumberToObject(root, "searchable_project_symbols", total);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text ? text : strdup("{}");
}

char *ctx_tools_call(const char *name, const cJSON *args, bool *is_error) {
    bool failed = false;
    char *out = NULL;
    CtxGraph *g = ctx_indexer_get_graph();
    const CtxToolSpec *spec = ctx_tools_find(name);

    if (!spec) {
        out = error_text("error: unknown tool %s\n", name ? name : "(null)");
        failed = true;
    } else if (!strcmp(spec->name, "status")) {
        out = status_text();
    } else {
        for (uint32_t i = 0; i < spec->arg_count && !failed; i++) {
            if (spec->args[i].required && spec->args[i].type == CTX_TOOL_ARG_STRING &&
                !arg_str(args, spec->args[i].name)) {
                out = error_text("error: missing required argument '%s'\n", spec->args[i].name);
                failed = true;
            }
        }
        if (!failed && !wait_until_ready()) {
            out = error_text("error: %s\n", "index still building; retry shortly (see status)");
            failed = true;
        }
    }

    if (!failed && !out) {
        if (!strcmp(spec->name, "search")) {
            CtxSearchRequest req = {
                .query = arg_str(args, "query"),
                .k = arg_uint(args, "k", 5),
                .bodies = arg_uint(args, "bodies", 1),
                .include_vendor = arg_bool(args, "include_vendor"),
            };
            out = ctx_search(g, &req);
        } else if (!strcmp(spec->name, "outline")) {
            out = ctx_nav_outline(g, arg_str(args, "path"), arg_uint(args, "from_line", 1),
                                  arg_uint(args, "limit", 0));
        } else if (!strcmp(spec->name, "source")) {
            CtxNavSourceRequest req = {
                .symbol = arg_str(args, "symbol"),
                .file = arg_str(args, "file"),
                .lines = arg_str(args, "lines"),
                .max_lines = arg_uint(args, "max_lines", 0),
            };
            if (!req.symbol && !(req.file && req.lines)) {
                out = error_text("error: %s\n", "pass symbol, or file + lines");
                failed = true;
            } else {
                out = ctx_nav_source(g, &req);
            }
        } else if (!strcmp(spec->name, "callers")) {
            out = ctx_nav_callers(g, arg_str(args, "symbol"), arg_str(args, "file"),
                                  arg_uint(args, "depth", 1));
        } else if (!strcmp(spec->name, "callees")) {
            out = ctx_nav_callees(g, arg_str(args, "symbol"), arg_str(args, "file"));
        } else if (!strcmp(spec->name, "impact")) {
            out = ctx_nav_impact(g, arg_str(args, "symbol"), arg_str(args, "file"));
        }
    }

    if (!out) out = strdup("");
    if (!failed && !strncmp(out, "error:", 6)) failed = true;
    if (is_error) *is_error = failed;
    return out;
}

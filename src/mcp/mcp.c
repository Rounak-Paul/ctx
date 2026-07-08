#include "mcp.h"
#include "../retrieve/retrieve.h"
#include "../indexer/indexer.h"
#include "../watcher/watcher.h"
#include "../stats/stats.h"
#include "../log/log.h"
#include "../../vendors/cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_NAME      "ctx"
#define MCP_SERVER_VERSION   "1.0.0"

/* JSON-RPC 2.0 error codes */
#define JSONRPC_PARSE_ERROR      -32700
#define JSONRPC_INVALID_REQUEST  -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS   -32602
#define JSONRPC_INTERNAL_ERROR   -32603

static void send_response(cJSON *response) {
    char *text = cJSON_PrintUnformatted(response);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(text);
    }
    cJSON_Delete(response);
}

static cJSON *make_response(cJSON *id) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id && !cJSON_IsNull(id))
        cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 0));
    else
        cJSON_AddNullToObject(r, "id");
    return r;
}

static void send_error(cJSON *id, int code, const char *message) {
    cJSON *r = make_response(id);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", (double)code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(r, "error", err);
    send_response(r);
}

static cJSON *build_tool_schema_string_prop(const char *desc) {
    cJSON *prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", "string");
    cJSON_AddStringToObject(prop, "description", desc);
    return prop;
}

static CtxRetrieveDetail parse_detail_arg(cJSON *args) {
    cJSON *detail = args ? cJSON_GetObjectItemCaseSensitive(args, "detail") : NULL;
    if (!cJSON_IsString(detail) || !detail->valuestring)
        return CTX_RETRIEVE_DETAIL_COMPACT;
    if (!strcmp(detail->valuestring, "full")) return CTX_RETRIEVE_DETAIL_FULL;
    if (!strcmp(detail->valuestring, "standard")) return CTX_RETRIEVE_DETAIL_STANDARD;
    return CTX_RETRIEVE_DETAIL_COMPACT;
}

static cJSON *build_tools_array(void) {
    cJSON *tools = cJSON_CreateArray();

    /* get_context */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", "get_context");
        cJSON_AddStringToObject(t, "description",
            "Retrieve structured codebase context for a task or question. "
            "Returns a compact packet with symbol cards, relation summaries, and expansion handles by default.");
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON_AddItemToObject(props, "task",
            build_tool_schema_string_prop("The task or question to get context for"));
        cJSON_AddItemToObject(props, "detail",
            build_tool_schema_string_prop("Optional detail mode: compact, standard, or full"));
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("task"));
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }

    /* get_symbol */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", "get_symbol");
        cJSON_AddStringToObject(t, "description",
            "Retrieve context anchored to a specific symbol name — its definition, "
            "callers, callees, related types, and expansion handles.");
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON_AddItemToObject(props, "name",
            build_tool_schema_string_prop("Symbol name to look up"));
        cJSON_AddItemToObject(props, "detail",
            build_tool_schema_string_prop("Optional detail mode: compact, standard, or full"));
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("name"));
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }

    /* get_file */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", "get_file");
        cJSON_AddStringToObject(t, "description",
            "Retrieve symbols defined in a specific file and their relationship handles.");
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON_AddItemToObject(props, "path",
            build_tool_schema_string_prop("Absolute path to the file"));
        cJSON_AddItemToObject(props, "detail",
            build_tool_schema_string_prop("Optional detail mode: compact, standard, or full"));
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("path"));
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }

    /* expand_context */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", "expand_context");
        cJSON_AddStringToObject(t, "description",
            "Expand a handle returned by get_context/get_symbol/get_file, such as expand:source:<id>, expand:entrypoints:<path>, or expand:file:<path>.");
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON_AddItemToObject(props, "handle",
            build_tool_schema_string_prop("Expansion handle returned by ctx"));
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("handle"));
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }

    /* get_stats */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", "get_stats");
        cJSON_AddStringToObject(t, "description",
            "Get indexing statistics, freshness state, and watcher readiness.");
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }

    /* get_status */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", "get_status");
        cJSON_AddStringToObject(t, "description",
            "Get readiness, indexing progress, graph generation, and file watcher status.");
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }

    return tools;
}

static void handle_initialize(cJSON *id, cJSON *params) {
    CTX_UNUSED(params);
    cJSON *r = make_response(id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(info, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);
    cJSON_AddItemToObject(r, "result", result);
    send_response(r);
}

static void handle_tools_list(cJSON *id) {
    cJSON *r = make_response(id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", build_tools_array());
    cJSON_AddItemToObject(r, "result", result);
    send_response(r);
}

static void handle_ping(cJSON *id) {
    cJSON *r = make_response(id);
    cJSON_AddItemToObject(r, "result", cJSON_CreateObject());
    send_response(r);
}

static cJSON *retrieve_to_content(CtxQueryKind kind, const char *text, CtxRetrieveDetail detail) {
    CtxRetrieveRequest req = { .kind = kind, .detail = detail, .text = text };
    char *output = ctx_retrieve(ctx_indexer_get_graph(), &req);

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", output ? output : "");
    free(output);
    cJSON_AddItemToArray(content, item);
    return content;
}

static cJSON *expand_to_content(const char *handle) {
    char *output = ctx_expand_context(ctx_indexer_get_graph(), handle);

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", output ? output : "");
    free(output);
    cJSON_AddItemToArray(content, item);
    return content;
}

static char *build_status_text(bool include_counts) {
    CtxIndexStatus is = {0};
    CtxGraphStats gs = {0};
    ctx_indexer_get_status(&is);
    ctx_indexer_get_stats(&gs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status",
        is.ready ? "ready" : is.progress.running ? "indexing" : "starting");
    cJSON_AddBoolToObject(root, "ready", is.ready);
    cJSON_AddBoolToObject(root, "cache_loaded", is.cache_loaded);
    cJSON_AddBoolToObject(root, "indexing", is.progress.running);
    cJSON_AddNumberToObject(root, "progress_done", (double)is.progress.done);
    cJSON_AddNumberToObject(root, "progress_total", (double)is.progress.total);
    cJSON_AddNumberToObject(root, "graph_generation", (double)is.graph_generation);
    cJSON_AddNumberToObject(root, "last_update_unix_ms", (double)is.last_update_unix_ms);
    cJSON_AddBoolToObject(root, "watcher_running", ctx_watcher_is_running());
    cJSON_AddNumberToObject(root, "watch_count", (double)ctx_watcher_active_count());

    if (include_counts) {
        cJSON_AddNumberToObject(root, "files", (double)gs.files);
        cJSON_AddNumberToObject(root, "symbols", (double)gs.symbols);
        cJSON_AddNumberToObject(root, "edges", (double)gs.edges);
        cJSON_AddNumberToObject(root, "errors", (double)gs.errors);
        cJSON_AddNumberToObject(root, "last_index_ms", (double)gs.duration_ms);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static cJSON *text_to_content(char *text) {
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    free(text);
    cJSON_AddItemToArray(content, item);
    return content;
}

static void handle_tools_call(cJSON *id, cJSON *params) {
    if (!params) { send_error(id, JSONRPC_INVALID_PARAMS, "missing params"); return; }

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    cJSON *args      = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(name_item)) {
        send_error(id, JSONRPC_INVALID_PARAMS, "missing tool name");
        return;
    }

    const char *tool = name_item->valuestring;
    cJSON *content = NULL;

    if (!strcmp(tool, "get_context")) {
        cJSON *task = args ? cJSON_GetObjectItemCaseSensitive(args, "task") : NULL;
        if (!cJSON_IsString(task)) { send_error(id, JSONRPC_INVALID_PARAMS, "missing 'task'"); return; }
        content = retrieve_to_content(CTX_QUERY_TASK, task->valuestring, parse_detail_arg(args));

    } else if (!strcmp(tool, "get_symbol")) {
        cJSON *sym = args ? cJSON_GetObjectItemCaseSensitive(args, "name") : NULL;
        if (!cJSON_IsString(sym)) { send_error(id, JSONRPC_INVALID_PARAMS, "missing 'name'"); return; }
        content = retrieve_to_content(CTX_QUERY_SYMBOL, sym->valuestring, parse_detail_arg(args));

    } else if (!strcmp(tool, "get_file")) {
        cJSON *path = args ? cJSON_GetObjectItemCaseSensitive(args, "path") : NULL;
        if (!cJSON_IsString(path)) { send_error(id, JSONRPC_INVALID_PARAMS, "missing 'path'"); return; }
        content = retrieve_to_content(CTX_QUERY_FILE, path->valuestring, parse_detail_arg(args));

    } else if (!strcmp(tool, "expand_context")) {
        cJSON *handle = args ? cJSON_GetObjectItemCaseSensitive(args, "handle") : NULL;
        if (!cJSON_IsString(handle)) { send_error(id, JSONRPC_INVALID_PARAMS, "missing 'handle'"); return; }
        content = expand_to_content(handle->valuestring);

    } else if (!strcmp(tool, "get_stats")) {
        content = text_to_content(build_status_text(true));

    } else if (!strcmp(tool, "get_status")) {
        content = text_to_content(build_status_text(false));

    } else {
        send_error(id, JSONRPC_METHOD_NOT_FOUND, "unknown tool");
        return;
    }

    cJSON *r = make_response(id);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddItemToObject(r, "result", result);
    send_response(r);
    ctx_stats_record_query(tool, 0);
}

static void dispatch(cJSON *msg) {
    cJSON *id_item     = cJSON_GetObjectItemCaseSensitive(msg, "id");
    cJSON *method_item = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *params      = cJSON_GetObjectItemCaseSensitive(msg, "params");

    if (!cJSON_IsString(method_item)) {
        if (id_item) send_error(id_item, JSONRPC_INVALID_REQUEST, "missing method");
        return;
    }

    const char *method = method_item->valuestring;
    bool is_notification = !id_item || cJSON_IsNull(id_item);

    CTX_LOG_DEBUG("MCP method=%s notification=%d", method, (int)is_notification);

    /* Notifications — process, no response */
    if (!strcmp(method, "notifications/initialized")) return;

    /* Requests — must have id */
    if (is_notification) return;

    if (!strcmp(method, "initialize"))   { handle_initialize(id_item, params); return; }
    if (!strcmp(method, "tools/list"))   { handle_tools_list(id_item);         return; }
    if (!strcmp(method, "tools/call"))   { handle_tools_call(id_item, params); return; }
    if (!strcmp(method, "ping"))         { handle_ping(id_item);               return; }

    send_error(id_item, JSONRPC_METHOD_NOT_FOUND, "method not found");
}

void ctx_mcp_run(void) {
    CTX_LOG_INFO("MCP server ready (stdio transport)");

    char  *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, stdin)) != -1) {
        if (line_len == 0 || (line_len == 1 && line[0] == '\n')) continue;

        cJSON *msg = cJSON_ParseWithLength(line, (size_t)line_len);
        if (!msg) {
            send_error(NULL, JSONRPC_PARSE_ERROR, "parse error");
            continue;
        }

        dispatch(msg);
        cJSON_Delete(msg);
    }

    free(line);
    CTX_LOG_INFO("MCP server stdin closed, shutting down");
}

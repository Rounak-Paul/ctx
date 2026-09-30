#include "mcp.h"
#include "../tools/tools.h"
#include "../stats/stats.h"
#include "../log/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_NAME      "ctx"
#define MCP_SERVER_VERSION   "2.0.0"

/* JSON-RPC 2.0 error codes */
#define JSONRPC_PARSE_ERROR      -32700
#define JSONRPC_INVALID_REQUEST  -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS   -32602
#define JSONRPC_INTERNAL_ERROR   -32603

/* Reply framing mirrors the client: newline-delimited JSON (MCP stdio spec)
 * unless the client sent Content-Length framed messages. */
static bool s_content_length_framing = false;

static void send_response(cJSON *response) {
    char *text = cJSON_PrintUnformatted(response);
    if (text) {
        if (s_content_length_framing) {
            fprintf(stdout, "Content-Length: %zu\r\n\r\n", strlen(text));
            fputs(text, stdout);
        } else {
            fputs(text, stdout);
            fputc('\n', stdout);
        }
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

static cJSON *build_tools_array(void) {
    cJSON *tools = cJSON_CreateArray();
    for (uint32_t i = 0; i < ctx_tools_count(); i++) {
        const CtxToolSpec *spec = ctx_tools_at(i);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", spec->name);
        cJSON_AddStringToObject(t, "description", spec->description);
        cJSON_AddItemToObject(t, "inputSchema", ctx_tools_input_schema(spec));
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

static void handle_tools_call(cJSON *id, cJSON *params) {
    if (!params) { send_error(id, JSONRPC_INVALID_PARAMS, "missing params"); return; }

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    cJSON *args      = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        send_error(id, JSONRPC_INVALID_PARAMS, "missing tool name");
        return;
    }
    if (!ctx_tools_find(name_item->valuestring)) {
        send_error(id, JSONRPC_METHOD_NOT_FOUND, "unknown tool");
        return;
    }

    bool is_error = false;
    char *text = ctx_tools_call(name_item->valuestring, cJSON_IsObject(args) ? args : NULL, &is_error);

    cJSON *r = make_response(id);
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    if (is_error) cJSON_AddBoolToObject(result, "isError", true);
    cJSON_AddItemToObject(r, "result", result);
    send_response(r);
    ctx_stats_record_query(name_item->valuestring, 0);
    free(text);
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

    for (;;) {
        char *line = NULL;
        size_t line_cap = 0;
        ssize_t line_len = getline(&line, &line_cap, stdin);
        if (line_len == -1) {
            free(line);
            break;
        }
        if (line_len == 0 || (line_len == 1 && line[0] == '\n')) {
            free(line);
            continue;
        }

        char *payload = NULL;
        size_t payload_len = 0;
        s_content_length_framing = line[0] != '{';
        if (!s_content_length_framing) {
            payload = line;
            payload_len = (size_t)line_len;
            line = NULL;
        } else {
            size_t content_length = 0;
            for (;;) {
                if (!strncasecmp(line, "Content-Length:", 15)) {
                    char *p = line + 15;
                    while (*p == ' ' || *p == '\t') p++;
                    content_length = (size_t)strtoull(p, NULL, 10);
                }

                bool header_end = !strcmp(line, "\n") || !strcmp(line, "\r\n");
                free(line);
                line = NULL;
                line_cap = 0;
                if (header_end) break;

                line_len = getline(&line, &line_cap, stdin);
                if (line_len == -1) break;
            }

            if (content_length == 0) {
                free(line);
                send_error(NULL, JSONRPC_INVALID_REQUEST, "missing Content-Length");
                continue;
            }

            payload = (char *)malloc(content_length + 1);
            if (!payload) {
                send_error(NULL, JSONRPC_INTERNAL_ERROR, "out of memory");
                continue;
            }

            size_t got = fread(payload, 1, content_length, stdin);
            payload[got] = '\0';
            payload_len = got;
            if (got != content_length) {
                free(payload);
                break;
            }
        }

        cJSON *msg = cJSON_ParseWithLength(payload, payload_len);
        if (!msg) {
            free(payload);
            send_error(NULL, JSONRPC_PARSE_ERROR, "parse error");
            continue;
        }

        dispatch(msg);
        cJSON_Delete(msg);
        free(payload);
    }
    CTX_LOG_INFO("MCP server stdin closed, shutting down");
}

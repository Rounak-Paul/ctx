#include "api.h"
#include "../context/context.h"
#include "../indexer/indexer.h"
#include "../stats/stats.h"
#include "../log/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* Graph is always fetched live from the indexer — no stale pointer. */
static volatile bool s_running = false;
static pthread_t s_thread;
static int s_server_fd = -1;

/* ---- minimal HTTP/1.1 helpers ---- */
static CtxGraph *get_graph(void) { return ctx_indexer_get_graph(); }

static void send_response(int fd, int status, const char *content_type, const char *body) {
    size_t body_len = body ? strlen(body) : 0;
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status,
        status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request",
        content_type, body_len);
    send(fd, header, (size_t)hlen, MSG_NOSIGNAL);
    if (body && body_len > 0) send(fd, body, body_len, MSG_NOSIGNAL);
}

static void send_json(int fd, int status, const char *json) {
    send_response(fd, status, "application/json", json);
}

static void send_text(int fd, const char *text) {
    send_response(fd, 200, "text/markdown", text);
}

/* ---- parse first line of HTTP request ---- */
typedef struct { char method[8]; char path[512]; char query[512]; } HttpReq;

static bool parse_request(int fd, HttpReq *req) {
    char buf[2048]; int n = 0, cap = (int)sizeof(buf) - 1;
    /* Read until we have at least the first line */
    while (n < cap) {
        int r = (int)recv(fd, buf + n, (size_t)(cap - n), 0);
        if (r <= 0) break;
        n += r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n")) break;
    }
    buf[n] = '\0';
    /* Parse method and path */
    char *p = buf;
    char *sp1 = strchr(p, ' ');
    if (!sp1) return false;
    *sp1 = '\0';
    strncpy(req->method, p, sizeof(req->method) - 1);
    p = sp1 + 1;
    char *sp2 = strchr(p, ' ');
    if (!sp2) sp2 = strchr(p, '\r');
    if (sp2) *sp2 = '\0';

    /* Separate query string */
    char *q = strchr(p, '?');
    if (q) { *q = '\0'; strncpy(req->query, q + 1, sizeof(req->query) - 1); }
    else req->query[0] = '\0';
    strncpy(req->path, p, sizeof(req->path) - 1);
    return true;
}

static char *get_param(const char *query, const char *key) {
    /* Returns heap-allocated value for key= in query string, or NULL */
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            char *val = (char *)malloc(vlen + 1);
            memcpy(val, p, vlen); val[vlen] = '\0';
            /* URL-decode spaces */
            for (size_t i = 0; val[i]; i++) if (val[i] == '+') val[i] = ' ';
            return val;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return NULL;
}

/* ---- route handlers ---- */
static void handle_health(int fd) {
    send_json(fd, 200, "{\"status\":\"ok\"}");
}

static void handle_summary(int fd) {
    CtxGraph *g = get_graph();
    char *text = ctx_context_summary(g, NULL);
    send_text(fd, text);
    free(text);
    ctx_stats_record_query("/summary", 0);
}

static void handle_symbol(int fd, const char *query) {
    char *name = get_param(query, "name");
    if (!name) { send_json(fd, 400, "{\"error\":\"missing ?name=\"}"); return; }
    CtxGraph *g = get_graph();
    char *text = ctx_context_for_symbol(g, name);
    free(name);
    send_text(fd, text);
    free(text);
    ctx_stats_record_query("/symbol", 0);
}

static void handle_file(int fd, const char *query) {
    char *path = get_param(query, "path");
    if (!path) { send_json(fd, 400, "{\"error\":\"missing ?path=\"}"); return; }
    CtxGraph *g = get_graph();
    char *text = ctx_context_for_file(g, path);
    free(path);
    send_text(fd, text);
    free(text);
    ctx_stats_record_query("/file", 0);
}

static void handle_query(int fd, const char *query) {
    char *q = get_param(query, "q");
    if (!q) { send_json(fd, 400, "{\"error\":\"missing ?q=\"}"); return; }
    CtxGraph *g = get_graph();
    char *text = ctx_context_query(g, q, 20);
    free(q);
    send_text(fd, text);
    free(text);
    ctx_stats_record_query("/query", 0);
}

static void handle_stats(int fd) {
    CtxGraph *g = get_graph();
    CtxGraphStats gs = {0};
    ctx_indexer_get_stats(&gs);
    char json[512];
    uint32_t syms = g ? ctx_graph_symbol_count(g) : 0;
    uint32_t edgs = g ? ctx_graph_edge_count(g) : 0;
    snprintf(json, sizeof(json),
        "{\"files\":%u,\"symbols\":%u,\"edges\":%u,\"errors\":%u,\"last_index_ms\":%"PRId64"}",
        gs.files, syms, edgs, gs.errors, gs.duration_ms);
    send_json(fd, 200, json);
    ctx_stats_record_query("/stats", 0);
}

static void handle_reindex(int fd) {
    ctx_indexer_index_all();
    send_json(fd, 200, "{\"status\":\"reindex_started\"}");
}

static void handle_request(int fd) {
    HttpReq req = {0};
    if (!parse_request(fd, &req)) { close(fd); return; }
    CTX_LOG_DEBUG("API %s %s", req.method, req.path);

    if (!strcmp(req.path, "/health"))      handle_health(fd);
    else if (!strcmp(req.path, "/summary")) handle_summary(fd);
    else if (!strcmp(req.path, "/symbol"))  handle_symbol(fd, req.query);
    else if (!strcmp(req.path, "/file"))    handle_file(fd, req.query);
    else if (!strcmp(req.path, "/query"))   handle_query(fd, req.query);
    else if (!strcmp(req.path, "/stats"))   handle_stats(fd);
    else if (!strcmp(req.path, "/reindex") && !strcmp(req.method, "POST")) handle_reindex(fd);
    else send_json(fd, 404, "{\"error\":\"not found\"}");

    close(fd);
}

static void *api_thread(void *arg) {
    CTX_UNUSED(arg);
    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (s_running) CTX_LOG_WARN("API accept failed: %s", strerror(errno));
            break;
        }
        handle_request(client_fd);
    }
    return NULL;
}

bool ctx_api_start(int port) {

    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0) { CTX_LOG_ERROR("API socket: %s", strerror(errno)); return false; }

    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CTX_LOG_ERROR("API bind port %d: %s", port, strerror(errno));
        close(s_server_fd); s_server_fd = -1; return false;
    }
    listen(s_server_fd, 16);

    s_running = true;
    pthread_create(&s_thread, NULL, api_thread, NULL);
    CTX_LOG_INFO("API server listening on http://127.0.0.1:%d", port);
    return true;
}

void ctx_api_stop(void) {
    if (!s_running) return;
    s_running = false;
    /* shutdown() unblocks accept() reliably; close() alone may not on Linux */
    if (s_server_fd >= 0) {
        shutdown(s_server_fd, SHUT_RDWR);
        close(s_server_fd);
        s_server_fd = -1;
    }
    pthread_join(s_thread, NULL);
}

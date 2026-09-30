#include "source.h"

#define CTX_SOURCE_MAX_BYTES (64u * 1024u * 1024u)

bool ctx_source_open(const char *path, CtxSource *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long size = ftell(fp);
    if (size < 0 || (unsigned long)size > CTX_SOURCE_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) { fclose(fp); return false; }
    size_t got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[got] = '\0';

    uint32_t lines = 1;
    for (size_t i = 0; i < got; i++)
        if (text[i] == '\n' && i + 1 < got) lines++;
    uint32_t *starts = (uint32_t *)malloc(lines * sizeof(uint32_t));
    if (!starts) { free(text); return false; }
    uint32_t n = 0;
    starts[n++] = 0;
    for (size_t i = 0; i < got && n < lines; i++)
        if (text[i] == '\n' && i + 1 < got) starts[n++] = (uint32_t)(i + 1);

    out->text = text;
    out->len = got;
    out->line_starts = starts;
    out->line_count = got ? n : 0;
    return true;
}

void ctx_source_close(CtxSource *src) {
    if (!src) return;
    free(src->text);
    free(src->line_starts);
    memset(src, 0, sizeof(*src));
}

const char *ctx_source_line(const CtxSource *src, uint32_t n, uint32_t *len) {
    if (!src || n == 0 || n > src->line_count) return NULL;
    uint32_t start = src->line_starts[n - 1];
    uint32_t end = n < src->line_count ? src->line_starts[n] : (uint32_t)src->len;
    while (end > start && (src->text[end - 1] == '\n' || src->text[end - 1] == '\r')) end--;
    if (len) *len = end - start;
    return src->text + start;
}

void ctx_path_display(const char *root, const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!path) { out[0] = '\0'; return; }
    size_t root_len = root ? strlen(root) : 0;
    if (root_len && !strncmp(path, root, root_len) && path[root_len] == '/')
        snprintf(out, out_size, "%s", path + root_len + 1);
    else
        snprintf(out, out_size, "%s", path);
}

bool ctx_path_resolve_locked(CtxGraph *g, const char *root, const char *query,
                             char *out, size_t out_size) {
    if (!g || !query || !query[0] || !out || out_size == 0) return false;
    while (query[0] == '.' && query[1] == '/') query += 2;

    if (ctx_graph_find_file_locked(g, query)) {
        snprintf(out, out_size, "%s", query);
        return true;
    }
    if (root && root[0]) {
        char joined[4096];
        int n = snprintf(joined, sizeof(joined), "%s/%s", root, query);
        if (n > 0 && (size_t)n < sizeof(joined) && ctx_graph_find_file_locked(g, joined)) {
            snprintf(out, out_size, "%s", joined);
            return true;
        }
    }

    size_t qlen = strlen(query);
    const char *best = NULL;
    size_t best_len = SIZE_MAX;
    CtxGraphFile *f, *tmp;
    HASH_ITER(hh, g->files, f, tmp) {
        size_t plen = strlen(f->path);
        if (plen < qlen || strcmp(f->path + plen - qlen, query) != 0) continue;
        if (plen > qlen && f->path[plen - qlen - 1] != '/') continue;
        if (plen < best_len) { best = f->path; best_len = plen; }
    }
    if (!best) return false;
    snprintf(out, out_size, "%s", best);
    return true;
}

bool ctx_path_is_vendor(const char *root, const char *path) {
    if (!path) return false;
    size_t root_len = root ? strlen(root) : 0;
    const char *rel = (root_len && !strncmp(path, root, root_len) && path[root_len] == '/')
                      ? path + root_len + 1 : path;
    static const char *const dirs[] = {
        "vendor/", "vendors/", "third_party/", "third-party/", "thirdparty/",
        "external/", "extern/", "node_modules/", "deps/", NULL
    };
    for (int i = 0; dirs[i]; i++) {
        size_t dl = strlen(dirs[i]);
        if (!strncmp(rel, dirs[i], dl)) return true;
        char needle[32];
        snprintf(needle, sizeof(needle), "/%s", dirs[i]);
        if (strstr(rel, needle)) return true;
    }
    return false;
}

static bool buf_reserve(CtxBuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < need) cap *= 2;
    char *next = (char *)realloc(b->data, cap);
    if (!next) return false;
    b->data = next;
    b->cap = cap;
    return true;
}

bool ctx_buf_append(CtxBuf *b, const char *text, size_t len) {
    if (!b || (!text && len)) return false;
    if (!buf_reserve(b, len)) return false;
    if (len) memcpy(b->data + b->len, text, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

bool ctx_buf_printf(CtxBuf *b, const char *fmt, ...) {
    if (!b || !fmt) return false;
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0 || !buf_reserve(b, (size_t)n)) { va_end(ap); return false; }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return true;
}

char *ctx_buf_take(CtxBuf *b) {
    char *out = b && b->data ? b->data : strdup("");
    if (b) memset(b, 0, sizeof(*b));
    return out;
}

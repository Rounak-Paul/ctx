#include "fetch.h"
#include "../log/log.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t  block[64];
    size_t   used;
} Sha256;

static const uint32_t k_sha256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + k_sha256[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_init(Sha256 *c) {
    static const uint32_t init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->state, init, sizeof(init));
    c->bits = 0;
    c->used = 0;
}

static void sha256_update(Sha256 *c, const uint8_t *data, size_t len) {
    c->bits += (uint64_t)len * 8;
    while (len) {
        size_t take = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->block + c->used, data, take);
        c->used += take;
        data += take;
        len -= take;
        if (c->used == 64) { sha256_block(c, c->block); c->used = 0; }
    }
}

static void sha256_final(Sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) sha256_update(c, &zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, len_be, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

bool ctx_sha256_file(const char *path, char out_hex[65]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    Sha256 c;
    sha256_init(&c);
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) sha256_update(&c, buf, n);
    bool ok = !ferror(fp);
    fclose(fp);
    if (!ok) return false;
    uint8_t digest[32];
    sha256_final(&c, digest);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    return true;
}

/* ---- download ---------------------------------------------------------------- */

static bool ensure_dir(const char *path) {
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

static int64_t mtime_ns(const struct stat *st) {
#if defined(CTX_PLATFORM_MACOS)
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#elif defined(CTX_PLATFORM_LINUX)
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#else
    return (int64_t)st->st_mtime * 1000000000LL;
#endif
}

/* Marker recording a completed verification: "<sha256> <size> <mtime_ns>". */
static void marker_line(const CtxModelArtifact *a, const struct stat *st, char *out, size_t out_size) {
    snprintf(out, out_size, "%s %" PRIu64 " %" PRId64 "\n", a->sha256, a->size, mtime_ns(st));
}

static bool marker_matches(const char *path, const CtxModelArtifact *a, const struct stat *st) {
    char marker[4096], expected[160], got[160] = {0};
    int n = snprintf(marker, sizeof(marker), "%s.verified", path);
    if (n <= 0 || (size_t)n >= sizeof(marker)) return false;
    FILE *fp = fopen(marker, "r");
    if (!fp) return false;
    bool read = fgets(got, sizeof(got), fp) != NULL;
    fclose(fp);
    marker_line(a, st, expected, sizeof(expected));
    return read && !strcmp(got, expected);
}

static void marker_write(const char *path, const CtxModelArtifact *a) {
    struct stat st;
    char marker[4096], line[160];
    int n = snprintf(marker, sizeof(marker), "%s.verified", path);
    if (stat(path, &st) != 0 || n <= 0 || (size_t)n >= sizeof(marker)) return;
    FILE *fp = fopen(marker, "w");
    if (!fp) return;
    marker_line(a, &st, line, sizeof(line));
    fputs(line, fp);
    fclose(fp);
}

/* True when path holds the artifact: size matches and either a verification
 * marker for this exact file state exists or the sha256 matches. */
static bool file_matches(const char *path, const CtxModelArtifact *a) {
    struct stat st;
    if (stat(path, &st) != 0 || (uint64_t)st.st_size != a->size) return false;
    if (marker_matches(path, a, &st)) return true;
    char hex[65];
    return ctx_sha256_file(path, hex) && !strcmp(hex, a->sha256);
}

bool ctx_model_artifact_present(const char *dir, const CtxModelArtifact *artifact,
                                char *out_path, size_t out_path_size) {
    if (!dir || !artifact || !out_path) return false;
    int n = snprintf(out_path, out_path_size, "%s/%s", dir, artifact->file);
    if (n <= 0 || (size_t)n >= out_path_size || !file_matches(out_path, artifact)) return false;
    marker_write(out_path, artifact);
    return true;
}

/* Runs curl to download url into dest; polls cancel while it runs. */
static bool run_curl(const char *url, const char *dest, volatile bool *cancel,
                     char *err, size_t err_size) {
    char *const argv[] = {
        "curl", "--location", "--fail", "--silent", "--show-error",
        "--retry", "3", "--connect-timeout", "30",
        "--output", (char *)dest, (char *)url, NULL
    };
    pid_t pid;
    int rc = posix_spawnp(&pid, "curl", NULL, NULL, argv, environ);
    if (rc != 0) {
        snprintf(err, err_size, "cannot run curl: %s", strerror(rc));
        return false;
    }
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) {
            snprintf(err, err_size, "waitpid failed: %s", strerror(errno));
            return false;
        }
        if (cancel && *cancel) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            snprintf(err, err_size, "download cancelled");
            return false;
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(err, err_size, "curl failed (exit %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

bool ctx_model_fetch(const char *dir, const CtxModelArtifact *artifact,
                     char *out_path, size_t out_path_size,
                     volatile bool *cancel, char *err, size_t err_size) {
    if (!dir || !artifact || !out_path || !err) return false;
    err[0] = '\0';
    int n = snprintf(out_path, out_path_size, "%s/%s", dir, artifact->file);
    if (n <= 0 || (size_t)n >= out_path_size) {
        snprintf(err, err_size, "model path too long");
        return false;
    }
    if (file_matches(out_path, artifact)) return true;
    if (!ensure_dir(dir)) {
        snprintf(err, err_size, "cannot create %s: %s", dir, strerror(errno));
        return false;
    }

    char tmp[4096];
    n = snprintf(tmp, sizeof(tmp), "%s.part.%d", out_path, (int)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        snprintf(err, err_size, "model path too long");
        return false;
    }
    CTX_LOG_INFO("Downloading model %s (%.1f MB)", artifact->file, (double)artifact->size / (1024.0 * 1024.0));
    bool ok = run_curl(artifact->url, tmp, cancel, err, err_size);
    if (ok && !file_matches(tmp, artifact)) {
        snprintf(err, err_size, "downloaded %s failed size/sha256 verification", artifact->file);
        ok = false;
    }
    if (ok && rename(tmp, out_path) != 0) {
        snprintf(err, err_size, "cannot move model into place: %s", strerror(errno));
        ok = false;
    }
    if (ok) marker_write(out_path, artifact);
    if (!ok) unlink(tmp);
    return ok;
}

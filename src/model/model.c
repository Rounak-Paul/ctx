#include "model.h"
#include "fetch.h"
#include "../log/log.h"

#ifdef CTX_HAS_MODELS
#include <llama.h>
#endif

/* Encoder models attend over the whole micro-batch at once (memory grows with
 * its square), so batches stay small; a sequence never exceeds MAX_TOKENS. */
#define CTX_MODEL_MAX_TOKENS  512
#define CTX_MODEL_BATCH       1024
#define CTX_MODEL_SEQ_MAX     8

typedef struct {
    const char       *id;
    CtxModelArtifact  artifact;
    const char       *env_override;
} RoleSpec;

static const RoleSpec k_roles[CTX_MODEL_ROLE_COUNT] = {
    [CTX_MODEL_RERANK] = {
        .id = "jina-reranker-v1-turbo-en-q8_0",
        .artifact = {
            .file = "jina-reranker-v1-turbo-en-Q8_0.gguf",
            .url = "https://huggingface.co/gpustack/jina-reranker-v1-turbo-en-GGUF/resolve/"
                   "93961807ed915fd207f20e76112427dae4abef22/jina-reranker-v1-turbo-en-Q8_0.gguf",
            .sha256 = "6633027dd42a9490313504ce698dcd8bbd44f8694e58ab555e2d06d8535f4f86",
            .size = 41719968,
        },
        .env_override = "CTX_RERANK_MODEL",
    },
    [CTX_MODEL_EMBED] = {
        .id = "jina-embeddings-v2-base-code-q8_0",
        .artifact = {
            .file = "jina-embeddings-v2-base-code-q8_0.gguf",
            .url = "https://huggingface.co/ggml-org/jina-embeddings-v2-base-code-Q8_0-GGUF/resolve/"
                   "05e79e9a6c8b99491e92ebb28d753268f8601e3c/jina-embeddings-v2-base-code-q8_0.gguf",
            .sha256 = "3bd1722f09350209aa3ada93df55882666c58194bfbbbe81c30545d731cb4e7a",
            .size = 172869280,
        },
        .env_override = "CTX_EMBED_MODEL",
    },
};

typedef struct {
    CtxModelState         state;
    char                  detail[256];
    char                  id[256];
    uint32_t              dim;
    pthread_mutex_t       run_lock;
#ifdef CTX_HAS_MODELS
    struct llama_model   *model;
    struct llama_context *ctx;
#endif
} RoleRuntime;

static RoleRuntime     s_roles[CTX_MODEL_ROLE_COUNT];
static pthread_mutex_t s_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       s_loader;
static bool            s_loader_started = false;
static volatile bool   s_cancel = false;

static void set_state(CtxModelRole role, CtxModelState state, const char *detail) {
    pthread_mutex_lock(&s_state_lock);
    s_roles[role].state = state;
    snprintf(s_roles[role].detail, sizeof(s_roles[role].detail), "%s", detail ? detail : "");
    pthread_mutex_unlock(&s_state_lock);
}

CtxModelState ctx_models_state(CtxModelRole role, char *detail, size_t detail_size) {
    if (role >= CTX_MODEL_ROLE_COUNT) return CTX_MODEL_DISABLED;
    pthread_mutex_lock(&s_state_lock);
    CtxModelState state = s_roles[role].state;
    if (detail && detail_size) snprintf(detail, detail_size, "%s", s_roles[role].detail);
    pthread_mutex_unlock(&s_state_lock);
    return state;
}

const char *ctx_models_state_name(CtxModelState state) {
    switch (state) {
    case CTX_MODEL_DISABLED:    return "disabled";
    case CTX_MODEL_PENDING:     return "pending";
    case CTX_MODEL_DOWNLOADING: return "downloading";
    case CTX_MODEL_LOADING:     return "loading";
    case CTX_MODEL_READY:       return "ready";
    case CTX_MODEL_FAILED:      return "failed";
    }
    return "unknown";
}

static bool role_ready(CtxModelRole role) {
    return ctx_models_state(role, NULL, 0) == CTX_MODEL_READY;
}

uint32_t ctx_models_embed_dim(void) {
    if (!role_ready(CTX_MODEL_EMBED)) return 0;
    pthread_mutex_lock(&s_state_lock);
    uint32_t dim = s_roles[CTX_MODEL_EMBED].dim;
    pthread_mutex_unlock(&s_state_lock);
    return dim;
}

const char *ctx_models_embed_id(void) {
    return role_ready(CTX_MODEL_EMBED) ? s_roles[CTX_MODEL_EMBED].id : "";
}

#ifdef CTX_HAS_MODELS

static void llama_log(enum ggml_log_level level, const char *text, void *user) {
    CTX_UNUSED(user);
    if (level == GGML_LOG_LEVEL_ERROR && text) CTX_LOG_WARN("llama: %.*s", (int)strcspn(text, "\n"), text);
}

static int32_t thread_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return (int32_t)(n > 8 ? 8 : n);
}

/*
 * Tokenizes text into a heap array, truncated to max tokens while keeping the
 * final special token when add_special is set. Returns the token count, or -1.
 */
static int32_t tokenize(const struct llama_vocab *vocab, const char *text, bool add_special,
                        bool parse_special, int32_t max, llama_token **out) {
    *out = NULL;
    int32_t len = (int32_t)strlen(text);
    int32_t cap = len + 8;
    llama_token *tokens = (llama_token *)malloc((size_t)cap * sizeof(llama_token));
    if (!tokens) return -1;
    int32_t n = llama_tokenize(vocab, text, len, tokens, cap, add_special, parse_special);
    if (n < 0) {
        cap = -n;
        llama_token *bigger = (llama_token *)realloc(tokens, (size_t)cap * sizeof(llama_token));
        if (!bigger) { free(tokens); return -1; }
        tokens = bigger;
        n = llama_tokenize(vocab, text, len, tokens, cap, add_special, parse_special);
        if (n < 0) { free(tokens); return -1; }
    }
    if (n > max) {
        if (add_special) tokens[max - 1] = tokens[n - 1];
        n = max;
    }
    *out = tokens;
    return n;
}

/* Builds the cross-encoder input for one (query, doc) pair. */
static int32_t rerank_tokens(const struct llama_model *model, const char *query, const char *doc,
                             llama_token **out) {
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    const char *tmpl = llama_model_chat_template(model, "rerank");
    if (tmpl) {
        size_t cap = strlen(tmpl) + strlen(query) + strlen(doc) + 1;
        char *prompt = (char *)malloc(cap);
        if (!prompt) return -1;
        size_t w = 0;
        for (const char *p = tmpl; *p;) {
            if (!strncmp(p, "{query}", 7))         { size_t l = strlen(query); memcpy(prompt + w, query, l); w += l; p += 7; }
            else if (!strncmp(p, "{document}", 10)) { size_t l = strlen(doc); memcpy(prompt + w, doc, l); w += l; p += 10; }
            else prompt[w++] = *p++;
        }
        prompt[w] = '\0';
        int32_t n = tokenize(vocab, prompt, false, true, CTX_MODEL_MAX_TOKENS, out);
        free(prompt);
        return n;
    }

    llama_token *q = NULL, *d = NULL;
    int32_t nq = tokenize(vocab, query, false, false, 96, &q);
    int32_t nd = nq < 0 ? -1 : tokenize(vocab, doc, false, false, CTX_MODEL_MAX_TOKENS, &d);
    if (nq < 0 || nd < 0) { free(q); free(d); return -1; }
    int32_t budget = CTX_MODEL_MAX_TOKENS - nq - 4;
    if (nd > budget) nd = budget > 0 ? budget : 0;

    llama_token *t = (llama_token *)malloc((size_t)(nq + nd + 4) * sizeof(llama_token));
    if (!t) { free(q); free(d); return -1; }
    llama_token eos = llama_vocab_eos(vocab);
    if (eos == LLAMA_TOKEN_NULL) eos = llama_vocab_sep(vocab);
    int32_t n = 0;
    if (llama_vocab_get_add_bos(vocab)) t[n++] = llama_vocab_bos(vocab);
    memcpy(t + n, q, (size_t)nq * sizeof(llama_token)); n += nq;
    if (llama_vocab_get_add_eos(vocab)) t[n++] = eos;
    if (llama_vocab_get_add_sep(vocab)) t[n++] = llama_vocab_sep(vocab);
    memcpy(t + n, d, (size_t)nd * sizeof(llama_token)); n += nd;
    if (llama_vocab_get_add_eos(vocab)) t[n++] = eos;
    free(q);
    free(d);
    *out = t;
    return n;
}

typedef struct {
    llama_token *tokens;
    int32_t      count;
} TokenSeq;

/*
 * Runs pooled inference over sequences, packing as many per batch as fit.
 * For each sequence, out_dim floats of the pooled output are written to out.
 */
static bool run_pooled(RoleRuntime *rt, const TokenSeq *seqs, uint32_t count, uint32_t out_dim,
                       float *out) {
    struct llama_batch batch = llama_batch_init(CTX_MODEL_BATCH, 0, 1);
    bool ok = true;
    uint32_t next = 0;
    while (ok && next < count) {
        batch.n_tokens = 0;
        uint32_t first = next;
        while (next < count && next - first < CTX_MODEL_SEQ_MAX &&
               batch.n_tokens + seqs[next].count <= CTX_MODEL_BATCH) {
            llama_seq_id seq = (llama_seq_id)(next - first);
            for (int32_t i = 0; i < seqs[next].count; i++) {
                int32_t k = batch.n_tokens++;
                batch.token[k] = seqs[next].tokens[i];
                batch.pos[k] = i;
                batch.n_seq_id[k] = 1;
                batch.seq_id[k][0] = seq;
                batch.logits[k] = 1;
            }
            next++;
        }
        if (next == first) { ok = false; break; }
        llama_memory_t mem = llama_get_memory(rt->ctx);
        if (mem) llama_memory_clear(mem, true);
        if (llama_decode(rt->ctx, batch) != 0) { ok = false; break; }
        for (uint32_t s = first; s < next; s++) {
            const float *e = llama_get_embeddings_seq(rt->ctx, (llama_seq_id)(s - first));
            if (!e) { ok = false; break; }
            memcpy(out + (size_t)s * out_dim, e, out_dim * sizeof(float));
        }
    }
    llama_batch_free(batch);
    return ok;
}

bool ctx_models_rerank(const char *query, const char *const *docs, uint32_t count, float *scores) {
    if (!query || !docs || !scores || count == 0 || !role_ready(CTX_MODEL_RERANK)) return false;
    RoleRuntime *rt = &s_roles[CTX_MODEL_RERANK];
    TokenSeq *seqs = (TokenSeq *)calloc(count, sizeof(TokenSeq));
    if (!seqs) return false;
    bool ok = true;
    pthread_mutex_lock(&rt->run_lock);
    for (uint32_t i = 0; ok && i < count; i++) {
        seqs[i].count = rerank_tokens(rt->model, query, docs[i] ? docs[i] : "", &seqs[i].tokens);
        ok = seqs[i].count > 0;
    }
    if (ok) ok = run_pooled(rt, seqs, count, 1, scores);
    pthread_mutex_unlock(&rt->run_lock);
    for (uint32_t i = 0; i < count; i++) free(seqs[i].tokens);
    free(seqs);
    return ok;
}

bool ctx_models_embed(const char *const *texts, uint32_t count, float *out) {
    if (!texts || !out || count == 0 || !role_ready(CTX_MODEL_EMBED)) return false;
    RoleRuntime *rt = &s_roles[CTX_MODEL_EMBED];
    uint32_t dim = rt->dim;
    TokenSeq *seqs = (TokenSeq *)calloc(count, sizeof(TokenSeq));
    if (!seqs) return false;
    bool ok = true;
    pthread_mutex_lock(&rt->run_lock);
    const struct llama_vocab *vocab = llama_model_get_vocab(rt->model);
    for (uint32_t i = 0; ok && i < count; i++) {
        seqs[i].count = tokenize(vocab, texts[i] ? texts[i] : "", true, false,
                                 CTX_MODEL_MAX_TOKENS, &seqs[i].tokens);
        ok = seqs[i].count > 0;
    }
    if (ok) ok = run_pooled(rt, seqs, count, dim, out);
    pthread_mutex_unlock(&rt->run_lock);
    for (uint32_t i = 0; ok && i < count; i++) {
        float *v = out + (size_t)i * dim;
        double norm = 0.0;
        for (uint32_t k = 0; k < dim; k++) norm += (double)v[k] * v[k];
        float inv = norm > 0.0 ? (float)(1.0 / sqrt(norm)) : 0.0f;
        for (uint32_t k = 0; k < dim; k++) v[k] *= inv;
    }
    for (uint32_t i = 0; i < count; i++) free(seqs[i].tokens);
    free(seqs);
    return ok;
}

static void model_dir(char *out, size_t out_size) {
    const char *dir = getenv("CTX_MODEL_DIR");
    if (dir && dir[0]) { snprintf(out, out_size, "%s", dir); return; }
    const char *home = getenv("HOME");
    snprintf(out, out_size, "%s/.ctx/models", home && home[0] ? home : ".");
}

/* Fetches (or locates) and loads one role; records the outcome in its state. */
static void load_role(CtxModelRole role) {
    const RoleSpec *spec = &k_roles[role];
    RoleRuntime *rt = &s_roles[role];
    char path[4096], err[512] = {0};
    const char *override = getenv(spec->env_override);
    if (override && override[0]) {
        snprintf(path, sizeof(path), "%s", override);
        snprintf(rt->id, sizeof(rt->id), "local:%s", override);
    } else {
        char dir[4096];
        model_dir(dir, sizeof(dir));
        bool present = ctx_model_artifact_present(dir, &spec->artifact, path, sizeof(path));
        if (!present) set_state(role, CTX_MODEL_DOWNLOADING, spec->artifact.file);
        if (!present &&
            !ctx_model_fetch(dir, &spec->artifact, path, sizeof(path), &s_cancel, err, sizeof(err))) {
            set_state(role, CTX_MODEL_FAILED, err);
            CTX_LOG_WARN("Model %s unavailable: %s", spec->id, err);
            return;
        }
        snprintf(rt->id, sizeof(rt->id), "%s", spec->id);
    }
    if (s_cancel) return;

    set_state(role, CTX_MODEL_LOADING, rt->id);
    struct llama_model_params mp = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(path, mp);
    if (!model) {
        set_state(role, CTX_MODEL_FAILED, "cannot load model file");
        return;
    }
    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.pooling_type = role == CTX_MODEL_RERANK ? LLAMA_POOLING_TYPE_RANK : LLAMA_POOLING_TYPE_UNSPECIFIED;
    cp.n_ctx = CTX_MODEL_BATCH;
    cp.n_batch = CTX_MODEL_BATCH;
    cp.n_ubatch = CTX_MODEL_BATCH;
    cp.n_seq_max = CTX_MODEL_SEQ_MAX;
    cp.n_threads = thread_count();
    cp.n_threads_batch = cp.n_threads;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        llama_model_free(model);
        set_state(role, CTX_MODEL_FAILED, "cannot create inference context");
        return;
    }
    if (role == CTX_MODEL_RERANK && llama_pooling_type(ctx) != LLAMA_POOLING_TYPE_RANK) {
        llama_free(ctx);
        llama_model_free(model);
        set_state(role, CTX_MODEL_FAILED, "model has no rank head");
        return;
    }

    pthread_mutex_lock(&rt->run_lock);
    rt->model = model;
    rt->ctx = ctx;
    pthread_mutex_unlock(&rt->run_lock);
    pthread_mutex_lock(&s_state_lock);
    rt->dim = role == CTX_MODEL_EMBED ? (uint32_t)llama_model_n_embd(model) : 1;
    pthread_mutex_unlock(&s_state_lock);
    set_state(role, CTX_MODEL_READY, rt->id);
    CTX_LOG_INFO("Model ready: %s", rt->id);
    CtxModelRole payload = role;
    ctx_event_emit(CTX_EVENT_MODEL_READY, &payload, sizeof(payload));
}

static void *loader_main(void *arg) {
    CTX_UNUSED(arg);
    llama_log_set(llama_log, NULL);
    llama_backend_init();
    for (int r = 0; r < CTX_MODEL_ROLE_COUNT && !s_cancel; r++) load_role((CtxModelRole)r);
    return NULL;
}

void ctx_models_start(bool enabled) {
    const char *env = getenv("CTX_MODELS");
    if (!enabled || (env && !strcmp(env, "0")) || s_loader_started) return;
    for (int r = 0; r < CTX_MODEL_ROLE_COUNT; r++) {
        pthread_mutex_init(&s_roles[r].run_lock, NULL);
        set_state((CtxModelRole)r, CTX_MODEL_PENDING, k_roles[r].id);
    }
    s_cancel = false;
    if (pthread_create(&s_loader, NULL, loader_main, NULL) != 0) {
        for (int r = 0; r < CTX_MODEL_ROLE_COUNT; r++)
            set_state((CtxModelRole)r, CTX_MODEL_FAILED, "cannot start loader thread");
        return;
    }
    s_loader_started = true;
}

void ctx_models_stop(void) {
    if (!s_loader_started) return;
    s_cancel = true;
    pthread_join(s_loader, NULL);
    s_loader_started = false;
    for (int r = 0; r < CTX_MODEL_ROLE_COUNT; r++) {
        RoleRuntime *rt = &s_roles[r];
        set_state((CtxModelRole)r, CTX_MODEL_DISABLED, "");
        pthread_mutex_lock(&rt->run_lock);
        if (rt->ctx) llama_free(rt->ctx);
        if (rt->model) llama_model_free(rt->model);
        rt->ctx = NULL;
        rt->model = NULL;
        pthread_mutex_unlock(&rt->run_lock);
        pthread_mutex_destroy(&rt->run_lock);
    }
    llama_backend_free();
}

#else /* !CTX_HAS_MODELS */

bool ctx_models_rerank(const char *query, const char *const *docs, uint32_t count, float *scores) {
    CTX_UNUSED(query); CTX_UNUSED(docs); CTX_UNUSED(count); CTX_UNUSED(scores);
    return false;
}

bool ctx_models_embed(const char *const *texts, uint32_t count, float *out) {
    CTX_UNUSED(texts); CTX_UNUSED(count); CTX_UNUSED(out);
    return false;
}

void ctx_models_start(bool enabled) {
    CTX_UNUSED(enabled);
    for (int r = 0; r < CTX_MODEL_ROLE_COUNT; r++)
        set_state((CtxModelRole)r, CTX_MODEL_DISABLED, "built without CTX_WITH_MODELS");
}

void ctx_models_stop(void) {}

#endif

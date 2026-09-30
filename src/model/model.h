#pragma once
#include "../pch.h"
#include "../event/event.h"

/*
 * Local models for search (llama.cpp): a cross-encoder reranker and a code
 * embedder. Models are fetched (pinned + sha256-verified) into the model
 * directory and loaded on a background thread; callers must check readiness
 * and fall back to non-model ranking when a role is not ready.
 *
 * Configuration (environment):
 *   CTX_MODELS=0          disable models entirely
 *   CTX_MODEL_DIR=<dir>   model directory (default ~/.ctx/models)
 *   CTX_RERANK_MODEL=<gguf>, CTX_EMBED_MODEL=<gguf>
 *                         use a local GGUF instead of the pinned download
 */

/* Emitted (payload: CtxModelRole) when a role becomes ready. */
#define CTX_EVENT_MODEL_READY (CTX_EVENT_USER_BASE + 20u)

typedef enum {
    CTX_MODEL_RERANK = 0,
    CTX_MODEL_EMBED,
    CTX_MODEL_ROLE_COUNT
} CtxModelRole;

typedef enum {
    CTX_MODEL_DISABLED = 0,
    CTX_MODEL_PENDING,
    CTX_MODEL_DOWNLOADING,
    CTX_MODEL_LOADING,
    CTX_MODEL_READY,
    CTX_MODEL_FAILED
} CtxModelState;

/*
 * Starts background fetch + load of both roles. No-op when models are
 * compiled out or disabled (enabled=false or CTX_MODELS=0).
 */
void ctx_models_start(bool enabled);

/* Cancels downloads, joins the loader, and frees models. */
void ctx_models_stop(void);

/* Current state of a role; detail (optional) receives the model id or error. */
CtxModelState ctx_models_state(CtxModelRole role, char *detail, size_t detail_size);

const char *ctx_models_state_name(CtxModelState state);

/*
 * Scores query–document relevance with the reranker (higher is better).
 * Documents are truncated to the model context. Serialised internally.
 * Returns false when the reranker is not ready or inference fails.
 */
bool ctx_models_rerank(const char *query, const char *const *docs, uint32_t count, float *scores);

/* Embedding dimension, 0 while the embedder is not ready. */
uint32_t ctx_models_embed_dim(void);

/* Stable identifier of the loaded embedder (cache key namespace), "" if none. */
const char *ctx_models_embed_id(void);

/*
 * Embeds texts into L2-normalised vectors (count * dim floats, row-major).
 * Serialised internally. Returns false when not ready or inference fails.
 */
bool ctx_models_embed(const char *const *texts, uint32_t count, float *out);

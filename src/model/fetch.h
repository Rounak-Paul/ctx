#pragma once
#include "../pch.h"

/*
 * Pinned downloadable model artifact.
 *
 * file    Local file name inside the model directory.
 * url     Immutable (commit-pinned) download URL.
 * sha256  Lowercase hex digest the downloaded file must match.
 * size    Exact size in bytes.
 */
typedef struct {
    const char *file;
    const char *url;
    const char *sha256;
    uint64_t    size;
} CtxModelArtifact;

/*
 * Ensures dir/artifact->file exists and matches size and digest, downloading
 * it with the system curl when missing or invalid. The file is written to a
 * temporary name and renamed only after verification.
 *
 * dir        Model directory (created when missing).
 * artifact   Artifact to fetch.
 * out_path   Receives the verified local path.
 * cancel     Optional flag; a download in progress is aborted when it turns true.
 * err        Receives a human-readable reason on failure.
 */
bool ctx_model_fetch(const char *dir, const CtxModelArtifact *artifact,
                     char *out_path, size_t out_path_size,
                     volatile bool *cancel, char *err, size_t err_size);

/*
 * Checks for a verified local copy without downloading. Verification is
 * remembered in "<file>.verified" (digest, size, mtime), so the digest is only
 * recomputed after the file changes.
 *
 * out_path  Receives dir/artifact->file.
 * Returns true when the file is present and valid.
 */
bool ctx_model_artifact_present(const char *dir, const CtxModelArtifact *artifact,
                                char *out_path, size_t out_path_size);

/*
 * Computes the SHA-256 of a file as lowercase hex (65 bytes incl. NUL).
 * Returns false when the file cannot be read.
 */
bool ctx_sha256_file(const char *path, char out_hex[65]);

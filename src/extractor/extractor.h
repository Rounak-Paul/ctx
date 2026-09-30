#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Parses one source file and collects its symbols and reference sites.
 *
 * path  Absolute path of a supported source file.
 * out   Zeroed and filled on success; release with ctx_file_extract_free.
 *       Left empty on failure.
 * Returns false when the file cannot be read or parsed.
 */
bool ctx_extract_file(const char *path, CtxFileExtract *out);

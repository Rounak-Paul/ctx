#pragma once
#include "../pch.h"
#include "../graph/graph.h"

bool ctx_extract_file(CtxGraph *g, const char *path);
void ctx_extract_directory(CtxGraph *g, const char *dir, bool recursive);

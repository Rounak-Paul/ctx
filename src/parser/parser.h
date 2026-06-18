#pragma once
#include "../pch.h"

typedef enum {
    CTX_LANG_C = 0,
    CTX_LANG_CPP,
    CTX_LANG_PYTHON,
    CTX_LANG_JS,
    CTX_LANG_TS,
    CTX_LANG_UNKNOWN
} CtxLanguage;

typedef struct {
    TSTree     *tree;
    char       *source;
    size_t      source_len;
    CtxLanguage lang;
    bool        has_errors;
    uint32_t    error_count;
} CtxParseResult;

bool        ctx_parser_init(void);
void        ctx_parser_shutdown(void);
CtxLanguage ctx_lang_from_path(const char *path);
bool        ctx_parser_parse_file(const char *path, CtxParseResult *out);
void        ctx_parser_reparse(CtxParseResult *r, const char *new_source, size_t len);
void        ctx_parser_free_result(CtxParseResult *r);

#pragma once
#include "../pch.h"

/*
 * Agent tool registry shared by the MCP server and the HTTP API, so both
 * transports expose identical names, arguments, and behaviour.
 */

typedef enum {
    CTX_TOOL_ARG_STRING = 0,
    CTX_TOOL_ARG_INT,
    CTX_TOOL_ARG_BOOL
} CtxToolArgType;

typedef struct {
    const char     *name;
    CtxToolArgType  type;
    bool            required;
    const char     *description;
} CtxToolArg;

typedef struct {
    const char       *name;
    const char       *description;
    const CtxToolArg *args;
    uint32_t          arg_count;
} CtxToolSpec;

/* Number of registered tools. */
uint32_t ctx_tools_count(void);

/* Tool at index i (0 ≤ i < ctx_tools_count()). */
const CtxToolSpec *ctx_tools_at(uint32_t i);

/* Tool by name, or NULL. */
const CtxToolSpec *ctx_tools_find(const char *name);

/* JSON Schema object describing a tool's arguments (caller deletes). */
cJSON *ctx_tools_input_schema(const CtxToolSpec *spec);

/*
 * Runs a tool.
 *
 * name      Tool name.
 * args      Argument object (may be NULL when the tool takes none).
 * is_error  Set true when the call failed (unknown tool, bad arguments, or
 *           a tool-level error such as an unknown symbol).
 * Returns heap text (never NULL); caller frees.
 */
char *ctx_tools_call(const char *name, const cJSON *args, bool *is_error);

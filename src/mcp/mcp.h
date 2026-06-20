#pragma once
#include "../pch.h"

/*
 * Runs the MCP (Model Context Protocol) server loop on stdin/stdout.
 * Implements JSON-RPC 2.0 over newline-delimited stdio transport.
 * Blocks until stdin is closed. Called from main() when --mcp is passed.
 */
void ctx_mcp_run(void);

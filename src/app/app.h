#pragma once

#include "../pch.h"

/* --------------------------------------------------------------------------
 * Application mode
 *
 * GUI mode is default when:
 *   - no --no-gui flag is present, AND
 *   - causality was compiled in (CTX_HAS_CAUSALITY defined)
 *
 * On non-GUI builds CTX_HAS_CAUSALITY is absent so gui_mode is always false.
 *
 * Double-click / shortcut launches have no terminal attached (stdin is not
 * a tty). On those launches we default to GUI even if --no-gui isn't passed
 * — which is the expected behaviour.
 * -------------------------------------------------------------------------- */

typedef struct {
    bool  gui_mode;         /* true → start causality window */
    bool  no_api;           /* true → don't start HTTP API server */
    bool  bench;            /* true → run retrieval benchmark, then exit */
    bool  mcp_mode;         /* true → run MCP server on stdio, no API, no GUI */
    int   api_port;         /* default 8765 */
    char  project_path[4096]; /* path to index; "" → cwd */
    int   argc;
    char **argv;
} CtxAppConfig;

/* Parse argc/argv, detect terminal attachment, return filled CtxAppConfig. */
CtxAppConfig ctx_app_parse_args(int argc, char **argv);

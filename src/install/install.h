#pragma once

#include "../app/app.h"

/*
 * Writes project-scoped MCP client config and agent instructions for ctx.
 *
 * cfg: Parsed application config containing project path and selected clients.
 * argv0: Executable path used to launch the MCP server.
 */
bool ctx_install_run(const CtxAppConfig *cfg, const char *argv0);

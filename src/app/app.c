#include "app.h"
#include "../log/log.h"

CtxAppConfig ctx_app_parse_args(int argc, char **argv)
{
    CtxAppConfig cfg = {0};
    cfg.argc     = argc;
    cfg.argv     = argv;
    cfg.api_port = 8765;

    /* Resolve cwd as default project path */
    if (!getcwd(cfg.project_path, sizeof(cfg.project_path)))
        strncpy(cfg.project_path, ".", sizeof(cfg.project_path) - 1);

#ifdef CTX_HAS_CAUSALITY
    cfg.gui_mode = true;
#else
    cfg.gui_mode = false;
#endif

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-gui")) {
            cfg.gui_mode = false;
        } else if (!strcmp(argv[i], "--no-api")) {
            cfg.no_api = true;
        } else if (!strcmp(argv[i], "--bench")) {
            cfg.bench   = true;
            cfg.gui_mode = false;
            cfg.no_api  = true;
        } else if (!strcmp(argv[i], "--project") && i + 1 < argc) {
            strncpy(cfg.project_path, argv[++i], sizeof(cfg.project_path) - 1);
        } else if (!strcmp(argv[i], "--api-port") && i + 1 < argc) {
            cfg.api_port = atoi(argv[++i]);
            if (cfg.api_port <= 0 || cfg.api_port > 65535) cfg.api_port = 8765;
        }
    }

#ifndef CTX_HAS_CAUSALITY
    CTX_UNUSED(argc);
    CTX_UNUSED(argv);
#endif

    CTX_LOG_INFO("Project path : %s", cfg.project_path);
    CTX_LOG_INFO("GUI mode     : %s", cfg.gui_mode ? "yes" : "no");
    CTX_LOG_INFO("API enabled  : %s (port %d)", cfg.no_api ? "no" : "yes", cfg.api_port);
    return cfg;
}

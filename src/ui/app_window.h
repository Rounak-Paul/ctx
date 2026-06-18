#pragma once

#ifdef CTX_HAS_CAUSALITY

#include "../pch.h"

/* --------------------------------------------------------------------------
 * ctx main window
 *
 * Owns the Ca_Instance and the primary Ca_Window.
 * Call ctx_ui_run() — it blocks until the window is closed.
 * -------------------------------------------------------------------------- */

bool ctx_ui_run(void);

#endif /* CTX_HAS_CAUSALITY */

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

/**
 * Requests that the running UI loop close its window and return from
 * ctx_ui_run(). Async-signal-safe: only sets a sig_atomic_t flag that the UI
 * loop polls every tick.
 */
void ctx_ui_request_close(void);

#endif /* CTX_HAS_CAUSALITY */

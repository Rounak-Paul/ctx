#pragma once

#include "../pch.h"

/* --------------------------------------------------------------------------
 * Log system
 *
 * Severity levels (lowest → highest):
 *   TRACE   — fine-grained execution flow (stripped in release)
 *   DEBUG   — diagnostic values            (stripped in release)
 *   INFO    — normal operational messages
 *   WARN    — recoverable unexpected state  (fires CTX_EVENT_LOG_WARN)
 *   ERROR   — operation failed             (fires CTX_EVENT_LOG_ERROR)
 *   FATAL   — unrecoverable error          (fires CTX_EVENT_LOG_FATAL)
 *
 * Thread-safe: multiple threads may call CTX_LOG_* concurrently.
 * Output: stderr, format:  [HH:MM:SS.mmm] LEVEL  file:line  message
 * -------------------------------------------------------------------------- */

typedef enum {
    CTX_LOG_TRACE = 0,
    CTX_LOG_DEBUG,
    CTX_LOG_INFO,
    CTX_LOG_WARN,
    CTX_LOG_ERROR,
    CTX_LOG_FATAL,
    CTX_LOG_LEVEL_COUNT
} CtxLogLevel;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
void ctx_log_init(CtxLogLevel min_level);   /* call before any log macro */
void ctx_log_shutdown(void);

/* Runtime filter — anything below min_level is silently dropped */
void ctx_log_set_level(CtxLogLevel min_level);

/* --------------------------------------------------------------------------
 * Internal — do not call directly
 * -------------------------------------------------------------------------- */
void ctx_log_write(CtxLogLevel level,
                   const char *file,
                   int         line,
                   const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/* --------------------------------------------------------------------------
 * Public macros
 *
 * In release builds (NDEBUG defined) TRACE and DEBUG expand to nothing,
 * so the format strings and arguments are never evaluated — zero overhead.
 * -------------------------------------------------------------------------- */

#define CTX_LOG_INFO(fmt, ...)  \
    ctx_log_write(CTX_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define CTX_LOG_WARN(fmt, ...)  \
    ctx_log_write(CTX_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define CTX_LOG_ERROR(fmt, ...) \
    ctx_log_write(CTX_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define CTX_LOG_FATAL(fmt, ...) \
    ctx_log_write(CTX_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifndef NDEBUG
#   define CTX_LOG_TRACE(fmt, ...) \
        ctx_log_write(CTX_LOG_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#   define CTX_LOG_DEBUG(fmt, ...) \
        ctx_log_write(CTX_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#   define CTX_LOG_TRACE(fmt, ...) ((void)0)
#   define CTX_LOG_DEBUG(fmt, ...) ((void)0)
#endif

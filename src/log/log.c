#include "log.h"
#include "../event/event.h"

/* --------------------------------------------------------------------------
 * Extend the event ID space for log events
 * We define these here so they sit just above CTX_EVENT_USER_BASE.
 * -------------------------------------------------------------------------- */
#define CTX_EVENT_LOG_WARN  (CTX_EVENT_USER_BASE + 0u)
#define CTX_EVENT_LOG_ERROR (CTX_EVENT_USER_BASE + 1u)
#define CTX_EVENT_LOG_FATAL (CTX_EVENT_USER_BASE + 2u)

/* --------------------------------------------------------------------------
 * Log event payload — passed as CtxEvent.data
 * -------------------------------------------------------------------------- */
typedef struct {
    CtxLogLevel  level;
    char         message[512];
    char         file[128];
    int          line;
} CtxLogEvent;

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */
static struct {
    CtxLogLevel   min_level;
    bool          initialised;
#if defined(CTX_PLATFORM_WINDOWS)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t  lock;
#endif
} s_log;

static const char *s_level_str[CTX_LOG_LEVEL_COUNT] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

/* ANSI colour codes (suppressed on Windows unless VT mode enabled) */
#if defined(CTX_PLATFORM_WINDOWS)
static const char *s_level_colour[CTX_LOG_LEVEL_COUNT] = {
    "", "", "", "", "", ""
};
static const char *s_reset = "";
#else
static const char *s_level_colour[CTX_LOG_LEVEL_COUNT] = {
    "\033[2m",      /* TRACE — dim       */
    "\033[36m",     /* DEBUG — cyan      */
    "\033[32m",     /* INFO  — green     */
    "\033[33m",     /* WARN  — yellow    */
    "\033[31m",     /* ERROR — red       */
    "\033[1;31m",   /* FATAL — bold red  */
};
static const char *s_reset = "\033[0m";
#endif

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void ctx_log_init(CtxLogLevel min_level)
{
    if (s_log.initialised) return;
#if defined(CTX_PLATFORM_WINDOWS)
    InitializeCriticalSection(&s_log.lock);
#else
    pthread_mutex_init(&s_log.lock, NULL);
#endif
    s_log.min_level   = min_level;
    s_log.initialised = true;
}

void ctx_log_shutdown(void)
{
    if (!s_log.initialised) return;
#if defined(CTX_PLATFORM_WINDOWS)
    DeleteCriticalSection(&s_log.lock);
#else
    pthread_mutex_destroy(&s_log.lock);
#endif
    s_log.initialised = false;
}

void ctx_log_set_level(CtxLogLevel min_level)
{
    s_log.min_level = min_level;
}

/* --------------------------------------------------------------------------
 * Write
 * -------------------------------------------------------------------------- */

void ctx_log_write(CtxLogLevel level,
                   const char *file,
                   int         line,
                   const char *fmt, ...)
{
    if (!s_log.initialised || level < s_log.min_level) return;

    /* Format the message */
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Timestamp */
    char ts[32];
#if defined(CTX_PLATFORM_WINDOWS)
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    struct tm tm_val;
    localtime_r(&tp.tv_sec, &tm_val);
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
             tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec,
             (int)(tp.tv_nsec / 1000000));
#endif

    /* Basename only for brevity */
    const char *base = file;
    for (const char *p = file; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;

#if defined(CTX_PLATFORM_WINDOWS)
    EnterCriticalSection(&s_log.lock);
#else
    pthread_mutex_lock(&s_log.lock);
#endif

    fprintf(stderr, "%s[%s] %-5s%s  %s:%d  %s\n",
            s_level_colour[level], ts, s_level_str[level], s_reset,
            base, line, msg);
    fflush(stderr);

#if defined(CTX_PLATFORM_WINDOWS)
    LeaveCriticalSection(&s_log.lock);
#else
    pthread_mutex_unlock(&s_log.lock);
#endif

    /* Pump WARN / ERROR / FATAL through the event system */
    if (level >= CTX_LOG_WARN) {
        CtxLogEvent ev_payload;
        ev_payload.level = level;
        strncpy(ev_payload.message, msg, sizeof(ev_payload.message) - 1);
        ev_payload.message[sizeof(ev_payload.message) - 1] = '\0';
        strncpy(ev_payload.file, base, sizeof(ev_payload.file) - 1);
        ev_payload.file[sizeof(ev_payload.file) - 1] = '\0';
        ev_payload.line = line;

        static const CtxEventId level_events[] = {
            CTX_EVENT_LOG_WARN,
            CTX_EVENT_LOG_ERROR,
            CTX_EVENT_LOG_FATAL,
        };
        int idx = (int)level - (int)CTX_LOG_WARN;
        if (idx >= 0 && idx < 3)
            ctx_event_emit(level_events[idx], &ev_payload, sizeof(ev_payload));
    }

    /* FATAL is reported through logs/events. Callers decide whether to retry,
       degrade, or shut down gracefully. */
}

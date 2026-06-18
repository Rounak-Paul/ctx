#pragma once

/* --------------------------------------------------------------------------
 * C standard library
 * -------------------------------------------------------------------------- */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Platform threading primitives
 * -------------------------------------------------------------------------- */
#if defined(CTX_PLATFORM_WINDOWS)
#   include <windows.h>
#   include <process.h>
#elif defined(CTX_PLATFORM_MACOS) || defined(CTX_PLATFORM_LINUX)
#   include <pthread.h>
#   include <semaphore.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <sys/stat.h>
#   include <sys/types.h>
#endif

/* --------------------------------------------------------------------------
 * Platform file-watch backends
 * -------------------------------------------------------------------------- */
#if defined(CTX_PLATFORM_LINUX)
#   include <sys/inotify.h>
#   include <poll.h>
#elif defined(CTX_PLATFORM_MACOS)
#   include <sys/event.h>   /* kqueue / kevent */
#   include <sys/time.h>
#endif

/* --------------------------------------------------------------------------
 * tree-sitter
 * -------------------------------------------------------------------------- */
#include <tree_sitter/api.h>

/* Language grammar forward declarations —
 * definitions live in the compiled grammar static libs. */
extern const TSLanguage *tree_sitter_c(void);
extern const TSLanguage *tree_sitter_cpp(void);
extern const TSLanguage *tree_sitter_python(void);
extern const TSLanguage *tree_sitter_javascript(void);
extern const TSLanguage *tree_sitter_typescript(void);
extern const TSLanguage *tree_sitter_tsx(void);

/* --------------------------------------------------------------------------
 * SQLite
 * -------------------------------------------------------------------------- */
#include <sqlite3.h>

/* --------------------------------------------------------------------------
 * cJSON
 * -------------------------------------------------------------------------- */
#include <cJSON.h>

/* --------------------------------------------------------------------------
 * uthash
 * -------------------------------------------------------------------------- */
#include <uthash.h>
#include <utarray.h>
#include <utlist.h>

/* --------------------------------------------------------------------------
 * causality (optional)
 * -------------------------------------------------------------------------- */
#ifdef CTX_HAS_CAUSALITY
#   include <causality.h>
#endif

/* --------------------------------------------------------------------------
 * Convenience macros
 * -------------------------------------------------------------------------- */
#define CTX_UNUSED(x)       ((void)(x))
#define CTX_ARRAY_LEN(a)    (sizeof(a) / sizeof((a)[0]))
#define CTX_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

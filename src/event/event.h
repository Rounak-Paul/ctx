#pragma once

#include "../pch.h"

/* --------------------------------------------------------------------------
 * Event system
 *
 * Design:
 *   - Events are identified by a 32-bit ID (use the CTX_EVENT_* constants or
 *     register custom IDs via ctx_event_register).
 *   - Subscribers register a callback + optional user data pointer.
 *   - ctx_event_dispatch is thread-safe: any thread may fire an event.
 *   - Callbacks are invoked on the dispatching thread (synchronous).
 *   - Subscriber list is protected by a read-write lock so concurrent
 *     dispatches on different event IDs do not serialise each other.
 * -------------------------------------------------------------------------- */

#define CTX_EVENT_INVALID       0u
#define CTX_EVENT_FILE_CREATED  1u
#define CTX_EVENT_FILE_MODIFIED 2u
#define CTX_EVENT_FILE_DELETED  3u
#define CTX_EVENT_FILE_RENAMED  4u
#define CTX_EVENT_JOB_COMPLETE  5u
#define CTX_EVENT_SHUTDOWN      6u
#define CTX_EVENT_USER_BASE     1024u   /* first ID available for callers */

#define CTX_EVENT_MAX_SUBSCRIBERS 64u

typedef uint32_t CtxEventId;

typedef struct {
    CtxEventId  id;
    void       *data;   /* event-specific payload — caller owns lifetime */
    size_t      data_size;
} CtxEvent;

typedef void (*CtxEventCallback)(const CtxEvent *event, void *user_data);

typedef struct {
    CtxEventCallback  callback;
    void             *user_data;
    uint64_t          handle;   /* opaque unsubscribe token */
} CtxSubscriber;

/* Opaque handle returned by subscribe; pass to ctx_event_unsubscribe. */
typedef uint64_t CtxEventHandle;
#define CTX_EVENT_HANDLE_INVALID 0ull

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
bool ctx_event_system_init(void);
void ctx_event_system_shutdown(void);

/* --------------------------------------------------------------------------
 * Pub / sub
 * -------------------------------------------------------------------------- */
CtxEventHandle ctx_event_subscribe(CtxEventId id,
                                   CtxEventCallback callback,
                                   void *user_data);

void           ctx_event_unsubscribe(CtxEventId id, CtxEventHandle handle);

/* Fire event synchronously on calling thread. */
void           ctx_event_dispatch(const CtxEvent *event);

/* Convenience: stack-allocate a CtxEvent and dispatch. */
static inline void ctx_event_emit(CtxEventId id, void *data, size_t data_size)
{
    CtxEvent ev = { id, data, data_size };
    ctx_event_dispatch(&ev);
}

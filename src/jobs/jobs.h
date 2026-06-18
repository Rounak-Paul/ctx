#pragma once

#include "../pch.h"

/* --------------------------------------------------------------------------
 * Job system
 *
 * Design:
 *   - Fixed-size thread pool created at init time (defaults to CPU count).
 *   - Jobs are submitted to a FIFO queue protected by a mutex + condvar.
 *   - Three priority lanes (HIGH / NORMAL / LOW); higher lanes are drained
 *     before lower ones within each scheduling pass.
 *   - ctx_job_wait_all blocks the caller until the queue is empty and all
 *     workers are idle.
 *   - Graceful shutdown: finishes in-flight jobs, drains queue, then joins.
 * -------------------------------------------------------------------------- */

typedef enum {
    CTX_JOB_PRIORITY_HIGH   = 0,
    CTX_JOB_PRIORITY_NORMAL = 1,
    CTX_JOB_PRIORITY_LOW    = 2,
    CTX_JOB_PRIORITY_COUNT  = 3
} CtxJobPriority;

typedef void (*CtxJobFn)(void *user_data);

typedef struct {
    CtxJobFn        fn;
    void           *user_data;
    CtxJobPriority  priority;
} CtxJob;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/* thread_count == 0 → use hardware_concurrency (at least 1) */
bool ctx_jobs_init(uint32_t thread_count);
void ctx_jobs_shutdown(void);   /* waits for all in-flight jobs to finish */

/* --------------------------------------------------------------------------
 * Submit
 * -------------------------------------------------------------------------- */
bool ctx_job_submit(CtxJobFn fn, void *user_data, CtxJobPriority priority);

/* Convenience: submit at NORMAL priority */
static inline bool ctx_job_submit_normal(CtxJobFn fn, void *user_data)
{
    return ctx_job_submit(fn, user_data, CTX_JOB_PRIORITY_NORMAL);
}

/* --------------------------------------------------------------------------
 * Synchronisation
 * -------------------------------------------------------------------------- */

/* Block until queue is empty and every worker is idle. */
void ctx_jobs_wait_all(void);

/* Returns the number of threads in the pool. */
uint32_t ctx_jobs_thread_count(void);

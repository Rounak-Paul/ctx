#include "pch.h"
#include "log/log.h"
#include <signal.h>
#include "event/event.h"
#include "jobs/jobs.h"
#include "watcher/watcher.h"
#include "app/app.h"
#include "parser/parser.h"
#include "indexer/indexer.h"
#include "api/api.h"
#include "stats/stats.h"
#include "ui/app_window.h"
#include "bench/bench.h"
#include "mcp/mcp.h"

static volatile sig_atomic_t s_quit = 0;
static void on_sigint(int sig) { CTX_UNUSED(sig); s_quit = 1; }

static void on_file_change(const CtxEvent *ev, void *user_data)
{
    CTX_UNUSED(user_data);
    const CtxFileEvent *fe = (const CtxFileEvent *)ev->data;
    if (!fe) return;
    if (fe->kind == CTX_FILE_EVENT_CREATED  || fe->kind == CTX_FILE_EVENT_MODIFIED ||
        fe->kind == CTX_FILE_EVENT_DELETED  || fe->kind == CTX_FILE_EVENT_RENAMED) {
        ctx_indexer_update_file(fe->path);
    }
}

/* Background thread that runs the initial full index */
static void *index_thread(void *arg)
{
    CTX_UNUSED(arg);
    ctx_indexer_index_all();
    return NULL;
}

static bool s_idx_joined = false;
static pthread_t s_idx_thread;

/* CLI progress bar — blocks until indexing finishes, then returns */
static void cli_progress_loop(void)
{
    struct timespec tiny = { 0, 5000000 }; /* 5ms: let index thread set prog_running */
    nanosleep(&tiny, NULL);

    CtxIndexProgress prog;
    bool printed = false;
    for (;;) {
        ctx_indexer_get_progress(&prog);
        if (prog.running && prog.total > 0) {
            uint32_t pct = prog.done * 100 / prog.total;
            uint32_t bar = pct / 5; /* 20-wide */
            fprintf(stderr, "\r  Indexing [");
            for (uint32_t i = 0; i < 20; i++) fputc(i < bar ? '#' : '-', stderr);
            fprintf(stderr, "] %3u%%  (%u/%u)", pct, prog.done, prog.total);
            fflush(stderr);
            printed = true;
        }
        if (!prog.running) break;
        struct timespec ts = { 0, 80000000 };
        nanosleep(&ts, NULL);
    }
    if (printed) fprintf(stderr, "\r%72s\r", "");

    pthread_join(s_idx_thread, NULL);
    s_idx_joined = true;
}

int main(int argc, char *argv[])
{
    int exit_code = 0;
    bool event_ready = false;
    bool jobs_ready = false;
    bool watcher_ready = false;
    bool parser_ready = false;
    bool indexer_ready = false;
    bool api_ready = false;
    bool idx_started = false;

#ifdef NDEBUG
    ctx_log_init(CTX_LOG_INFO);
#else
    ctx_log_init(CTX_LOG_DEBUG);
#endif
    CTX_LOG_INFO("ctx starting");

    /* Block SIGINT/SIGTERM before any threads are created — child threads inherit the mask.
     * Main thread unblocks them after all threads exist so only main receives these signals. */
    sigset_t sig_set;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGINT);
    sigaddset(&sig_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sig_set, NULL);

    CtxAppConfig cfg = ctx_app_parse_args(argc, argv);

    if (!ctx_event_system_init()) {
        CTX_LOG_FATAL("Failed to init event system");
        exit_code = 1;
        goto shutdown;
    }
    event_ready = true;

    if (!ctx_jobs_init(0)) {
        CTX_LOG_FATAL("Failed to init job system");
        exit_code = 1;
        goto shutdown;
    }
    jobs_ready = true;

    if (ctx_watcher_init()) {
        watcher_ready = true;
    } else {
        CTX_LOG_WARN("File watcher unavailable — live updates disabled");
    }

    if (!ctx_parser_init()) {
        CTX_LOG_FATAL("Failed to init parser");
        exit_code = 1;
        goto shutdown;
    }
    parser_ready = true;

    if (!ctx_indexer_init(cfg.project_path)) {
        CTX_LOG_FATAL("Failed to init indexer for %s", cfg.project_path);
        exit_code = 1;
        goto shutdown;
    }
    indexer_ready = true;

    ctx_event_subscribe(CTX_EVENT_FILE_CREATED,  on_file_change, NULL);
    ctx_event_subscribe(CTX_EVENT_FILE_MODIFIED, on_file_change, NULL);
    ctx_event_subscribe(CTX_EVENT_FILE_DELETED,  on_file_change, NULL);
    ctx_event_subscribe(CTX_EVENT_FILE_RENAMED,  on_file_change, NULL);

    if (watcher_ready) ctx_watcher_add(cfg.project_path, true);

    if (pthread_create(&s_idx_thread, NULL, index_thread, NULL) != 0) {
        CTX_LOG_FATAL("Failed to start index thread: %s", strerror(errno));
        exit_code = 1;
        goto shutdown;
    }
    idx_started = true;

    if (cfg.bench) {
        cli_progress_loop();
        exit_code = ctx_bench_run(ctx_indexer_get_graph());
        goto shutdown;
    }

    if (cfg.mcp_mode) {
        cli_progress_loop();
        ctx_mcp_run();
        goto shutdown;
    }

    if (!cfg.no_api)
        api_ready = ctx_api_start(cfg.api_port);

    /* Install handlers and unblock signals on main thread only — all threads already created */
    {
        struct sigaction sa = {0};
        sa.sa_handler = on_sigint;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        pthread_sigmask(SIG_UNBLOCK, &sig_set, NULL);
    }

    if (cfg.gui_mode) {
#ifdef CTX_HAS_CAUSALITY
        CTX_LOG_INFO("Starting GUI");
        ctx_ui_run();
        s_quit = 1;
#else
        CTX_LOG_WARN("GUI requested but causality not available — running in CLI mode");
        cli_progress_loop();
        CTX_LOG_INFO("Watching for changes — Ctrl-C to exit  (API: http://127.0.0.1:%d)", cfg.api_port);
        while (!s_quit) { pause(); }
#endif
    } else {
        cli_progress_loop();
        CTX_LOG_INFO("Watching for changes — Ctrl-C to exit  (API: http://127.0.0.1:%d)", cfg.api_port);
        while (!s_quit) { pause(); }
    }

shutdown:
    if (event_ready) ctx_event_emit(CTX_EVENT_SHUTDOWN, NULL, 0);
    if (api_ready) ctx_api_stop();
    if (watcher_ready) ctx_watcher_shutdown();
    if (idx_started && !s_idx_joined) {
        pthread_join(s_idx_thread, NULL);
        s_idx_joined = true;
    }
    if (jobs_ready) ctx_jobs_shutdown();
    if (indexer_ready) ctx_indexer_shutdown();
    if (parser_ready) ctx_parser_shutdown();

    ctx_stats_print_summary();

    if (event_ready) ctx_event_system_shutdown();
    ctx_log_shutdown();
    return exit_code;
}

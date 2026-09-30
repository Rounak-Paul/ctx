#include "bench.h"
#include "../search/search.h"
#include "../log/log.h"

/*
 * Each case asserts that every must_have token appears in the top search
 * results (lexical ranking only: bench mode runs without models, so this is
 * the quality floor). Presence-based to stay robust as ranking evolves.
 */
typedef struct {
    const char *task;
    const char *must_have[6];
} BenchCase;

static const BenchCase k_cases[] = {
    { "where is the API status endpoint implemented",
      { "api.c", "build_status_json", NULL } },
    { "how are reference sites resolved to target symbols",
      { "graph.c", "resolve_site", NULL } },
    { "where are symbols persisted to sqlite",
      { "store.c", NULL } },
    { "how does a file change trigger an incremental reindex",
      { "on_file_change", "file_change_job_fn", NULL } },
    { "how does the extractor walk the AST",
      { "extractor.c", "walk_tree", NULL } },
    { "which callers call a symbol",
      { "ctx_nav_callers", NULL } },
    { "how does the force graph render nodes",
      { "force_graph.c", NULL } },
};

static bool contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

int ctx_bench_run(CtxGraph *g) {
    uint32_t total = (uint32_t)(sizeof(k_cases) / sizeof(k_cases[0]));
    uint32_t failed = 0;

    fprintf(stdout, "\n=== ctx search benchmark (%u cases) ===\n", total);

    for (uint32_t i = 0; i < total; i++) {
        const BenchCase *c = &k_cases[i];
        CtxSearchRequest req = { .query = c->task, .k = 8, .bodies = 0 };
        char *out = ctx_search(g, &req);

        bool ok = true;
        const char *missing = NULL;
        for (int k = 0; c->must_have[k]; k++) {
            if (!contains(out, c->must_have[k])) { ok = false; missing = c->must_have[k]; break; }
        }

        if (!ok) {
            failed++;
            fprintf(stdout, "  [FAIL] \"%s\"\n", c->task);
            fprintf(stdout, "         missing: %s\n", missing);
        } else {
            size_t out_len = out ? strlen(out) : 0;
            fprintf(stdout, "  [PASS] \"%s\" (~%zu bytes)\n", c->task, out_len);
        }
        free(out);
    }

    fprintf(stdout, "=== %u/%u passed ===\n\n", total - failed, total);
    return (int)failed;
}

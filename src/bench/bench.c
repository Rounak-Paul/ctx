#include "bench.h"
#include "../retrieve/retrieve.h"
#include "../log/log.h"

/*
 * Each case asserts that every must_have token appears somewhere in the
 * retrieval output. Assertions are presence-based to stay robust as ranking
 * evolves. No budget check — output is unbounded by design now.
 */
typedef struct {
    const char *task;
    const char *must_have[6];
} BenchCase;

static const BenchCase k_cases[] = {
    { "where is the API status endpoint implemented",
      { "api.c", "handle_stats", NULL } },
    { "how are pending semantic edges resolved",
      { "graph.c", "resolve_calls", NULL } },
    { "how is context retrieved for a task query",
      { "retrieve.c", NULL } },
    { "where are symbols persisted to the database",
      { "store.c", NULL } },
    { "how does the extractor walk the AST",
      { "extractor.c", NULL } },
    { "how does the force graph render nodes",
      { "force_graph.c", NULL } },
};

static bool contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

int ctx_bench_run(CtxGraph *g) {
    uint32_t total = (uint32_t)(sizeof(k_cases) / sizeof(k_cases[0]));
    uint32_t failed = 0;

    fprintf(stdout, "\n=== ctx retrieval benchmark (%u cases) ===\n", total);

    for (uint32_t i = 0; i < total; i++) {
        const BenchCase *c = &k_cases[i];
        CtxRetrieveRequest req = { .kind = CTX_QUERY_TASK, .text = c->task };
        char *out = ctx_retrieve(g, &req);

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

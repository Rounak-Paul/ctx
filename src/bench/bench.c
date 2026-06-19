#include "bench.h"
#include "../retrieve/retrieve.h"
#include "../log/log.h"

/*
 * Each case asserts that every `must_have` token appears somewhere in the
 * retrieval output for `task` within `budget` tokens. Assertions are on
 * presence of important files/symbols, never on exact scores or ordering, to
 * stay robust as ranking evolves.
 */
typedef struct {
    const char *task;
    uint32_t    budget;
    const char *must_have[6];   /* NULL-terminated list of required substrings */
} BenchCase;

static const BenchCase k_cases[] = {
    { "where is the API status endpoint implemented", 1500,
      { "api.c", "handle_stats", NULL } },
    { "how are pending semantic edges resolved", 1500,
      { "graph.c", "resolve_calls", NULL } },
    { "why does indexing crash on vendor files", 1800,
      { "extractor.c", NULL } },
    { "how is the context budget packed for a task", 1800,
      { "retrieve.c", NULL } },
    { "where are symbols persisted to the database", 1500,
      { "store.c", NULL } },
    { "fix graph legend colors", 1800,
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
        CtxRetrieveRequest req = {
            .kind = CTX_QUERY_TASK, .text = c->task,
            .budget = c->budget, .format = CTX_FMT_MARKDOWN
        };
        char *out = ctx_retrieve(g, &req);

        bool ok = true;
        const char *missing = NULL;
        for (int k = 0; c->must_have[k]; k++) {
            if (!contains(out, c->must_have[k])) { ok = false; missing = c->must_have[k]; break; }
        }
        size_t out_len = out ? strlen(out) : 0;
        uint32_t approx_tokens = (uint32_t)(out_len / 4);
        bool within = approx_tokens <= c->budget + c->budget / 2; /* allow framing overhead */

        if (!ok || !within) {
            failed++;
            fprintf(stdout, "  [FAIL] \"%s\"\n", c->task);
            if (!ok)     fprintf(stdout, "         missing expected: %s\n", missing);
            if (!within) fprintf(stdout, "         over budget: ~%u tokens (budget %u)\n",
                                  approx_tokens, c->budget);
        } else {
            fprintf(stdout, "  [PASS] \"%s\" (~%u tokens)\n", c->task, approx_tokens);
        }
        free(out);
    }

    fprintf(stdout, "=== %u/%u passed ===\n\n", total - failed, total);
    return (int)failed;
}

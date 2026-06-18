#include "stats.h"
#include "../log/log.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t s_query_count   = 0;
static double   s_query_ms_total = 0.0;
static uint64_t s_files_indexed = 0;
static uint64_t s_symbols_found = 0;
static double   s_index_ms_total = 0.0;

void ctx_stats_record_query(const char *endpoint, double duration_ms) {
    CTX_UNUSED(endpoint);
    pthread_mutex_lock(&s_lock);
    s_query_count++;
    s_query_ms_total += duration_ms;
    pthread_mutex_unlock(&s_lock);
}

void ctx_stats_record_index(uint32_t files, uint32_t symbols, double duration_ms) {
    pthread_mutex_lock(&s_lock);
    s_files_indexed += files;
    s_symbols_found += symbols;
    s_index_ms_total += duration_ms;
    pthread_mutex_unlock(&s_lock);
}

void ctx_stats_print_summary(void) {
    pthread_mutex_lock(&s_lock);
    CTX_LOG_INFO("=== ctx usage stats ===");
    CTX_LOG_INFO("  API queries served : %" PRIu64, s_query_count);
    if (s_query_count > 0)
        CTX_LOG_INFO("  Avg query latency  : %.1f ms", s_query_ms_total / (double)s_query_count);
    CTX_LOG_INFO("  Files indexed      : %" PRIu64, s_files_indexed);
    CTX_LOG_INFO("  Symbols found      : %" PRIu64, s_symbols_found);
    if (s_index_ms_total > 0)
        CTX_LOG_INFO("  Total index time   : %.1f ms", s_index_ms_total);
    CTX_LOG_INFO("=======================");
    pthread_mutex_unlock(&s_lock);
}

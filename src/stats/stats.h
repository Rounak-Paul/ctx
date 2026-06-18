#pragma once
#include "../pch.h"

void ctx_stats_record_query(const char *endpoint, double duration_ms);
void ctx_stats_record_index(uint32_t files, uint32_t symbols, double duration_ms);
void ctx_stats_print_summary(void);

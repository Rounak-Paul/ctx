#pragma once
#include "../pch.h"
#include "../graph/graph.h"

/*
 * Runs the built-in retrieval benchmark against the live graph: a fixed set of
 * representative tasks, each asserting that expected files/symbols appear in the
 * compact context packet. Prints a pass/fail report.
 *
 * Returns the number of failed cases (0 = all passed).
 */
int ctx_bench_run(CtxGraph *g);

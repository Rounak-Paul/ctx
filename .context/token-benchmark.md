# ctx Token-Savings Benchmark

Method: read-only coding tasks, each run by two Sonnet subagents — baseline
(Read/Grep only) vs ctx (tools over HTTP, grep fallback). Metrics from agent
transcripts: final context tokens and cumulative billed input (sum over turns).

## Round 1 (2026-09-30, packet retrieval `get_context`) — no savings
| Task | Baseline | ctx |
|---|---|---|
| T1 watcher→graph chain | 47.7k | 48.4k (+1.5%) |
| T2 add-Java plan | 53.6k | 55.8k (+4.1%) |
| T3 remove_file impact | 40.3k | 41.7k (+3.5%) |
Causes: stale ghost symbols, noisy packets, agents re-grepped everything.

## Round 2 (2026-09-30, precise tools; see `precise-tools.md`)
Fixed prompt ≈ 24.5k tokens in every run (constant, not addressable by ctx).

ctx repo (exploratory, multi-file):
| Task | final ctx base→ctx | cumulative input base→ctx |
|---|---|---|
| C1 watcher→graph chain | 48.0k → 41.7k (−13%) | 584k → 397k (−32%) |
| C2 add-Java plan | 52.9k → 49.2k (−7%) | 540k → 381k (−29%) |
| C3 replace_file impact | 39.0k → 35.5k (−9%) | 322k → 209k (−35%) |

llama.cpp (628k LOC; tasks name exact identifiers):
| Task | final ctx base→ctx | cumulative input base→ctx |
|---|---|---|
| L1 n_gpu_layers placement | 33.9k → 40.9k (+21%) | 181k → 272k (+50%) |
| L2 llama_batch_init callers | 32.4k → 33.7k (+4%) | 172k → 183k (+6%) |
| L3 embeddings HTTP path | 34.3k → 33.6k (−2%) | 214k → 178k (−17%) |
| L4 flash-attn decision | 35.3k → 42.8k (+21%) | 155k → 329k (+113%) |

Quality: equal on all tasks except L2, where ctx missed Swift callers (Swift
not indexed; baseline grep found them).

Findings:
- ctx saves ~30% cumulative input on exploratory/conceptual tasks.
- When the task already names an exact identifier, one grep is near-optimal;
  ctx lost by over-fetching (search bodies, 200-line source ranges).
- Follow-ups applied (not yet re-measured): search bodies 2→1, excerpt 30→20
  lines, source default 200→120 lines, policy lines "exact identifier → call
  source/callers/impact directly" and "outline first, then narrow lines".
- Bug found by an agent and fixed: C++ in-class methods were named after their
  first parameter (declarator field now used; `Scope::name` qualifier kept).

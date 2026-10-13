# Experiment 4 — compute-tail swap causal validation

CCT is the maximum GOAL rank finish. Each DAG uses its own all-minimal CCT reference. The four interventions each detour exactly one flow; the two references only establish baseline CCT and slack.

## Four comparisons

| DAG context | A detour, B minimal: CCT / ΔCCT (µs) | B detour, A minimal: CCT / ΔCCT (µs) | Better detour |
| --- | --- | --- | --- |
| DAG-1: A critical | 35.023 / +0.501 (Misaligned) | 33.939 / -0.583 (Slack-aligned) | B |
| DAG-2: B critical | 33.939 / -0.666 (Slack-aligned) | 35.023 / +0.418 (Misaligned) | A |

## All-minimal references and oracle slack

| DAG | Reference CCT (µs) | A slack (µs) | B slack (µs) |
| --- | ---: | ---: | ---: |
| DAG-1 | 34.522 | 0.000 | 14.917 |
| DAG-2 | 34.605 | 15.083 | 0.000 |

## Network tradeoff

FCT uses HTSim's fractional-ns sender completion; ΔFCT is relative to the same DAG's all-minimal run.

| Case | Detour | A FCT (µs) | ΔA FCT (µs) | B FCT (µs) | ΔB FCT (µs) |
| --- | --- | ---: | ---: | ---: | ---: |
| exp4.ref1 | reference | 4.51824 | +0.00000 | 4.60144 | +0.00000 |
| exp4.ref2 | reference | 4.51824 | +0.00000 | 4.60144 | +0.00000 |
| exp4.1 | A | 5.01904 | +0.50080 | 3.93584 | -0.66560 |
| exp4.2 | B | 3.93584 | -0.58240 | 5.01904 | +0.41760 |
| exp4.3 | A | 5.01904 | +0.50080 | 3.93584 | -0.66560 |
| exp4.4 | B | 3.93584 | -0.58240 | 5.01904 | +0.41760 |

## Fixed-routing network invariance

PASS: each cross-DAG pair has identical assigned/runtime routes, every flowsInfo.csv field, send/recv lifecycle records, and the ordered PacketHop trace (no sorting or timestamp tolerance). Traces include DATA and control packets, picosecond time, packet/flow ID, type/size, endpoints, ECN flags, trim/bounce status, next sink and its queue occupancy before receive.

| Fixed assignment | Matched hop events | Trace SHA-256 |
| --- | ---: | --- |
| all-minimal | 198 | `d76b9af910b6b6b9ca2277c2093bb14ba1a145710b052000a92a6fe2338d2cbc` |
| A detour | 222 | `a86f2e251cddff32e37f0eaae2557678059905e6965d4d567100c14c3e121449` |
| B detour | 222 | `7c027a7a8dafd33537d06615a5b69d08cb290d473dbde8c4c7edc301a9cf250d` |

PASS: same simulator binary hash, topology, placement, messagelet configuration and network settings across six runs. Templates and compiled GOAL dependencies/identities match; only the two receiver compute durations swap. Exactly two simultaneous 32 KiB flows, one messagelet each; no background traffic or artificial communication delay.

## Three success criteria

| Evidence | Result |
| --- | --- |
| network_tradeoff | PASS |
| slack_aligned_lower_cct | PASS |
| decision_reversal | PASS |

With network observations unchanged for each fixed route assignment, changing only DAG criticality reverses the CCT-preferred detour: B in DAG-1, A in DAG-2. A policy using only these unchanged network inputs cannot distinguish the two contexts. This demonstrates the value of execution information for this controlled decision.

These are deterministic runs with the simulator's fixed seed 13. This experiment compares two predefined allocations; it does not benchmark an implemented congestion-only policy or establish general policy performance. GOAL completion uses integer ns (fractional network completion is truncated); compute anchors and the local sink add 2 ns after receive. The sink is rank-local; application completion is the maximum rank finish, as in exp1.

## Artifact provenance

- exp4.ref1: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref1/artifacts/20261013-161206`
- exp4.ref2: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref2/artifacts/20261013-161206`
- exp4.1: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.1/artifacts/20261013-161207`
- exp4.2: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.2/artifacts/20261013-161207`
- exp4.3: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.3/artifacts/20261013-161207`
- exp4.4: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.4/artifacts/20261013-161207`

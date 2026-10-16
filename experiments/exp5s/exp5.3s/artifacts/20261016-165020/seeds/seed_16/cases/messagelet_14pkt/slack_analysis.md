# Offline oracle-slack analysis: exp2a1_collective_internal_slack

- Input artifact: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp5s/exp5.3s/artifacts/20261016-165020/seeds/seed_16/cases/messagelet_14pkt`
- Observed `C_ref`: **48145 ns**
- Ready-window width: **100 ns** (fixed half-open bins)
- Mapped logical flows: **60/60**
- Authoritative GOAL task DAG: **200 nodes, 336 edges**
- Flow-collapsed DAG: **140 nodes, 330 edges**

## Method and communication-collapse rule

Each metadata flow replaces its matched send/recv GOAL pair with one logical node. Incoming and outgoing GOAL edges from both endpoints are retained and deduplicated; calc nodes remain intact.

`ready_ns` and `issue_ns` are the matched **send task's** dependency-ready and issue times: these represent when the message data became eligible and when it was actually launched. `finish_ns` is the matched **recv task's** finish time, because receiver completion is the semantic completion point for downstream dependencies. In this artifact every matched send/recv pair has the same finish time.

For an observed-time backward path, every collapsed-DAG edge is weighted by the increase in observed finish time. Thus `remaining_critical_ns` is the largest elapsed observed time from flow completion to a reachable terminal. The edge weights telescope; overlapping send/recv lifetimes are not counted twice. This is an offline property of the recorded execution, not a counterfactual duration model.

## Validation

- PASS — all 60 metadata flows map to valid, unique send/recv GOAL tasks.
- PASS — lifecycle records cover all 200 GOAL tasks and all mapped endpoints.
- PASS — authoritative GOAL DAG and 140-node collapsed DAG are acyclic.
- PASS — all projected dependency edges are nondecreasing in observed finish time.
- PASS — every backward result reaches a valid terminal; the global horizon is reached.
- PASS — no negative oracle slack was computed.
- PASS — lifecycle maximum, `Htsim time`, and application graph makespan all equal 48145 ns.
- PASS — CSV identities are logical GOAL ranks 0–11; physical topology host IDs are never used.

## Slack results

Overall oracle slack (min / median / max): **0 / 6154 / 7863 ns**.

| Algorithm | Flows | Min slack (ns) | Median slack (ns) | Max slack (ns) |
| --- | ---: | ---: | ---: | ---: |
| direct | 12 | 6154 | 6154 | 6154 |
| ring | 24 | 6834 | 6834 | 7863 |
| tree | 24 | 0 | 0 | 5685 |

## Ready windows with the largest slack spread

| Ready window (ns) | Flows | Collectives | Min slack | Max slack | Spread |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0–100 | 13 | 3 | 0 | 6834 | 6834 |
| 14200–14300 | 3 | 2 | 0 | 6154 | 6154 |
| 16600–16700 | 2 | 1 | 0 | 5685 | 5685 |
| 17400–17500 | 2 | 1 | 0 | 5685 | 5685 |
| 21200–21300 | 2 | 1 | 0 | 5685 | 5685 |
| 22000–22100 | 2 | 1 | 0 | 5685 | 5685 |
| 28300–28400 | 3 | 2 | 6154 | 6834 | 680 |

## Representative same-window flows

Ready window: **0–100 ns**

| Flow | Algorithm | Phase | Ready | Issue | Slack (ns) |
| ---: | --- | --- | ---: | ---: | ---: |
| 0 | ring | reduce_scatter | 2 | 2 | 6834 |
| 1 | ring | reduce_scatter | 2 | 2 | 6834 |
| 2 | ring | reduce_scatter | 2 | 2 | 6834 |
| 3 | ring | reduce_scatter | 2 | 2 | 6834 |
| 26 | tree | reduce | 2 | 5660 | 0 |
| 28 | tree | reduce | 2 | 5660 | 0 |
| 32 | tree | reduce | 2 | 2831 | 0 |
| 34 | tree | reduce | 2 | 8489 | 0 |
| 38 | tree | reduce | 2 | 8489 | 0 |
| 40 | tree | reduce | 2 | 2831 | 0 |
| 44 | tree | reduce | 2 | 2 | 0 |
| 46 | tree | reduce | 2 | 2 | 0 |
| 48 | direct | exchange | 2 | 2 | 6154 |

Measured slack spread in this window: **6834 ns**. This table reports the observation without assigning causal or statistical significance.

## Sufficiency and limitation

The artifact is sufficient for this offline oracle-slack measurement: it contains the authoritative GOAL dependencies, unique flow-to-task mappings, complete lifecycle timestamps, and an independently logged application makespan. No additional simulator instrumentation is required for the reported metric.

The remaining-path value is an **observed-time** quantity. Reinterpreting it as a sum of isolated node service durations would require an additional causal/resource model and can double-count overlapping communication lifetimes after send/recv collapse; that alternative is intentionally not claimed here.

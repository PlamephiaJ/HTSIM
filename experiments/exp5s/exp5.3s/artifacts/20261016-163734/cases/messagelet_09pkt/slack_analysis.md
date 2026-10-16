# Offline oracle-slack analysis: exp2a1_collective_internal_slack

- Input artifact: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp5s/exp5.3s/artifacts/20261016-163734/cases/messagelet_09pkt`
- Observed `C_ref`: **44212 ns**
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
- PASS — lifecycle maximum, `Htsim time`, and application graph makespan all equal 44212 ns.
- PASS — CSV identities are logical GOAL ranks 0–11; physical topology host IDs are never used.

## Slack results

Overall oracle slack (min / median / max): **0 / 3384 / 6855 ns**.

| Algorithm | Flows | Min slack (ns) | Median slack (ns) | Max slack (ns) |
| --- | ---: | ---: | ---: | ---: |
| direct | 12 | 4134 | 4134 | 4134 |
| ring | 24 | 3384 | 3384 | 6855 |
| tree | 24 | 0 | 0 | 5685 |

## Ready windows with the largest slack spread

| Ready window (ns) | Flows | Collectives | Min slack | Max slack | Spread |
| --- | ---: | ---: | ---: | ---: | ---: |
| 13800–13900 | 2 | 1 | 0 | 5685 | 5685 |
| 16500–16600 | 2 | 1 | 0 | 5685 | 5685 |
| 19300–19400 | 2 | 1 | 0 | 5685 | 5685 |
| 20600–20700 | 2 | 1 | 0 | 5685 | 5685 |
| 0–100 | 13 | 3 | 0 | 4134 | 4134 |
| 7300–7400 | 2 | 2 | 0 | 3384 | 3384 |
| 13400–13500 | 2 | 1 | 4134 | 4134 | 0 |
| 26200–26300 | 2 | 1 | 4134 | 4134 | 0 |

## Representative same-window flows

Ready window: **13800–13900 ns**

| Flow | Algorithm | Phase | Ready | Issue | Slack (ns) |
| ---: | --- | --- | ---: | ---: | ---: |
| 43 | tree | broadcast | 13830 | 13830 | 0 |
| 45 | tree | broadcast | 13830 | 16659 | 5685 |

Measured slack spread in this window: **5685 ns**. This table reports the observation without assigning causal or statistical significance.

## Sufficiency and limitation

The artifact is sufficient for this offline oracle-slack measurement: it contains the authoritative GOAL dependencies, unique flow-to-task mappings, complete lifecycle timestamps, and an independently logged application makespan. No additional simulator instrumentation is required for the reported metric.

The remaining-path value is an **observed-time** quantity. Reinterpreting it as a sum of isolated node service durations would require an additional causal/resource model and can double-count overlapping communication lifetimes after send/recv collapse; that alternative is intentionally not claimed here.

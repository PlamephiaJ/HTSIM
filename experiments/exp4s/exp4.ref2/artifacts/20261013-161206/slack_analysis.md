# Offline oracle-slack analysis: exp4.dag2

- Input artifact: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref2/artifacts/20261013-161206`
- Observed `C_ref`: **34605 ns**
- Ready-window width: **100 ns** (fixed half-open bins)
- Mapped logical flows: **2/2**
- Authoritative GOAL task DAG: **20 nodes, 16 edges**
- Flow-collapsed DAG: **18 nodes, 16 edges**

## Method and communication-collapse rule

Each metadata flow replaces its matched send/recv GOAL pair with one logical node. Incoming and outgoing GOAL edges from both endpoints are retained and deduplicated; calc nodes remain intact.

`ready_ns` and `issue_ns` are the matched **send task's** dependency-ready and issue times: these represent when the message data became eligible and when it was actually launched. `finish_ns` is the matched **recv task's** finish time, because receiver completion is the semantic completion point for downstream dependencies. In this artifact every matched send/recv pair has the same finish time.

For an observed-time backward path, every collapsed-DAG edge is weighted by the increase in observed finish time. Thus `remaining_critical_ns` is the largest elapsed observed time from flow completion to a reachable terminal. The edge weights telescope; overlapping send/recv lifetimes are not counted twice. This is an offline property of the recorded execution, not a counterfactual duration model.

## Validation

- PASS — all 2 metadata flows map to valid, unique send/recv GOAL tasks.
- PASS — lifecycle records cover all 20 GOAL tasks and all mapped endpoints.
- PASS — authoritative GOAL DAG and 18-node collapsed DAG are acyclic.
- PASS — all projected dependency edges are nondecreasing in observed finish time.
- PASS — every backward result reaches a valid terminal; the global horizon is reached.
- PASS — no negative oracle slack was computed.
- PASS — lifecycle maximum, `Htsim time`, and application graph makespan all equal 34605 ns.
- PASS — CSV identities are logical GOAL ranks 0–3; physical topology host IDs are never used.

## Slack results

Overall oracle slack (min / median / max): **0 / 7541.5 / 15083 ns**.

| Algorithm | Flows | Min slack (ns) | Median slack (ns) | Max slack (ns) |
| --- | ---: | ---: | ---: | ---: |
| unknown | 2 | 0 | 7541.5 | 15083 |

## Ready windows with the largest slack spread

| Ready window (ns) | Flows | Collectives | Min slack | Max slack | Spread |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0–100 | 2 | 1 | 0 | 15083 | 15083 |

## Representative same-window flows

Ready window: **0–100 ns**

| Flow | Algorithm | Phase | Ready | Issue | Slack (ns) |
| ---: | --- | --- | ---: | ---: | ---: |
| 0 | unknown |  | 2 | 2 | 15083 |
| 1 | unknown |  | 2 | 2 | 0 |

Measured slack spread in this window: **15083 ns**. This table reports the observation without assigning causal or statistical significance.

## Sufficiency and limitation

The artifact is sufficient for this offline oracle-slack measurement: it contains the authoritative GOAL dependencies, unique flow-to-task mappings, complete lifecycle timestamps, and an independently logged application makespan. No additional simulator instrumentation is required for the reported metric.

The remaining-path value is an **observed-time** quantity. Reinterpreting it as a sum of isolated node service durations would require an additional causal/resource model and can double-count overlapping communication lifetimes after send/recv collapse; that alternative is intentionally not claimed here.

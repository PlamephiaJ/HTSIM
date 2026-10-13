# exp4.ref1: DAG-1: all-minimal reference — 20261013-161206

**Status:** SUCCESS (exit code `0`)

## Application result

- Computation-graph makespan: **34522 ns (34.522 µs)**
- Critical GOAL rank: **2**
- Completed GOAL ranks: **4**

| GOAL rank | Graph finish (ns) | Graph finish (µs) |
| ---: | ---: | ---: |
| 0 | 4521 | 4.521 |
| 1 | 4604 | 4.604 |
| 2 **(critical)** | 34522 | 34.522 |
| 3 | 19605 | 19.605 |

## GOAL DAG lifecycle

- Recorded nodes: **20**
- Completed nodes: **20/20**
- Operations: **calc=16, recv=2, send=2**
- Ready-to-issue wait min / average / max: **0 / 0 / 0 ns**
- Issue-to-finish duration min / average / max: **1 / 3162.6 / 30000 ns**

| Rank | Node offset | Op | Peer | Tag | Size | Ready (ns) | Issue (ns) | Finish (ns) |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | calc | 0 | 0 | 0 | 0 | 0 | 1 |
| 0 | 1 | calc | 0 | 0 | 0 | 1 | 1 | 2 |
| 0 | 2 | calc | 0 | 0 | 0 | 4520 | 4520 | 4521 |
| 0 | 3 | send | 2 | 101 | 32768 | 2 | 2 | 4520 |
| 1 | 0 | calc | 0 | 0 | 0 | 0 | 0 | 1 |
| 1 | 1 | calc | 0 | 0 | 0 | 1 | 1 | 2 |
| 1 | 2 | calc | 0 | 0 | 0 | 4603 | 4603 | 4604 |
| 1 | 3 | send | 3 | 201 | 32768 | 2 | 2 | 4603 |
| 2 | 0 | calc | 0 | 0 | 0 | 0 | 0 | 1 |
| 2 | 1 | calc | 0 | 0 | 0 | 1 | 1 | 2 |
| 2 | 2 | calc | 0 | 0 | 0 | 4520 | 4520 | 4521 |
| 2 | 3 | recv | 0 | 101 | 32768 | 2 | 2 | 4520 |
| 2 | 4 | calc | 0 | 0 | 30000 | 4521 | 4521 | 34521 |
| 2 | 5 | calc | 0 | 0 | 0 | 34521 | 34521 | 34522 |
| 3 | 0 | calc | 0 | 0 | 0 | 0 | 0 | 1 |
| 3 | 1 | calc | 0 | 0 | 0 | 1 | 1 | 2 |
| 3 | 2 | calc | 0 | 0 | 0 | 4603 | 4603 | 4604 |
| 3 | 3 | recv | 1 | 201 | 32768 | 2 | 2 | 4603 |
| 3 | 4 | calc | 0 | 0 | 15000 | 4604 | 4604 | 19604 |
| 3 | 5 | calc | 0 | 0 | 0 | 19604 | 19604 | 19605 |

## Configuration

- Started: `2026-10-13T16:12:06-07:00`
- Finished: `2026-10-13T16:12:06-07:00`
- Routing: `SOURCE`
- Workload: `workload/exp4s/exp4.dag1/workload.bin`
- Topology: `${SCRIPT_DIR}/../topology/p3a1h2`
- Sender CC: `nscc`
- Multipath algorithm: `mixed`
- Path entropy: `64`
- Initial cwnd: `0 bytes` (`0` means simulator default)
- Queue: `88` packets
- Rank placement snapshot: `config_snapshot/rank_placement.json`
- Messagelet config: `${SCRIPT_DIR}/../messagelet_configs/default.json`
- Flow routing config: `${SCRIPT_DIR}/routing_configs/default.json`

## Rank placement

Derived from the snapshotted `config_snapshot/rank_placement.json`; no descriptive placement string from `config.env` is used.

- Group 0, switch 0: ranks 0, 1
- Group 1, switch 0: ranks 2, 3

| GOAL rank | Group | Switch | Host | Physical HTSIM host |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 1 | 0 | 0 | 1 | 1 |
| 2 | 1 | 0 | 0 | 3 |
| 3 | 1 | 0 | 1 | 4 |

## Network

- Flow pattern: `A: 0->2, B: 1->3; two simultaneous 32 KiB flows with receiver compute tails`
- Link speed: `400.000000 Gbps`
- Link delays (global/local/host): `500/25/25 ns`
- Packet size: `4160 bytes`
- Queue size: `366080 bytes`
- ECN thresholds (low/high): `70720` / `291200 bytes`
- SACK threshold: `16384 bytes`

## Flow results

The raw `flowsInfo.csv` `srcNode`/`dstNode` values are **physical HTSIM host IDs after rank placement**.
The GOAL rank endpoints below are reverse-mapped from the snapshotted `rank_placement.json` and Dragonfly topology.

| GOAL rank flow | Physical HTSIM host flow | Size (bytes) | Start (ns) | End (ns) | FCT (ns) | Base RTT (ns) | Packets |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0→2 | 0→3 | 32768 | 2.000000 | 4520.240000 | 4518.240 | 3353.440000 | 8 |
| 1→3 | 1→4 | 32768 | 2.000000 | 4603.440000 | 4601.440 | 3436.640000 | 8 |

- Completed flows: **2**
- Total communicated data: **65536 bytes (64 KiB)**
- Total data packets: **16**
- Completion order (GOAL rank flows): **0→2 → 1→3**
- FCT min / average / max: **4518.240 / 4559.840 / 4601.440 ns**

## Reproducibility

- Git revision: `9e599aff08b22662af83668e923e13f2bb342111`
- Worktree at launch: `dirty`
- Exact command:

```bash
/home/yuhao/workspace/slack_ws/HTSIM/htsim/sim/build/datacenter/htsim_uec_df -basepath /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref1/artifacts/20261013-161206/config_snapshot/topology -goal /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref1/artifacts/20261013-161206/config_snapshot/workload.bin -routing SOURCE -sender_cc_algo nscc -load_balancing_algo mixed -paths 64 -q 88 -cwnd 0 -rank_placement /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref1/artifacts/20261013-161206/config_snapshot/rank_placement.json -messagelet_config /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref1/artifacts/20261013-161206/config_snapshot/messagelet_config.json -routing_config /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp4s/exp4.ref1/artifacts/20261013-161206/config_snapshot/routing_config.json
```

<details><summary>Worktree changes at launch</summary>

```text
M htsim/sim/network.cpp
?? experiments/common/__pycache__/
?? experiments/exp4s/
?? workload/exp4s/
```

</details>

## Files

- Full simulator output: `simulator.log`
- Per-flow CSV: `output_metrics/flowsInfo.csv`
- GOAL DAG lifecycle CSV: `output_metrics/flow_dag_info.csv`
- Global CSV: `output_metrics/globalInfo.csv`
- Exact input snapshot: `config_snapshot/`
- Resolved command: `command.txt`

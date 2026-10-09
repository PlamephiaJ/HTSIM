# exp3.a: Experiment 3 baseline with concurrent collectives and no injected delay — 20261009-165219

**Status:** FAILED (exit code `139`)

## Application result

- Computation-graph makespan: **not available**
- Completed GOAL ranks: **0**

## GOAL DAG lifecycle

No GOAL DAG lifecycle records were produced. See `output_metrics/flow_dag_info.csv` and `simulator.log`.

## Configuration

- Started: `2026-10-09T16:52:19-07:00`
- Finished: `2026-10-09T16:52:19-07:00`
- Routing: `SOURCE`
- Workload: `workload/exp3s/exp3.a/workload.bin`
- Topology: `${SCRIPT_DIR}/topology/p6a1h2`
- Sender CC: `nscc`
- Multipath algorithm: `mixed`
- Path entropy: `64`
- Initial cwnd: `0 bytes` (`0` means simulator default)
- Queue: `88` packets
- Rank placement snapshot: `config_snapshot/rank_placement.json`
- Messagelet config: `${SCRIPT_DIR}/messagelet_configs/default.json`
- Flow routing config: `${SCRIPT_DIR}/routing_configs/default.json`

## Rank placement

Derived from the snapshotted `config_snapshot/rank_placement.json`; no descriptive placement string from `config.env` is used.

- Group 0, switch 0: ranks 0, 2, 4, 7, 8, 10
- Group 1, switch 0: ranks 1, 3, 5, 6, 9, 11

| GOAL rank | Group | Switch | Host | Physical HTSIM host |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 1 | 1 | 0 | 0 | 6 |
| 2 | 0 | 0 | 1 | 1 |
| 3 | 1 | 0 | 1 | 7 |
| 4 | 0 | 0 | 2 | 2 |
| 5 | 1 | 0 | 2 | 8 |
| 6 | 1 | 0 | 3 | 9 |
| 7 | 0 | 0 | 3 | 3 |
| 8 | 0 | 0 | 4 | 4 |
| 9 | 1 | 0 | 4 | 10 |
| 10 | 0 | 0 | 5 | 5 |
| 11 | 1 | 0 | 5 | 11 |

## Network

- Flow pattern: `ranks 0-3 ring all-reduce; ranks 4-7 tree all-reduce; ranks 8-11 direct all-to-all`

## Flow results

The raw `flowsInfo.csv` `srcNode`/`dstNode` values are **physical HTSIM host IDs after rank placement**.
The GOAL rank endpoints below are reverse-mapped from the snapshotted `rank_placement.json` and Dragonfly topology.

No completed-flow CSV records were produced. See `simulator.log`.

## Reproducibility

- Git revision: `cc2873ee3ed0fb26e6cc4c4a9d1e55cb977d4ca7`
- Worktree at launch: `dirty`
- Exact command:

```bash
/home/yuhao/workspace/slack_ws/HTSIM/htsim/sim/build/datacenter/htsim_uec_df -basepath /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a/artifacts/20261009-165219/config_snapshot/topology -goal /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a/artifacts/20261009-165219/config_snapshot/workload.bin -routing SOURCE -sender_cc_algo nscc -load_balancing_algo mixed -paths 64 -q 88 -cwnd 0 -rank_placement /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a/artifacts/20261009-165219/config_snapshot/rank_placement.json -messagelet_config /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a/artifacts/20261009-165219/config_snapshot/messagelet_config.json -routing_config /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a/artifacts/20261009-165219/config_snapshot/routing_config.json
```

<details><summary>Worktree changes at launch</summary>

```text
M experiments/common/summarize.py
 M experiments/exp1s/exp1.1/config.env
 M experiments/exp1s/exp1.1/run.sh
 M experiments/exp1s/exp1.2/config.env
 M experiments/exp1s/exp1.2/run.sh
 M experiments/exp1s/exp1.3/config.env
 M experiments/exp1s/exp1.3/run.sh
 M experiments/exp1s/exp1.4/config.env
 M experiments/exp1s/exp1.4/run.sh
 M experiments/exp1s/exp1.5/config.env
 M experiments/exp1s/exp1.5/run.sh
 M experiments/exp1s/exp1/config.env
 M experiments/exp1s/exp1/run.sh
 M experiments/exp2s/exp2.a.1/README.md
 M experiments/exp2s/exp2.a.1/config.env
 M experiments/exp2s/exp2.a.1/run.sh
 M experiments/exp2s/exp2.a.2/README.md
 M experiments/exp2s/exp2.a.2/config.env
 M experiments/exp2s/exp2.a.2/run.sh
 M experiments/exp2s/exp2.c.1/README.md
 M experiments/exp2s/exp2.c.1/config.env
 M experiments/exp2s/exp2.c.1/run.sh
 M experiments/exp2s/exp2.c.2/README.md
 M experiments/exp2s/exp2.c.2/config.env
 M experiments/exp2s/exp2.c.2/run.sh
 M experiments/exp2s/exp2.c.3/README.md
 M experiments/exp2s/exp2.c.3/config.env
 M experiments/exp2s/exp2.c.3/run.sh
 M experiments/exp2s/exp2.c.4/README.md
 M experiments/exp2s/exp2.c.4/config.env
 M experiments/exp2s/exp2.c.4/run.sh
 M htsim/sim/CMakeLists.txt
 M htsim/sim/datacenter/dragonfly_switch.cpp
 M htsim/sim/datacenter/main_uec_df.cpp
 M htsim/sim/messagelet_runtime.cpp
 M htsim/sim/messagelet_runtime.h
 M htsim/sim/network.h
 M htsim/sim/uec.h
?? experiments/common/README.md
?? experiments/common/__pycache__/
?? experiments/common/generate_routing_config.py
?? experiments/common/test_routing_config.py
?? experiments/exp2s/exp2.a.1/routing_configs/
?? experiments/exp2s/exp2.a.2/routing_configs/
?? experiments/exp2s/exp2.c.1/routing_configs/
?? experiments/exp2s/exp2.c.2/routing_configs/
?? experiments/exp2s/exp2.c.3/routing_configs/
?? experiments/exp2s/exp2.c.4/routing_configs/
?? experiments/exp3s/
?? htsim/sim/dragonfly_source_route.h
?? htsim/sim/flow_routing_config.cpp
?? htsim/sim/flow_routing_config.h
?? workload/exp3s/
```

</details>

## Files

- Full simulator output: `simulator.log`
- Per-flow CSV: `output_metrics/flowsInfo.csv`
- GOAL DAG lifecycle CSV: `output_metrics/flow_dag_info.csv`
- Global CSV: `output_metrics/globalInfo.csv`
- Exact input snapshot: `config_snapshot/`
- Resolved command: `command.txt`

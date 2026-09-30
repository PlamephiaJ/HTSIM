# exp1.5: Dragonfly minimal-path contention with DAG level delay. Slack 35 us. — 20260929-173906

**Status:** SUCCESS (exit code `0`)

## Application result

- Computation-graph makespan: **42185 ns (42.185 µs)**
- Critical rank: **5**
- Completed ranks: **6**

| Rank | Graph finish (ns) | Graph finish (µs) |
| ---: | ---: | ---: |
| 0 | 7100 | 7.100 |
| 1 | 7266 | 7.266 |
| 2 | 7183 | 7.183 |
| 3 | 37101 | 37.101 |
| 4 | 22267 | 22.267 |
| 5 **(critical)** | 42185 | 42.185 |

## Configuration

- Started: `2026-09-29T17:39:06-07:00`
- Finished: `2026-09-29T17:39:06-07:00`
- Routing: `MINIMAL`
- Workload: `workload/exp1_5_delay_slack_35us/workload.bin`
- Topology: `${SCRIPT_DIR}/topology/p3a1h2`
- Sender CC: `nscc`
- Multipath algorithm: `mixed`
- Path entropy: `64`
- Initial cwnd: `0 bytes` (`0` means simulator default)
- Queue: `88` packets

## Network

- Rank placement: `ranks 0,1,2 -> group 0; ranks 3,4,5 -> group 1; group 2 -> two-hop alternative`
- Flow pattern: `0->3, 1->4, 2->5 (concurrent)`
- Link speed: `400.000000 Gbps`
- Link delays (global/local/host): `500/25/25 ns`
- Packet size: `4160 bytes`
- Queue size: `366080 bytes`
- ECN thresholds (low/high): `70720` / `291200 bytes`
- SACK threshold: `16384 bytes`

## Flow results

| Flow | Size (bytes) | Start (ns) | End (ns) | FCT (ns) | Base RTT (ns) | Packets |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0→3 | 65536 | 2.000000 | 7099.440000 | 7097.440 | 3353.440000 | 16 |
| 2→5 | 65536 | 2.000000 | 7182.640000 | 7180.640 | 3436.640000 | 16 |
| 1→4 | 65536 | 2.000000 | 7265.840000 | 7263.840 | 3519.840000 | 16 |

- Completed flows: **3**
- Total communicated data: **196608 bytes (192 KiB)**
- Total data packets: **48**
- Completion order: **0→3 → 2→5 → 1→4**
- FCT min / average / max: **7097.440 / 7180.640 / 7263.840 ns**

## Reproducibility

- Git revision: `0311b25667b41588dca6f8d8b5a3d800d4ce42ec`
- Worktree at launch: `dirty`
- Exact command:

```bash
/home/yuhao/workspace/slack_ws/HTSIM/htsim/sim/build/datacenter/htsim_uec_df -basepath /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp1.5/artifacts/20260929-173906/config_snapshot/topology -goal /home/yuhao/workspace/slack_ws/HTSIM/experiments/exp1.5/artifacts/20260929-173906/config_snapshot/workload.bin -routing MINIMAL -sender_cc_algo nscc -load_balancing_algo mixed -paths 64 -q 88 -cwnd 0
```

<details><summary>Worktree changes at launch</summary>

```text
A  .gitmodules
 M experiments/exp1/README.md
 M experiments/exp1/config.env
 M experiments/exp1/run.sh
 D experiments/exp1/summarize.py
A? workload_gen
?? experiments/common/
?? experiments/exp1.1/
?? experiments/exp1.2/
?? experiments/exp1.3/
?? experiments/exp1.4/
?? experiments/exp1.5/
?? workload/exp1.1/
?? workload/exp1.2/
?? workload/exp1.3/
?? workload/exp1_4_delay_medium_20us/
?? workload/exp1_5_delay_slack_35us/
```

</details>

## Files

- Full simulator output: `simulator.log`
- Per-flow CSV: `output_metrics/flowsInfo.csv`
- Global CSV: `output_metrics/globalInfo.csv`
- Exact input snapshot: `config_snapshot/`
- Resolved command: `command.txt`

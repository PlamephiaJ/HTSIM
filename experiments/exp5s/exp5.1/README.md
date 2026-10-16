# Experiment 5.1 — UGAL-L messagelet load balancing baseline

复用 `exp2.a.2` 的原始计算图、Dragonfly 拓扑、rank placement 和网络参数。
每个 parent flow 是一个 messagelet，在首次打标前查询 UGAL-L，随后全部包及重传
沿用该标签。策略只观察网络负载；slack 在仿真后离线分析，不参与选路。

## 输入和配置

- Workload: `workload/exp2s/exp2a1_collective_internal_slack/workload.bin`，直接复用，
  不重新生成或修改 DAG。共 12 ranks、60 条 64 KiB flow、200 个 GOAL task。
- ranks 0–3 为 ring all-reduce，4–7 为 tree all-reduce，8–11 为 direct all-to-all。
- 拓扑和 placement 是 exp2.a.2 的本地副本：`p6a1h2`，三个 group，各有一个 switch
  和六个 host。group 0、1 承载 ranks，group 2 可供绕路。
- 保持 400 Gbps、global/local/host latency = 500/25/25 ns、switch latency = 500 ns、
  88-packet queue、`nscc`、`mixed`、64 path entropy、默认 cwnd。
- `runtime.granularity="flow"`、`messagelet.size_bytes=0`：每个 flow 一个 messagelet。
- `routing.network_decision="ugal_l"`，`logging.trace_decisions=true`。
- `ROUTING=SOURCE` 执行固定标签；`ROUTING_CONFIG` 为空。
  `LOAD_BALANCING_ALGO=mixed` 保留原 transport 配置，messagelet 标签由 runtime 的
  `NetworkDecision` 决定。

粒度是 runtime 的独立配置：后续实验可设为 `messagelet` 并指定正的 `size_bytes`，
或设为 `packet` 并保持 `size_bytes=0`。本 baseline 固定为 `flow`；首个历史 artifact
尚未显式写出 `runtime.granularity`，按兼容规则同样解析为 `flow`。

UGAL-L 直接调用 HTSim 的共享 `DragonflySwitch::ugalDecision()`：沿用候选采样、
per-switch seed 33、跳数计算和本地出口队列字节数。仅当
`q_min * h_min > q_val * h_val` 时选择 Valiant，代价相等时选择 minimum。
两个候选的第一跳相同则直接使用 minimum；同 switch 的通信本地交付。
选中的路径在 messagelet 内固定，不在后续 packet hop 重新调用 UGAL-L。

## 运行

从仓库任意目录调用：

```bash
./experiments/exp5s/exp5.1/run.sh
```

入口默认以 4 个构建任务增量编译 simulator，随后仿真、生成通用 summary、运行共享
slack analyzer，并验证、汇总 UGAL-L 决策。时间戳使用 America/Vancouver 时区。
配置入口为 `config.env`；可将另一份配置文件作为 `run.sh` 的第一个参数。

每次运行写入独立的 `artifacts/YYYYMMDD-HHMMSS/`；`artifacts/latest` 指向最近一次
运行。主要产物：

- `config_snapshot/`：配置、workload binary/GOAL/metadata、拓扑、placement 和分析脚本。
- `command.txt`、`binary-sha256.txt`、Git revision/status：实际命令与二进制身份。
- `simulator.patch`、`config_snapshot/added_sources/`：未提交的 simulator 修改与新增源码，
  记录当前 network decision 实现。
- `simulator.log`：原始决策及完成日志。
- `summary.md`、`slack_analysis.md`：应用完成时间、flow/lifecycle 汇总和离线 slack。
- `output_metrics/network_decisions.csv`：metadata flow ID、GOAL send task ID、transport
  flow ID、物理端点、路径标签、候选队列/跳数/代价和 FCT。
- `network_decision_analysis.md`、`network_decision_validation.json`：决策统计与输入校验。
- 既有 `flowsInfo.csv`、`flow_dag_info.csv`、`globalInfo.csv` 及 slack CSV。

同一 `(src, dst, tag)` 可以对应多个 GOAL send；决策日志额外记录 `SendTaskId` 和
`TransportFlowId`，分析时用 `(source rank, send task ID)` 匹配 metadata，避免按 tag
或 transport 分配顺序猜测 flow ID。动态策略不产生静态 routing config 的 `flowRouting.csv`。

单独重做决策分析：

```bash
python3 experiments/exp5s/exp5.1/analyze_routing.py \
  experiments/exp5s/exp5.1/artifacts/latest \
  --reference experiments/exp2s/exp2.a.2
```

分析器检查所有输入的 SHA-256 与 exp2.a.2 一致、CC/queue/path 配置一致、全部 60 个
metadata send 各有一次 index-0 决策、决策满足 UGAL-L 比较规则、所有 transport flow
和 GOAL task 完成，以及 GOAL rank finish 与 lifecycle 的 CCT 一致。

## 实测结果

首个 artifact：[`20261016-160032`](artifacts/20261016-160032/network_decision_analysis.md)。
仿真和两项离线分析均成功，输入与决策校验为 **PASS**。

| 指标 | 结果 |
| --- | ---: |
| CCT = 最大 GOAL rank graph finish | **45.676 µs** |
| 完成 flows / messagelets | **60 / 60** |
| 完成 GOAL tasks | **200 / 200** |
| Minimum / Valiant messagelets | **45 / 15** |
| 实际进行队列代价比较 | **28** |
| 同 switch 本地交付 | **4** |

| Collective | Messagelets | Minimum | Valiant |
| --- | ---: | ---: | ---: |
| Ring all-reduce | 24 | 18 | 6 |
| Tree all-reduce | 24 | 18 | 6 |
| Direct all-to-all | 12 | 9 | 3 |

这是固定 simulator seed 13 / UGAL seed 33 的单次 baseline，未做参数搜索或多 seed
统计。完整逐 flow 决策见该 artifact 的 `output_metrics/network_decisions.csv`。

# exp5.3s — Source UGAL-L runtime 粒度 sweep

研究同一个 source UGAL-L policy 在不同 runtime 决策粒度下的表现。
计算图、拓扑、placement 和网络参数沿用 exp5.1；所有点都使用 `runtime.mode=label`、
`routing.network_decision=ugal_l` 和 `-routing SOURCE`。

完整结果见本目录根部的 [comparison.md](comparison.md)，平均统计见
[comparison.csv](comparison.csv)，逐 seed 原始指标见 [per_seed.csv](per_seed.csv)，
校验见 [validation.json](validation.json)。当前默认 **18 个粒度 × 5 个种子 = 90 次运行**，
使用 **32 路并行**。

## Sweep 范围

默认 18 个点，按粒度从小到大排列：

1. `packet`：每个新数据包在源端决策一次。
2. `messagelet_01pkt` 到 `messagelet_16pkt`：每组 1–16 个包，步长为 1。
3. `flow`：每条 parent flow 在源端决策一次。

当前 workload 的每条 flow 为 65,536-byte payload，每包 4096-byte payload +
64-byte header，因此每条 flow 恰好发送 16 个完整的 4160-byte 包。
runtime 按 wire bytes 统计 messagelet，预算设为 `N × 4160`，范围为 4160–66,560 bytes。
这避免把 4 KiB wire budget 误解成 4 KiB payload。

N 不整除 16 时，最后一组较短，例如 10 包配置分成 10+6。
同一次 flow 的每个 messagelet 可选择不同路径，组内及重传仍保留原标签。
两个冗余端点用于校验实现：1 包 messagelet 应等价于 packet；16 包 messagelet 应等价于 flow。
本次均核对到每组路径与完整 flow/DAG/network metrics 相同。

## 固定输入和控制

- Workload：直接复用 `workload/exp2s/exp2a1_collective_internal_slack/workload.bin`，不重新生成 DAG。
- 12 ranks、60 条 64 KiB flow、200 个 GOAL task；ring/tree all-reduce 与 direct all-to-all 并发。
- `topology/p6a1h2/dragonfly.topo` 和 `rank_placement.json` 是 exp5.1 的逐字节副本。
- 沿用 400 Gbps、global/local/host latency = 500/25/25 ns、500 ns switch latency、
  88-packet queue、nscc/mixed、64 path entropy、默认 cwnd。
- 每批只构建一次 simulator，所有点运行前后校验同一 binary SHA-256。
- 使用五对 `(simulation seed, routing seed)`：`(13,33)`、`(14,34)`、`(15,35)`、
  `(16,36)`、`(17,37)`；每对都覆盖相同的 18 个粒度，每次运行独立启动进程。
- `-seed` 控制全局 simulator RNG；`-routing_seed` 控制每个 switch 的 UGAL 候选采样 RNG。
  原来两个值分别固定为 13/33，现在可从命令行选择，默认行为保持不变。
- static routing config 为空，未启用 transparent / 交换机原生 `UGAL_L`。
  exp5.0 的原生逐包 UGAL-L 不纳入本次粒度对比。
- Slack 仅在仿真后离线计算，不参与选路。

粒度改变会改变查询时机、RNG 消耗次数和网络队列演化；这是当前 source-UGAL-L
决策频率变化的整体效果，不表示跨粒度使用相同的候选路径序列。

## 运行

```bash
./experiments/exp5s/exp5.3s/run_all.sh
# 可调整并行度：
./experiments/exp5s/exp5.3s/run_all.sh --jobs 64
```

`sweep_config.json` 是 sweep 定义，包含 `messagelet_packets`、`seeds`、`parallel_jobs`
和固定网络参数。可修改 packet 数或 seed 对列表后重跑，也可以传入另一份配置：

```bash
./experiments/exp5s/exp5.3s/run_all.sh --config /path/to/sweep_config.json
```

每个 seed 都包含 packet 和 flow 两个参考点。预算按配置中的 packet size 计算，并与实际
`globalInfo.csv` 核对。运行前核对 workload、拓扑、placement 与 exp5.1 的哈希及网络控制参数。
流程只使用 Python 标准库和仓库现有 CMake/simulator，不需要额外绘图库。

每批输出到本目录下 `artifacts/YYYYMMDD-HHMMSS/`，时间采用 America/Vancouver；
`artifacts/latest` 指向最近一次成功批次。根部 `comparison.md`、`comparison.csv`、
`per_seed.csv`、`validation.json` 更新为该批结果，旧批报告仍保留在对应 artifact 中。
并行任务分别使用独立进程和输出目录；完成顺序不会影响任何 seed 的仿真。

```text
exp5.3s/
  sweep_config.json
  run_all.sh / run_sweep.py / compare.py
  comparison.md / comparison.csv / per_seed.csv / validation.json
  rank_placement.json / topology/
  artifacts/<batch>/
    manifest.json
    config_snapshot/                 # 共同输入、运行/比较脚本和 sweep 配置
    comparison.md / comparison.csv / per_seed.csv / validation.json
    seeds/seed_<simulation>/cases/<point>/
      command.txt / binary-sha256.txt / exit_code.txt
      config_snapshot/               # 每点实际执行的输入、runtime 配置及 seed.json
      simulator.log / analysis.log
      summary.md / slack_analysis.md / granularity_validation.json
      output_metrics/                # FCT、DAG、slack、randomSeeds.csv、network_decisions.csv
```

单独重做某个已完成批次的比较，不重跑 simulator：

```bash
python3 -B experiments/exp5s/exp5.3s/compare.py \
  --batch experiments/exp5s/exp5.3s/artifacts/latest \
  --output-dir experiments/exp5s/exp5.3s
```

比较器验证：所有输入和 binary hash 一致、实际命令与网络配置一致、全部 flow/task 完成、
每个 GOAL send 匹配唯一 transport flow、messagelet index 和数量符合预算，以及每个决策
满足 HTSim UGAL-L 规则。每点输出的 `network_decisions.csv` 带 metadata flow ID，保留
标签、队列/跳数、实际分组的新数据包数量及 wire bytes。

多种子校验还核对 `randomSeeds.csv` 中实际执行的两个 RNG seed，确认更换 routing seed
确实产生不同的 packet 路由结果，并在每个 seed 内检查两个等价端点。
均值等权汇总 5 个 seed，标准差是 seed 间样本标准差（n−1），不是置信区间。
ΔCCT 按同 seed 的 flow 基线配对计算；P95 FCT 是每个 seed 先计算 P95 再取平均。

## 五种子平均结果

批次：`20261016-165020`；90 次运行全部 PASS，5 个 seed 均有不同的 packet 路由结果。

| 代表粒度 | Mean CCT ± sample SD | 相对 flow 均值降低 |
| --- | ---: | ---: |
| Packet | 44.364 ± 1.589 µs | 1.70% |
| 2 包/messagelet | 42.296 ± 2.244 µs | 6.28% |
| **4 包/messagelet** | **40.509 ± 0.755 µs** | **10.24%** |
| 8 包/messagelet | 44.565 ± 1.335 µs | 1.25% |
| 10 包/messagelet | 43.578 ± 2.641 µs | 3.44% |
| Flow | 45.132 ± 1.518 µs | 0 |

本批按平均 CCT 排名最佳为 4 包/messagelet，上一批单 seed 的最佳 10 包未保持平均优势。
每个 seed 的 1 包 == packet、16 包 == flow 均通过路径和完整 metrics 等价校验。
此外，种子对 13/33 的全部 18 个点逐字节复现了历史串行批次的 flow、DAG 和 network CSV，
确认可配置种子的默认值及并行执行保留了原有结果。

## 历史单种子结果

批次：`20261016-163734`；固定种子对 13/33，18 个点全部 PASS。原始报告保留在该 artifact。

| 代表粒度 | CCT | 相对 flow |
| --- | ---: | ---: |
| Packet | 45.289 µs | −0.387 µs |
| 2 包/messagelet | 40.606 µs | −5.070 µs |
| 4 包/messagelet | 40.148 µs | −5.528 µs |
| 8 包/messagelet | 45.420 µs | −0.256 µs |
| **10 包/messagelet** | **39.909 µs** | **−5.767 µs（降低 12.63%）** |
| Flow | 45.676 µs | 0 |

当前配置下，CCT 对粒度的响应不是单调的；最小粒度未取得本批最小 CCT。
10 包点有 120 次决策、264/960 个逻辑新数据包使用 Valiant 标签，OOO count 为 107；
packet 点有 960 次决策、300/960 个包使用 Valiant，OOO count 为 271。
这些结果描述当前 DAG、拓扑和固定 seed，未进行多 seed 统计。

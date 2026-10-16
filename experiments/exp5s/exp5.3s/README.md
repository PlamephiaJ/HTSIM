# exp5.3s — Source UGAL-L runtime 粒度 sweep

研究同一个 source UGAL-L policy 在不同 runtime 决策粒度下的表现。
计算图、拓扑、placement 和网络参数沿用 exp5.1；所有点都使用 `runtime.mode=label`、
`routing.network_decision=ugal_l` 和 `-routing SOURCE`。

完整结果见本目录根部的 [comparison.md](comparison.md)，机器可读结果见
[comparison.csv](comparison.csv)，校验见 [validation.json](validation.json)。

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
- 每个点独立启动进程，simulator seed 13 / per-switch UGAL RNG seed 33。
- static routing config 为空，未启用 transparent / 交换机原生 `UGAL_L`。
  exp5.0 的原生逐包 UGAL-L 不纳入本次粒度对比。
- Slack 仅在仿真后离线计算，不参与选路。

粒度改变会改变查询时机、RNG 消耗次数和网络队列演化；这是当前 source-UGAL-L
决策频率变化的整体效果，不表示跨粒度使用相同的候选路径序列。

## 运行

```bash
./experiments/exp5s/exp5.3s/run_all.sh
```

`sweep_config.json` 是 sweep 定义，包含待扫描的 `messagelet_packets` 和固定网络参数。
可修改 packet 数列表后重跑，也可以传入另一份配置：

```bash
./experiments/exp5s/exp5.3s/run_all.sh --config /path/to/sweep_config.json
```

每批都包含 packet 和 flow 两个参考点。预算按配置中的 packet size 计算，并与实际
`globalInfo.csv` 核对。运行前核对 workload、拓扑、placement 与 exp5.1 的哈希及网络控制参数。
流程只使用 Python 标准库和仓库现有 CMake/simulator，不需要额外绘图库。

每批输出到本目录下 `artifacts/YYYYMMDD-HHMMSS/`，时间采用 America/Vancouver；
`artifacts/latest` 指向最近一次成功批次。根部 `comparison.md`、`comparison.csv`、
`validation.json` 更新为该批结果，旧批报告仍保留在对应 artifact 中。

```text
exp5.3s/
  sweep_config.json
  run_all.sh / run_sweep.py / compare.py
  comparison.md / comparison.csv / validation.json
  rank_placement.json / topology/
  artifacts/<batch>/
    manifest.json
    config_snapshot/                 # 共同输入、运行/比较脚本和 sweep 配置
    comparison.md / comparison.csv / validation.json
    cases/<point>/
      command.txt / binary-sha256.txt / exit_code.txt
      config_snapshot/               # 每点实际执行的输入与 runtime 配置
      simulator.log / analysis.log
      summary.md / slack_analysis.md / granularity_validation.json
      output_metrics/                # FCT、DAG、slack 和 network_decisions.csv
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

## 首批结果

批次：`20261016-163734`；18 个点全部 PASS。

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

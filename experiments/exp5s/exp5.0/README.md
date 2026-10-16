# Experiment 5.0 — 原生 packet-level UGAL-L

计算图、拓扑、rank placement 和网络参数与 [exp5.1](../exp5.1/README.md) 相同。
关闭 runtime 的 network decision 与打标功能，包到达交换机后由 HTSim 原生 UGAL-L
决策；重传同样走原生 UEC/交换机路径。runtime 保留 GOAL 提交和完成通知所需的连接。

| 配置 | exp5.0 | exp5.1 |
| --- | --- | --- |
| `runtime.mode` | `transparent` | `label`（默认） |
| `routing.network_decision` | `none` | `ugal_l` |
| Simulator `-routing` | `UGAL_L` | `SOURCE` |
| 决策位置 | 包到达交换机时 | flow 首次打标前 |
| 标签缓存 | 原生 transport entropy，不固定 runtime 路径 | 每个 flow 一个固定路径标签 |

## 输入

- 直接复用 `workload/exp2s/exp2a1_collective_internal_slack/workload.bin`，
  不重新生成计算图；共 12 ranks、60 条 64 KiB flow、200 个 GOAL task。
- ranks 0–3：ring all-reduce；4–7：tree all-reduce；8–11：direct all-to-all。
- `topology/p6a1h2/dragonfly.topo` 和 `rank_placement.json` 是 exp5.1 的逐字节副本。
  三个 group 各有一个 switch、六个 host，group 0/1 承载 ranks，group 2 可供绕路。
- 沿用 400 Gbps、global/local/host latency = 500/25/25 ns、500 ns switch latency、
  4160-byte packet、88-packet queue、`nscc`、`mixed`、64 path entropy 和默认 cwnd。
- `ROUTING_CONFIG` 为空；没有 SOURCE 路径配置或 runtime UGAL-L policy。

`messagelet_configs/default.json` 选择 `runtime.mode="transparent"`。未使用 runtime 的
`packet` 粒度：该粒度仍然是在源端逐包打标，而本实验直接使用交换机原生 UGAL-L。
`trace_decisions=true` 用于核查没有 runtime 决策记录；`TRACE_PACKET_HOPS=1` 保存
实际 DATA/control 逐跳记录，不新增 event、RNG 调用或修改 packet。

## 运行和产物

```bash
./experiments/exp5s/exp5.0/run.sh
```

入口沿用 exp5.1 的运行方式：以 4 个构建任务增量编译 simulator，保存独立输入快照，
运行仿真、通用 summary、共享离线 slack 分析与本地原生路由校验。
`config.env` 保存实验参数；`run.sh` 也接受另一份配置文件作为第一个参数。

时间戳使用 America/Vancouver；每次运行写入独立的 `artifacts/YYYYMMDD-HHMMSS/`，
`artifacts/latest` 指向最近运行。主要文件：

- `config_snapshot/`：workload binary/GOAL/metadata、拓扑、placement、配置及分析脚本。
- `command.txt`、`binary-sha256.txt`、Git revision/status、`simulator.patch`：执行记录。
- `simulator.log`：原始输出和 DATA/control packet-hop trace。
- `summary.md`、`slack_analysis.md` 及原有 flow/DAG/global/slack CSV。
- `native_routing_analysis.md`、`native_routing_validation.json`：结果和输入校验。
- `output_metrics/packet_routes.csv`：每次 DATA packet 发送的 transport flow ID、
  sequence、attempt、注入/交付时间、switch 路径及实际 minimum/detour 分类。

分析器核对输入与 exp5.1 的 SHA-256 一致、网络控制参数一致、执行策略为 `UGAL_L`、
无静态路由或 runtime 决策、所有 flow/task 完成，以及 rank finish 与 lifecycle CCT 一致。
同一 sequence 的重传作为新 attempt 记录。这里的 minimum/detour 是在 `a=1` 拓扑上
按实际 packet 跳数分类，不代表固定 flow 标签，也不是 UGAL 代价比较次数。
逐 flow 的 metadata 映射由共享 slack analyzer 校验，packet 路径使用原生 transport ID。

单独重做原生路由分析：

```bash
python3 experiments/exp5s/exp5.0/analyze_routing.py \
  experiments/exp5s/exp5.0/artifacts/latest \
  --reference experiments/exp5s/exp5.1
```

## 实测结果

首个 artifact：[`20261016-162437`](artifacts/20261016-162437/native_routing_analysis.md)。
仿真、slack 分析和原生路由校验全部成功。

| 指标 | 结果 |
| --- | ---: |
| CCT = 最大 GOAL rank graph finish | **41.799 µs** |
| 完成 flow | **60 / 60** |
| 完成 GOAL task | **200 / 200** |
| Runtime 路由决策 | **0** |
| DATA/control 逐跳记录 | **12,177** |
| DATA packet attempts / delivered | **960 / 960** |
| 实际 minimum / detour 数据包 | **666 / 294** |

固定 simulator seed 13、per-switch UGAL RNG seed 33 的单次 baseline。
Slack 仅离线分析，不参与决策。exp5.1 已保存的历史结果来自另一 binary hash；
需要严格比较两种策略时，应在同一个构建版本下分别重跑并保留各自快照。

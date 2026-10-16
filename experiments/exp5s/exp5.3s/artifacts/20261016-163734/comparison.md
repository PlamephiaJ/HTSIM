# exp5.3s — Source UGAL-L runtime 粒度 sweep

批次：`20261016-163734`；18 个点，全部校验 **PASS**。

本批最小 CCT 为 **39.909 µs**（messagelet_10pkt），相对同批 flow 的 **45.676 µs** 改变 **-5.767 µs**，降低 **12.63%**。

所有点均为 `runtime.mode=label`、`routing.network_decision=ugal_l`、`-routing SOURCE`。改变的只有 runtime 粒度和对应 messagelet 字节预算；未混入 transparent / 原生交换机 UGAL-L。

## CCT、FCT 和乱序

| Sweep point | Budget (wire bytes) | CCT (µs) | ΔCCT vs flow (µs) | Mean FCT (µs) | P95 FCT (µs) | OOO packets |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [packet](cases/packet/summary.md) | — | **45.289** | -0.387 | 5.853 | 6.959 | 271 |
| [messagelet_01pkt](cases/messagelet_01pkt/summary.md) | 4160 | **45.289** | -0.387 | 5.853 | 6.959 | 271 |
| [messagelet_02pkt](cases/messagelet_02pkt/summary.md) | 8320 | **40.606** | -5.070 | 5.769 | 7.237 | 245 |
| [messagelet_03pkt](cases/messagelet_03pkt/summary.md) | 12480 | **40.086** | -5.590 | 5.904 | 7.469 | 162 |
| [messagelet_04pkt](cases/messagelet_04pkt/summary.md) | 16640 | **40.148** | -5.528 | 5.622 | 6.716 | 192 |
| [messagelet_05pkt](cases/messagelet_05pkt/summary.md) | 20800 | **40.304** | -5.372 | 5.815 | 7.277 | 122 |
| [messagelet_06pkt](cases/messagelet_06pkt/summary.md) | 24960 | **44.650** | -1.026 | 5.818 | 7.277 | 120 |
| [messagelet_07pkt](cases/messagelet_07pkt/summary.md) | 29120 | **44.678** | -0.998 | 5.949 | 7.803 | 85 |
| [messagelet_08pkt](cases/messagelet_08pkt/summary.md) | 33280 | **45.420** | -0.256 | 5.708 | 7.549 | 63 |
| [messagelet_09pkt](cases/messagelet_09pkt/summary.md) | 37440 | **44.212** | -1.464 | 5.838 | 7.271 | 55 |
| [messagelet_10pkt](cases/messagelet_10pkt/summary.md) | 41600 | **39.909** | -5.767 | 5.776 | 6.944 | 107 |
| [messagelet_11pkt](cases/messagelet_11pkt/summary.md) | 45760 | **43.594** | -2.082 | 5.876 | 7.356 | 84 |
| [messagelet_12pkt](cases/messagelet_12pkt/summary.md) | 49920 | **45.638** | -0.038 | 5.781 | 7.130 | 81 |
| [messagelet_13pkt](cases/messagelet_13pkt/summary.md) | 54080 | **40.381** | -5.295 | 5.919 | 7.447 | 70 |
| [messagelet_14pkt](cases/messagelet_14pkt/summary.md) | 58240 | **42.463** | -3.213 | 6.197 | 8.220 | 39 |
| [messagelet_15pkt](cases/messagelet_15pkt/summary.md) | 62400 | **44.012** | -1.664 | 6.287 | 8.954 | 21 |
| [messagelet_16pkt](cases/messagelet_16pkt/summary.md) | 66560 | **45.676** | +0.000 | 6.426 | 9.518 | 0 |
| [flow](cases/flow/summary.md) | — | **45.676** | +0.000 | 6.426 | 9.518 | 0 |

CCT 是 12 个 GOAL rank graph finish 的最大值；FCT 来自 `flowsInfo.csv`。P95 使用排序后线性插值；OOO 是各 flow 的 `oooCount` 之和，不表示重传数。

## 决策频率与选路

| Sweep point | Decisions | Mean packets / decision | Valiant decisions | Valiant decision % | New packets on Valiant | Packet-weighted Valiant % |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| packet | 960 | 1.00 | 300 | 31.25% | 300 / 960 | 31.25% |
| messagelet_01pkt | 960 | 1.00 | 300 | 31.25% | 300 / 960 | 31.25% |
| messagelet_02pkt | 480 | 2.00 | 151 | 31.46% | 302 / 960 | 31.46% |
| messagelet_03pkt | 360 | 2.67 | 119 | 33.06% | 309 / 960 | 32.19% |
| messagelet_04pkt | 240 | 4.00 | 71 | 29.58% | 284 / 960 | 29.58% |
| messagelet_05pkt | 240 | 4.00 | 72 | 30.00% | 252 / 960 | 26.25% |
| messagelet_06pkt | 180 | 5.33 | 46 | 25.56% | 236 / 960 | 24.58% |
| messagelet_07pkt | 180 | 5.33 | 47 | 26.11% | 244 / 960 | 25.42% |
| messagelet_08pkt | 120 | 8.00 | 34 | 28.33% | 272 / 960 | 28.33% |
| messagelet_09pkt | 120 | 8.00 | 33 | 27.50% | 251 / 960 | 26.15% |
| messagelet_10pkt | 120 | 8.00 | 34 | 28.33% | 264 / 960 | 27.50% |
| messagelet_11pkt | 120 | 8.00 | 36 | 30.00% | 264 / 960 | 27.50% |
| messagelet_12pkt | 120 | 8.00 | 33 | 27.50% | 228 / 960 | 23.75% |
| messagelet_13pkt | 120 | 8.00 | 34 | 28.33% | 252 / 960 | 26.25% |
| messagelet_14pkt | 120 | 8.00 | 40 | 33.33% | 260 / 960 | 27.08% |
| messagelet_15pkt | 120 | 8.00 | 40 | 33.33% | 236 / 960 | 24.58% |
| messagelet_16pkt | 60 | 16.00 | 15 | 25.00% | 240 / 960 | 25.00% |
| flow | 60 | 16.00 | 15 | 25.00% | 240 / 960 | 25.00% |

每条 flow 的 64 KiB payload 对应 16 个完整的 4160-byte 包（含 64-byte header）。messagelet 预算按完整 packet 的 wire bytes 设置为 `N × 4160`。N 不整除 16 时，末组较短，因此同时报告决策占比和按逻辑新数据包数加权的选路占比；重传保留原包标签，不触发新决策。

## 各 collective 的 graph finish

| Sweep point | Ring all-reduce (µs) | Tree all-reduce (µs) | Direct all-to-all (µs) |
| --- | ---: | ---: | ---: |
| packet | 38.499 | 45.289 | 39.178 |
| messagelet_01pkt | 38.499 | 45.289 | 39.178 |
| messagelet_02pkt | 36.845 | 40.606 | 39.887 |
| messagelet_03pkt | 38.917 | 40.085 | 40.086 |
| messagelet_04pkt | 35.210 | 39.423 | 40.148 |
| messagelet_05pkt | 38.067 | 40.304 | 39.788 |
| messagelet_06pkt | 38.661 | 44.650 | 37.860 |
| messagelet_07pkt | 39.455 | 44.678 | 40.798 |
| messagelet_08pkt | 39.959 | 45.420 | 40.293 |
| messagelet_09pkt | 40.828 | 44.212 | 40.078 |
| messagelet_10pkt | 39.909 | 39.159 | 36.496 |
| messagelet_11pkt | 37.558 | 43.594 | 41.188 |
| messagelet_12pkt | 39.261 | 45.638 | 37.585 |
| messagelet_13pkt | 40.381 | 38.870 | 40.213 |
| messagelet_14pkt | 42.463 | 41.052 | 42.121 |
| messagelet_15pkt | 43.011 | 43.262 | 44.012 |
| messagelet_16pkt | 45.676 | 43.962 | 42.790 |
| flow | 45.676 | 43.962 | 42.790 |

## 控制与复现

- 同一 simulator SHA-256：`14057d3ba4d44f8a044a92399460ea095c5c1a68f58e210750a352a65fd2ac9e`。
- 所有点的 workload binary/GOAL/metadata、拓扑和 placement 哈希一致，并与 exp5.1 输入一致。
- 实际 simulator 参数和 `globalInfo.csv` 一致；SOURCE 路径策略、nscc/mixed、queue、cwnd 与 path entropy 均固定。
- 每个点全部 60 条 flow 和 200 个 GOAL task 完成；metadata send 与 transport ID、决策 index、预算和 UGAL-L 代价规则均已核对。
- 每个点独立重启进程：simulator seed 13 / per-switch UGAL RNG seed 33。粒度改变后 RNG 消耗与队列演化也会改变，这是本次策略粒度变化的一部分。
- Slack 仅在每次仿真后离线分析，不进入 policy。

- **PASS**：`messagelet_01pkt == packet` 的每组路径以及 flow/DAG/network metrics 完全一致。
- **PASS**：`messagelet_16pkt == flow` 的每组路径以及 flow/DAG/network metrics 完全一致。

这是当前 DAG、拓扑和固定 seed 的单次 sweep；对粒度优劣的解释限于本批结果。完整逐 flow / messagelet 记录见各点的 `output_metrics/network_decisions.csv`，输入快照和固定批次路径见 `manifest.json` 与 `validation.json`。

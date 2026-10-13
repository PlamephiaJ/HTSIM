# Experiment 4 — 两个 flow 的 compute-tail swap 因果验证

验证一个最小的路径资源分配决策：网络信息不变时，仅交换 DAG criticality，
更有利于 CCT 的绕路对象是否随之反转。采用 exp1 的 flow-level GOAL DAG、
Dragonfly `p3a1h2` 和最大 rank graph finish 作为 CCT，四个对照每次恰好一个 flow 绕路。

## Workload 和严格控制

```text
start ── A: 0 → 2 ── compute tail A ── local sink ──┐
      └─ B: 1 → 3 ── compute tail B ── local sink ──┴─ CCT = max(rank finish)
```

图中的汇合表示应用完成的最大值，不是新增网络同步；GOAL sink 仍是 rank-local。
没有额外通信、background traffic 或 synthetic post-flow delay。

| Flow | Metadata ID | GOAL ranks | Physical hosts | Size | DAG-1 tail | DAG-2 tail |
| --- | ---: | --- | --- | ---: | ---: | ---: |
| A | 0 | 0 → 2 | 0 → 3 | 32 KiB | 30 µs | 15 µs |
| B | 1 | 1 → 3 | 1 → 4 | 32 KiB | 15 µs | 30 µs |

Ranks 0、1 位于 group 0 的不同 host，ranks 2、3 位于 group 1 的不同 host。
两个 flow 都在 2 ns 同时 ready / issue。minimal 共享 group 0 → 1 的 global link；
valiant 固定经过 group 2，路径为 0 → 2 → 1。ACK/control 使用原有 minimal 回程。
每个 flow 只含一条 GOAL send/recv 消息和一个 messagelet，不拆成额外 flow。

网络配置沿用 exp1：400 Gbps，global/local/host delay = 500/25/25 ns，
switch latency = 500 ns，4160-byte packet，88-packet queue，`nscc` / `mixed`，
64 path entropy，默认 cwnd，固定 simulator seed 13。
使用 `SOURCE` 加手动 `minimum` / `valiant` 标签固定路径，不进行 adaptive route 选择。

32 KiB 是为同时获得 **绕路自身正 FCT penalty** 和 **另一条 flow 的拥塞缓解**
选定的固定参数。预检的 64 KiB 在该配置下绕路自身也加快，不符合本实验的 tradeoff 条件；
正式四个对照及两个参考全部使用同一个 32 KiB 配置，没有按 DAG 单独调参。

## 四个对照和两个参考

| DAG context | A 绕路，B minimal | B 绕路，A minimal | All-minimal reference |
| --- | --- | --- | --- |
| DAG-1：A critical | `exp4.1` — Misaligned | `exp4.2` — Slack-aligned | `exp4.ref1` |
| DAG-2：B critical | `exp4.3` — Slack-aligned | `exp4.4` — Misaligned | `exp4.ref2` |

两个 reference 只用于分别确定各 DAG 的 all-minimal CCT、oracle slack 和 ΔFCT，
不计入四个单 flow 绕路对照。ΔCCT = case CCT − 对应 DAG 的 all-minimal CCT。
不能共用一个参考：all-minimal 下 A、B 的 FCT 存在 83.2 ns 的确定性时序差异。

两个 DAG 的模板、编译后的 GOAL 节点和依赖完全一致，只有 A/B compute tail 时长互换。
compute 位于接收之后，后面没有新的通信，因此不改变网络 offered load。

## 运行与产物

从仓库任意位置运行：

```bash
./experiments/exp4s/run_all.sh
```

该入口生成两个 workload 和六个 routing config，增量构建 simulator，运行六次，
再用这批明确的 artifact 路径执行比较。新结果位于
`experiments/exp4s/artifacts/latest/comparison.md`，并附带 `comparison.csv`、
`validation.json` 和每个 case 的控制台日志。
每个 case 沿用现有实验格式，提供 `config.env`、`routing_configs/default.json`、
`run.sh`、时间戳 `artifacts/`、`config_snapshot/`、`summary.md`、`slack_analysis.md`、
simulator log 和 CSV metrics。两个生成的 workload 位于 `workload/exp4s/`。
模板放在本实验的 `templates/` 中，与实验配置一起保存。

单独重跑、比较或测试：

```bash
python3 experiments/exp4s/generate.py
./experiments/exp4s/exp4.1/run.sh
python3 experiments/exp4s/compare.py
python3 -B -m unittest discover -s experiments/exp4s -p test_controls.py -v
```

`compare.py` 默认读取六个 case 的 `artifacts/latest`，输出到本目录。
可用 `--ref1`、`--ref2`、`--1`、`--2`、`--3`、`--4` 固定 artifact，
用 `--output-dir` 指定比较结果位置。

## 实测结果

| DAG context | A 绕路 CCT | ΔCCT | B 绕路 CCT | ΔCCT | 更优绕路对象 |
| --- | ---: | ---: | ---: | ---: | --- |
| DAG-1：A critical | 35.023 µs | +0.501 µs | **33.939 µs** | −0.583 µs | **B** |
| DAG-2：B critical | **33.939 µs** | −0.666 µs | 35.023 µs | +0.418 µs | **A** |

DAG-1 的参考 CCT = **34.522 µs**，A/B oracle slack = **0 / 14.917 µs**。
DAG-2 的参考 CCT = **34.605 µs**，A/B oracle slack = **15.083 / 0 µs**。
两种 DAG 下 Slack-aligned 均比 Misaligned 低 **1.084 µs**。

| Fixed allocation | A FCT | ΔA FCT | B FCT | ΔB FCT |
| --- | ---: | ---: | ---: | ---: |
| All-minimal | 4.51824 µs | 0 | 4.60144 µs | 0 |
| A 绕路 | 5.01904 µs | **+0.50080 µs** | 3.93584 µs | **−0.66560 µs** |
| B 绕路 | 3.93584 µs | **−0.58240 µs** | 5.01904 µs | **+0.41760 µs** |

这个 FCT 表在交换 tails 前后完全不变。fractional-ns HTSim flow completion 和
integer-ns GOAL finish 存在截断差异；接收后的 anchor 和 sink 合计添加 2 ns，
所以 CCT 差与原始 FCT 差在末位有细微区别。

## 自动成功判定

只判定三项实验结果：

| 观察指标 | 证据 | 实测 |
| --- | --- | --- |
| Network tradeoff | 绕路 flow 的 ΔFCT > 0，另一 flow 的 ΔFCT < 0 | PASS |
| CCT | 每个 DAG 的 Slack-aligned CCT < Misaligned CCT | PASS |
| Decision reversal | DAG-1 优选 B 绕路，DAG-2 优选 A 绕路 | PASS |

严格控制验证是判定之前的必要条件。`compare.py` 核对 executed binary 与模板重新编译
逐字节一致；相同 DAG 的 workload 完全一致；跨 DAG 只交换 tails；网络配置和 binary hash
一致。对于同一列的固定路由分配，以下记录要求精确一致：

- assigned 和实际路由、waypoint、物理端点；
- `flowsInfo.csv` 全部字段及 send/recv DAG lifecycle；
- 按原始事件顺序比较的 DATA/control 逐跳 trace，包括 picosecond 时间、packet/flow ID、
  packet type/size、端点、ECN flags、trim/bounce status、下一跳名称及入队前 queue occupancy。

`HTSIM_TRACE_PACKET_HOPS=1` 开启 opt-in 记录，默认关闭；不创建 event、不调用 RNG、
不改变 packet。验证测试也会关闭 trace 重新仿真两种路由，检查所有网络和 DAG metrics 相同。
本次 all-minimal 配对的 198 条事件一致，A/B 绕路配对各自的 222 条事件一致；
SHA-256 及固定 artifact 路径见 [完整比较报告](comparison.md)。

因此，本实验支持：**网络观察保持不变时，仅改变 DAG criticality，会反转更有利于 CCT
的路径分配选择。** 只使用相同网络输入的策略不能区分这两个 context。
本实验是固定 seed 的受控 allocation 对照，没有实际运行 congestion-only 决策策略，
也不把该结果外推为一般 workload 上的策略性能结论。

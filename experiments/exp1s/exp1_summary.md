# 板块 1：这个问题真的存在吗？

## Experiment 1 — Execution Slack 是否对应真实的 CCT Delay Tolerance？

### 1. 实验目的

本实验验证如下现象：

> 对于同时 ready、网络行为相近、共享同一网络资源的通信，由于它们处于 execution DAG 中不同的位置，会具有不同的 completion-delay tolerance。

实验希望建立下面这条关系：

$$
\text{DAG position}
\rightarrow
\text{execution slack heterogeneity}
\rightarrow
\text{different CCT sensitivity}
$$

这一现象构成后续按 slack 分配低延迟路径的基础。

---

## 2. Workload 设计

使用一个最小三分支 flow-level GOAL DAG。

三个 P2P flow 满足：

- 同时 ready；
- 大小完全相同；
- 共享同一 Dragonfly minimal global corridor；
- endpoint 不同，避免单一 destination NIC 成为主要瓶颈；
- 主要差异来自 flow 完成后的 downstream compute tail。

### 2.1 Rank 与 flow

| Branch   | Flow     | Size   | Downstream compute |
| -------- | -------- | ------:| ------------------:|
| Critical | `0 -> 3` | 64 KiB | 30 us              |
| Medium   | `1 -> 4` | 64 KiB | 15 us              |
| Slack    | `2 -> 5` | 64 KiB | 0 us               |

Rank placement：

- ranks `0,1,2` -> Dragonfly group 0
- ranks `3,4,5` -> Dragonfly group 1
- group 2 作为两跳 alternative path
- routing 固定为 `MINIMAL`，三个 flow 竞争 group 0 -> group 1 的 direct global corridor

概念结构：

```text
flow 0->3 ---- compute 30 us ----+
                                 |
flow 1->4 ---- compute 15 us ----+---- workload completion
                                 |
flow 2->5 ---- compute  0 us ----+
```

GOAL dependency 是 rank-local 的，因此 workload completion 使用 simulator 报告的 computation-graph makespan，即所有 rank graph finish time 的最大值。

---

## 3. Network 配置

| Parameter               | Value                |
| ----------------------- | -------------------- |
| Topology                | Dragonfly `p3a1h2`   |
| Routing                 | `MINIMAL`            |
| Sender CC               | `nscc`               |
| Multipath algorithm     | `mixed`              |
| Path entropy            | 64                   |
| Link speed              | 400 Gbps             |
| Global/local/host delay | 500 / 25 / 25 ns     |
| Packet size             | 4160 bytes           |
| Queue                   | 88 packets           |
| Queue size              | 366080 bytes         |
| ECN low/high            | 70720 / 291200 bytes |
| SACK threshold          | 16384 bytes          |

三个 flow 都在约 `t = 2 ns` 时开始。

---

## 4. Baseline：exp1

### 4.1 Flow-level 结果

| Flow     | Start (ns) | End (ns) | FCT (ns) | Packets |
| -------- | ----------:| --------:| --------:| -------:|
| `0 -> 3` | 2.000      | 7099.440 | 7097.440 | 16      |
| `2 -> 5` | 2.000      | 7182.640 | 7180.640 | 16      |
| `1 -> 4` | 2.000      | 7265.840 | 7263.840 | 16      |

FCT：

- min: `7097.440 ns`
- avg: `7180.640 ns`
- max: `7263.840 ns`

最大 FCT spread：

$$
7263.840 - 7097.440 = 166.400\ \text{ns}
$$

三个 flow 的网络执行时间相当接近。

### 4.2 Application-level 结果

Computation-graph makespan：

$$
C_{\mathrm{ref}} = 37101\ \text{ns} = 37.101\ \mu s
$$

Rank finish：

| Rank | Graph finish  |
| ----:| -------------:|
| 0    | 7.100 us      |
| 1    | 7.266 us      |
| 2    | 7.183 us      |
| 3    | **37.101 us** |
| 4    | 22.267 us     |
| 5    | 7.185 us      |

Baseline critical rank 为 `rank 3`。

### 4.3 Oracle slack

使用 baseline 实际 makespan 作为 reference completion horizon：

$$
C_{\mathrm{ref}} = 37.101\ \mu s
$$

对于每条 branch，oracle slack 定义为：

$$
s_m = C_{\mathrm{ref}} - F_{\mathrm{branch},m}
$$

Critical branch：

$$
s_{\mathrm{critical}}
= 37.101 - 37.101
= 0\ \mu s
$$

Medium branch：

$$
s_{\mathrm{medium}}
= 37.101 - 22.267
= 14.834\ \mu s
$$

Slack branch：

$$
s_{\mathrm{slack}}
= 37.101 - 7.185
= 29.916\ \mu s
$$

| Branch   | Oracle slack  |
| -------- | -------------:|
| Critical | **0 us**      |
| Medium   | **14.834 us** |
| Slack    | **29.916 us** |

Baseline 已经表现出明显的 execution-slack heterogeneity。

---

## 5. Perturbation 方法

为了验证 slack 是否对应真实的 completion-delay tolerance，在目标 flow 完成之后插入 synthetic DAG-level `delay` node：

```text
flow
  |
  v
synthetic delay delta
  |
  v
downstream compute
  |
  v
workload completion
```

`delay` 在 GOAL 中 lowering 为 rank-local `calc`。在当前 micro workload 中，每个 destination rank 没有其他并发 compute，因此可以用它表示 post-flow dependency delay perturbation。

整个实验中 network execution 保持一致，只有对应 branch 的 DAG-visible completion 被推迟。

理论预期为：

$$
\Delta CCT_m = \max(0, \delta - s_m)
$$

其中：

- $\delta$：注入到该 branch 的 synthetic post-flow delay；
- $s_m$：baseline oracle slack。

---

## 6. exp1.1 — Critical branch +10 us

对 `0 -> 3` branch 注入：

$$
\delta = 10\ \mu s
$$

结果：

- makespan: `47.101 us`
- critical rank: `3`

因此：

$$
\Delta CCT
= 47.101 - 37.101
= 10.000\ \mu s
$$

理论值：

$$
\max(0, 10 - 0) = 10\ \mu s
$$

实测与理论值一致。

---

## 7. exp1.2 — Medium branch +10 us

对 `1 -> 4` branch 注入：

$$
\delta = 10\ \mu s
$$

结果：

- rank 4 finish: `32.267 us`
- makespan: `37.101 us`
- critical rank: `3`

因此：

$$
\Delta CCT = 0
$$

由于：

$$
10 < 14.834
$$

理论值：

$$
\max(0, 10 - 14.834) = 0
$$

实测与理论值一致。

---

## 8. exp1.3 — High-slack branch +10 us

对 `2 -> 5` branch 注入：

$$
\delta = 10\ \mu s
$$

结果：

- rank 5 finish: `17.185 us`
- makespan: `37.101 us`
- critical rank: `3`

因此：

$$
\Delta CCT = 0
$$

由于：

$$
10 < 29.916
$$

理论值：

$$
\max(0, 10 - 29.916) = 0
$$

实测与理论值一致。

---

## 9. exp1.4 — Medium branch +20 us

对 `1 -> 4` branch 注入：

$$
\delta = 20\ \mu s
$$

结果：

- rank 4 finish: `42.267 us`
- makespan: `42.267 us`
- critical rank: `4`

因此：

$$
\Delta CCT
= 42.267 - 37.101
= 5.166\ \mu s
$$

理论值：

$$
\max(0, 20 - 14.834)
= 5.166\ \mu s
$$

实测与理论值一致。

这一结果同时表现出 critical-path switching：medium branch 消耗完自身 slack 后成为新的 critical branch。

---

## 10. exp1.5 — High-slack branch +35 us

对 `2 -> 5` branch 注入：

$$
\delta = 35\ \mu s
$$

结果：

- rank 5 finish: `42.185 us`
- makespan: `42.185 us`
- critical rank: `5`

因此：

$$
\Delta CCT
= 42.185 - 37.101
= 5.084\ \mu s
$$

理论值：

$$
\max(0, 35 - 29.916)
= 5.084\ \mu s
$$

实测与理论值一致。

这一结果同样表现出 critical-path switching：high-slack branch 在消耗完自身 slack 后成为新的 critical branch。

---

## 11. 汇总结果

| Experiment | Perturbed branch | Oracle slack | Injected delay | Expected Delta CCT | Measured CCT | Measured Delta CCT | Critical rank |
| ---------- | ---------------- | ------------:| --------------:| ------------------:| ------------:| ------------------:| -------------:|
| exp1       | none             | —            | 0 us           | 0                  | 37.101 us    | 0                  | 3             |
| exp1.1     | Critical         | 0 us         | 10 us          | 10.000 us          | 47.101 us    | **10.000 us**      | 3             |
| exp1.2     | Medium           | 14.834 us    | 10 us          | 0                  | 37.101 us    | **0**              | 3             |
| exp1.3     | Slack            | 29.916 us    | 10 us          | 0                  | 37.101 us    | **0**              | 3             |
| exp1.4     | Medium           | 14.834 us    | 20 us          | 5.166 us           | 42.267 us    | **5.166 us**       | 4             |
| exp1.5     | Slack            | 29.916 us    | 35 us          | 5.084 us           | 42.185 us    | **5.084 us**       | 5             |

五个 perturbation 结果全部满足：

$$
\boxed{\Delta CCT_m = \max(0, \delta - s_m)}
$$

---

## 12. Network-side consistency

在 exp1、exp1.1、exp1.2、exp1.3、exp1.4 和 exp1.5 中，三个 P2P flow 的 HTSim FCT 保持一致：

| Flow     | FCT         |
| -------- | -----------:|
| `0 -> 3` | 7097.440 ns |
| `1 -> 4` | 7263.840 ns |
| `2 -> 5` | 7180.640 ns |

因此不同实验之间的 network execution 保持一致，而 CCT 的变化来自 branch 在 DAG 中可吸收 delay 的能力不同。

---

## 13. 实验结论

Experiment 1 表明：

> 三个同时 ready、大小相同、共享同一 minimal network corridor、且具有相近网络完成时间的通信，因为位于 execution DAG 中不同的位置，对相同的 communication-completion perturbation 具有显著不同的 CCT sensitivity。

具体表现为：

1. zero-slack branch 的额外 completion delay 会直接转化为 CCT 增长；
2. positive-slack branch 可以吸收不超过自身 slack 的额外 delay；
3. 当额外 delay 超过 slack 后，超出的部分近似一比一转化为 CCT 增长；
4. 当原本 non-critical branch 消耗完 slack 后，会切换成为新的 critical branch。

因此，在该受控 workload 中，execution slack 可以直接解释 communication completion delay 对最终 CCT 的敏感性：

$$
\boxed{
\text{same network-level communication}
+
\text{different DAG context}
\Rightarrow
\text{different delay tolerance}
}
$$

并且实测关系与 oracle slack 完全对应：

$$
\boxed{\Delta CCT_m = \max(0, \delta - s_m)}
$$

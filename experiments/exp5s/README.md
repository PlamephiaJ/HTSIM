# Experiment 5 — Messagelet load balancing

- [exp5.0](exp5.0/README.md): 与 exp5.1 相同的计算图、拓扑和 placement，runtime 透传，
  由交换机执行原生 HTSim packet-level UGAL-L。
- [exp5.1](exp5.1/README.md): exp2.a.2 的计算图和拓扑，UGAL-L network decision，
  每个 flow 一个 messagelet，作为只使用网络负载信息的 load balancing baseline。
- [exp5.3s](exp5.3s/README.md): 相同 source UGAL-L 的 runtime 粒度 sweep，
  覆盖 packet、1–16 包/messagelet 和 flow；根部 [对比报告](exp5.3s/comparison.md)。

# Experiment 5 — Messagelet load balancing

- [exp5.0](exp5.0/README.md): 与 exp5.1 相同的计算图、拓扑和 placement，runtime 透传，
  由交换机执行原生 HTSim packet-level UGAL-L。
- [exp5.1](exp5.1/README.md): exp2.a.2 的计算图和拓扑，UGAL-L network decision，
  每个 flow 一个 messagelet，作为只使用网络负载信息的 load balancing baseline。

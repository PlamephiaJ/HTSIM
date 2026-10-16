# exp5.1 — UGAL-L messagelet baseline

Status: **PASS**

- CCT: **45.676 µs** (45676 ns), measured as maximum GOAL rank graph finish.
- Completed flows / messagelets: **60 / 60**; one decision per flow.
- Completed GOAL tasks: **200**.
- Minimum / Valiant: **45 / 15**.
- Queue-cost comparisons: **28**; local same-switch deliveries: **4**.

| Collective | Messagelets | Minimum | Valiant |
| --- | ---: | ---: | ---: |
| direct_alltoall | 12 | 9 | 3 |
| ring_allreduce | 24 | 18 | 6 |
| tree_allreduce | 24 | 18 | 6 |

Validation checks the snapshotted workload binary, GOAL, metadata, topology and rank placement byte-for-byte against exp2.a.2, and preserves its CC/queue/path settings. Every metadata send maps through `(source rank, GOAL send task ID)` to one unique transport flow and one messagelet decision; every transport flow and GOAL task completed.

UGAL-L uses live local egress queue bytes and HTSim's candidate/hop-count logic. Valiant wins only for `q_min * h_min > q_val * h_val`; ties choose minimal. Same first-hop candidates bypass the comparison. SOURCE forwarding executes the frozen label. Same-switch sends remain local.

This is one deterministic run (HTSim seed 13, per-switch UGAL RNG seed 33). No DAG/slack signal enters the routing policy; slack is analyzed offline.

Detailed decisions: `output_metrics/network_decisions.csv`. Input hashes and checks: `network_decision_validation.json`.

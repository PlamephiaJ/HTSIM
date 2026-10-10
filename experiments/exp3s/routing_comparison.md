# Experiment 3.a: single-flow routing comparison

CCT is the maximum GOAL rank graph finish time. ΔCCT = case − E3-0; positive reduction means a shorter CCT. Flow IDs are workload metadata IDs.

## Verified baseline identities

| Flow | Algorithm | GOAL ranks | Baseline slack (µs) | Ready / issue (ns) | Role |
| ---: | --- | --- | ---: | --- | --- |
| 46 | Tree | 7 → 5 | 0.000 | 2 / 2 | Critical |
| 48 | All-to-All (direct) | 8 → 9 | 3.890 | 2 / 2 | Medium-slack |
| 0 | Ring | 0 → 1 | 4.686 | 2 / 2 | Higher-slack |

## CCT results

| Case | Detour flow | CCT (µs) | ΔCCT (µs) | CCT reduction | Critical ranks |
| --- | --- | ---: | ---: | ---: | --- |
| E3-0 | None | 50.952 | +0.000 | 0.00% | 5,7 |
| E3-1 | 0 | 48.418 | -2.534 | 4.97% | 5,7 |
| E3-2 | 48 | 44.087 | -6.865 | 13.47% | 2,3 |
| E3-3 | 46 | 44.633 | -6.319 | 12.40% | 5,7 |

## Per-collective graph completion

| Case | Ring (µs) | Tree (µs) | All-to-All (µs) |
| --- | ---: | ---: | ---: |
| E3-0 | 46.266 | 50.952 | 47.062 |
| E3-1 | 43.565 | 48.418 | 45.116 |
| E3-2 | 44.087 | 41.484 | 41.198 |
| E3-3 | 42.248 | 44.633 | 43.632 |

## Selected flow duration

Receiver finish minus send issue, from integer-ns GOAL lifecycle metrics.

| Case | Flow | Baseline (µs) | Detour (µs) | Δduration (µs) |
| --- | ---: | ---: | ---: | ---: |
| E3-1 | 0 | 8.632 | 5.731 | -2.901 |
| E3-2 | 48 | 8.463 | 5.731 | -2.732 |
| E3-3 | 46 | 8.379 | 5.731 | -2.648 |

## Validation and interpretation

- PASS: all runs succeeded; 60 unique flow records and 200 lifecycle nodes per run; all 12 ranks completed; logged CCT equals the lifecycle maximum.
- PASS: workload, topology, placement, messagelet config, simulator settings, and routing identities match the baseline snapshots.
- PASS: E3-0 has 60 minimum routes; each other case has exactly its selected flow marked valiant through group 2, with the other 59 routes minimum.

These are individual deterministic simulations (the simulator defaults to seed 13). Baseline slack is an observed-execution property. It does not predict the sign of a routing intervention: changed routes also change contention and subsequent timing. The single-flow cases compare three concrete flows; their differences cannot be attributed to slack alone. Longer routes can still complete faster under contention.

## Artifact provenance

- E3-0: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a/artifacts/20261009-165502`
- E3-1: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a.1/artifacts/20261009-170211`
- E3-2: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a.2/artifacts/20261009-170211`
- E3-3: `/home/yuhao/workspace/slack_ws/HTSIM/experiments/exp3s/exp3.a.3/artifacts/20261009-170211`

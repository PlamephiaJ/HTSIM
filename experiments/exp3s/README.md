# Experiment 3.a: single-flow predefined routing

The four cases share the original E3-0 workload, placement, topology,
messagelet configuration, and simulator settings. Each intervention changes
exactly one metadata flow's routing label from `minimum` to `valiant`.
The selected flow travels group 0 → group 2 → group 1; the other 59 flows
keep their minimum routes. No artificial delay is injected.

| Case | Directory | Routing operation | Validation purpose |
| --- | --- | --- | --- |
| E3-0 | `exp3.a` | All flows minimum | Reference CCT |
| E3-1 | `exp3.a.1` | Only Flow 0 valiant | Benefit of detouring a higher-slack flow |
| E3-2 | `exp3.a.2` | Only Flow 48 valiant | Effect of a different positive-slack flow |
| E3-3 | `exp3.a.3` | Only Flow 46 valiant | Measure any cost of detouring a zero-slack flow |

## Baseline flow identity

Verified against the workload metadata and baseline artifact
`exp3.a/artifacts/20261009-165502`:

| Flow ID | Collective | Phase | GOAL ranks | Baseline slack | Path role |
| ---: | --- | --- | --- | ---: | --- |
| 46 | Tree all-reduce | reduce, chunk 3 | 7 → 5 | 0 µs | Critical |
| 48 | All-to-All (`direct`) | exchange | 8 → 9 | 3.890 µs | Medium-slack |
| 0 | Ring all-reduce | reduce_scatter, chunk 3 | 0 → 1 | 4.686 µs | Higher-slack |

All three flows have baseline ready and issue times of 2 ns, transfer 65536
bytes, and cross group 0 → group 1. These IDs come from `workload.meta.json`,
and are independent of HTSim's internal flow IDs. Slack is the offline
observed-time oracle slack reported by the shared analyzer.

## Run and compare

From the repository root, reuse the existing baseline and run the three cases:

```bash
./experiments/exp3s/exp3.a.1/run.sh
./experiments/exp3s/exp3.a.2/run.sh
./experiments/exp3s/exp3.a.3/run.sh
python3 experiments/exp3s/compare.py \
  --baseline experiments/exp3s/exp3.a/artifacts/20261009-165502
```

Each case runner creates its own timestamped input snapshot, simulator log,
summary, flow routing records, and slack analysis. See the individual case
READMEs for details. Routing labels can be regenerated using the
[shared routing generator](../common/README.md).

`compare.py` verifies matching baseline inputs and simulator settings,
complete flow/lifecycle records, matching CCT measurements, and the expected
runtime routing label and waypoint for every flow. It writes
[routing_comparison.md](routing_comparison.md) and
[routing_comparison.csv](routing_comparison.csv), including CCT, CCT changes,
per-collective graph completion, selected flow duration, and resolved artifact
paths. By default it reads all four `artifacts/latest` links; use `--baseline`,
`--e3-1`, `--e3-2`, and `--e3-3` to pin particular runs. `--output-dir` selects
where to save a separate comparison.

## Measured results

The three intervention artifacts are `20261009-170211` in their respective
case directories. CCT is the maximum graph completion time across all 12
GOAL ranks; ΔCCT is intervention minus baseline.

| Case | CCT (µs) | ΔCCT (µs) | CCT reduction |
| --- | ---: | ---: | ---: |
| E3-0 | 50.952 | 0.000 | 0.00% |
| E3-1 | 48.418 | −2.534 | 4.97% |
| E3-2 | 44.087 | −6.865 | 13.47% |
| E3-3 | 44.633 | −6.319 | 12.40% |

E3-1 confirms a CCT benefit for Flow 0 in this setup. E3-2 provides a larger
benefit despite Flow 48's lower baseline slack. E3-3 also improves CCT, so this
run does **not** demonstrate a cost from detouring the zero-slack flow.
Flow 46's own observed duration falls from 8.379 µs to 5.731 µs; all three
collectives finish sooner in E3-3 than in E3-0.

These observations are consistent with relief of shared-link contention,
but the artifacts do not establish a per-link queueing explanation. A
zero-slack classification describes the baseline execution, while a routing
change alters flow duration, contention, and subsequent scheduling. These
three concrete flow interventions therefore do not establish slack alone
as a predictor of detour benefit. The measurements are single deterministic
runs with the simulator's default seed 13.

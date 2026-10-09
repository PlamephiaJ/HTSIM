# Experiment 3.a: baseline

This is the baseline for experiment 3. It uses the same simulation setup as
`exp2.a.2`, but reads the experiment-3 workload from
`workload/exp3s/exp3.a/workload.bin`. No artificial delay is injected.

Three collectives start together:

- ranks 0–3: ring all-reduce;
- ranks 4–7: tree all-reduce;
- ranks 8–11: direct all-to-all.

## Run

From anywhere in the repository:

```bash
./experiments/exp3s/exp3.a/run.sh
```

Each invocation writes an immutable input snapshot, logs, CSV metrics, and a
summary under `artifacts/YYYYMMDD-HHMMSS/`; `artifacts/latest` points to the
most recent completed run.

After a successful simulation, the runner invokes the shared
`experiments/common/analyze_slack.py` offline analyzer. It writes
`slack_analysis.md` plus `oracle_slack.csv`, `ready_set_slack.csv`, and
`ready_window_summary.csv` in the same timestamped artifact. The CSV files
are stored under `output_metrics/`. `SLACK_READY_WINDOW_NS` in `config.env`
controls the ready-time bin width and defaults to 100 ns.

## Rank placement

The bundled `p6a1h2` Dragonfly has three groups with six hosts each. Group 0
and group 1 carry the workload; group 2 has no assigned rank and provides a
two-global-hop alternative. `rank_placement.json` is the sole source of truth
for the rank-to-endpoint mapping.

Each placement row names the GOAL rank, Dragonfly group, switch within that
group, and host within that switch; all endpoint coordinates are zero-based.
Clear `RANK_PLACEMENT_CONFIG` in `config.env` to omit the simulator flag and
restore the default sequential rank-to-host placement.

The default `ROUTING=SOURCE` executes the per-flow labels in
`routing_configs/default.json`. All 60 flows initially have label `minimum`;
cross-group traffic uses the direct group-0 ↔ group-1 link. Change a flow's label
to `valiant` to make it detour through group 2. See the
[shared manual routing guide](../../common/README.md) for generation and editing.
The runner snapshots the labels and exports `output_metrics/flowRouting.csv`.

## Messagelet configuration

`messagelet_configs/default.json` is a local copy of the simulator default. It
uses one messagelet per parent flow, minimum routing, and disabled decision
tracing. The runner passes it explicitly and snapshots it with every run.

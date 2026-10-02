# Experiment 2.a.2: collective interference with a bypass group

This experiment runs the existing 12-rank
`exp2a1_collective_internal_slack` workload. Three collectives start together:

- ranks 0–3: ring all-reduce;
- ranks 4–7: tree all-reduce;
- ranks 8–11: direct all-to-all.

## Run

From anywhere in the repository:

```bash
./experiments/exp2s/exp2.a.2/run.sh
```

Each invocation writes an immutable input snapshot, logs, CSV metrics, and a
summary under `artifacts/YYYYMMDD-HHMMSS/`; `artifacts/latest` points to the
most recent completed run.

After a successful simulation, the runner also invokes the shared
`experiments/common/analyze_slack.py` offline analyzer. It writes
`slack_analysis.md` plus `oracle_slack.csv`, `ready_set_slack.csv`, and
`ready_window_summary.csv` in that same timestamped artifact (the CSV files
live under `output_metrics/`). `SLACK_READY_WINDOW_NS` in `config.env`
controls the ready-time bin width and defaults to 100 ns.

## Rank placement

The bundled `p6a1h2` Dragonfly has three groups with six hosts each. Group 0
and group 1 carry the workload; group 2 has no assigned rank and provides the
same kind of two-global-hop alternative used by experiment 1.
`rank_placement.json` is the sole source of truth for the rank-to-endpoint
mapping. Run summaries derive their group membership and physical HTSIM host
IDs directly from the snapshotted JSON instead of duplicating the rank lists
in configuration text.

Each placement row explicitly names the GOAL rank, Dragonfly group, switch
within that group, and host within that switch; all three endpoint coordinates
are zero-based. Clear `RANK_PLACEMENT_CONFIG` in `config.env` to omit the
simulator flag and restore the default sequential rank-to-host placement.

The default `ROUTING=MINIMAL` uses the direct group-0 ↔ group-1 link. Change it
to `UGAL_L` when the experiment should be allowed to select group 2 as the
two-global-hop bypass.

## Messagelet configuration

`messagelet_configs/default.json` is a local copy of the simulator default. It
uses one messagelet per parent flow, minimum routing, and disabled decision
tracing. The runner passes it explicitly and saves it with every run.

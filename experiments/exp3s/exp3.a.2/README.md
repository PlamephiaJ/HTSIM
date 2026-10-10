# Experiment 3.a.2: E3-2, detour flow 48

Only metadata Flow **48** (medium-slack all-to-all flow) uses `valiant`; all other 59
flows use `minimum`. This is the same workload, topology, rank placement,
messagelet configuration, and simulator settings as E3-0 (`../exp3.a`).
No artificial delay is injected. The selected flow crosses group 0 → 1 and
its predefined valiant route goes through group 2.

Run from anywhere:

```bash
./experiments/exp3s/exp3.a.2/run.sh
```

Each run snapshots its inputs and writes logs, completion/slack metrics, and
`output_metrics/flowRouting.csv` under `artifacts/<timestamp>/`.
`artifacts/latest` points to the latest run. The routing CSV records metadata
flow IDs and the assigned intermediate group.

See [the case comparison](../README.md) for baseline flow identities, validation,
and the command to regenerate the comparison from completed artifacts.

# Experiment 1: Dragonfly minimal-path contention

This experiment runs the serialized `workload/exp1/workload.bin` directly on a
three-group Dragonfly.
Ranks 0, 1, and 2 send 64 KiB concurrently to ranks 3, 4, and 5.

## Run

From anywhere in the repository:

```bash
./experiments/exp1/run.sh
```

Edit `config.env` to change the routing or transport settings. The default is
`ROUTING=MINIMAL`. To allow adaptive selection of the longer path through group
2, set `ROUTING=UGAL_L` and run the script again.

The runner discovers the HTSIM root through Git, so it does not depend on a fixed
directory depth. Shared paths in `config.env` are root-relative, while the bundled
topology and generated artifacts follow the experiment directory. The experiment
directory can therefore be moved under another directory within this worktree.

The summary title, rank placement, and flow description come from
`EXPERIMENT_NAME`, `EXPERIMENT_DESCRIPTION`, `RANK_PLACEMENT`, and
`FLOW_PATTERN` in `config.env`; the shared `experiments/common/summarize.py` contains no experiment-specific mapping.

Each invocation creates a new directory named
`artifacts/YYYYMMDD-HHMMSS/`. It contains:

- `config_snapshot/`: the exact config, binary workload, readable GOAL and workload metadata when available, topology, runner, and shared summarizer used;
- `summary.md`: graph makespan and critical rank, per-rank finish times, configuration, network settings, flow results, aggregate FCT, and reproducibility information;
- `command.txt`: the fully resolved simulator command;
- `simulator.log` and `logout.dat`: simulator logs;
- `output_metrics/`: simulator CSV metrics, including per-node GOAL DAG lifecycle data in `flow_dag_info.csv`;
- timestamps, exit status, Git revision, and worktree status.

`artifacts/latest` points to the latest completed invocation, so the usual result entry point is `artifacts/latest/summary.md`.

## Topology and rank placement

The topology uses `p=3`, `a=1`, and `h=2`, hence `g=a*h+1=3` groups and
9 hosts total:

| Group | Switch | Hosts/ranks | Role |
| --- | --- | --- | --- |
| 0 | 0 | 0, 1, 2 | senders |
| 1 | 1 | 3, 4, 5 | receivers |
| 2 | 2 | 6, 7, 8 | intermediate group for the long path |

The direct group-0 to group-1 global link is the one-global-hop minimal path,
so all three data flows contend on it. The alternative group-0 to group-2 to
group-1 route has two global hops.

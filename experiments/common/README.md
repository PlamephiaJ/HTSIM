# Manual flow routing

`generate_routing_config.py` reads `workload.meta.json` and writes a separate
`routing_configs/default.json` under the experiment directory. Each computation
graph flow has one editable `label`: `minimum` or `valiant`. The file records
`flow_count`, all metadata flow IDs, and the GOAL send identity for each flow.
These IDs are independent of HTSim's internal flow IDs and GOAL tags.

From the repository root:

```bash
# Create an initial file (all flows minimum). Existing files are protected.
python3 experiments/common/generate_routing_config.py experiments/exp3s/exp3.a

# Replace it with a manual assignment: flow 0 and flow 24 take valiant paths.
python3 experiments/common/generate_routing_config.py experiments/exp3s/exp3.a \
  --assign 0=valiant --assign 24=valiant --force

# Write a separate preset, assigning every flow valiant.
python3 experiments/common/generate_routing_config.py experiments/exp3s/exp3.a \
  --label valiant --output experiments/exp3s/exp3.a/routing_configs/all_valiant.json
```

Alternatively, edit only `flows.<flow_id>.label` in the JSON. For example:

```json
{
  "schema_version": 1,
  "workload_name": "example",
  "flow_count": 1,
  "flows": {
    "41": {
      "src": 0,
      "dst": 1,
      "tag": 123,
      "bytes": 4096,
      "send_task_id": 7,
      "label": "valiant"
    }
  }
}
```

The script locates metadata using `WORKLOAD` in `config.env`. `--meta PATH`
overrides that location. Legacy workloads without a `.meta.json` need metadata
matching their existing binary before labels can be generated. Regenerate the
routing file after changing the computation graph; the runtime rejects missing
sends, mismatched endpoints/tags/sizes, unknown labels, and incorrect flow counts.

Set these fields in the experiment's `config.env`:

```bash
ROUTING=SOURCE
ROUTING_CONFIG="${SCRIPT_DIR}/routing_configs/default.json"
```

Exp2 and exp3 already select SOURCE with their generated all-minimum presets.
Exp1 runners also accept the optional `ROUTING_CONFIG` setting. All runners copy
the selected JSON to `artifacts/<run>/config_snapshot/routing_config.json` and
pass it as `-routing_config`. An empty `ROUTING_CONFIG` preserves the simulator's
existing default routing behavior. Build the updated simulator once:

```bash
cmake -S htsim/sim -B htsim/sim/build
cmake --build htsim/sim/build --target htsim_uec_df --parallel 4
./experiments/exp3s/exp3.a/run.sh
```

The runtime matches the original GOAL `(source rank, send task ID)` before rank
placement, then computes a path using the physical endpoints. One label applies
to the entire flow, including all its messagelets. With `messagelet.size_bytes=0`,
each flow is one messagelet. New packets reuse its routing mark; retransmissions
reuse the original packet's mark and restart the waypoint phase.

- `minimum`: use the existing SOURCE two-switch encoding for a minimum path.
- `valiant`: select a group distinct from source and destination groups, then
  travel minimally to its incoming switch and minimally to the destination.
  Candidate groups are sorted by ID; `flow_id % candidate_count` selects one
  deterministically. On the three-group experimental topology, cross-group
  traffic between groups 0 and 1 therefore detours through group 2. Same-switch
  and same-group flows also leave their group before returning. If no distinct
  group exists, the runtime reports an error instead of silently taking minimum.

Manual valiant marks reserve bit 31 of `path_id` for waypoint routing and store
the waypoint switch ID in the remaining bits. Each packet tracks whether it has
reached that switch; the mark itself remains unchanged. SOURCE topologies are
limited to 32768 switches to keep this encoding distinct from legacy paths.
ACK/NACK and receiver credit packets use minimum return paths. GOAL SOURCE runs
compute these return paths directly and do not need `host_table/*.lt` files.

`output_metrics/flowRouting.csv` records metadata flow IDs, labels, path IDs,
physical endpoints, and selected intermediate groups. Enable
`logging.trace_decisions` in the messagelet config for messagelet decision logs.
Decision logs reflect assigned routes; they are not a per-hop packet trace.

Validation:

```bash
python3 -m unittest discover -s experiments/common -p test_routing_config.py -v
```

The suite runs actual simulations to compare minimum and valiant completion
times for cross-group, same-switch, and multi-switch flows, and checks malformed
configs and receiver-driven control packets.

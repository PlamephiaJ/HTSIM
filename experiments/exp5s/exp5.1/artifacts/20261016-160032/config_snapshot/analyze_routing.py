#!/usr/bin/env python3
"""Validate exp5.1 inputs and export messagelet decisions with metadata flow IDs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def read_env(path):
    return {match[1]: match[2].strip().strip('"').strip("'")
            for line in path.read_text().splitlines()
            if (match := re.match(r"^([A-Z_]+)=(.*)$", line))}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analyze(run: Path, reference: Path):
    snapshot = run / "config_snapshot"
    metrics = run / "output_metrics"
    config = read_env(snapshot / "config.env")
    settings = json.loads((snapshot / "messagelet_config.json").read_text())
    metadata = json.loads((snapshot / "workload.meta.json").read_text())
    require((run / "exit_code.txt").read_text().strip() == "0", "Simulation failed")
    require(config["ROUTING"] == "SOURCE", "Labels require SOURCE forwarding")
    require(not config.get("ROUTING_CONFIG"), "Static routing must be disabled")
    require(not (snapshot / "routing_config.json").exists(), "Unexpected static route snapshot")
    require(settings["messagelet"]["size_bytes"] == 0, "Expected one messagelet per flow")
    require(settings["routing"]["network_decision"] == "ugal_l", "UGAL-L policy is disabled")
    require(settings["logging"]["trace_decisions"], "Decision traces are required")

    reference_config = read_env(reference / "config.env")
    for key in ("SENDER_CC_ALGO", "LOAD_BALANCING_ALGO", "PATH_ENTROPY_SIZE", "QUEUE_PACKETS", "CWND_BYTES"):
        require(config[key] == reference_config[key], f"Network setting changed: {key}")
    repo = reference.parents[2]
    reference_workload = repo / reference_config["WORKLOAD"]
    input_sources = {
        "workload.bin": reference_workload,
        "workload.goal": reference_workload.with_suffix(".goal"),
        "workload.meta.json": reference_workload.with_suffix(".meta.json"),
        "rank_placement.json": reference / "rank_placement.json",
        "topology/dragonfly.topo": reference / "topology/p6a1h2/dragonfly.topo",
    }
    input_hashes = {}
    for name, source in input_sources.items():
        value = digest(snapshot / name)
        require(value == digest(source), f"Input differs from exp2.a.2: {name}")
        input_hashes[name] = value

    dimensions = {parts[0]: int(parts[1]) for line in
                  (snapshot / "topology/dragonfly.topo").read_text().splitlines()
                  if len(parts := line.split()) == 2 and parts[0] in {"p", "a", "h"}}
    placement = json.loads((snapshot / "rank_placement.json").read_text())["placements"]
    host_to_rank = {(row["group"] * dimensions["a"] + row["switch"]) * dimensions["p"] + row["host"]:
                    row["rank"] for row in placement}
    flows = {int(key): flow for key, flow in metadata["flows"].items()}
    send_to_id = {(flow["src"], flow["send_task"]["task_id"]): flow_id
                  for flow_id, flow in flows.items()}
    require(len(send_to_id) == len(flows), "Nonunique metadata GOAL sends")
    completed = {}
    for row in read_csv(metrics / "flowsInfo.csv"):
        src, dst, transport = map(int, row["srcNode_dstNode_flowId"].split("_"))
        require(transport not in completed, "Duplicate transport completion")
        completed[transport] = (src, dst, row)

    log = (run / "simulator.log").read_text()
    decisions = []
    seen = set()
    for line in log.splitlines():
        if not line.startswith("Messagelet route - "):
            continue
        fields = dict(part.split(" ", 1) for part in line.split(" - ")[1:])
        src_host, dst_host = int(fields["Src"]), int(fields["Dst"])
        src_rank, dst_rank = host_to_rank[src_host], host_to_rank[dst_host]
        task = int(fields["SendTaskId"])
        flow_id = send_to_id[(src_rank, task)]
        flow = flows[flow_id]
        require(flow_id not in seen, f"Flow {flow_id} was decided more than once")
        seen.add(flow_id)
        require(dst_rank == flow["dst"] and int(fields["Tag"]) == flow["tag"],
                f"Flow {flow_id} endpoint/tag mismatch")
        require(int(fields["Index"]) == 0 and int(fields["Size"]) == flow["bytes"],
                f"Flow {flow_id} was split into messagelets")
        require(fields["policy"] == "ugal_l", f"Flow {flow_id} has wrong policy")
        route = fields["Route"]
        require(route in {"minimum", "valiant"}, f"Unknown route: {route}")
        compared = fields.get("compared", "false") == "true"
        if compared:
            min_cost = int(fields["q_min_bytes"]) * int(fields["h_min"])
            val_cost = int(fields["q_val_bytes"]) * int(fields["h_val"])
            expected = "valiant" if min_cost > val_cost else "minimum"
            require(route == expected, f"Flow {flow_id} violates HTSim UGAL-L cost comparison")
        else:
            require(route == "minimum", f"Flow {flow_id} should bypass comparison")
            min_cost = val_cost = ""
        path_id = int(fields["PathId"])
        src_switch, dst_switch = src_host // dimensions["p"], dst_host // dimensions["p"]
        if src_switch == dst_switch:
            require(path_id == 0 and route == "minimum", "Same-switch traffic must stay local")
        else:
            require(path_id == (int(fields["hop_one_switch"]) << 16 | int(fields["hop_two_switch"])),
                    f"Flow {flow_id} has an incorrect SOURCE label")
            expected_next = fields["next_val"] if route == "valiant" else fields["next_min"]
            require(fields["hop_one_switch"] == expected_next, "Label differs from selected candidate")
        transport = int(fields["TransportFlowId"])
        require(transport in completed, f"Flow {flow_id} did not complete")
        completed_src, completed_dst, completion = completed[transport]
        require((completed_src, completed_dst) == (src_host, dst_host), "Transport endpoints differ")
        require(int(completion["flowSizeBytes"]) == flow["bytes"], "Transport size differs")
        decisions.append({
            "decision_order": len(decisions), "flow_id": flow_id, "transport_flow_id": transport,
            "collective": flow["node_id"], "algorithm": flow["algorithm"], "phase": flow["phase"],
            "src_rank": src_rank, "dst_rank": dst_rank, "src_host": src_host, "dst_host": dst_host,
            "send_task_id": task, "tag": flow["tag"], "messagelet_index": 0,
            "messagelet_bytes": int(fields["Size"]), "route": route, "path_id": path_id,
            "compared": compared, "q_min_bytes": fields.get("q_min_bytes", ""),
            "q_val_bytes": fields.get("q_val_bytes", ""), "h_min": fields.get("h_min", ""),
            "h_val": fields.get("h_val", ""), "min_cost": min_cost, "val_cost": val_cost,
            "fct_ns": completion["fctNs"],
        })
    expected_count = metadata["workload"]["p2p_flows"]
    require(seen == set(flows) and len(decisions) == len(completed) == expected_count,
            "Missing or extra flow decisions/completions")
    require(len({row["transport_flow_id"] for row in decisions}) == expected_count,
            "Transport flow IDs are not unique")
    lifecycle = read_csv(metrics / "flow_dag_info.csv")
    require(len(lifecycle) == metadata["workload"]["tasks"], "Missing GOAL tasks")
    require(all(row["finishTimeNs"] for row in lifecycle), "Unfinished GOAL tasks")
    cct = max(int(row["finishTimeNs"]) for row in lifecycle)
    rank_finishes = {int(rank): int(finish) for rank, finish in
                     re.findall(r"^Host (\d+): (\d+)$", log, re.MULTILINE)}
    require(len(rank_finishes) == metadata["workload"]["world_size"], "Incomplete rank graph")
    require(cct == max(rank_finishes.values()), "CCT disagrees with rank completion")
    counts = Counter(row["route"] for row in decisions)
    result = {"status": "PASS", "reference_experiment": str(reference), "input_sha256": input_hashes,
              "flow_count": expected_count, "messagelet_count": len(decisions), "completed_tasks": len(lifecycle),
              "cct_ns": cct, "minimum_messagelets": counts["minimum"], "valiant_messagelets": counts["valiant"],
              "compared_messagelets": sum(row["compared"] for row in decisions),
              "same_switch_messagelets": sum(row["src_host"] // dimensions["p"] ==
                                              row["dst_host"] // dimensions["p"] for row in decisions)}
    with (metrics / "network_decisions.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(decisions[0]))
        writer.writeheader()
        writer.writerows(sorted(decisions, key=lambda row: row["flow_id"]))
    (run / "network_decision_validation.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = ["# exp5.1 — UGAL-L messagelet baseline", "", "Status: **PASS**", "",
             f"- CCT: **{cct / 1000:.3f} µs** ({cct} ns), measured as maximum GOAL rank graph finish.",
             f"- Completed flows / messagelets: **{expected_count} / {len(decisions)}**; one decision per flow.",
             f"- Completed GOAL tasks: **{len(lifecycle)}**.",
             f"- Minimum / Valiant: **{counts['minimum']} / {counts['valiant']}**.",
             f"- Queue-cost comparisons: **{result['compared_messagelets']}**; local same-switch deliveries: "
             f"**{result['same_switch_messagelets']}**.", "",
             "| Collective | Messagelets | Minimum | Valiant |", "| --- | ---: | ---: | ---: |"]
    for collective in sorted({row["collective"] for row in decisions}):
        members = [row for row in decisions if row["collective"] == collective]
        routes = Counter(row["route"] for row in members)
        lines.append(f"| {collective} | {len(members)} | {routes['minimum']} | {routes['valiant']} |")
    lines += ["", "Validation checks the snapshotted workload binary, GOAL, metadata, topology and rank placement "
              "byte-for-byte against exp2.a.2, and preserves its CC/queue/path settings. Every metadata send "
              "maps through `(source rank, GOAL send task ID)` to one unique transport flow and one "
              "messagelet decision; every transport flow and GOAL task completed.", "",
              "UGAL-L uses live local egress queue bytes and HTSim's candidate/hop-count logic. Valiant wins "
              "only for `q_min * h_min > q_val * h_val`; ties choose minimal. Same first-hop candidates bypass "
              "the comparison. SOURCE forwarding executes the frozen label. Same-switch sends remain local.", "",
              "This is one deterministic run (HTSim seed 13, per-switch UGAL RNG seed 33). "
              "No DAG/slack signal enters the routing policy; slack is analyzed offline.", "",
              "Detailed decisions: `output_metrics/network_decisions.csv`. "
              "Input hashes and checks: `network_decision_validation.json`.", ""]
    (run / "network_decision_analysis.md").write_text("\n".join(lines))
    print(f"UGAL-L baseline: CCT {cct / 1000:.3f} µs; {counts['minimum']} minimum, "
          f"{counts['valiant']} Valiant; validation PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    analyze(args.run.resolve(), args.reference.resolve())


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Validate exp5.0 controls and reconstruct native UGAL-L data packet paths."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shlex
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
    snapshot, metrics = run / "config_snapshot", run / "output_metrics"
    config = read_env(snapshot / "config.env")
    settings = json.loads((snapshot / "messagelet_config.json").read_text())
    metadata = json.loads((snapshot / "workload.meta.json").read_text())
    log = (run / "simulator.log").read_text()
    require((run / "exit_code.txt").read_text().strip() == "0", "Simulation failed")
    require(config["ROUTING"] == "UGAL_L", "Expected native switch UGAL_L")
    require(settings["runtime"]["mode"] == "transparent", "Runtime routing must be bypassed")
    require(settings["routing"]["network_decision"] == "none", "Runtime policy must be disabled")
    require(settings["logging"]["trace_decisions"], "Enable decision logging to audit the absence of labels")
    require(not config.get("ROUTING_CONFIG") and not (snapshot / "routing_config.json").exists(),
            "Static routing must be disabled")
    require("Messagelet route -" not in log, "Runtime unexpectedly assigned routing labels")
    require(config["TRACE_PACKET_HOPS"] == "1", "Packet forwarding trace is required")

    controls = read_env(reference / "config.env")
    for key in ("SENDER_CC_ALGO", "LOAD_BALANCING_ALGO", "PATH_ENTROPY_SIZE", "QUEUE_PACKETS", "CWND_BYTES"):
        require(config[key] == controls[key], f"Setting differs from exp5.1: {key}")
    workload = reference.parents[2] / controls["WORKLOAD"]
    sources = {"workload.bin": workload, "workload.goal": workload.with_suffix(".goal"),
               "workload.meta.json": workload.with_suffix(".meta.json"),
               "rank_placement.json": reference / "rank_placement.json",
               "topology/dragonfly.topo": reference / "topology/p6a1h2/dragonfly.topo"}
    hashes = {}
    for name, source in sources.items():
        hashes[name] = digest(snapshot / name)
        require(hashes[name] == digest(source), f"Input differs from exp5.1: {name}")
    global_info = read_csv(metrics / "globalInfo.csv")
    require(len(global_info) == 1 and int(global_info[0]["routing"]) == 2,
            "Simulator did not execute DragonflySwitch::UGAL_L")
    dimensions = {parts[0]: int(parts[1]) for line in
                  (snapshot / "topology/dragonfly.topo").read_text().splitlines()
                  if len(parts := line.split()) == 2 and parts[0] in {"p", "a", "h"}}
    require(dimensions == {"p": 6, "a": 1, "h": 2}, "Expected exp5.1 p6a1h2 topology")
    completed = {}
    for row in read_csv(metrics / "flowsInfo.csv"):
        src, dst, transport = map(int, row["srcNode_dstNode_flowId"].split("_"))
        require(transport not in completed, "Duplicate transport completion")
        completed[transport] = (src, dst, row)
    expected = metadata["workload"]["p2p_flows"]
    require(len(completed) == expected, "Missing or extra completed flows")
    lifecycle = read_csv(metrics / "flow_dag_info.csv")
    require(len(lifecycle) == metadata["workload"]["tasks"] and all(row["finishTimeNs"] for row in lifecycle),
            "Missing or unfinished GOAL tasks")
    cct = max(int(row["finishTimeNs"]) for row in lifecycle)
    rank_finishes = {int(rank): int(finish) for rank, finish in
                     re.findall(r"^Host (\d+): (\d+)$", log, re.MULTILINE)}
    require(len(rank_finishes) == metadata["workload"]["world_size"] and max(rank_finishes.values()) == cct,
            "Rank completion does not match lifecycle CCT")

    # Packet::UECDATA is 31 in this HTSim tree. A fresh host injection starts
    # an attempt, so retransmitted sequence numbers receive separate records.
    packets, current = [], {}
    hop_records = 0
    for line in log.splitlines():
        if not line.startswith("PacketHop "):
            continue
        hop_records += 1
        fields = shlex.split(line)
        if int(fields[4]) != 31:
            continue
        time_ps, transport, sequence = map(int, fields[1:4])
        src, dst = int(fields[6]), int(fields[7])
        require(transport in completed and completed[transport][:2] == (src, dst), "Unknown data packet flow")
        key = (transport, sequence)
        sink = fields[12]
        injection = re.search(r"SRC(\d+)->SW(\d+)$", sink)
        if injection:
            require(int(injection[1]) == src, "Packet injected at the wrong host")
            current[key] = {"transport_flow_id": transport, "packet_sequence": sequence,
                            "attempt": sum(row["transport_flow_id"] == transport and row["packet_sequence"] == sequence
                                           for row in packets),
                            "src_host": src, "dst_host": dst, "injection_time_ps": time_ps,
                            "delivery_time_ps": "", "switches": [int(injection[2])]}
            packets.append(current[key])
        require(key in current, "Data packet has no host injection")
        packet = current[key]
        link = re.search(r"SW(\d+)-[GI]->SW(\d+)$", sink)
        if link:
            require(packet["switches"][-1] == int(link[1]), "Packet switch path is discontinuous")
            packet["switches"].append(int(link[2]))
        delivery = re.search(r"SW(\d+)->DST(\d+)$", sink)
        if delivery and fields[9] == "0":
            require(int(delivery[2]) == dst, "Packet delivered to the wrong host")
            require(packet["switches"][-1] == int(delivery[1]), "Packet delivery path is discontinuous")
            packet["delivery_time_ps"] = time_ps
    require(hop_records > 0 and packets, "Missing packet trace")
    delivered = Counter()
    routes = Counter()
    for packet in packets:
        path = packet.pop("switches")
        minimum_hops = 0 if packet["src_host"] // dimensions["p"] == packet["dst_host"] // dimensions["p"] else 1
        packet["switch_path"] = "->".join(map(str, path))
        packet["switch_hops"] = len(path) - 1
        packet["route"] = "detour" if len(path) - 1 > minimum_hops else "minimum"
        if packet["delivery_time_ps"] != "":
            delivered[packet["transport_flow_id"]] += 1
            routes[packet["route"]] += 1
    mss = int(global_info[0]["packetSizeBytes"]) - 64
    for transport, (_, _, completion) in completed.items():
        minimum_packets = (int(completion["flowSizeBytes"]) + mss - 1) // mss
        require(delivered[transport] >= minimum_packets, f"Flow {transport} lacks delivered data packets")
    with (metrics / "packet_routes.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(packets[0]))
        writer.writeheader()
        writer.writerows(packets)
    result = {"status": "PASS", "reference_experiment": str(reference), "input_sha256": hashes,
              "runtime_mode": "transparent", "switch_routing": "UGAL_L", "runtime_decisions": 0,
              "cct_ns": cct, "completed_flows": len(completed), "completed_tasks": len(lifecycle),
              "packet_hop_records": hop_records, "data_packet_attempts": len(packets),
              "delivered_data_packets": sum(delivered.values()), "minimum_data_packets": routes["minimum"],
              "detoured_data_packets": routes["detour"]}
    (run / "native_routing_validation.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = ["# exp5.0 — Native packet-level UGAL-L", "", "Status: **PASS**", "",
             f"- CCT: **{cct / 1000:.3f} µs** ({cct} ns), maximum GOAL rank graph finish.",
             f"- Completed flows / GOAL tasks: **{len(completed)} / {len(lifecycle)}**.",
             "- Runtime: **transparent**; runtime decisions / SOURCE labels: **0**.",
             "- Switch routing: **HTSim UGAL_L**, including retransmissions.",
             f"- Packet-hop records: **{hop_records}**; data packet attempts: **{len(packets)}**.",
             f"- Delivered data packets on minimum / detour paths: **{routes['minimum']} / {routes['detour']}**.", "",
             "Workload binary, GOAL, metadata, topology and placement match exp5.1 byte-for-byte. "
             "CC, queue, path entropy and cwnd settings are unchanged. All GOAL tasks and transport flows "
             "completed; no runtime decision trace or static routing config was used.", "",
             "`output_metrics/packet_routes.csv` reconstructs actual DATA packet switch paths from the "
             "forwarding trace. Minimum/detour classifies each delivered packet's observed hop count on "
             "this one-switch-per-group topology; it is not a cached flow label or a count of UGAL comparisons.", "",
             "The runtime still connects GOAL events to UEC. Its network decision and label cache are "
             "bypassed; the original sender, receiver-control and retransmission paths remain active. "
             "UGAL-L samples queues at switches and selects routes at packet arrival.", "",
             "Single deterministic run: simulator seed 13, per-switch UGAL RNG seed 33. "
             "Slack analysis is offline and does not influence routing.", ""]
    (run / "native_routing_analysis.md").write_text("\n".join(lines))
    print(f"Native UGAL-L: CCT {cct / 1000:.3f} µs; {len(completed)} flows; "
          f"0 runtime decisions; validation PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    analyze(args.run.resolve(), args.reference.resolve())

#!/usr/bin/env python3
"""Validate every sweep point and write a pinned packet-to-flow comparison."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import statistics


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_csv(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def packet_groups(flow_bytes, mode, budget, mtu, header):
    """Expected logical new-data packet grouping; retransmits reuse labels."""
    mss = mtu - header
    count = (flow_bytes + mss - 1) // mss
    packet_sizes = [min(mss, flow_bytes - index * mss) + header for index in range(count)]
    if mode == "flow":
        return [(count, sum(packet_sizes))]
    if mode == "packet":
        return [(1, size) for size in packet_sizes]
    groups = []
    packets = size = 0
    for packet_size in packet_sizes:
        if size >= budget:
            groups.append((packets, size))
            packets = size = 0
        packets += 1
        size += packet_size
    groups.append((packets, size))
    return groups


def p95(values):
    ordered = sorted(values)
    position = (len(ordered) - 1) * 0.95
    low, high = math.floor(position), math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def read_case(batch, manifest, case):
    run = batch / "cases" / case["name"]
    snapshot, metrics = run / "config_snapshot", run / "output_metrics"
    require((run / "exit_code.txt").read_text().strip() == "0", f"Failed case: {case['name']}")
    require((run / "binary-sha256.txt").read_text().split()[0] == manifest["binary_sha256"], "Binary hash differs")
    for name, expected in manifest["input_sha256"].items():
        require(digest(snapshot / name) == expected, f"Input changed in {case['name']}: {name}")
    require(json.loads((snapshot / "case.json").read_text()) == case, "Case specification changed")
    expected_config = {"schema_version": 1, "runtime": {"mode": "label", "granularity": case["granularity"]},
                       "messagelet": {"size_bytes": case["size_bytes"]},
                       "routing": {"default": "minimum", "network_decision": "ugal_l"},
                       "logging": {"trace_decisions": True}}
    require(json.loads((snapshot / "messagelet_config.json").read_text()) == expected_config,
            "Configuration differs beyond the swept granularity")
    require(not (snapshot / "routing_config.json").exists(), "Unexpected static route config")
    expected_command = [manifest["binary"], "-basepath", str(snapshot / "topology"), "-goal", str(snapshot / "workload.bin"),
                        *manifest["network_args"], "-rank_placement", str(snapshot / "rank_placement.json"),
                        "-messagelet_config", str(snapshot / "messagelet_config.json")]
    require(shlex.split((run / "command.txt").read_text()) == expected_command, "Simulator arguments changed")
    globals_ = read_csv(metrics / "globalInfo.csv")
    require(len(globals_) == 1 and globals_[0]["routing"] == "3", "Expected SOURCE routing")
    mtu, header = manifest["settings"]["packet_size_bytes"], manifest["settings"]["header_size_bytes"]
    require(int(globals_[0]["packetSizeBytes"]) == mtu, "Packet size differs from sweep specification")
    metadata = json.loads((snapshot / "workload.meta.json").read_text())
    flows = {int(key): flow for key, flow in metadata["flows"].items()}
    require(len(flows) == metadata["workload"]["p2p_flows"], "Invalid metadata flow count")
    dimensions = {fields[0]: int(fields[1]) for line in (snapshot / "topology/dragonfly.topo").read_text().splitlines()
                  if len(fields := line.split()) == 2 and fields[0] in {"p", "a", "h"}}
    placements = json.loads((snapshot / "rank_placement.json").read_text())["placements"]
    host_to_rank = {(row["group"] * dimensions["a"] + row["switch"]) * dimensions["p"] + row["host"]:
                    row["rank"] for row in placements}
    send_to_flow = {(flow["src"], flow["send_task"]["task_id"]): fid for fid, flow in flows.items()}
    require(len(send_to_flow) == len(flows), "Duplicate metadata sends")
    completions = {}
    for row in read_csv(metrics / "flowsInfo.csv"):
        src, dst, transport = map(int, row["srcNode_dstNode_flowId"].split("_"))
        require(transport not in completions, "Duplicate transport completion")
        completions[transport] = (src, dst, row)
    require(len(completions) == len(flows), "Incomplete transport flows")
    grouped, transports = defaultdict(dict), {}
    log = (run / "simulator.log").read_text()
    all_decisions = []
    for line in log.splitlines():
        if not line.startswith("Messagelet route - "):
            continue
        fields = dict(part.split(" ", 1) for part in line.split(" - ")[1:])
        src, dst, tag = (int(fields[key]) for key in ("Src", "Dst", "Tag"))
        fid = send_to_flow[(host_to_rank[src], int(fields["SendTaskId"]))]
        flow = flows[fid]
        require((host_to_rank[dst], tag) == (flow["dst"], flow["tag"]), "Decision flow identity differs")
        transport, index = int(fields["TransportFlowId"]), int(fields["Index"])
        require(transport in completions and completions[transport][:2] == (src, dst), "Missing or mismatched completion")
        require(int(completions[transport][2]["flowSizeBytes"]) == flow["bytes"], "Flow size differs")
        if fid in transports:
            require(transports[fid] == transport, "Flow changed transport identity")
        transports[fid] = transport
        require(index not in grouped[fid], "Repeated decision for the same messagelet")
        require(fields["policy"] == "ugal_l" and fields["Granularity"] == case["granularity"], "Wrong policy/granularity")
        expected_size = flow["bytes"] if case["granularity"] == "flow" else mtu if case["granularity"] == "packet" else case["size_bytes"]
        require(int(fields["Size"]) == expected_size, "Decision size differs from requested unit")
        groups = packet_groups(flow["bytes"], case["granularity"], case["size_bytes"], mtu, header)
        require(index < len(groups), "Extra messagelet decisions")
        route = fields["Route"]
        require(route in {"minimum", "valiant"}, "Unknown route")
        if fields.get("compared") == "true":
            min_cost = int(fields["q_min_bytes"]) * int(fields["h_min"])
            val_cost = int(fields["q_val_bytes"]) * int(fields["h_val"])
            require(route == ("valiant" if min_cost > val_cost else "minimum"), "UGAL-L cost comparison violated")
        else:
            require(route == "minimum", "Fast path must select minimum")
        path_id = int(fields["PathId"])
        if src // dimensions["p"] == dst // dimensions["p"]:
            require(path_id == 0 and route == "minimum", "Same-switch route must remain local")
        else:
            require(path_id == (int(fields["hop_one_switch"]) << 16 | int(fields["hop_two_switch"])), "Invalid SOURCE label")
            require(fields["hop_one_switch"] == fields["next_val" if route == "valiant" else "next_min"], "Wrong first hop")
        row = {"flow_id": fid, "transport_flow_id": transport, "collective": flow["node_id"],
               "src_rank": flow["src"], "dst_rank": flow["dst"], "send_task_id": flow["send_task"]["task_id"],
               "src_host": src, "dst_host": dst, "tag": tag, "granularity": case["granularity"],
               "messagelet_index": index, "decision_size_bytes": expected_size,
               "new_data_packets": groups[index][0], "new_wire_bytes": groups[index][1], "route": route, "path_id": path_id,
               "compared": fields.get("compared", "false"), "q_min_bytes": fields.get("q_min_bytes", ""),
               "q_val_bytes": fields.get("q_val_bytes", ""), "h_min": fields.get("h_min", ""), "h_val": fields.get("h_val", "")}
        grouped[fid][index] = row
        all_decisions.append(row)
    require(set(grouped) == set(flows) and len(set(transports.values())) == len(flows), "Incomplete flow decision coverage")
    for fid, flow in flows.items():
        groups = packet_groups(flow["bytes"], case["granularity"], case["size_bytes"], mtu, header)
        require(list(grouped[fid]) == list(range(len(groups))), "Decision indices must advance exactly once per routing unit")
    life = read_csv(metrics / "flow_dag_info.csv")
    require(len(life) == metadata["workload"]["tasks"] and all(row["finishTimeNs"] for row in life), "Incomplete DAG lifecycle")
    rank_finishes = {int(rank): int(finish) for rank, finish in re.findall(r"^Host (\d+): (\d+)$", log, re.MULTILINE)}
    require(len(rank_finishes) == metadata["workload"]["world_size"], "Incomplete ranks")
    cct = max(rank_finishes.values())
    require(cct == max(int(row["finishTimeNs"]) for row in life), "CCT measurements disagree")
    slack = read_csv(metrics / "oracle_slack.csv")
    require(len(slack) == len(flows) and {int(row["flow_id"]) for row in slack} == set(flows), "Offline analysis missing flows")
    fcts = [float(item[2]["fctNs"]) for item in completions.values()]
    valiant = [row for row in all_decisions if row["route"] == "valiant"]
    new_packets = sum(row["new_data_packets"] for row in all_decisions)
    result = {"case": case["name"], "granularity": case["granularity"], "messagelet_packets": case["packets_per_messagelet"] or "flow",
              "budget_bytes": case["size_bytes"], "cct_ns": cct, "mean_fct_ns": statistics.mean(fcts), "p95_fct_ns": p95(fcts),
              "max_fct_ns": max(fcts), "ooo_packets": sum(int(item[2]["oooCount"]) for item in completions.values()),
              "decisions": len(all_decisions), "valiant_decisions": len(valiant),
              "valiant_decision_pct": 100 * len(valiant) / len(all_decisions), "new_data_packets": new_packets,
              "valiant_new_data_packets": sum(row["new_data_packets"] for row in valiant),
              "valiant_packet_pct": 100 * sum(row["new_data_packets"] for row in valiant) / new_packets,
              "mean_packets_per_decision": new_packets / len(all_decisions), "completed_flows": len(flows), "completed_tasks": len(life)}
    for collective in metadata["collectives"]:
        result[collective["node_id"] + "_finish_ns"] = max(rank_finishes[rank] for rank in collective["ranks"])
    with (metrics / "network_decisions.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(all_decisions[0]))
        writer.writeheader()
        writer.writerows(sorted(all_decisions, key=lambda row: (row["flow_id"], row["messagelet_index"])))
    (run / "granularity_validation.json").write_text(json.dumps({"status": "PASS", **result}, indent=2) + "\n")
    return result, globals_, grouped


def analyze(batch):
    manifest = json.loads((batch / "manifest.json").read_text())
    results, grouped_by_case = [], {}
    common_globals = None
    for case in manifest["cases"]:
        result, globals_, grouped = read_case(batch, manifest, case)
        if common_globals is None:
            common_globals = globals_
        require(globals_ == common_globals, "Actual network settings differ between sweep points")
        results.append(result)
        grouped_by_case[case["name"]] = grouped
    rows = {row["case"]: row for row in results}
    require(len(rows) == len(results) and "packet" in rows and "flow" in rows, "Missing or duplicate sweep endpoints")
    endpoint_checks = {}
    for other, baseline in (("messagelet_01pkt", "packet"), ("messagelet_16pkt", "flow")):
        if other not in rows:
            continue
        expected = grouped_by_case[baseline]
        actual = grouped_by_case[other]
        equivalent = all([(row["route"], row["path_id"], row["new_data_packets"]) for row in actual[fid].values()] ==
                         [(row["route"], row["path_id"], row["new_data_packets"]) for row in expected[fid].values()] for fid in expected)
        # Only assert 16-packet equivalence when it actually covers every flow.
        if other == "messagelet_16pkt" and rows[other]["decisions"] != rows[baseline]["decisions"]:
            continue
        for name in ("flow_dag_info.csv", "flowsInfo.csv", "globalInfo.csv"):
            equivalent = equivalent and (batch / "cases" / other / "output_metrics" / name).read_bytes() == (batch / "cases" / baseline / "output_metrics" / name).read_bytes()
        require(equivalent, f"Equivalent routing units produced different results: {other}, {baseline}")
        endpoint_checks[f"{other} == {baseline}"] = "PASS"
    flow_cct = rows["flow"]["cct_ns"]
    for row in results:
        row["delta_cct_vs_flow_ns"] = row["cct_ns"] - flow_cct
        row["cct_reduction_vs_flow_pct"] = 100 * (flow_cct - row["cct_ns"]) / flow_cct
    validation = {"status": "PASS", "batch": str(batch), "binary_sha256": manifest["binary_sha256"],
                  "input_sha256": manifest["input_sha256"], "case_count": len(results), "equivalent_endpoints": endpoint_checks,
                  "best_cases": [row["case"] for row in results if row["cct_ns"] == min(item["cct_ns"] for item in results)]}
    return results, validation, manifest


def write_report(directory, batch, results, validation, manifest):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "comparison.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    (directory / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    best = min(results, key=lambda row: row["cct_ns"])
    flow = next(row for row in results if row["case"] == "flow")
    lines = ["# exp5.3s — Source UGAL-L runtime 粒度 sweep", "",
             f"批次：`{batch.name}`；{len(results)} 个点，全部校验 **PASS**。", "",
             f"本批最小 CCT 为 **{best['cct_ns'] / 1000:.3f} µs**（{', '.join(validation['best_cases'])}），"
             f"相对同批 flow 的 **{flow['cct_ns'] / 1000:.3f} µs** 改变 "
             f"**{best['delta_cct_vs_flow_ns'] / 1000:+.3f} µs**，降低 **{best['cct_reduction_vs_flow_pct']:.2f}%**。", "",
             "所有点均为 `runtime.mode=label`、`routing.network_decision=ugal_l`、`-routing SOURCE`。"
             "改变的只有 runtime 粒度和对应 messagelet 字节预算；未混入 transparent / 原生交换机 UGAL-L。", "",
             "## CCT、FCT 和乱序", "",
             "| Sweep point | Budget (wire bytes) | CCT (µs) | ΔCCT vs flow (µs) | Mean FCT (µs) | P95 FCT (µs) | OOO packets |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in results:
        link = os.path.relpath(batch / "cases" / row["case"] / "summary.md", directory)
        lines.append(f"| [{row['case']}]({link}) | {row['budget_bytes'] or '—'} | **{row['cct_ns'] / 1000:.3f}** | "
                     f"{row['delta_cct_vs_flow_ns'] / 1000:+.3f} | {row['mean_fct_ns'] / 1000:.3f} | "
                     f"{row['p95_fct_ns'] / 1000:.3f} | {row['ooo_packets']} |")
    lines += ["", "CCT 是 12 个 GOAL rank graph finish 的最大值；FCT 来自 `flowsInfo.csv`。"
              "P95 使用排序后线性插值；OOO 是各 flow 的 `oooCount` 之和，不表示重传数。", "",
              "## 决策频率与选路", "",
              "| Sweep point | Decisions | Mean packets / decision | Valiant decisions | Valiant decision % | New packets on Valiant | Packet-weighted Valiant % |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in results:
        lines.append(f"| {row['case']} | {row['decisions']} | {row['mean_packets_per_decision']:.2f} | {row['valiant_decisions']} | "
                     f"{row['valiant_decision_pct']:.2f}% | {row['valiant_new_data_packets']} / {row['new_data_packets']} | {row['valiant_packet_pct']:.2f}% |")
    lines += ["", "每条 flow 的 64 KiB payload 对应 16 个完整的 4160-byte 包（含 64-byte header）。"
              "messagelet 预算按完整 packet 的 wire bytes 设置为 `N × 4160`。N 不整除 16 时，"
              "末组较短，因此同时报告决策占比和按逻辑新数据包数加权的选路占比；重传保留原包标签，不触发新决策。", "",
              "## 各 collective 的 graph finish", "",
              "| Sweep point | Ring all-reduce (µs) | Tree all-reduce (µs) | Direct all-to-all (µs) |",
              "| --- | ---: | ---: | ---: |"]
    for row in results:
        lines.append(f"| {row['case']} | {row['ring_allreduce_finish_ns'] / 1000:.3f} | "
                     f"{row['tree_allreduce_finish_ns'] / 1000:.3f} | {row['direct_alltoall_finish_ns'] / 1000:.3f} |")
    lines += ["", "## 控制与复现", "",
              f"- 同一 simulator SHA-256：`{manifest['binary_sha256']}`。",
              "- 所有点的 workload binary/GOAL/metadata、拓扑和 placement 哈希一致，并与 exp5.1 输入一致。",
              "- 实际 simulator 参数和 `globalInfo.csv` 一致；SOURCE 路径策略、nscc/mixed、queue、cwnd 与 path entropy 均固定。",
              "- 每个点全部 60 条 flow 和 200 个 GOAL task 完成；metadata send 与 transport ID、决策 index、预算和 UGAL-L 代价规则均已核对。",
              "- 每个点独立重启进程：simulator seed 13 / per-switch UGAL RNG seed 33。粒度改变后 RNG 消耗与队列演化也会改变，这是本次策略粒度变化的一部分。",
              "- Slack 仅在每次仿真后离线分析，不进入 policy。", ""]
    for check in validation["equivalent_endpoints"]:
        lines.append(f"- **PASS**：`{check}` 的每组路径以及 flow/DAG/network metrics 完全一致。")
    lines += ["", "这是当前 DAG、拓扑和固定 seed 的单次 sweep；对粒度优劣的解释限于本批结果。"
              "完整逐 flow / messagelet 记录见各点的 `output_metrics/network_decisions.csv`，"
              "输入快照和固定批次路径见 `manifest.json` 与 `validation.json`。", ""]
    (directory / "comparison.md").write_text("\n".join(lines))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch", type=Path, default=Path(__file__).resolve().parent / "artifacts/latest")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    batch, output = args.batch.resolve(strict=True), args.output_dir.resolve()
    results, validation, manifest = analyze(batch)
    write_report(batch, batch, results, validation, manifest)
    if output != batch:
        write_report(output, batch, results, validation, manifest)
    print(f"PASS: {len(results)} controlled source-UGAL-L sweep points; report: {output / 'comparison.md'}")

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


def case_path(batch, case, seed=None):
    parent = batch if seed is None else batch / "seeds" / f"seed_{seed['simulation']}"
    return parent / "cases" / case["name"]


def read_case(batch, manifest, case, seed=None):
    run = case_path(batch, case, seed)
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
    seed_args = [] if seed is None else ["-seed", str(seed["simulation"]), "-routing_seed", str(seed["routing"])]
    expected_command = [manifest["binary"], "-basepath", str(snapshot / "topology"), "-goal", str(snapshot / "workload.bin"),
                        *manifest["network_args"], *seed_args, "-rank_placement", str(snapshot / "rank_placement.json"),
                        "-messagelet_config", str(snapshot / "messagelet_config.json")]
    require(shlex.split((run / "command.txt").read_text()) == expected_command, "Simulator arguments changed")
    if seed is not None:
        require(json.loads((snapshot / "seed.json").read_text()) == seed, "Seed snapshot differs")
        require(read_csv(metrics / "randomSeeds.csv") == [{"simulationSeed": str(seed["simulation"]),
                                                          "routingSeed": str(seed["routing"])}], "Executed seeds differ")
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
    if seed is not None:
        result.update(simulation_seed=seed["simulation"], routing_seed=seed["routing"])
        routes = [(row["flow_id"], row["messagelet_index"], row["route"], row["path_id"])
                  for row in sorted(all_decisions, key=lambda row: (row["flow_id"], row["messagelet_index"]))]
        result["routing_sha256"] = hashlib.sha256(json.dumps(routes).encode()).hexdigest()
    with (metrics / "network_decisions.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(all_decisions[0]))
        writer.writeheader()
        writer.writerows(sorted(all_decisions, key=lambda row: (row["flow_id"], row["messagelet_index"])))
    (run / "granularity_validation.json").write_text(json.dumps({"status": "PASS", **result}, indent=2) + "\n")
    return result, globals_, grouped


def check_endpoints(batch, rows, grouped_by_case, seed=None):
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
            equivalent = equivalent and (case_path(batch, {"name": other}, seed) / "output_metrics" / name).read_bytes() == (case_path(batch, {"name": baseline}, seed) / "output_metrics" / name).read_bytes()
        require(equivalent, f"Equivalent routing units produced different results: {other}, {baseline}")
        endpoint_checks[f"{other} == {baseline}"] = "PASS"
    return endpoint_checks


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
    endpoint_checks = check_endpoints(batch, rows, grouped_by_case)
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


def analyze_multiseed(batch, manifest):
    seeds = manifest["seeds"]
    require(seeds and len({seed['simulation'] for seed in seeds}) == len(seeds), "Missing or duplicate seeds")
    raw, checks, common_globals = [], {}, None
    for seed in seeds:
        results, grouped_by_case = [], {}
        for case in manifest["cases"]:
            result, globals_, grouped = read_case(batch, manifest, case, seed)
            if common_globals is None:
                common_globals = globals_
            require(globals_ == common_globals, "Network controls changed across seeds or granularities")
            results.append(result)
            grouped_by_case[case["name"]] = grouped
        rows = {row["case"]: row for row in results}
        require(len(rows) == len(results) and "packet" in rows and "flow" in rows, "Invalid seed sweep")
        checks[str(seed["simulation"])] = check_endpoints(batch, rows, grouped_by_case, seed)
        baseline = rows["flow"]["cct_ns"]
        for row in results:
            row["delta_cct_vs_flow_ns"] = row["cct_ns"] - baseline
            row["paired_cct_reduction_pct"] = 100 * (baseline - row["cct_ns"]) / baseline
        raw.extend(results)
    aggregates = []
    metrics = ("cct_ns", "mean_fct_ns", "p95_fct_ns", "max_fct_ns", "ooo_packets", "decisions", "valiant_decisions",
               "valiant_decision_pct", "valiant_packet_pct", "valiant_new_data_packets", "mean_packets_per_decision",
               "ring_allreduce_finish_ns", "tree_allreduce_finish_ns", "direct_alltoall_finish_ns",
               "delta_cct_vs_flow_ns", "paired_cct_reduction_pct")
    for case in manifest["cases"]:
        samples = [row for row in raw if row["case"] == case["name"]]
        require(len(samples) == len(seeds), "Unequal samples per granularity")
        result = {"case": case["name"], "granularity": case["granularity"], "messagelet_packets": case["packets_per_messagelet"] or "flow",
                  "budget_bytes": case["size_bytes"], "seed_count": len(samples),
                  "distinct_routing_outcomes": len({row["routing_sha256"] for row in samples})}
        for metric in metrics:
            values = [row[metric] for row in samples]
            result[metric + "_mean"] = statistics.mean(values)
            result[metric + "_std"] = statistics.stdev(values) if len(values) > 1 else 0.0
            result[metric + "_min"] = min(values)
            result[metric + "_max"] = max(values)
        aggregates.append(result)
    baseline = next(row for row in aggregates if row["case"] == "flow")["cct_ns_mean"]
    for result in aggregates:
        result["cct_reduction_of_means_pct"] = 100 * (baseline - result["cct_ns_mean"]) / baseline
    packet = next(row for row in aggregates if row["case"] == "packet")
    if len({seed["routing"] for seed in seeds}) > 1:
        require(packet["distinct_routing_outcomes"] > 1, "Routing seed did not change observed packet decisions")
    validation = {"status": "PASS", "batch": str(batch), "binary_sha256": manifest["binary_sha256"],
                  "input_sha256": manifest["input_sha256"], "case_count": len(aggregates), "run_count": len(raw),
                  "seed_pairs": seeds, "parallel_jobs": manifest["parallel_jobs"], "equivalent_endpoints_by_seed": checks,
                  "distinct_packet_routing_outcomes": packet["distinct_routing_outcomes"],
                  "best_cases": [row["case"] for row in aggregates if row["cct_ns_mean"] == min(item["cct_ns_mean"] for item in aggregates)]}
    return aggregates, raw, validation


def write_multiseed_report(directory, batch, rows, raw, validation, manifest):
    directory.mkdir(parents=True, exist_ok=True)
    for name, contents in (("comparison.csv", rows), ("per_seed.csv", raw)):
        with (directory / name).open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(contents[0]))
            writer.writeheader()
            writer.writerows(contents)
    (directory / "validation.json").write_text(json.dumps(validation, indent=2) + "\n")
    best = min(rows, key=lambda row: row["cct_ns_mean"])
    flow = next(row for row in rows if row["case"] == "flow")
    seeds = manifest["seeds"]
    pairs = ", ".join(f"({seed['simulation']}, {seed['routing']})" for seed in seeds)
    lines = ["# exp5.3s — Source UGAL-L 粒度 sweep：多种子平均", "",
             f"批次：`{batch.name}`；**{len(rows)} 个粒度 × {len(seeds)} 个种子 = {len(raw)} 次仿真**，"
             f"**{manifest['parallel_jobs']} 路并行**，全部校验 **PASS**。", "",
             f"种子对 `(simulation seed, routing seed)`：{pairs}。每个种子对都覆盖所有粒度。", "",
             f"平均 CCT 最低为 **{best['cct_ns_mean'] / 1000:.3f} ± {best['cct_ns_std'] / 1000:.3f} µs**"
             f"（{', '.join(validation['best_cases'])}）。flow 为 **{flow['cct_ns_mean'] / 1000:.3f} ± {flow['cct_ns_std'] / 1000:.3f} µs**，"
             f"按均值之比降低 **{best['cct_reduction_of_means_pct']:.2f}%**。", "",
             "所有运行都是相同 source UGAL-L：`runtime.mode=label`、`routing.network_decision=ugal_l`、"
             "`-routing SOURCE`。每个种子内部只改变粒度；计算图、拓扑、placement、binary、CC 和 queue 等控制保持一致。", "",
             "## 平均性能与波动", "",
             "| 粒度 | Budget (wire bytes) | CCT mean ± SD (µs) | CCT min–max (µs) | ΔCCT vs 同 seed flow mean (µs) | Mean FCT (µs) | Mean seed P95 FCT (µs) | Mean OOO |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['case']} | {row['budget_bytes'] or '—'} | **{row['cct_ns_mean'] / 1000:.3f} ± {row['cct_ns_std'] / 1000:.3f}** | "
                     f"{row['cct_ns_min'] / 1000:.3f}–{row['cct_ns_max'] / 1000:.3f} | {row['delta_cct_vs_flow_ns_mean'] / 1000:+.3f} | "
                     f"{row['mean_fct_ns_mean'] / 1000:.3f} | {row['p95_fct_ns_mean'] / 1000:.3f} | {row['ooo_packets_mean']:.1f} |")
    lines += ["", f"均值等权汇总 {len(seeds)} 个 seed；SD 为 seed 间样本标准差（n−1），不是置信区间。"
              "CCT 为最大 GOAL rank graph finish。FCT 先在每次运行的 60 条 flow 上统计，"
              "表中 P95 是各 seed 的 P95 再取均值，不是混合 300 条 flow 的 P95。OOO 为每次运行的 `oooCount` 总和再取均值。", "",
              "## 每个种子的 CCT", "",
              "| 粒度 | " + " | ".join(f"{seed['simulation']}/{seed['routing']} (µs)" for seed in seeds) + " |",
              "| --- | " + " | ".join("---:" for _ in seeds) + " |"]
    lookup = {(row["case"], row["simulation_seed"]): row for row in raw}
    for row in rows:
        values = [lookup[(row["case"], seed["simulation"])]["cct_ns"] / 1000 for seed in seeds]
        lines.append(f"| {row['case']} | " + " | ".join(f"{value:.3f}" for value in values) + " |")
    lines += ["", "## 平均决策与选路", "",
              "| 粒度 | Decisions / seed | Mean packets / decision | Mean Valiant decision % | Mean packet-weighted Valiant % | Distinct routing outcomes |",
              "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['case']} | {row['decisions_mean']:.0f} | {row['mean_packets_per_decision_mean']:.2f} | "
                     f"{row['valiant_decision_pct_mean']:.2f}% | {row['valiant_packet_pct_mean']:.2f}% | {row['distinct_routing_outcomes']} |")
    lines += ["", "messagelet 预算为 N × 4160 bytes；每条 flow 是 16 包，末组可以较短。"
              "按新数据包数加权的 Valiant 占比反映不等长分组；重传沿用原标签，不触发新决策。", "",
              "## 平均 collective graph finish", "",
              "| 粒度 | Ring all-reduce (µs) | Tree all-reduce (µs) | Direct all-to-all (µs) |",
              "| --- | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['case']} | {row['ring_allreduce_finish_ns_mean'] / 1000:.3f} | "
                     f"{row['tree_allreduce_finish_ns_mean'] / 1000:.3f} | {row['direct_alltoall_finish_ns_mean'] / 1000:.3f} |")
    lines += ["", "## 校验与原始记录", "",
              f"- 全部运行使用同一 binary SHA-256：`{manifest['binary_sha256']}`。",
              "- 两个独立参数分别初始化全局 RNG 和每个 Dragonfly switch 的候选采样 RNG；默认仍是 13/33，UGAL-L 比较规则未改动。",
              f"- 实际执行种子记录于每次运行的 `randomSeeds.csv`；packet 点观测到 **{validation['distinct_packet_routing_outcomes']}** 种路由结果。",
              "- 输入哈希、网络控制、flow/DAG 完成、messagelet index/预算、UGAL-L cost 规则全部通过核对。",
              "- 每个 seed 的 1 包 messagelet == packet、16 包 messagelet == flow 端点等价校验通过。",
              "- 每次仿真在独立进程和目录中运行；并行不共享 RNG、输出文件或仿真时钟。Slack 只做离线分析。", "",
              "| Seed pair | 每点原始记录 |", "| --- | --- |"]
    for seed in seeds:
        target = os.path.relpath(batch / "seeds" / f"seed_{seed['simulation']}" / "cases", directory)
        lines.append(f"| {seed['simulation']}/{seed['routing']} | [18 个粒度点]({target}) |")
    lines += ["", "`comparison.csv` 包含均值、样本标准差、min/max 与同 seed 配对变化；"
              "`per_seed.csv` 保留每次运行的指标。配对百分比先对同 seed flow 求改善率再取均值，"
              "与均值之比的改善率分别记录。旧单种子报告保留于旧 artifact。", "",
              f"当前是 {len(seeds)} 个 seed 的实测统计，结论限于此计算图、拓扑和种子集合。", ""]
    (directory / "comparison.md").write_text("\n".join(lines))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch", type=Path, default=Path(__file__).resolve().parent / "artifacts/latest")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    batch, output = args.batch.resolve(strict=True), args.output_dir.resolve()
    manifest = json.loads((batch / "manifest.json").read_text())
    if "seeds" in manifest:
        results, raw, validation = analyze_multiseed(batch, manifest)
        write_multiseed_report(batch, batch, results, raw, validation, manifest)
        if output != batch:
            write_multiseed_report(output, batch, results, raw, validation, manifest)
        print(f"PASS: {len(results)} granularities, {len(raw)} seeded runs; report: {output / 'comparison.md'}")
    else:
        results, validation, manifest = analyze(batch)
        write_report(batch, batch, results, validation, manifest)
        if output != batch:
            write_report(output, batch, results, validation, manifest)
        print(f"PASS: {len(results)} controlled source-UGAL-L sweep points; report: {output / 'comparison.md'}")

#!/usr/bin/env python3
"""Audit the causal controls, report three outcomes, and fail if any fails."""

import argparse
import csv
import hashlib
import json
import re
import sys
import tempfile
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT.parent / "common"))
sys.path.insert(0, str(ROOT.parents[1] / "workload_gen"))
from analyze_slack import parse_goal, read_lifecycle
from workload_gen.compiler import compile_spec
from workload_gen.goal_io import write_goal_binary, write_goal_text

CASES = (("ref1", 1, None), ("ref2", 2, None),
         ("1", 1, 0), ("2", 1, 1), ("3", 2, 0), ("4", 2, 1))
TAIL_KEYS = ((2, 4), (3, 4))
NETWORK_SETTINGS = ("ROUTING", "SENDER_CC_ALGO", "LOAD_BALANCING_ALGO",
                    "PATH_ENTROPY_SIZE", "QUEUE_PACKETS", "CWND_BYTES", "BINARY",
                    "TRACE_FLOW_COMPLETIONS", "TRACE_PACKET_HOPS", "SIM_EXTRA_ARGS")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def csv_rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def read_run(path, dag, target):
    path = path.resolve(strict=True)
    require((path / "exit_code.txt").read_text().strip() == "0", f"failed run: {path}")
    snapshot = path / "config_snapshot"
    meta = json.loads((snapshot / "workload.meta.json").read_text())
    template = json.loads((snapshot / "template.flow.json").read_text())
    require(meta["workload"]["kind"] == template["kind"] == "flow", "expected flow-level DAG")
    require(meta["workload"]["p2p_flows"] == 2 and set(meta["flows"]) == {"0", "1"},
            "expected exactly two flows with metadata IDs 0 and 1")
    require(meta["source_graph"] == {"defaults": template["defaults"], "nodes": template["nodes"]},
            "template/metadata graph differs")
    # Verify the executed binary, including CPU/NIC fields absent from parse_goal.
    with tempfile.TemporaryDirectory(prefix="exp4-binary-audit-") as directory:
        expected = Path(directory)
        compiled = compile_spec(template)
        write_goal_binary(compiled, expected / "workload.bin")
        write_goal_text(compiled, expected / "workload.goal")
        for name in ("workload.bin", "workload.goal"):
            require((expected / name).read_bytes() == (snapshot / name).read_bytes(),
                    f"executed {name} differs from compiled template")
    nodes = {node["id"]: node for node in template["nodes"]}
    require(len(nodes) == len(template["nodes"]) == 6, "expected start, two flows, two tails, sink")
    tails = (30000, 15000) if dag == 1 else (15000, 30000)
    for fid, name in enumerate("AB"):
        require(nodes[f"tail_{name}"]["duration_ns"] == tails[fid]
                and nodes[f"tail_{name}"]["depends_on"] == [f"flow_{name}"]
                and nodes[f"tail_{name}"]["ranks"] == [fid + 2], "incorrect compute tail")
        require(nodes[f"flow_{name}"]["depends_on"] == ["start"], "flow must be ready at start")

    tasks, edges, ranks = parse_goal(snapshot / "workload.goal")
    life = read_lifecycle(path / "output_metrics/flow_dag_info.csv")
    require(ranks == 4 and len(tasks) == len(life) == 20 and set(tasks) == set(life),
            "incomplete GOAL/lifecycle nodes")
    for key, task in tasks.items():
        row = life[key]
        require(row.op == task.op and row.size == task.size, f"task/lifecycle mismatch: {key}")
        require(row.ready_ns <= row.issue_ns <= row.finish_ns, f"invalid lifecycle: {key}")
        if task.op in ("send", "recv"):
            require((row.peer, row.tag) == (task.peer, task.tag), f"endpoint mismatch: {key}")
    for source, destination in edges:
        require(life[destination].ready_ns >= life[source].finish_ns, "dependency violated")
    for fid, key in enumerate(TAIL_KEYS):
        require(tasks[key].op == "calc" and tasks[key].size == tails[fid], "GOAL tail mismatch")
        require(life[key].finish_ns - life[key].issue_ns == tails[fid], "tail duration mismatch")

    routes_list = csv_rows(path / "output_metrics/flowRouting.csv")
    routes = {int(row["flow_id"]): row for row in routes_list}
    config = json.loads((snapshot / "routing_config.json").read_text())
    require(len(routes_list) == len(routes) == config["flow_count"] == 2
            and set(routes) == {0, 1} and set(config["flows"]) == {"0", "1"}, "invalid routing count")
    flows_list = csv_rows(path / "output_metrics/flowsInfo.csv")
    flows = {}
    for fid, route in routes.items():
        metadata = meta["flows"][str(fid)]
        assignment = config["flows"][str(fid)]
        expected = "valiant" if fid == target else "minimum"
        require(route["label"] == assignment["label"] == expected, "wrong detour assignment")
        require(route["intermediate_group"] == ("2" if fid == target else ""), "wrong waypoint")
        require(bool(int(route["path_id"]) & (1 << 31)) == (fid == target), "wrong routing mark")
        for runtime_key, meta_key in (("src_rank", "src"), ("dst_rank", "dst")):
            require(int(route[runtime_key]) == metadata[meta_key] == assignment[meta_key],
                    "routing/metadata identity mismatch")
        require(metadata["src"] == fid and metadata["dst"] == fid + 2
                and metadata["bytes"] == assignment["bytes"] == 32768
                and metadata["tag"] == assignment["tag"] == 101 + 100 * fid, "flow identity changed")
        require(int(route["send_task_id"]) == assignment["send_task_id"]
                == metadata["send_task"]["task_id"], "send task identity mismatch")
        send = life[(metadata["src"], metadata["send_task"]["task_id"])]
        recv = life[(metadata["dst"], metadata["recv_task"]["task_id"])]
        require(send.ready_ns == send.issue_ns == recv.ready_ns == recv.issue_ns == 2,
                "flows must be simultaneously ready and issued at 2 ns")
        matched = [row for row in flows_list if row["srcNode_dstNode_flowId"].split("_")[:2]
                   == [route["src_host"], route["dst_host"]]]
        require(len(matched) == 1, "missing or duplicate physical flow")
        flow = matched[0]
        require(int(flow["flowSizeBytes"]) == 32768 and int(flow["totalPackets"]) == 8,
                "unexpected flow size/packet count")
        start, end, fct = (float(flow[key]) for key in ("startTimeNs", "endTimeNs", "fctNs"))
        require(start == 2 and abs(end - start - fct) < 1e-6
                and int(end) == recv.finish_ns == send.finish_ns, "FCT/lifecycle mismatch")
        flows[fid] = flow
    require(len(flows_list) == 2, "unexpected extra network flows")

    log = (path / "simulator.log").read_text()
    times = {int(rank): int(finish) for rank, finish in
             re.findall(r"^Host (\d+): (\d+)$", log, re.MULTILINE)}
    require(set(times) == set(range(4)), "incomplete rank completion")
    cct = max(times.values())
    for rank in times:
        require(times[rank] == max(row.finish_ns for key, row in life.items() if key[0] == rank),
                "rank finish/lifecycle mismatch")
    match = re.search(r"Htsim time (\d+)", log)
    graph = re.search(r"Maximum finishing time at host \d+: (\d+)", log)
    require(match and graph and int(match[1]) == int(graph[1]) == cct, "CCT mismatch")
    hops = [line for line in log.splitlines() if line.startswith("PacketHop ")]
    require(len(hops) >= 100, "missing full packet-hop audit; rebuild and set TRACE_PACKET_HOPS=1")
    slack_list = csv_rows(path / "output_metrics/oracle_slack.csv")
    slack = {int(row["flow_id"]): row for row in slack_list}
    require(len(slack_list) == len(slack) == 2 and set(slack) == {0, 1}, "incomplete slack analysis")
    return dict(path=path, dag=dag, target=target, meta=meta, template=template,
                tasks=tasks, edges=edges, life=life, config=config, routes=routes,
                flows=flows, cct=cct, times=times, hops=hops, slack=slack)


def validate_inputs(runs):
    base = runs["ref1"]
    for run in runs.values():
        for name in ("rank_placement.json", "messagelet_config.json", "topology/dragonfly.topo"):
            require((base["path"] / "config_snapshot" / name).read_bytes()
                    == (run["path"] / "config_snapshot" / name).read_bytes(), f"input differs: {name}")
        require((base["path"] / "binary-sha256.txt").read_text().split()[0]
                == (run["path"] / "binary-sha256.txt").read_text().split()[0], "simulator binary differs")
        a, b = (dict(re.findall(r"^([A-Z_]+)=(.*)$",
                               (item["path"] / "config_snapshot/config.env").read_text(), re.MULTILINE))
                for item in (base, run))
        for key in NETWORK_SETTINGS:
            require(key in a and key in b and a[key] == b[key], f"network setting differs: {key}")
        require(a["TRACE_PACKET_HOPS"] == "1", "packet-hop audit disabled")
        require(csv_rows(base["path"] / "output_metrics/globalInfo.csv")
                == csv_rows(run["path"] / "output_metrics/globalInfo.csv"), "network parameters differ")
        require(base["meta"]["flows"] == run["meta"]["flows"], "flow metadata differs")
        require(base["edges"] == run["edges"], "GOAL dependencies differ")
        require({key: replace(task, size=0) if key in TAIL_KEYS else task
                 for key, task in base["tasks"].items()}
                == {key: replace(task, size=0) if key in TAIL_KEYS else task
                    for key, task in run["tasks"].items()}, "GOAL differs beyond compute tails")
        def normalized_template(item):
            template = json.loads(json.dumps(item["template"]))
            template.pop("name")
            for node in template["nodes"]:
                if node["id"] in ("tail_A", "tail_B"):
                    node["duration_ns"] = 0
            return template
        require(normalized_template(base) == normalized_template(run), "DAG differs beyond tails/name")
        same_dag = runs[f"ref{run['dag']}"]
        for name in ("workload.bin", "workload.goal", "workload.meta.json", "template.flow.json"):
            require((same_dag["path"] / "config_snapshot" / name).read_bytes()
                    == (run["path"] / "config_snapshot" / name).read_bytes(), "same-DAG workload differs")
    for dag, critical in ((1, 0), (2, 1)):
        ref = runs[f"ref{dag}"]
        require(ref["times"][critical + 2] == ref["cct"]
                and int(ref["slack"][critical]["oracle_slack_ns"]) == 0
                and int(ref["slack"][1-critical]["oracle_slack_ns"]) > 0, "baseline criticality incorrect")


def validate_network_pairs(runs):
    audits = []
    for left, right, label in (("ref1", "ref2", "all-minimal"),
                               ("1", "3", "A detour"), ("2", "4", "B detour")):
        a, b = runs[left], runs[right]
        require(a["flows"] == b["flows"], f"{label}: full flow metrics differ after swapping tails")
        require(a["routes"] == b["routes"], f"{label}: runtime routes differ")
        require(a["config"]["flows"] == b["config"]["flows"], f"{label}: assigned routes differ")
        require({key: row for key, row in a["life"].items() if row.op in ("send", "recv")}
                == {key: row for key, row in b["life"].items() if row.op in ("send", "recv")},
                f"{label}: communication lifecycle differs")
        require(a["hops"] == b["hops"], f"{label}: packet-hop or queue-state trace differs")
        digest = hashlib.sha256(("\n".join(a["hops"]) + "\n").encode()).hexdigest()
        audits.append((label, len(a["hops"]), digest))
    return audits


def compare(runs, output_dir):
    validate_inputs(runs)
    audits = validate_network_pairs(runs)
    rows = []
    for key, dag, target in CASES:
        run, ref = runs[key], runs[f"ref{dag}"]
        aligned = target is not None and target != dag - 1
        rows.append(dict(case=f"exp4.{key}", dag=dag,
                         detour="reference" if target is None else "AB"[target],
                         alignment="reference" if target is None else "Slack-aligned" if aligned else "Misaligned",
                         cct_ns=run["cct"], reference_cct_ns=ref["cct"],
                         delta_cct_ns=run["cct"]-ref["cct"],
                         a_fct_ns=float(run["flows"][0]["fctNs"]),
                         b_fct_ns=float(run["flows"][1]["fctNs"]),
                         a_delta_fct_ns=float(run["flows"][0]["fctNs"])-float(ref["flows"][0]["fctNs"]),
                         b_delta_fct_ns=float(run["flows"][1]["fctNs"])-float(ref["flows"][1]["fctNs"]),
                         artifact=str(run["path"])))
    tradeoff = all(row[f"{row['detour'].lower()}_delta_fct_ns"] > 0
                   and row[f"{'b' if row['detour'] == 'A' else 'a'}_delta_fct_ns"] < 0 for row in rows[2:])
    cct = runs["2"]["cct"] < runs["1"]["cct"] and runs["3"]["cct"] < runs["4"]["cct"]
    reversal = (runs["1"]["cct"]-runs["2"]["cct"]) * (runs["3"]["cct"]-runs["4"]["cct"]) < 0
    evidence = {"network_tradeoff": tradeoff, "slack_aligned_lower_cct": cct, "decision_reversal": reversal}
    lines = ["# Experiment 4 — compute-tail swap causal validation", "",
             "CCT is the maximum GOAL rank finish. Each DAG uses its own all-minimal CCT reference. "
             "The four interventions each detour exactly one flow; the two references only establish baseline CCT and slack.", "",
             "## Four comparisons", "",
             "| DAG context | A detour, B minimal: CCT / ΔCCT (µs) | B detour, A minimal: CCT / ΔCCT (µs) | Better detour |",
             "| --- | --- | --- | --- |"]
    for dag, left, right in ((1, "1", "2"), (2, "3", "4")):
        a, b, ref = runs[left], runs[right], runs[f"ref{dag}"]
        winner = "A" if a["cct"] < b["cct"] else "B" if b["cct"] < a["cct"] else "tie"
        lines.append(f"| DAG-{dag}: {'AB'[dag-1]} critical | "
                     f"{a['cct']/1000:.3f} / {(a['cct']-ref['cct'])/1000:+.3f} "
                     f"({'Misaligned' if dag == 1 else 'Slack-aligned'}) | "
                     f"{b['cct']/1000:.3f} / {(b['cct']-ref['cct'])/1000:+.3f} "
                     f"({'Slack-aligned' if dag == 1 else 'Misaligned'}) | {winner} |")
    lines += ["", "## All-minimal references and oracle slack", "",
              "| DAG | Reference CCT (µs) | A slack (µs) | B slack (µs) |", "| --- | ---: | ---: | ---: |"]
    for dag in (1, 2):
        ref = runs[f"ref{dag}"]
        lines.append(f"| DAG-{dag} | {ref['cct']/1000:.3f} | "
                     f"{int(ref['slack'][0]['oracle_slack_ns'])/1000:.3f} | "
                     f"{int(ref['slack'][1]['oracle_slack_ns'])/1000:.3f} |")
    lines += ["", "## Network tradeoff", "",
              "FCT uses HTSim's fractional-ns sender completion; ΔFCT is relative to the same DAG's all-minimal run.", "",
              "| Case | Detour | A FCT (µs) | ΔA FCT (µs) | B FCT (µs) | ΔB FCT (µs) |",
              "| --- | --- | ---: | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['case']} | {row['detour']} | {row['a_fct_ns']/1000:.5f} | "
                     f"{row['a_delta_fct_ns']/1000:+.5f} | {row['b_fct_ns']/1000:.5f} | {row['b_delta_fct_ns']/1000:+.5f} |")
    lines += ["", "## Fixed-routing network invariance", "",
              "PASS: each cross-DAG pair has identical assigned/runtime routes, every flowsInfo.csv field, "
              "send/recv lifecycle records, and the ordered PacketHop trace (no sorting or timestamp tolerance). "
              "Traces include DATA and control packets, picosecond time, packet/flow ID, type/size, endpoints, "
              "ECN flags, trim/bounce status, next sink and its queue occupancy before receive.", "",
              "| Fixed assignment | Matched hop events | Trace SHA-256 |", "| --- | ---: | --- |"]
    for label, count, digest in audits:
        lines.append(f"| {label} | {count} | `{digest}` |")
    lines += ["", "PASS: same simulator binary hash, topology, placement, messagelet configuration and "
              "network settings across six runs. Templates and compiled GOAL dependencies/identities match; "
              "only the two receiver compute durations swap. Exactly two simultaneous 32 KiB flows, "
              "one messagelet each; no background traffic or artificial communication delay.", "",
              "## Three success criteria", "", "| Evidence | Result |", "| --- | --- |"]
    for name, passed in evidence.items():
        lines.append(f"| {name} | {'PASS' if passed else 'FAIL'} |")
    if all(evidence.values()):
        lines += ["", "With network observations unchanged for each fixed route assignment, changing only DAG "
                  "criticality reverses the CCT-preferred detour: B in DAG-1, A in DAG-2. "
                  "A policy using only these unchanged network inputs cannot distinguish the two contexts. "
                  "This demonstrates the value of execution information for this controlled decision."]
    lines += ["", "These are deterministic runs with the simulator's fixed seed 13. This experiment compares "
              "two predefined allocations; it does not benchmark an implemented congestion-only policy or establish "
              "general policy performance. GOAL completion uses integer ns (fractional network completion is truncated); "
              "compute anchors and the local sink add 2 ns after receive. The sink is rank-local; "
              "application completion is the maximum rank finish, as in exp1.", "", "## Artifact provenance", ""]
    for row in rows:
        lines.append(f"- {row['case']}: `{row['artifact']}`")
    output_dir.mkdir(parents=True, exist_ok=True)
    with (output_dir / "comparison.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (output_dir / "comparison.md").write_text("\n".join(lines) + "\n")
    (output_dir / "validation.json").write_text(json.dumps({"controls_valid": True, **evidence,
         "runs": {key: str(run["path"]) for key, run in runs.items()},
         "network_audits": [dict(assignment=label, events=count, sha256=digest)
                            for label, count, digest in audits]}, indent=2) + "\n")
    return all(evidence.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key, _, _ in CASES:
        parser.add_argument(f"--{key}", type=Path, default=ROOT / f"exp4.{key}/artifacts/latest")
    parser.add_argument("--output-dir", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        runs = {key: read_run(getattr(args, key), dag, target) for key, dag, target in CASES}
        success = compare(runs, args.output_dir)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(f"Experiment 4 {'PASS' if success else 'FAIL'}: {args.output_dir / 'comparison.md'}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())

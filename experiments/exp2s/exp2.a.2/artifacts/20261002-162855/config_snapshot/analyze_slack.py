#!/usr/bin/env python3
"""Offline oracle-slack analysis for a GOAL workload artifact.

Only Python's standard library is required. Existing input files are not
modified; generated files are written below --output-dir.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import statistics
import sys
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


TaskKey = tuple[int, int]
ProjectedKey = tuple[str, int] | tuple[str, int, int]


@dataclass(frozen=True)
class GoalTask:
    rank: int
    task_id: int
    op: str
    peer: int | None
    tag: int | None
    size: int


@dataclass(frozen=True)
class Lifecycle:
    rank: int
    task_id: int
    op: str
    peer: int
    tag: int
    size: int
    ready_ns: int
    issue_ns: int
    finish_ns: int


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def parse_goal(path: Path) -> tuple[dict[TaskKey, GoalTask], list[tuple[TaskKey, TaskKey]], int]:
    tasks: dict[TaskKey, GoalTask] = {}
    edges: list[tuple[TaskKey, TaskKey]] = []
    rank: int | None = None
    declared_ranks: int | None = None
    task_re = re.compile(r"l(\d+): (send|recv|calc)\s+(.*)")
    dep_re = re.compile(r"l(\d+) requires l(\d+)")

    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        match = re.fullmatch(r"num_ranks\s+(\d+)", line)
        if match:
            declared_ranks = int(match.group(1))
            continue
        match = re.fullmatch(r"rank\s+(\d+)\s+\{", line)
        if match:
            require(rank is None, f"nested rank block at {path}:{line_number}")
            rank = int(match.group(1))
            continue
        if line == "}":
            require(rank is not None, f"unmatched closing brace at {path}:{line_number}")
            rank = None
            continue
        match = task_re.fullmatch(line)
        if match:
            require(rank is not None, f"task outside rank block at {path}:{line_number}")
            task_id = int(match.group(1))
            op = match.group(2)
            rest = match.group(3)
            size_match = re.match(r"(\d+)(?:b)?", rest)
            require(size_match is not None, f"missing task size at {path}:{line_number}")
            peer_match = re.search(r"\b(?:to|from)\s+(\d+)", rest)
            tag_match = re.search(r"\btag\s+(\d+)", rest)
            key = (rank, task_id)
            require(key not in tasks, f"duplicate GOAL task {key}")
            tasks[key] = GoalTask(
                rank=rank,
                task_id=task_id,
                op=op,
                peer=int(peer_match.group(1)) if peer_match else None,
                tag=int(tag_match.group(1)) if tag_match else None,
                size=int(size_match.group(1)),
            )
            continue
        match = dep_re.fullmatch(line)
        if match:
            require(rank is not None, f"dependency outside rank block at {path}:{line_number}")
            dependent = (rank, int(match.group(1)))
            prerequisite = (rank, int(match.group(2)))
            edges.append((prerequisite, dependent))
            continue
        raise ValueError(f"unrecognized GOAL line at {path}:{line_number}: {line!r}")

    require(rank is None, f"unterminated rank block in {path}")
    require(declared_ranks is not None, f"missing num_ranks in {path}")
    for prerequisite, dependent in edges:
        require(prerequisite in tasks, f"dependency references missing task {prerequisite}")
        require(dependent in tasks, f"dependency references missing task {dependent}")
    return tasks, edges, declared_ranks


def read_lifecycle(path: Path) -> dict[TaskKey, Lifecycle]:
    rows: dict[TaskKey, Lifecycle] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for raw in csv.DictReader(handle):
            key = (int(raw["rank"]), int(raw["nodeOffset"]))
            require(key not in rows, f"duplicate lifecycle row for {key}")
            for field in ("readyTimeNs", "issueTimeNs", "finishTimeNs"):
                require(raw[field] != "", f"missing {field} for lifecycle task {key}")
            rows[key] = Lifecycle(
                rank=key[0],
                task_id=key[1],
                op=raw["op"],
                peer=int(raw["peer"]),
                tag=int(raw["tag"]),
                size=int(raw["size"]),
                ready_ns=int(raw["readyTimeNs"]),
                issue_ns=int(raw["issueTimeNs"]),
                finish_ns=int(raw["finishTimeNs"]),
            )
    return rows


def topological_order(nodes: Iterable, edges: Iterable[tuple], label: str) -> tuple[list, dict]:
    node_set = set(nodes)
    successors: dict = {node: set() for node in node_set}
    indegree = {node: 0 for node in node_set}
    for source, target in edges:
        require(source in node_set and target in node_set, f"{label} edge has missing endpoint")
        if target not in successors[source]:
            successors[source].add(target)
            indegree[target] += 1
    ready = deque(sorted((node for node, degree in indegree.items() if degree == 0), key=str))
    order = []
    while ready:
        node = ready.popleft()
        order.append(node)
        for target in sorted(successors[node], key=str):
            indegree[target] -= 1
            if indegree[target] == 0:
                ready.append(target)
    require(len(order) == len(node_set), f"{label} contains a cycle")
    return order, successors


def parse_log_makespans(path: Path) -> tuple[int | None, int | None]:
    text = path.read_text(encoding="utf-8")
    htsim = re.search(r"Htsim time (\d+)", text)
    graph = re.search(r"Maximum finishing time at host \d+: (\d+)", text)
    return (int(htsim.group(1)) if htsim else None, int(graph.group(1)) if graph else None)


def median_number(values: list[int]) -> int | float:
    value = statistics.median(values)
    return int(value) if float(value).is_integer() else value


def write_csv(path: Path, fieldnames: list[str], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def format_table_value(value: int | float) -> str:
    return f"{value:g}" if isinstance(value, float) else str(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path, help="timestamped experiment artifact directory")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="analysis output directory (default: the artifact directory)",
    )
    parser.add_argument("--ready-window-ns", type=int, default=100)
    args = parser.parse_args()
    require(args.ready_window_ns > 0, "--ready-window-ns must be positive")

    artifact = args.artifact.resolve()
    output_dir = (args.output_dir or args.artifact).resolve()
    goal_path = artifact / "config_snapshot" / "workload.goal"
    meta_path = artifact / "config_snapshot" / "workload.meta.json"
    lifecycle_path = artifact / "output_metrics" / "flow_dag_info.csv"
    log_path = artifact / "simulator.log"
    for path in (goal_path, meta_path, lifecycle_path, log_path):
        require(path.is_file(), f"required input does not exist: {path}")

    tasks, task_edges, declared_ranks = parse_goal(goal_path)
    lifecycle = read_lifecycle(lifecycle_path)
    metadata = json.loads(meta_path.read_text(encoding="utf-8"))
    raw_flows = metadata.get("flows")
    require(isinstance(raw_flows, dict), "metadata 'flows' must be an object")
    flows = {int(flow_id): value for flow_id, value in raw_flows.items()}

    expected_flow_count = int(metadata["workload"]["p2p_flows"])
    require(expected_flow_count > 0, "metadata contains no P2P flows")
    require(len(flows) == expected_flow_count, "metadata flow count mismatch")
    require(len(tasks) == int(metadata["workload"]["tasks"]), "GOAL/metadata task count mismatch")
    require(
        len(task_edges) == int(metadata["workload"]["dependency_edges"]),
        "GOAL/metadata dependency-edge count mismatch",
    )
    require(declared_ranks == int(metadata["workload"]["world_size"]), "GOAL/metadata rank count mismatch")
    require(len(lifecycle) == len(tasks), "lifecycle/GOAL task count mismatch")
    require(set(lifecycle) == set(tasks), "lifecycle and GOAL task identities differ")
    require({rank for rank, _ in tasks} == set(range(declared_ranks)), "GOAL ranks are not 0..num_ranks-1")
    require({rank for rank, _ in lifecycle} == set(range(declared_ranks)), "lifecycle ranks are not logical GOAL ranks")

    for key, task in tasks.items():
        row = lifecycle[key]
        require(row.op == task.op, f"op mismatch for task {key}: GOAL={task.op}, lifecycle={row.op}")
        if task.op in {"send", "recv"}:
            require(row.peer == task.peer and row.tag == task.tag, f"peer/tag mismatch for task {key}")
            require(row.size == task.size, f"size mismatch for task {key}")
        require(row.finish_ns >= row.ready_ns, f"finish precedes ready for task {key}")

    # The authoritative GOAL graph must be acyclic before communication collapse.
    task_order, _ = topological_order(tasks, task_edges, "GOAL task DAG")
    require(len(task_order) == len(tasks), "GOAL topological traversal is incomplete")

    task_to_flow: dict[TaskKey, int] = {}
    endpoint_records: dict[int, tuple[TaskKey, TaskKey]] = {}
    for flow_id, flow in sorted(flows.items()):
        send = (int(flow["send_task"]["rank"]), int(flow["send_task"]["task_id"]))
        recv = (int(flow["recv_task"]["rank"]), int(flow["recv_task"]["task_id"]))
        require(send in tasks and recv in tasks, f"flow {flow_id} maps to a missing GOAL task")
        require(tasks[send].op == "send" and tasks[recv].op == "recv", f"flow {flow_id} endpoint op mismatch")
        require(send not in task_to_flow, f"GOAL task {send} is reused by multiple flows")
        task_to_flow[send] = flow_id
        require(recv not in task_to_flow, f"GOAL task {recv} is reused by multiple flows")
        task_to_flow[recv] = flow_id
        require(send[0] == int(flow["src"]) and recv[0] == int(flow["dst"]), f"flow {flow_id} rank mismatch")
        require(tasks[send].peer == recv[0] and tasks[recv].peer == send[0], f"flow {flow_id} peer mismatch")
        require(tasks[send].tag == tasks[recv].tag == int(flow["tag"]), f"flow {flow_id} tag mismatch")
        require(send in lifecycle and recv in lifecycle, f"flow {flow_id} lacks lifecycle data")
        require(
            lifecycle[send].finish_ns == lifecycle[recv].finish_ns,
            f"flow {flow_id} send/recv finish mismatch: "
            f"{lifecycle[send].finish_ns} vs {lifecycle[recv].finish_ns}",
        )
        endpoint_records[flow_id] = (send, recv)
    require(len(task_to_flow) == 2 * len(flows), "flow endpoint mappings are not unique")
    require(
        set(task_to_flow) == {key for key, task in tasks.items() if task.op in {"send", "recv"}},
        "metadata does not cover every GOAL communication task exactly once",
    )

    def project(key: TaskKey) -> ProjectedKey:
        return ("flow", task_to_flow[key]) if key in task_to_flow else ("task", key[0], key[1])

    projected_nodes = {project(key) for key in tasks}
    projected_edges = {
        (project(source), project(target))
        for source, target in task_edges
        if project(source) != project(target)
    }
    projected_order, projected_successors = topological_order(
        projected_nodes, projected_edges, "flow-collapsed DAG"
    )

    projected_finish: dict[ProjectedKey, int] = {}
    for node in projected_nodes:
        if node[0] == "flow":
            _, recv = endpoint_records[node[1]]
            projected_finish[node] = lifecycle[recv].finish_ns
        else:
            projected_finish[node] = lifecycle[(node[1], node[2])].finish_ns

    # Every projected dependency must move forward (or tie) on the observed time axis.
    # This permits an observed path length to be defined as edge-to-edge elapsed time,
    # avoiding double counting overlapping send/recv lifetimes after collapse.
    for source, target in projected_edges:
        require(
            projected_finish[target] >= projected_finish[source],
            f"projected edge runs backward in time: {source} -> {target}",
        )

    terminal_finish: dict[ProjectedKey, int] = {}
    terminal_node: dict[ProjectedKey, ProjectedKey] = {}
    for node in reversed(projected_order):
        if projected_successors[node]:
            candidates = [
                (terminal_finish[target], terminal_node[target])
                for target in projected_successors[node]
            ]
        else:
            candidates = [(projected_finish[node], node)]
        finish, terminal = max(candidates, key=lambda item: (item[0], str(item[1])))
        terminal_finish[node] = finish
        terminal_node[node] = terminal

    terminals = {node for node in projected_nodes if not projected_successors[node]}
    require(
        all(terminal_node[node] in terminals for node in projected_nodes),
        "backward analysis did not resolve every node to a graph terminal",
    )

    c_ref = max(row.finish_ns for row in lifecycle.values())
    htsim_makespan, graph_makespan = parse_log_makespans(log_path)
    require(htsim_makespan is not None and graph_makespan is not None, "simulator log lacks makespan records")
    require(c_ref == htsim_makespan == graph_makespan, "lifecycle and simulator makespans differ")
    require(max(terminal_finish.values()) == c_ref, "backward analysis does not reach the completion horizon")

    flow_rows: list[dict] = []
    negative: list[tuple[int, int]] = []
    for flow_id, flow in sorted(flows.items()):
        send, recv = endpoint_records[flow_id]
        send_lifecycle = lifecycle[send]
        recv_lifecycle = lifecycle[recv]
        node: ProjectedKey = ("flow", flow_id)
        remaining = terminal_finish[node] - recv_lifecycle.finish_ns
        slack = c_ref - (recv_lifecycle.finish_ns + remaining)
        if slack < 0:
            negative.append((flow_id, slack))
        flow_rows.append(
            {
                "flow_id": flow_id,
                "collective_id": flow.get("collective_id", ""),
                "collective_node": flow.get("node_id", ""),
                "algorithm": flow.get("algorithm", flow.get("type", "unknown")),
                "phase": flow.get("phase", ""),
                "step": flow.get("step", ""),
                "chunk": flow.get("chunk", ""),
                "channel": flow.get("channel", ""),
                "src": flow["src"],
                "dst": flow["dst"],
                "tag": flow["tag"],
                "send_rank": send[0],
                "send_task_id": send[1],
                "recv_rank": recv[0],
                "recv_task_id": recv[1],
                "ready_ns": send_lifecycle.ready_ns,
                "issue_ns": send_lifecycle.issue_ns,
                "finish_ns": recv_lifecycle.finish_ns,
                "remaining_critical_ns": remaining,
                "oracle_slack_ns": slack,
                "ready_to_issue_wait_ns": send_lifecycle.issue_ns - send_lifecycle.ready_ns,
                "flow_duration_ns": recv_lifecycle.finish_ns - send_lifecycle.ready_ns,
                "ordering_domain": flow.get("ordering_domain", ""),
                "downstream_terminal": ":".join(map(str, terminal_node[node])),
                "downstream_terminal_finish_ns": terminal_finish[node],
            }
        )
    require(not negative, f"materially negative oracle slack: {negative}")

    windows: dict[int, list[dict]] = defaultdict(list)
    for row in flow_rows:
        windows[int(row["ready_ns"]) // args.ready_window_ns].append(row)

    ready_rows: list[dict] = []
    window_rows: list[dict] = []
    for window_id, members in sorted(windows.items()):
        start = window_id * args.ready_window_ns
        end = start + args.ready_window_ns
        slacks = [int(row["oracle_slack_ns"]) for row in members]
        summary = {
            "ready_window_id": window_id,
            "ready_window_start_ns": start,
            "ready_window_end_exclusive_ns": end,
            "ready_flow_count": len(members),
            "min_slack_ns": min(slacks),
            "max_slack_ns": max(slacks),
            "slack_spread_ns": max(slacks) - min(slacks),
            "distinct_collectives": len({row["collective_id"] for row in members}),
        }
        window_rows.append(summary)
        for row in sorted(members, key=lambda item: (int(item["ready_ns"]), int(item["flow_id"]))):
            ready_rows.append(
                summary
                | {
                    "flow_id": row["flow_id"],
                    "collective_id": row["collective_id"],
                    "collective_node": row["collective_node"],
                    "algorithm": row["algorithm"],
                    "phase": row["phase"],
                    "step": row["step"],
                    "chunk": row["chunk"],
                    "ready_ns": row["ready_ns"],
                    "issue_ns": row["issue_ns"],
                    "oracle_slack_ns": row["oracle_slack_ns"],
                }
            )

    metrics_dir = output_dir / "output_metrics"
    flow_fields = list(flow_rows[0])
    ready_fields = list(ready_rows[0])
    window_fields = list(window_rows[0])
    write_csv(metrics_dir / "oracle_slack.csv", flow_fields, flow_rows)
    write_csv(metrics_dir / "ready_set_slack.csv", ready_fields, ready_rows)
    write_csv(metrics_dir / "ready_window_summary.csv", window_fields, window_rows)

    all_slacks = [int(row["oracle_slack_ns"]) for row in flow_rows]
    algorithms = sorted({str(row["algorithm"]) for row in flow_rows})
    heterogeneous_windows = sorted(
        (row for row in window_rows if int(row["ready_flow_count"]) > 1),
        key=lambda row: (-int(row["slack_spread_ns"]), int(row["ready_window_start_ns"])),
    )
    workload_name = str(metadata["workload"].get("name", artifact.name))
    report = [
        f"# Offline oracle-slack analysis: {workload_name}",
        "",
        f"- Input artifact: `{artifact}`",
        f"- Observed `C_ref`: **{c_ref} ns**",
        f"- Ready-window width: **{args.ready_window_ns} ns** (fixed half-open bins)",
        f"- Mapped logical flows: **{len(flow_rows)}/{expected_flow_count}**",
        f"- Authoritative GOAL task DAG: **{len(tasks)} nodes, {len(task_edges)} edges**",
        f"- Flow-collapsed DAG: **{len(projected_nodes)} nodes, {len(projected_edges)} edges**",
        "",
        "## Method and communication-collapse rule",
        "",
        "Each metadata flow replaces its matched send/recv GOAL pair with one logical node. "
        "Incoming and outgoing GOAL edges from both endpoints are retained and deduplicated; calc nodes remain intact.",
        "",
        "`ready_ns` and `issue_ns` are the matched **send task's** dependency-ready and issue times: "
        "these represent when the message data became eligible and when it was actually launched. "
        "`finish_ns` is the matched **recv task's** finish time, because receiver completion is the semantic "
        "completion point for downstream dependencies. In this artifact every matched send/recv pair has the same finish time.",
        "",
        "For an observed-time backward path, every collapsed-DAG edge is weighted by the increase in observed "
        "finish time. Thus `remaining_critical_ns` is the largest elapsed observed time from flow completion "
        "to a reachable terminal. The edge weights telescope; overlapping send/recv lifetimes are not counted twice. "
        "This is an offline property of the recorded execution, not a counterfactual duration model.",
        "",
        "## Validation",
        "",
        f"- PASS — all {expected_flow_count} metadata flows map to valid, unique send/recv GOAL tasks.",
        f"- PASS — lifecycle records cover all {len(tasks)} GOAL tasks and all mapped endpoints.",
        f"- PASS — authoritative GOAL DAG and {len(projected_nodes)}-node collapsed DAG are acyclic.",
        "- PASS — all projected dependency edges are nondecreasing in observed finish time.",
        "- PASS — every backward result reaches a valid terminal; the global horizon is reached.",
        "- PASS — no negative oracle slack was computed.",
        f"- PASS — lifecycle maximum, `Htsim time`, and application graph makespan all equal {c_ref} ns.",
        f"- PASS — CSV identities are logical GOAL ranks 0–{declared_ranks - 1}; physical topology host IDs are never used.",
        "",
        "## Slack results",
        "",
        f"Overall oracle slack (min / median / max): **{min(all_slacks)} / "
        f"{format_table_value(median_number(all_slacks))} / {max(all_slacks)} ns**.",
        "",
        "| Algorithm | Flows | Min slack (ns) | Median slack (ns) | Max slack (ns) |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for algorithm in algorithms:
        values = [int(row["oracle_slack_ns"]) for row in flow_rows if row["algorithm"] == algorithm]
        report.append(
            f"| {algorithm} | {len(values)} | {min(values)} | "
            f"{format_table_value(median_number(values))} | {max(values)} |"
        )

    report.extend(["", "## Ready windows with the largest slack spread", ""])
    if heterogeneous_windows:
        report.extend(
            [
                "| Ready window (ns) | Flows | Collectives | Min slack | Max slack | Spread |",
                "| --- | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in heterogeneous_windows[:10]:
            report.append(
                f"| {row['ready_window_start_ns']}–{row['ready_window_end_exclusive_ns']} | "
                f"{row['ready_flow_count']} | {row['distinct_collectives']} | {row['min_slack_ns']} | "
                f"{row['max_slack_ns']} | {row['slack_spread_ns']} |"
            )
    else:
        report.append("No ready window contains more than one flow.")

    if heterogeneous_windows:
        best = heterogeneous_windows[0]
        best_members = windows[int(best["ready_window_id"])]
        report.extend(
            [
                "",
                "## Representative same-window flows",
                "",
                f"Ready window: **{best['ready_window_start_ns']}–{best['ready_window_end_exclusive_ns']} ns**",
                "",
                "| Flow | Algorithm | Phase | Ready | Issue | Slack (ns) |",
                "| ---: | --- | --- | ---: | ---: | ---: |",
            ]
        )
        for row in sorted(best_members, key=lambda item: int(item["flow_id"])):
            report.append(
                f"| {row['flow_id']} | {row['algorithm']} | {row['phase']} | {row['ready_ns']} | "
                f"{row['issue_ns']} | {row['oracle_slack_ns']} |"
            )
        report.extend(
            [
                "",
                f"Measured slack spread in this window: **{best['slack_spread_ns']} ns**. "
                "This table reports the observation without assigning causal or statistical significance.",
            ]
        )

    report.extend(
        [
            "",
            "## Sufficiency and limitation",
            "",
            "The artifact is sufficient for this offline oracle-slack measurement: it contains the authoritative GOAL "
            "dependencies, unique flow-to-task mappings, complete lifecycle timestamps, and an independently logged "
            "application makespan. No additional simulator instrumentation is required for the reported metric.",
            "",
            "The remaining-path value is an **observed-time** quantity. Reinterpreting it as a sum of isolated node "
            "service durations would require an additional causal/resource model and can double-count overlapping "
            "communication lifetimes after send/recv collapse; that alternative is intentionally not claimed here.",
            "",
        ]
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "slack_analysis.md").write_text("\n".join(report), encoding="utf-8")

    print(f"PASS: analyzed {len(flow_rows)} flows; C_ref={c_ref} ns")
    print(
        "PASS: oracle slack min/median/max="
        f"{min(all_slacks)}/{format_table_value(median_number(all_slacks))}/{max(all_slacks)} ns"
    )
    print(f"PASS: wrote analysis to {output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (KeyError, TypeError, ValueError, OSError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)

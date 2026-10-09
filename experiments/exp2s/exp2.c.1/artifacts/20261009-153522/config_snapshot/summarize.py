#!/usr/bin/env python3
"""Build one human-readable summary for an experiment run directory."""

from __future__ import annotations

import csv
import json
import re
import sys
from pathlib import Path


def read_text(path: Path, default: str = "unknown") -> str:
    try:
        value = path.read_text(encoding="utf-8").strip()
        return value or default
    except OSError:
        return default


def read_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return values
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.isidentifier() and not value.startswith("("):
            values[key] = value.strip().strip('"').strip("'")
    return values


def read_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except OSError:
        return []


def read_dragonfly_dimensions(path: Path) -> tuple[int, int] | None:
    values: dict[str, int] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    for raw in lines:
        tokens = raw.split()
        if len(tokens) >= 2 and tokens[0] in {"p", "a"}:
            try:
                values[tokens[0]] = int(tokens[1])
            except ValueError:
                return None
    if "p" not in values or "a" not in values:
        return None
    return values["p"], values["a"]


def read_rank_placements(run_dir: Path) -> list[dict[str, int]]:
    """Read the snapshotted placement and derive physical host IDs.

    The snapshot is the sole source of truth. The returned records always
    contain ``rank`` and ``physical_host``; the coordinate fields are present
    when the current placement schema is used.
    """
    placement_path = run_dir / "config_snapshot" / "rank_placement.json"
    try:
        placement = json.loads(placement_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []

    # Retain support for summaries of runs made with the original compact schema.
    rank_to_host = placement.get("rank_to_host")
    if isinstance(rank_to_host, list):
        try:
            records = [
                {"rank": rank, "physical_host": int(host)}
                for rank, host in enumerate(rank_to_host)
            ]
        except (TypeError, ValueError):
            return []
        if len({row["physical_host"] for row in records}) != len(records):
            return []
        return records

    dimensions = read_dragonfly_dimensions(
        run_dir / "config_snapshot" / "topology" / "dragonfly.topo"
    )
    placements = placement.get("placements")
    if dimensions is None or not isinstance(placements, list):
        return []

    hosts_per_switch, switches_per_group = dimensions
    records: list[dict[str, int]] = []
    try:
        for endpoint in placements:
            rank = int(endpoint["rank"])
            group = int(endpoint["group"])
            switch = int(endpoint["switch"])
            host = int(endpoint["host"])
            physical_host = (
                (group * switches_per_group + switch) * hosts_per_switch + host
            )
            records.append(
                {
                    "rank": rank,
                    "group": group,
                    "switch": switch,
                    "host": host,
                    "physical_host": physical_host,
                }
            )
    except (KeyError, TypeError, ValueError):
        return []
    if len({row["rank"] for row in records}) != len(records):
        return []
    if len({row["physical_host"] for row in records}) != len(records):
        return []
    return sorted(records, key=lambda row: row["rank"])


def split_flow_id(value: str) -> tuple[str, str, str]:
    parts = value.split("_", 2)
    return tuple(parts) if len(parts) == 3 else (value, "?", "?")


def format_delays(raw: str) -> str:
    try:
        return "/".join(f"{float(value) / 1000:g}" for value in raw.split(":"))
    except ValueError:
        return raw


def parse_graph_metrics(log_text: str) -> tuple[int | None, int | None, dict[int, int]]:
    time_match = re.search(r"Htsim time (\d+)", log_text)
    critical_match = re.search(r"Maximum finishing time at host (\d+): (\d+)", log_text)
    host_times = {
        int(rank): int(finish)
        for rank, finish in re.findall(r"^Host (\d+): (\d+)$", log_text, re.MULTILINE)
    }
    simulated_ns = int(time_match.group(1)) if time_match else None
    critical_rank = int(critical_match.group(1)) if critical_match else None
    return simulated_ns, critical_rank, host_times


def format_duration_ns(value: int | float) -> str:
    if value >= 1000:
        return f"{value:g} ns ({value / 1000:g} µs)"
    return f"{value:g} ns"


def parse_optional_int(value: str | None) -> int | None:
    try:
        return int(value) if value not in (None, "") else None
    except ValueError:
        return None


def dag_node_sort_key(row: dict[str, str]) -> tuple[int, int]:
    rank = parse_optional_int(row.get("rank"))
    offset = parse_optional_int(row.get("nodeOffset"))
    return rank if rank is not None else sys.maxsize, offset if offset is not None else sys.maxsize


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} RUN_DIR", file=sys.stderr)
        return 2

    run_dir = Path(sys.argv[1]).resolve()
    config = read_env(run_dir / "config_snapshot" / "config.env")
    global_rows = read_csv(run_dir / "output_metrics" / "globalInfo.csv")
    flows = read_csv(run_dir / "output_metrics" / "flowsInfo.csv")
    dag_nodes = read_csv(run_dir / "output_metrics" / "flow_dag_info.csv")
    global_info = global_rows[0] if global_rows else {}
    exit_code = read_text(run_dir / "exit_code.txt")
    status = "SUCCESS" if exit_code == "0" else "FAILED"
    log_text = read_text(run_dir / "simulator.log", default="")
    simulated_ns, critical_rank, host_times = parse_graph_metrics(log_text)
    git_revision = read_text(run_dir / "git-revision.txt")
    git_status = read_text(run_dir / "git-status.txt", default="")
    command = read_text(run_dir / "command.txt")
    rank_placements = read_rank_placements(run_dir)
    physical_host_to_goal_rank = {
        row["physical_host"]: row["rank"] for row in rank_placements
    }
    experiment_name = config.get("EXPERIMENT_NAME", run_dir.parent.parent.name)
    experiment_description = config.get("EXPERIMENT_DESCRIPTION", "")
    title = f"{experiment_name}: {experiment_description}" if experiment_description else experiment_name

    lines = [
        f"# {title} — {run_dir.name}",
        "",
        f"**Status:** {status} (exit code `{exit_code}`)",
        "",
        "## Application result",
        "",
    ]
    if simulated_ns is not None:
        lines.append(f"- Computation-graph makespan: **{format_duration_ns(simulated_ns)}**")
    else:
        lines.append("- Computation-graph makespan: **not available**")
    if critical_rank is not None:
        lines.append(f"- Critical GOAL rank: **{critical_rank}**")
    lines.append(f"- Completed GOAL ranks: **{len(host_times)}**")

    if host_times:
        lines.extend(
            [
                "",
                "| GOAL rank | Graph finish (ns) | Graph finish (µs) |",
                "| ---: | ---: | ---: |",
            ]
        )
        for rank, finish in sorted(host_times.items()):
            marker = " **(critical)**" if rank == critical_rank else ""
            lines.append(f"| {rank}{marker} | {finish} | {finish / 1000:.3f} |")

    lines.extend(["", "## GOAL DAG lifecycle", ""])
    if dag_nodes:
        completed_nodes = sum(
            parse_optional_int(row.get("finishTimeNs")) is not None for row in dag_nodes
        )
        op_counts: dict[str, int] = {}
        ready_to_issue: list[int] = []
        issue_to_finish: list[int] = []
        for row in dag_nodes:
            op = row.get("op", "unknown") or "unknown"
            op_counts[op] = op_counts.get(op, 0) + 1
            ready = parse_optional_int(row.get("readyTimeNs"))
            issue = parse_optional_int(row.get("issueTimeNs"))
            finish = parse_optional_int(row.get("finishTimeNs"))
            if ready is not None and issue is not None:
                ready_to_issue.append(issue - ready)
            if issue is not None and finish is not None:
                issue_to_finish.append(finish - issue)

        counts = ", ".join(f"{op}={count}" for op, count in sorted(op_counts.items()))
        lines.extend(
            [
                f"- Recorded nodes: **{len(dag_nodes)}**",
                f"- Completed nodes: **{completed_nodes}/{len(dag_nodes)}**",
                f"- Operations: **{counts}**",
            ]
        )
        if ready_to_issue:
            lines.append(
                "- Ready-to-issue wait min / average / max: "
                f"**{min(ready_to_issue):g} / "
                f"{sum(ready_to_issue) / len(ready_to_issue):g} / "
                f"{max(ready_to_issue):g} ns**"
            )
        if issue_to_finish:
            lines.append(
                "- Issue-to-finish duration min / average / max: "
                f"**{min(issue_to_finish):g} / "
                f"{sum(issue_to_finish) / len(issue_to_finish):g} / "
                f"{max(issue_to_finish):g} ns**"
            )

        max_dag_rows = 200
        shown_nodes = sorted(dag_nodes, key=dag_node_sort_key)[:max_dag_rows]
        lines.extend(
            [
                "",
                "| Rank | Node offset | Op | Peer | Tag | Size | Ready (ns) | Issue (ns) | Finish (ns) |",
                "| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in shown_nodes:
            lines.append(
                f"| {row.get('rank', '?')} | {row.get('nodeOffset', '?')} | "
                f"{row.get('op', '?')} | {row.get('peer', '?')} | {row.get('tag', '?')} | "
                f"{row.get('size', '?')} | {row.get('readyTimeNs') or '?'} | "
                f"{row.get('issueTimeNs') or '?'} | {row.get('finishTimeNs') or '?'} |"
            )
        if len(dag_nodes) > max_dag_rows:
            lines.extend(
                [
                    "",
                    f"Showing the first {max_dag_rows} nodes in rank/offset order; "
                    "the complete lifecycle is in `output_metrics/flow_dag_info.csv`.",
                ]
            )
    else:
        lines.append(
            "No GOAL DAG lifecycle records were produced. See "
            "`output_metrics/flow_dag_info.csv` and `simulator.log`."
        )

    lines.extend(
        [
            "",
            "## Configuration",
            "",
            f"- Started: `{read_text(run_dir / 'started_at.txt')}`",
            f"- Finished: `{read_text(run_dir / 'finished_at.txt')}`",
            f"- Routing: `{config.get('ROUTING', 'unknown')}`",
            f"- Workload: `{config.get('WORKLOAD', 'unknown')}`",
            f"- Topology: `{config.get('TOPOLOGY', 'unknown')}`",
            f"- Sender CC: `{config.get('SENDER_CC_ALGO', 'unknown')}`",
            f"- Multipath algorithm: `{config.get('LOAD_BALANCING_ALGO', 'unknown')}`",
            f"- Path entropy: `{config.get('PATH_ENTROPY_SIZE', 'unknown')}`",
            f"- Initial cwnd: `{config.get('CWND_BYTES', 'unknown')} bytes` (`0` means simulator default)",
            f"- Queue: `{config.get('QUEUE_PACKETS', 'unknown')}` packets",
            "- Rank placement snapshot: "
            + (
                "`config_snapshot/rank_placement.json`"
                if rank_placements
                else "not available"
            ),
            f"- Messagelet config: `{config.get('MESSAGELET_CONFIG', 'not provided')}`",
            "",
            "## Rank placement",
            "",
        ]
    )

    if rank_placements:
        lines.append(
            "Derived from the snapshotted `config_snapshot/rank_placement.json`; "
            "no descriptive placement string from `config.env` is used."
        )
        if all({"group", "switch", "host"} <= row.keys() for row in rank_placements):
            ranks_by_location: dict[tuple[int, int], list[int]] = {}
            for row in rank_placements:
                location = (row["group"], row["switch"])
                ranks_by_location.setdefault(location, []).append(row["rank"])
            lines.append("")
            for (group, switch), ranks in sorted(ranks_by_location.items()):
                rank_list = ", ".join(str(rank) for rank in sorted(ranks))
                lines.append(f"- Group {group}, switch {switch}: ranks {rank_list}")
            lines.extend(
                [
                    "",
                    "| GOAL rank | Group | Switch | Host | Physical HTSIM host |",
                    "| ---: | ---: | ---: | ---: | ---: |",
                ]
            )
            for row in rank_placements:
                lines.append(
                    f"| {row['rank']} | {row['group']} | {row['switch']} | "
                    f"{row['host']} | {row['physical_host']} |"
                )
        else:
            lines.extend(
                [
                    "",
                    "| GOAL rank | Physical HTSIM host |",
                    "| ---: | ---: |",
                ]
            )
            for row in rank_placements:
                lines.append(f"| {row['rank']} | {row['physical_host']} |")
    else:
        lines.append("No valid snapshotted rank placement was available.")

    lines.extend(
        [
            "",
            "## Network",
            "",
            f"- Flow pattern: `{config.get('FLOW_PATTERN', 'not specified')}`",
        ]
    )

    if global_info:
        lines.extend(
            [
                f"- Link speed: `{global_info.get('linkSpeedGbps', '?')} Gbps`",
                "- Link delays (global/local/host): "
                f"`{format_delays(global_info.get('linkDelayNs', '?'))} ns`",
                f"- Packet size: `{global_info.get('packetSizeBytes', '?')} bytes`",
                f"- Queue size: `{global_info.get('queueSizeBytes', '?')} bytes`",
                f"- ECN thresholds (low/high): `{global_info.get('kMinBytes', '?')}` / "
                f"`{global_info.get('kMaxBytes', '?')} bytes`",
                f"- SACK threshold: `{global_info.get('sackThresholdBytes', '?')} bytes`",
            ]
        )

    lines.extend(
        [
            "",
            "## Flow results",
            "",
            "The raw `flowsInfo.csv` `srcNode`/`dstNode` values are **physical HTSIM "
            "host IDs after rank placement**.",
        ]
    )
    if physical_host_to_goal_rank:
        lines.extend(
            [
                "The GOAL rank endpoints below are reverse-mapped from the snapshotted "
                "`rank_placement.json` and Dragonfly topology.",
                "",
            ]
        )
    else:
        lines.extend(
            [
                "No rank-placement snapshot was available, so this summary does not infer "
                "logical GOAL rank endpoints.",
                "",
            ]
        )
    if flows:
        if physical_host_to_goal_rank:
            lines.extend(
                [
                    "| GOAL rank flow | Physical HTSIM host flow | Size (bytes) | Start (ns) | End (ns) | FCT (ns) | Base RTT (ns) | Packets |",
                    "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
                ]
            )
        else:
            lines.extend(
                [
                    "| Physical HTSIM host flow | Size (bytes) | Start (ns) | End (ns) | FCT (ns) | Base RTT (ns) | Packets |",
                    "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
                ]
            )
        fcts: list[float] = []
        total_bytes = 0
        total_packets = 0
        completion_order: list[str] = []
        for row in flows:
            src, dst, _ = split_flow_id(row.get("srcNode_dstNode_flowId", "?"))
            fct = float(row.get("fctNs", "nan"))
            fcts.append(fct)
            total_bytes += int(row.get("flowSizeBytes", "0"))
            total_packets += int(row.get("totalPackets", "0"))
            physical_flow = f"{src}→{dst}"
            if physical_host_to_goal_rank:
                try:
                    goal_src = physical_host_to_goal_rank[int(src)]
                    goal_dst = physical_host_to_goal_rank[int(dst)]
                    goal_flow = f"{goal_src}→{goal_dst}"
                except (KeyError, ValueError):
                    goal_flow = "?→?"
                completion_order.append(goal_flow)
                lines.append(
                    f"| {goal_flow} | {physical_flow} | {row.get('flowSizeBytes', '?')} | "
                    f"{row.get('startTimeNs', '?')} | {row.get('endTimeNs', '?')} | "
                    f"{fct:.3f} | {row.get('baseRttNs', '?')} | {row.get('totalPackets', '?')} |"
                )
            else:
                completion_order.append(physical_flow)
                lines.append(
                    f"| {physical_flow} | {row.get('flowSizeBytes', '?')} | "
                    f"{row.get('startTimeNs', '?')} | {row.get('endTimeNs', '?')} | "
                    f"{fct:.3f} | {row.get('baseRttNs', '?')} | {row.get('totalPackets', '?')} |"
                )
        lines.extend(
            [
                "",
                f"- Completed flows: **{len(fcts)}**",
                f"- Total communicated data: **{total_bytes} bytes ({total_bytes / 1024:.0f} KiB)**",
                f"- Total data packets: **{total_packets}**",
                "- Completion order "
                f"({'GOAL rank flows' if physical_host_to_goal_rank else 'physical host flows'}): "
                f"**{' → '.join(completion_order)}**",
                f"- FCT min / average / max: **{min(fcts):.3f} / "
                f"{sum(fcts) / len(fcts):.3f} / {max(fcts):.3f} ns**",
            ]
        )
    else:
        lines.append("No completed-flow CSV records were produced. See `simulator.log`.")

    lines.extend(
        [
            "",
            "## Reproducibility",
            "",
            f"- Git revision: `{git_revision}`",
            f"- Worktree at launch: `{'dirty' if git_status else 'clean'}`",
            "- Exact command:",
            "",
            "```bash",
            command,
            "```",
        ]
    )
    if git_status:
        lines.extend(
            [
                "",
                "<details><summary>Worktree changes at launch</summary>",
                "",
                "```text",
                git_status,
                "```",
                "",
                "</details>",
            ]
        )

    lines.extend(
        [
            "",
            "## Files",
            "",
            "- Full simulator output: `simulator.log`",
            "- Per-flow CSV: `output_metrics/flowsInfo.csv`",
            "- GOAL DAG lifecycle CSV: `output_metrics/flow_dag_info.csv`",
            "- Global CSV: `output_metrics/globalInfo.csv`",
            "- Exact input snapshot: `config_snapshot/`",
            "- Resolved command: `command.txt`",
            "",
        ]
    )
    (run_dir / "summary.md").write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

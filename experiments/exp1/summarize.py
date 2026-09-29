#!/usr/bin/env python3
"""Build one human-readable summary for an exp1 run directory."""

from __future__ import annotations

import csv
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


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} RUN_DIR", file=sys.stderr)
        return 2

    run_dir = Path(sys.argv[1]).resolve()
    config = read_env(run_dir / "config_snapshot" / "config.env")
    global_rows = read_csv(run_dir / "output_metrics" / "globalInfo.csv")
    flows = read_csv(run_dir / "output_metrics" / "flowsInfo.csv")
    global_info = global_rows[0] if global_rows else {}
    exit_code = read_text(run_dir / "exit_code.txt")
    status = "SUCCESS" if exit_code == "0" else "FAILED"
    log_text = read_text(run_dir / "simulator.log", default="")
    simulated_ns, critical_rank, host_times = parse_graph_metrics(log_text)
    git_revision = read_text(run_dir / "git-revision.txt")
    git_status = read_text(run_dir / "git-status.txt", default="")
    command = read_text(run_dir / "command.txt")

    lines = [
        f"# Experiment 1 summary — {run_dir.name}",
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
        lines.append(f"- Critical rank: **{critical_rank}**")
    lines.append(f"- Completed ranks: **{len(host_times)}**")

    if host_times:
        lines.extend(
            [
                "",
                "| Rank | Graph finish (ns) | Graph finish (µs) |",
                "| ---: | ---: | ---: |",
            ]
        )
        for rank, finish in sorted(host_times.items()):
            marker = " **(critical)**" if rank == critical_rank else ""
            lines.append(f"| {rank}{marker} | {finish} | {finish / 1000:.3f} |")

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
            "",
            "## Network",
            "",
            "- Rank placement: `0,1,2 → group 0`; `3,4,5 → group 1`; group 2 is the two-hop alternative.",
            "- Concurrent flows: `0→3`, `1→4`, `2→5`.",
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

    lines.extend(["", "## Flow results", ""])
    if flows:
        lines.extend(
            [
                "| Flow | Size (bytes) | Start (ns) | End (ns) | FCT (ns) | Base RTT (ns) | Packets |",
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
            completion_order.append(f"{src}→{dst}")
            lines.append(
                f"| {src}→{dst} | {row.get('flowSizeBytes', '?')} | "
                f"{row.get('startTimeNs', '?')} | {row.get('endTimeNs', '?')} | "
                f"{fct:.3f} | {row.get('baseRttNs', '?')} | {row.get('totalPackets', '?')} |"
            )
        lines.extend(
            [
                "",
                f"- Completed flows: **{len(fcts)}**",
                f"- Total communicated data: **{total_bytes} bytes ({total_bytes / 1024:.0f} KiB)**",
                f"- Total data packets: **{total_packets}**",
                f"- Completion order: **{' → '.join(completion_order)}**",
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

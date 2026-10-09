#!/usr/bin/env python3
"""Assign manual SOURCE routing labels to the flows in a workload metadata file."""

import argparse
import json
import re
from pathlib import Path

LABELS = ("minimum", "valiant")
REPO_ROOT = Path(__file__).resolve().parents[2]


def nonnegative(value: object, field: str, maximum: int = 2**32 - 1) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"{field} must be an integer in 0..{maximum}")
    return value


def build_config(meta: dict, default_label: str, assignments: dict[int, str]) -> dict:
    raw_flows = meta.get("flows")
    if not isinstance(raw_flows, dict):
        raise ValueError("metadata must contain a flows object")
    if default_label not in LABELS or any(label not in LABELS for label in assignments.values()):
        raise ValueError("labels must be minimum or valiant")
    flows = {}
    send_tasks = set()
    for key, flow in sorted(raw_flows.items(), key=lambda item: int(item[0])):
        flow_id = nonnegative(int(key), "flow ID", 2**64 - 1)
        if key != str(flow_id) or flow.get("flow_id", flow_id) != flow_id:
            raise ValueError(f"inconsistent flow ID {key}")
        src = nonnegative(flow["src"], f"flow {key}.src")
        dst = nonnegative(flow["dst"], f"flow {key}.dst")
        task = flow["send_task"]
        if task["rank"] != src:
            raise ValueError(f"flow {key} send rank differs from src")
        task_id = nonnegative(task["task_id"], f"flow {key}.send_task.task_id")
        if (src, task_id) in send_tasks:
            raise ValueError(f"flow {key} repeats a GOAL send task")
        send_tasks.add((src, task_id))
        flows[key] = {
            "src": src,
            "dst": dst,
            "tag": nonnegative(flow["tag"], f"flow {key}.tag"),
            "bytes": nonnegative(flow["bytes"], f"flow {key}.bytes", 2**64 - 1),
            "send_task_id": task_id,
            "label": assignments.get(flow_id, default_label),
        }
    unknown = assignments.keys() - {int(key) for key in flows}
    if unknown:
        raise ValueError(f"assignments reference unknown flow IDs: {sorted(unknown)}")
    declared_count = meta.get("workload", {}).get("p2p_flows", len(flows))
    if declared_count != len(flows):
        raise ValueError("metadata p2p_flows does not match flows")
    return {
        "schema_version": 1,
        "workload_name": meta.get("workload", {}).get("name", "unknown"),
        "flow_count": len(flows),
        "flows": flows,
    }


def find_metadata(experiment: Path) -> Path:
    # Read the declarative WORKLOAD value without executing config.env.
    config = (experiment / "config.env").read_text(encoding="utf-8")
    match = re.search(r"^WORKLOAD=(.*?)\s*$", config, re.MULTILINE)
    if not match:
        raise ValueError("config.env has no WORKLOAD; use --meta")
    value = match[1].strip().strip('"\'').replace("${SCRIPT_DIR}", str(experiment))
    if "$" in value:
        raise ValueError("WORKLOAD contains an unsupported shell expression; use --meta")
    workload = Path(value)
    if not workload.is_absolute():
        workload = REPO_ROOT / workload
    meta = workload.with_suffix(".meta.json")
    if not meta.is_file():
        raise ValueError(f"workload metadata not found: {meta}; use --meta for another location")
    return meta


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", type=Path, help="experiment directory containing config.env")
    parser.add_argument("--meta", type=Path, help="explicit workload.meta.json path")
    parser.add_argument("--label", choices=LABELS, default="minimum", help="label assigned to all flows")
    parser.add_argument("--assign", action="append", default=[], metavar="FLOW_ID=LABEL",
                        help="override one flow; may be repeated")
    parser.add_argument("--output", type=Path, help="default: EXPERIMENT/routing_configs/default.json")
    parser.add_argument("--force", action="store_true", help="replace an existing output file")
    args = parser.parse_args()
    try:
        experiment = args.experiment.resolve()
        metadata = args.meta.resolve() if args.meta else find_metadata(experiment)
        assignments = {}
        for assignment in args.assign:
            key, label = assignment.split("=", 1)
            flow_id = nonnegative(int(key), "assigned flow ID", 2**64 - 1)
            if flow_id in assignments:
                raise ValueError(f"flow {flow_id} is assigned more than once")
            assignments[flow_id] = label
        config = build_config(json.loads(metadata.read_text(encoding="utf-8")), args.label, assignments)
        output = args.output or experiment / "routing_configs" / "default.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w" if args.force else "x", encoding="utf-8") as stream:
            stream.write(json.dumps(config, indent=2) + "\n")
        counts = {label: sum(flow["label"] == label for flow in config["flows"].values()) for label in LABELS}
        print(f"Wrote {output}: {config['flow_count']} flows, {counts}")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()

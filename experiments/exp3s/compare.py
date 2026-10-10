#!/usr/bin/env python3
"""Validate and compare E3-0..E3-3 completed experiment artifacts."""

import argparse
import csv
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CASES = (("E3-0", "exp3.a", None), ("E3-1", "exp3.a.1", 0),
         ("E3-2", "exp3.a.2", 48), ("E3-3", "exp3.a.3", 46))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def by_flow(rows):
    indexed = {int(row["flow_id"]): row for row in rows}
    require(len(indexed) == len(rows) == 60, "expected 60 unique metadata flow IDs")
    require(set(indexed) == set(range(60)), "expected metadata flow IDs 0..59")
    return indexed


def read_run(path, target):
    path = path.resolve(strict=True)
    require((path / "exit_code.txt").read_text().strip() == "0", f"failed run: {path}")
    snapshot = path / "config_snapshot"
    routes = by_flow(read_csv(path / "output_metrics/flowRouting.csv"))
    slack = by_flow(read_csv(path / "output_metrics/oracle_slack.csv"))
    config = json.loads((snapshot / "routing_config.json").read_text())
    require(config["flow_count"] == len(config["flows"]) == 60, "invalid routing flow count")
    for fid, row in routes.items():
        expected = "valiant" if fid == target else "minimum"
        assignment = config["flows"][str(fid)]
        require(row["label"] == assignment["label"] == expected,
                f"{path}: flow {fid} must use {expected}")
        require(row["intermediate_group"] == ("2" if fid == target else ""),
                f"{path}: unexpected intermediate group for flow {fid}")
        require(bool(int(row["path_id"]) & (1 << 31)) == (fid == target),
                f"{path}: unexpected waypoint mark for flow {fid}")
        for route_key, config_key in (("src_rank", "src"), ("dst_rank", "dst"),
                                      ("send_task_id", "send_task_id")):
            require(int(row[route_key]) == assignment[config_key] == int(slack[fid][config_key]),
                    f"{path}: flow identity mismatch for {fid}")
    log = (path / "simulator.log").read_text()
    times = {int(rank): int(finish) for rank, finish in
             re.findall(r"^Host (\d+): (\d+)$", log, re.MULTILINE)}
    require(set(times) == set(range(12)), f"{path}: incomplete GOAL rank completion")
    cct = max(times.values())
    match = re.search(r"Htsim time (\d+)", log)
    require(match is not None and int(match[1]) == cct, f"{path}: CCT mismatch")
    dag = read_csv(path / "output_metrics/flow_dag_info.csv")
    require(len(dag) == 200 and max(int(row["finishTimeNs"]) for row in dag) == cct,
            f"{path}: incomplete or inconsistent lifecycle metrics")
    return {"path": path, "routes": routes, "slack": slack, "config": config,
            "times": times, "cct": cct, "dag": dag}


def validate_inputs(base, run):
    for name in ("workload.bin", "workload.goal", "workload.meta.json",
                 "rank_placement.json", "messagelet_config.json", "topology/dragonfly.topo"):
        require((base["path"] / "config_snapshot" / name).read_bytes() ==
                (run["path"] / "config_snapshot" / name).read_bytes(),
                f"{run['path']}: baseline input differs: {name}")
    def without_labels(config):
        result = json.loads(json.dumps(config))
        for flow in result["flows"].values():
            flow.pop("label")
        return result
    require(without_labels(base["config"]) == without_labels(run["config"]),
            f"{run['path']}: routing identities differ from baseline")
    # Parse declarative settings without executing the snapshotted shell config.
    def settings(path):
        return dict(re.findall(r"^([A-Z_]+)=(.*)$",
                               (path / "config_snapshot/config.env").read_text(), re.MULTILINE))
    base_settings, run_settings = settings(base["path"]), settings(run["path"])
    for key in ("ROUTING", "SENDER_CC_ALGO", "LOAD_BALANCING_ALGO", "PATH_ENTROPY_SIZE",
                "QUEUE_PACKETS", "CWND_BYTES", "BINARY", "TRACE_FLOW_COMPLETIONS",
                "SLACK_READY_WINDOW_NS", "SIM_EXTRA_ARGS"):
        require(base_settings.get(key) == run_settings.get(key),
                f"{run['path']}: simulator setting differs: {key}")
    require(read_csv(base["path"] / "output_metrics/globalInfo.csv") ==
            read_csv(run["path"] / "output_metrics/globalInfo.csv"),
            f"{run['path']}: network settings differ")
    for fid, route in base["routes"].items():
        for key in ("src_rank", "dst_rank", "send_task_id", "src_host", "dst_host"):
            require(route[key] == run["routes"][fid][key],
                    f"{run['path']}: runtime identity differs for flow {fid}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "exp3.a/artifacts/latest")
    for index in range(1, 4):
        parser.add_argument(f"--e3-{index}", type=Path,
                            default=ROOT / f"exp3.a.{index}/artifacts/latest")
    parser.add_argument("--output-dir", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        paths = [args.baseline, args.e3_1, args.e3_2, args.e3_3]
        runs = [read_run(path, case[2]) for path, case in zip(paths, CASES)]
        base = runs[0]
        for run in runs[1:]:
            validate_inputs(base, run)
        results = []
        for (case, experiment, fid), run in zip(CASES, runs):
            delta = run["cct"] - base["cct"]
            results.append({
                "case": case, "experiment": experiment, "artifact": str(run["path"]),
                "detour_flow_id": fid if fid is not None else "",
                "baseline_slack_ns": base["slack"][fid]["oracle_slack_ns"] if fid is not None else "",
                "cct_ns": run["cct"], "delta_cct_ns": delta,
                "cct_reduction_pct": -100 * delta / base["cct"],
                "critical_ranks": ",".join(str(rank) for rank, t in sorted(run["times"].items())
                                           if t == run["cct"]),
                "ring_cct_ns": max(run["times"][rank] for rank in range(4)),
                "tree_cct_ns": max(run["times"][rank] for rank in range(4, 8)),
                "alltoall_cct_ns": max(run["times"][rank] for rank in range(8, 12)),
                "baseline_target_duration_ns": base["slack"][fid]["flow_duration_ns"] if fid is not None else "",
                "target_duration_ns": run["slack"][fid]["flow_duration_ns"] if fid is not None else "",
            })
        lines = ["# Experiment 3.a: single-flow routing comparison", "",
                 "CCT is the maximum GOAL rank graph finish time. ΔCCT = case − E3-0; "
                 "positive reduction means a shorter CCT. Flow IDs are workload metadata IDs.", "",
                 "## Verified baseline identities", "",
                 "| Flow | Algorithm | GOAL ranks | Baseline slack (µs) | Ready / issue (ns) | Role |",
                 "| ---: | --- | --- | ---: | --- | --- |"]
        for fid, algorithm, role in ((46, "Tree", "Critical"), (48, "All-to-All (direct)", "Medium-slack"),
                                     (0, "Ring", "Higher-slack")):
            row = base["slack"][fid]
            lines.append(f"| {fid} | {algorithm} | {row['src']} → {row['dst']} | "
                         f"{int(row['oracle_slack_ns']) / 1000:.3f} | {row['ready_ns']} / {row['issue_ns']} | {role} |")
        lines += ["", "## CCT results", "",
                  "| Case | Detour flow | CCT (µs) | ΔCCT (µs) | CCT reduction | Critical ranks |",
                  "| --- | --- | ---: | ---: | ---: | --- |"]
        for row in results:
            lines.append(f"| {row['case']} | {row['detour_flow_id'] if row['detour_flow_id'] != '' else 'None'} | "
                         f"{row['cct_ns'] / 1000:.3f} | {row['delta_cct_ns'] / 1000:+.3f} | "
                         f"{row['cct_reduction_pct']:.2f}% | {row['critical_ranks']} |")
        lines += ["", "## Per-collective graph completion", "",
                  "| Case | Ring (µs) | Tree (µs) | All-to-All (µs) |",
                  "| --- | ---: | ---: | ---: |"]
        for row in results:
            lines.append(f"| {row['case']} | {row['ring_cct_ns'] / 1000:.3f} | "
                         f"{row['tree_cct_ns'] / 1000:.3f} | {row['alltoall_cct_ns'] / 1000:.3f} |")
        lines += ["", "## Selected flow duration", "",
                  "Receiver finish minus send issue, from integer-ns GOAL lifecycle metrics.", "",
                  "| Case | Flow | Baseline (µs) | Detour (µs) | Δduration (µs) |",
                  "| --- | ---: | ---: | ---: | ---: |"]
        for row in results[1:]:
            before, after = int(row["baseline_target_duration_ns"]), int(row["target_duration_ns"])
            lines.append(f"| {row['case']} | {row['detour_flow_id']} | {before / 1000:.3f} | "
                         f"{after / 1000:.3f} | {(after - before) / 1000:+.3f} |")
        lines += ["", "## Validation and interpretation", "",
                  "- PASS: all runs succeeded; 60 unique flow records and 200 lifecycle nodes per run; "
                  "all 12 ranks completed; logged CCT equals the lifecycle maximum.",
                  "- PASS: workload, topology, placement, messagelet config, simulator settings, "
                  "and routing identities match the baseline snapshots.",
                  "- PASS: E3-0 has 60 minimum routes; each other case has exactly its selected "
                  "flow marked valiant through group 2, with the other 59 routes minimum.", "",
                  "These are individual deterministic simulations (the simulator defaults to seed 13). "
                  "Baseline slack is an observed-execution property. It does not predict the sign of a "
                  "routing intervention: changed routes also change contention and subsequent timing. "
                  "The single-flow cases compare three concrete flows; their differences cannot be "
                  "attributed to slack alone. Longer routes can still complete faster under contention.", "",
                  "## Artifact provenance", ""]
        for row in results:
            lines.append(f"- {row['case']}: `{row['artifact']}`")
        args.output_dir.mkdir(parents=True, exist_ok=True)
        csv_path = args.output_dir / "routing_comparison.csv"
        with csv_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(results[0]))
            writer.writeheader()
            writer.writerows(results)
        report = args.output_dir / "routing_comparison.md"
        report.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"Validated E3-0..E3-3; wrote {report} and {csv_path}")
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()

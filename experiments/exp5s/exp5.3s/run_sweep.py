#!/usr/bin/env python3
"""Run a controlled source-UGAL-L sweep from packet to flow granularity."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]


def now():
    return datetime.now(ZoneInfo("America/Vancouver"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def read_env(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            result[key] = value.strip().strip('"')
    return result


def run(config_path, jobs_override=None):
    settings = json.loads(config_path.read_text())
    if settings["schema_version"] not in (1, 2):
        raise ValueError("Unsupported sweep schema")
    counts = settings["messagelet_packets"]
    if not counts or any(type(count) is not int or count <= 0 for count in counts) or len(set(counts)) != len(counts):
        raise ValueError("messagelet_packets must contain unique positive integers")
    if not 0 < settings["header_size_bytes"] < settings["packet_size_bytes"]:
        raise ValueError("Invalid packet/header sizes")
    seeds = settings.get("seeds", [{"simulation": 13, "routing": 33}])
    if not seeds or any(set(seed) != {"simulation", "routing"} or
                        any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in seed.values())
                        for seed in seeds):
        raise ValueError("seeds must contain unsigned 32-bit simulation/routing seed pairs")
    if len({seed["simulation"] for seed in seeds}) != len(seeds):
        raise ValueError("Simulation seed identifiers must be unique")
    jobs = jobs_override if jobs_override is not None else settings.get("parallel_jobs", 32)
    if type(jobs) is not int or jobs <= 0:
        raise ValueError("parallel_jobs must be a positive integer")
    reference = REPO / settings["reference_experiment"]
    reference_env = read_env(reference / "config.env")
    network = settings["network"]
    for key, reference_key in (("sender_cc_algo", "SENDER_CC_ALGO"), ("load_balancing_algo", "LOAD_BALANCING_ALGO"),
                               ("paths", "PATH_ENTROPY_SIZE"), ("queue_packets", "QUEUE_PACKETS"), ("cwnd_bytes", "CWND_BYTES")):
        if str(network[key]) != reference_env[reference_key]:
            raise ValueError(f"Setting differs from exp5.1: {key}")
    if reference_env.get("SIM_EXTRA_ARGS") != "()":
        raise ValueError("Reference has extra simulator arguments; review sweep controls")
    workload = REPO / settings["workload"]
    reference_workload = REPO / reference_env["WORKLOAD"]
    sources = {"workload.bin": workload, "workload.goal": workload.with_suffix(".goal"),
               "workload.meta.json": workload.with_suffix(".meta.json"),
               "rank_placement.json": ROOT / "rank_placement.json",
               "topology/dragonfly.topo": ROOT / "topology/p6a1h2/dragonfly.topo"}
    reference_sources = {"workload.bin": reference_workload, "workload.goal": reference_workload.with_suffix(".goal"),
                         "workload.meta.json": reference_workload.with_suffix(".meta.json"),
                         "rank_placement.json": reference / "rank_placement.json",
                         "topology/dragonfly.topo": reference / "topology/p6a1h2/dragonfly.topo"}
    hashes = {name: digest(source) for name, source in sources.items()}
    if any(hashes[name] != digest(source) for name, source in reference_sources.items()):
        raise ValueError("Workload, topology or placement differs from exp5.1")

    subprocess.run(["cmake", "-S", str(REPO / "htsim/sim"), "-B", str(REPO / "htsim/sim/build")], check=True)
    subprocess.run(["cmake", "--build", str(REPO / "htsim/sim/build"), "--target", "htsim_uec_df",
                    "--parallel", str(settings["build_jobs"])], check=True)
    binary = (REPO / settings["binary"]).resolve(strict=True)
    binary_hash = digest(binary)
    batch = ROOT / "artifacts" / now().strftime("%Y%m%d-%H%M%S")
    index = 1
    while batch.exists():
        batch = batch.with_name(now().strftime("%Y%m%d-%H%M%S") + f"-{index:02d}")
        index += 1
    batch.mkdir(parents=True)
    shared = batch / "config_snapshot"
    shared.mkdir()
    for source in (Path(__file__), ROOT / "compare.py", ROOT / "run_all.sh", config_path):
        shutil.copy2(source, shared / ("sweep_config.json" if source == config_path else source.name))
    for name, source in sources.items():
        target = shared / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for name in ("summarize.py", "analyze_slack.py"):
        shutil.copy2(REPO / "experiments/common" / name, shared / name)
    (batch / "git-revision.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True))
    (batch / "git-status.txt").write_text(subprocess.check_output(["git", "status", "--short"], cwd=REPO, text=True))
    (batch / "simulator.patch").write_bytes(subprocess.check_output(["git", "diff", "--binary", "HEAD", "--", "htsim"], cwd=REPO))
    for name in subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "--", "htsim"], cwd=REPO, text=True).splitlines():
        target = shared / "added_sources" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / name, target)
    network_args = ["-routing", "SOURCE", "-sender_cc_algo", network["sender_cc_algo"],
                    "-load_balancing_algo", network["load_balancing_algo"], "-paths", str(network["paths"]),
                    "-q", str(network["queue_packets"]), "-cwnd", str(network["cwnd_bytes"])]
    cases = [{"name": "packet", "granularity": "packet", "packets_per_messagelet": 1, "size_bytes": 0}]
    cases += [{"name": f"messagelet_{count:02d}pkt", "granularity": "messagelet",
               "packets_per_messagelet": count, "size_bytes": count * settings["packet_size_bytes"]} for count in sorted(counts)]
    cases += [{"name": "flow", "granularity": "flow", "packets_per_messagelet": None, "size_bytes": 0}]
    jobs = min(jobs, len(cases) * len(seeds))
    manifest = {"schema_version": 2, "started_at": now().isoformat(), "reference_experiment": str(reference),
                "settings": settings, "binary": str(binary), "binary_sha256": binary_hash, "input_sha256": hashes,
                "network_args": network_args, "cases": cases, "seeds": seeds, "parallel_jobs": jobs}
    save_json(batch / "manifest.json", manifest)
    env = {**os.environ, "TZ": "America/Vancouver", "HTSIM_TRACE_FLOW_COMPLETIONS": "1", "HTSIM_TRACE_PACKET_HOPS": "0"}
    def run_one(case, seed):
        case_dir = batch / "seeds" / f"seed_{seed['simulation']}" / "cases" / case["name"]
        snapshot = case_dir / "config_snapshot"
        snapshot.mkdir(parents=True)
        for name in sources:
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(shared / name, target)
        for name in ("summarize.py", "analyze_slack.py"):
            shutil.copy2(shared / name, snapshot / name)
        save_json(snapshot / "case.json", case)
        save_json(snapshot / "seed.json", seed)
        save_json(snapshot / "messagelet_config.json", {
            "schema_version": 1, "runtime": {"mode": "label", "granularity": case["granularity"]},
            "messagelet": {"size_bytes": case["size_bytes"]},
            "routing": {"default": "minimum", "network_decision": "ugal_l"}, "logging": {"trace_decisions": True}})
        env_values = {"EXPERIMENT_NAME": f"exp5.3s/seed_{seed['simulation']}/{case['name']}", "EXPERIMENT_DESCRIPTION": "Source UGAL-L granularity sweep",
                      "FLOW_PATTERN": reference_env["FLOW_PATTERN"], "ROUTING": "SOURCE", "SENDER_CC_ALGO": network["sender_cc_algo"],
                      "LOAD_BALANCING_ALGO": network["load_balancing_algo"], "PATH_ENTROPY_SIZE": network["paths"],
                      "QUEUE_PACKETS": network["queue_packets"], "CWND_BYTES": network["cwnd_bytes"],
                      "WORKLOAD": settings["workload"], "TOPOLOGY": str(ROOT / "topology/p6a1h2"), "BINARY": str(binary),
                      "RANK_PLACEMENT_CONFIG": str(ROOT / "rank_placement.json"), "MESSAGELET_CONFIG": "config_snapshot/messagelet_config.json",
                      "ROUTING_CONFIG": "", "TRACE_FLOW_COMPLETIONS": 1, "TRACE_PACKET_HOPS": 0,
                      "SLACK_READY_WINDOW_NS": settings["slack_ready_window_ns"],
                      "SIMULATION_SEED": seed["simulation"], "ROUTING_SEED": seed["routing"]}
        (snapshot / "config.env").write_text("".join(f'{key}="{value}"\n' for key, value in env_values.items()))
        for name in ("git-revision.txt", "git-status.txt"):
            shutil.copy2(batch / name, case_dir / name)
        (case_dir / "binary-sha256.txt").write_text(f"{binary_hash}  {binary}\n")
        command = [str(binary), "-basepath", str(snapshot / "topology"), "-goal", str(snapshot / "workload.bin"),
                   *network_args, "-seed", str(seed["simulation"]), "-routing_seed", str(seed["routing"]),
                   "-rank_placement", str(snapshot / "rank_placement.json"),
                   "-messagelet_config", str(snapshot / "messagelet_config.json")]
        (case_dir / "command.txt").write_text(shlex.join(command) + "\n")
        (case_dir / "started_at.txt").write_text(now().isoformat() + "\n")
        try:
            if digest(binary) != binary_hash:
                raise RuntimeError("Simulator binary changed during sweep")
            with (case_dir / "simulator.log").open("w") as log:
                result = subprocess.run(command, cwd=case_dir, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=settings["simulation_timeout_seconds"])
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = 124
        (case_dir / "finished_at.txt").write_text(now().isoformat() + "\n")
        (case_dir / "exit_code.txt").write_text(str(status) + "\n")
        subprocess.run([sys.executable, "-B", str(snapshot / "summarize.py"), str(case_dir)], check=True,
                       stdout=subprocess.DEVNULL)
        if status != 0:
            raise RuntimeError(f"{case['name']} failed with exit code {status}; see {case_dir / 'simulator.log'}")
        if digest(binary) != binary_hash:
            raise RuntimeError("Simulator binary changed during simulation")
        with (case_dir / "analysis.log").open("w") as log:
            subprocess.run([sys.executable, "-B", str(snapshot / "analyze_slack.py"), str(case_dir),
                            "--ready-window-ns", str(settings["slack_ready_window_ns"])], stdout=log, stderr=subprocess.STDOUT, check=True)
        return f"seed {seed['simulation']}/{seed['routing']} — {case['name']}"

    tasks = [(case, seed) for seed in seeds for case in cases]
    print(f"Running {len(tasks)} simulations with {jobs} parallel workers ...", flush=True)
    failures = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(run_one, case, seed): (case, seed) for case, seed in tasks}
        for position, future in enumerate(as_completed(futures), 1):
            case, seed = futures[future]
            try:
                print(f"[{position}/{len(tasks)}] completed {future.result()}", flush=True)
            except Exception as error:
                failures.append(f"seed {seed['simulation']}/{seed['routing']}, {case['name']}: {error}")
                print(f"[{position}/{len(tasks)}] FAILED {failures[-1]}", flush=True)
    if failures:
        save_json(batch / "failures.json", failures)
        raise RuntimeError(f"{len(failures)} sweep jobs failed; see {batch / 'failures.json'}")
    manifest["finished_at"] = now().isoformat()
    save_json(batch / "manifest.json", manifest)
    subprocess.run([sys.executable, "-B", str(shared / "compare.py"), "--batch", str(batch), "--output-dir", str(ROOT)], check=True)
    latest = ROOT / "artifacts/latest"
    latest.unlink(missing_ok=True)
    latest.symlink_to(batch.name, target_is_directory=True)
    (batch / "exit_code.txt").write_text("0\n")
    print(f"Sweep completed: {batch}\nComparison: {ROOT / 'comparison.md'}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "sweep_config.json")
    parser.add_argument("--jobs", type=int, help="parallel simulation/analysis workers (default: config parallel_jobs)")
    args = parser.parse_args()
    run(args.config.resolve(), args.jobs)

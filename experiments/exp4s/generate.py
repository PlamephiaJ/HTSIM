#!/usr/bin/env python3
"""Generate the two flow DAGs and the four interventions plus two references."""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / "experiments/common"))
from generate_routing_config import build_config

CASES = (("exp4.ref1", 1, None), ("exp4.ref2", 2, None),
         ("exp4.1", 1, 0), ("exp4.2", 1, 1),
         ("exp4.3", 2, 0), ("exp4.4", 2, 1))


def main():
    for dag in (1, 2):
        subprocess.run([sys.executable, str(REPO / "workload_gen/generate.py"),
                        str(ROOT / f"templates/exp4.dag{dag}.flow.json"),
                        "--output-root", str(REPO / "workload/exp4s")], check=True)
    for case, dag, target in CASES:
        meta = json.loads((REPO / f"workload/exp4s/exp4.dag{dag}/workload.meta.json").read_text())
        config = build_config(meta, "minimum", {} if target is None else {target: "valiant"})
        path = ROOT / case / "routing_configs/default.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(config, indent=2) + "\n")
        print(f"Generated {path.relative_to(REPO)}")


if __name__ == "__main__":
    main()

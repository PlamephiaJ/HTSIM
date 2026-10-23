"""Entry switch routing integration tests; build htsim_uec_df first."""

import csv
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

BINARY = REPO_ROOT / "htsim/sim/build/datacenter/htsim_uec_df"


@unittest.skipUnless(BINARY.is_file(), "build htsim_uec_df first")
class EntrySwitchRoutingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="htsim-network-decision-")
        self.root = Path(self.temp.name)
        self.write_workload(65536)
        self.topology = self.root / "topology"
        self.topology.mkdir()
        (self.topology / "dragonfly.topo").write_text("p 2\na 4\nh 2\n")

    def write_workload(self, size):
        spec = {"schema_version": 1, "kind": "flow", "name": "decision", "world_size": 2,
                "nodes": [{"id": "send", "op": "flow", "flow_id": 1, "src": 0,
                           "dst": 1, "bytes": size, "tag": 7}]}
        spec_path = self.root / "spec.json"
        spec_path.write_text(json.dumps(spec))
        subprocess.run(["python3", str(REPO_ROOT / "workload_gen/generate.py"),
                        str(spec_path), "--output-root", str(self.root)],
                       check=True, capture_output=True, text=True)
        self.workload = self.root / "decision/workload.bin"

    def tearDown(self):
        self.temp.cleanup()

    def run_flow(self, dst_group=3, dst_switch=0, strategy="ugal_l", size=8192,
                 routing="SOURCE", extra=(), granularity=None, mode=None, alpha=None,
                 routing_mode=None, dag_id=0):
        placement = self.root / "placement.json"
        placement.write_text(json.dumps({"schema_version": 1, "placements": [
            {"rank": 0, "group": 0, "switch": 0, "host": 0},
            {"rank": 1, "group": dst_group, "switch": dst_switch, "host": 1}]}))
        config = self.root / "messagelet.json"
        settings = {"routing": {"network_decision": strategy},
                    "logging": {"trace_decisions": True}}
        if alpha is not None:
            settings["routing"]["commitment_alpha"] = alpha
        if size is not None:
            settings["messagelet"] = {"size_bytes": size}
        if granularity is not None:
            settings["runtime"] = {"granularity": granularity}
        if mode is not None:
            settings.setdefault("runtime", {})["mode"] = mode
        if routing_mode is not None:
            settings.setdefault("runtime", {})["routing_mode"] = routing_mode
        settings.setdefault("runtime", {})["dag_id"] = dag_id
        config.write_text(json.dumps(settings))
        run = self.root / f"run-{len(list(self.root.iterdir()))}"
        run.mkdir()
        self.last_run = run
        return subprocess.run([str(BINARY), "-basepath", str(self.topology),
                               "-goal", str(self.workload), "-routing", routing,
                               "-rank_placement", str(placement),
                               "-messagelet_config", str(config), *extra],
                              cwd=run, capture_output=True, text=True, timeout=30,
                              env={**os.environ, "HTSIM_TRACE_PACKET_HOPS": "0",
                                   "HTSIM_TRACE_GOAL_SEND_GATE": "0"})

    def test_entry_switch_metadata_commitment_and_tail(self):
        self.write_workload(65537)
        for group, switch in ((3, 0), (0, 2), (0, 0)):
            for granularity, size, expected in (
                    ("messagelet", 16640, [16384] * 4 + [1]),
                    ("messagelet", 8321, [12288] * 5 + [4097]),
                    ("flow", 0, [65537]),
                    ("packet", 0, [4096] * 16 + [1])):
                with self.subTest(group=group, switch=switch, granularity=granularity, size=size):
                    result = self.run_flow(dst_group=group, dst_switch=switch,
                                           strategy="commitment_aware_ugal", size=size, alpha=1,
                                           granularity=granularity, routing="ENTRY_SWITCH",
                                           routing_mode="ENTRY_SWITCH", dag_id=7)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    metrics = self.last_run / "output_metrics"
                    self.assertFalse((metrics / "runtime_decisions.csv").exists())
                    with (metrics / "entry_decisions.csv").open() as stream:
                        decisions = list(csv.DictReader(stream))
                    self.assertEqual([int(r["mlet_size"]) for r in decisions], expected)
                    self.assertEqual([int(r["messagelet_index"]) for r in decisions], list(range(len(expected))))
                    self.assertEqual({r["total_mlet"] for r in decisions}, {str(len(expected))})
                    self.assertEqual({r["flow_id"] for r in decisions}, {"1"})
                    self.assertEqual({r["dag_id"] for r in decisions}, {"7"})
                    for decision in decisions:
                        state = dict(item.split("=", 1) for item in decision["policy_state"].split(";") if item)
                        if state.get("compared") == "true":
                            self.assertEqual(float(state["pressure_min"]),
                                             int(state["q_min_bytes"]) + int(state["r_min_bytes"]))
                            self.assertEqual(float(state["pressure_val"]),
                                             int(state["q_val_bytes"]) + int(state["r_val_bytes"]))
                            valiant = (float(state["pressure_min"]) * int(state["h_min"]) >
                                       float(state["pressure_val"]) * int(state["h_val"]))
                            self.assertEqual(decision["route"], "valiant" if valiant else "minimum")
                    if group != 0:
                        with (metrics / "gra_events.csv").open() as stream:
                            events = list(csv.DictReader(stream))
                        self.assertEqual(sum(r["action"] == "commit" for r in events), len(expected))
                        admissions = [r for r in events if r["action"] == "first_admission"]
                        self.assertEqual(len(admissions), 17)
                        self.assertEqual(sum(int(r["payload_bytes"]) for r in admissions), 65537)
                        self.assertEqual(sum(int(r["remaining_messagelet_bytes"])
                                             for r in events if r["action"] == "commit"), 65537)
                        for decision in decisions:
                            related = [r for r in events if r["messagelet_index"] == decision["messagelet_index"]]
                            self.assertEqual(related[0]["action"], "commit")
                            self.assertEqual(int(related[-1]["remaining_messagelet_bytes"]), 0)
                            self.assertEqual(related[0]["time_ps"], decision["time_ps"])
                    else:
                        self.assertFalse((metrics / "gra_events.csv").exists())

    def test_entry_switch_control_and_configuration(self):
        for strategy in ("none", "ugal_l", "commitment_aware_ugal"):
            result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH",
                                   strategy=strategy, extra=("-receiver_cc_only",))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for routing, routing_mode in (("SOURCE", "ENTRY_SWITCH"), ("ENTRY_SWITCH", "SOURCE")):
            result = self.run_flow(routing=routing, routing_mode=routing_mode)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must agree on ENTRY_SWITCH", result.stderr)
        result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH", mode="transparent")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires runtime.mode 'label'", result.stderr)
        self.workload.with_suffix(".meta.json").unlink()
        result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires workload flow metadata", result.stderr)


if __name__ == "__main__":
    unittest.main()

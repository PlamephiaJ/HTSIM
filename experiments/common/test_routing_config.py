"""Manual routing tests. Build htsim_uec_df before running this suite."""

import csv
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from generate_routing_config import REPO_ROOT, build_config

BINARY = REPO_ROOT / "htsim/sim/build/datacenter/htsim_uec_df"
TOPOLOGY = REPO_ROOT / "experiments/exp3s/exp3.a/topology/p6a1h2"


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.meta = {"workload": {"name": "sparse", "p2p_flows": 1}, "flows": {
            "41": {"flow_id": 41, "src": 0, "dst": 1, "tag": 123,
                   "bytes": 4096, "send_task": {"rank": 0, "task_id": 7}}}}

    def test_sparse_id_does_not_use_tag_or_launch_order(self):
        config = build_config(self.meta, "minimum", {41: "valiant"})
        self.assertEqual(config["flow_count"], 1)
        self.assertEqual(config["flows"]["41"]["tag"], 123)
        self.assertEqual(config["flows"]["41"]["send_task_id"], 7)
        self.assertEqual(config["flows"]["41"]["label"], "valiant")

    def test_reject_unknown_flow_and_label_and_count(self):
        for assignments in ({42: "minimum"}, {41: "ugal"}):
            with self.assertRaises(ValueError):
                build_config(self.meta, "minimum", assignments)
        self.meta["workload"]["p2p_flows"] = 2
        with self.assertRaises(ValueError):
            build_config(self.meta, "minimum", {})


@unittest.skipUnless(BINARY.is_file(), "build htsim_uec_df first")
class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="htsim-manual-routing-")
        self.root = Path(self.temp.name)
        spec = {"schema_version": 1, "kind": "flow", "name": "test", "world_size": 2,
                "nodes": [{"id": "start", "op": "compute", "ranks": "all", "duration_ns": 0},
                          {"id": "send", "op": "flow", "flow_id": 41, "src": 0,
                           "dst": 1, "bytes": 4096, "tag": 123, "depends_on": ["start"]}]}
        spec_path = self.root / "spec.json"
        spec_path.write_text(json.dumps(spec))
        subprocess.run(["python3", str(REPO_ROOT / "workload_gen/generate.py"),
                        str(spec_path), "--output-root", str(self.root)], check=True,
                       capture_output=True, text=True)
        self.workload = self.root / "test/workload.bin"
        self.meta = json.loads(self.workload.with_suffix(".meta.json").read_text())
        self.placement = self.root / "placement.json"
        self.routing = self.root / "routing.json"
        self.messagelet = self.root / "messagelet.json"
        self.messagelet.write_text(json.dumps({"messagelet": {"size_bytes": 0},
                                              "logging": {"trace_decisions": True}}))

    def tearDown(self):
        self.temp.cleanup()

    def run_flow(self, label, dst_group, mutate=None, topology=TOPOLOGY, extra=(),
                 src_group=0, dst_switch=0):
        self.placement.write_text(json.dumps({"schema_version": 1, "placements": [
            {"rank": 0, "group": src_group, "switch": 0, "host": 0},
            {"rank": 1, "group": dst_group, "switch": dst_switch, "host": 1}]}))
        config = build_config(self.meta, label, {})
        if mutate:
            mutate(config)
        self.routing.write_text(json.dumps(config))
        run = self.root / f"{label}-{dst_group}-{len(list(self.root.iterdir()))}"
        run.mkdir()
        result = subprocess.run([str(BINARY), "-basepath", str(topology), "-goal", str(self.workload),
                                 "-routing", "SOURCE", "-rank_placement", str(self.placement),
                                 "-routing_config", str(self.routing), "-messagelet_config",
                                 str(self.messagelet), *extra], cwd=run,
                                capture_output=True, text=True, timeout=30)
        return result, run

    def duration(self, run):
        with (run / "output_metrics/flow_dag_info.csv").open() as stream:
            rows = list(csv.DictReader(stream))
        return max(int(row["finishTimeNs"]) for row in rows)

    def test_valiant_really_detours_cross_group_and_same_switch(self):
        for dst_group in (0, 1):
            minimum, min_run = self.run_flow("minimum", dst_group)
            valiant, val_run = self.run_flow("valiant", dst_group)
            self.assertEqual(minimum.returncode, 0, minimum.stderr + minimum.stdout)
            self.assertEqual(valiant.returncode, 0, valiant.stderr + valiant.stdout)
            self.assertGreater(self.duration(val_run), self.duration(min_run))
            self.assertIn("FlowId 41", valiant.stdout)
            with (val_run / "output_metrics/flowRouting.csv").open() as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["flow_id"], "41")
            self.assertEqual(row["label"], "valiant")
            self.assertNotIn(int(row["intermediate_group"]), (0, dst_group))

    def test_unknown_label_missing_flow_stale_task_and_count_fail(self):
        mutations = [lambda c: c["flows"]["41"].update(label="ugal"),
                     lambda c: c.update(flows={}, flow_count=0),
                     lambda c: c["flows"]["41"].update(send_task_id=999),
                     lambda c: c["flows"]["41"].update(tag=999),
                     lambda c: c.update(flow_count=2)]
        for mutate in mutations:
            result, _ = self.run_flow("minimum", 1, mutate)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("routing", result.stderr.lower())

    def test_requires_source(self):
        result, _ = self.run_flow("minimum", 1, extra=("-routing", "MINIMAL"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires -routing SOURCE", result.stderr)

    def test_receiver_control_packets_need_no_host_tables(self):
        result, _ = self.run_flow("valiant", 1, extra=("-receiver_cc_only",))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_multi_switch_topology_both_directions_and_same_group(self):
        topology = REPO_ROOT / "htsim/sim/datacenter/topologies/dragonfly/p3a6h3"
        for src_group, dst_group, dst_switch in ((0, 1, 0), (1, 0, 0), (1, 1, 0), (1, 1, 2)):
            options = dict(topology=topology, src_group=src_group, dst_switch=dst_switch)
            minimum, min_run = self.run_flow("minimum", dst_group, **options)
            valiant, val_run = self.run_flow("valiant", dst_group, **options)
            self.assertEqual(minimum.returncode, 0, minimum.stdout + minimum.stderr)
            self.assertEqual(valiant.returncode, 0, valiant.stdout + valiant.stderr)
            self.assertGreater(self.duration(val_run), self.duration(min_run))

    def test_valiant_without_intermediate_group_fails(self):
        topology = self.root / "two-groups"
        topology.mkdir()
        (topology / "dragonfly.topo").write_text("p 2\na 1\nh 1\n")
        result, _ = self.run_flow("valiant", 1, topology=topology)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No intermediate group", result.stderr)

    def test_full_collective_graph_all_labels(self):
        workload = REPO_ROOT / "workload/exp3s/exp3.a/workload.bin"
        if not workload.exists():
            self.skipTest("exp3.a workload is unavailable")
        metadata = json.loads(workload.with_suffix(".meta.json").read_text())
        placement = REPO_ROOT / "experiments/exp3s/exp3.a/rank_placement.json"
        for label in ("minimum", "valiant"):
            config = build_config(metadata, label, {})
            routing = self.root / "collective-routing.json"
            routing.write_text(json.dumps(config))
            run = self.root / f"collectives-{label}"
            run.mkdir()
            result = subprocess.run([str(BINARY), "-basepath", str(TOPOLOGY), "-goal", str(workload),
                                     "-routing", "SOURCE", "-rank_placement", str(placement),
                                     "-routing_config", str(routing)], cwd=run,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with (run / "output_metrics/flowRouting.csv").open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), config["flow_count"])
            self.assertEqual({row["flow_id"] for row in rows}, set(config["flows"]))
            self.assertEqual({row["label"] for row in rows}, {label})


if __name__ == "__main__":
    unittest.main()

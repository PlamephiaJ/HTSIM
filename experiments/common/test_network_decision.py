"""GOAL integration tests for messagelet network decisions; build htsim_uec_df first."""

import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from generate_routing_config import REPO_ROOT, build_config

BINARY = REPO_ROOT / "htsim/sim/build/datacenter/htsim_uec_df"


@unittest.skipUnless(BINARY.is_file(), "build htsim_uec_df first")
class NetworkDecisionTests(unittest.TestCase):
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
                 routing="SOURCE", extra=(), granularity=None, mode=None, trace_hops=False):
        placement = self.root / "placement.json"
        placement.write_text(json.dumps({"schema_version": 1, "placements": [
            {"rank": 0, "group": 0, "switch": 0, "host": 0},
            {"rank": 1, "group": dst_group, "switch": dst_switch, "host": 1}]}))
        config = self.root / "messagelet.json"
        settings = {"routing": {"network_decision": strategy},
                    "logging": {"trace_decisions": True}}
        if size is not None:
            settings["messagelet"] = {"size_bytes": size}
        if granularity is not None:
            settings["runtime"] = {"granularity": granularity}
        if mode is not None:
            settings.setdefault("runtime", {})["mode"] = mode
        config.write_text(json.dumps(settings))
        run = self.root / f"run-{len(list(self.root.iterdir()))}"
        run.mkdir()
        self.last_run = run
        return subprocess.run([str(BINARY), "-basepath", str(self.topology),
                               "-goal", str(self.workload), "-routing", routing,
                               "-rank_placement", str(placement),
                               "-messagelet_config", str(config), *extra],
                              cwd=run, capture_output=True, text=True, timeout=30,
                              env={**os.environ, "HTSIM_TRACE_PACKET_HOPS": "1" if trace_hops else "0"})

    def test_ugal_at_each_boundary_cross_group_same_group_and_local(self):
        for group, switch in ((3, 0), (0, 2), (0, 0)):
            with self.subTest(group=group, switch=switch):
                result = self.run_flow(dst_group=group, dst_switch=switch)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                traces = [line for line in result.stdout.splitlines()
                          if line.startswith("Messagelet route -")]
                self.assertGreater(len(traces), 1)
                indices = [int(re.search(r" - Index (\d+)", line).group(1)) for line in traces]
                self.assertEqual(indices, list(range(len(traces))))
                self.assertIn(" - Route minimum", traces[0])
                for trace in traces:
                    self.assertIn(" - policy ugal_l", trace)
                    if " - compared true" in trace:
                        values = dict(re.findall(r" - (q_min_bytes|q_val_bytes|h_min|h_val) (\d+)",
                                                 trace))
                        self.assertEqual(set(values), {"q_min_bytes", "q_val_bytes", "h_min", "h_val"})
                        use_valiant = (int(values["q_min_bytes"]) * int(values["h_min"]) >
                                       int(values["q_val_bytes"]) * int(values["h_val"]))
                        self.assertIn(" - Route " + ("valiant" if use_valiant else "minimum"), trace)
                    else:
                        self.assertIn(" - Route minimum", trace)
                if group != 0 or switch != 0:
                    self.assertTrue(any(" - Route valiant" in trace for trace in traces))

    def test_whole_flow_and_receiver_control_packets(self):
        for extra in ((), ("-receiver_cc_only",)):
            result = self.run_flow(size=0, extra=extra)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("Messagelet route -"), 1)

    def test_explicit_granularities_match_legacy_configs(self):
        for size, granularity in ((0, "flow"), (8192, "messagelet")):
            legacy = self.run_flow(size=size)
            explicit = self.run_flow(size=size, granularity=granularity)
            for result in (legacy, explicit):
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            traces = lambda result: [line for line in result.stdout.splitlines()
                                     if line.startswith("Messagelet route -")]
            self.assertEqual(traces(legacy), traces(explicit))
            self.assertTrue(all(" - Granularity " + granularity in line for line in traces(explicit)))

    def test_default_flow_does_not_split_large_payload_due_to_headers(self):
        self.write_workload(1048576)
        for size, granularity in ((None, None), (0, None), (None, "flow")):
            result = self.run_flow(size=size, granularity=granularity)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("Messagelet route -"), 1)
            self.assertIn(" - Granularity flow", result.stdout)

    def test_packet_granularity_decides_once_per_new_data_packet(self):
        self.write_workload(65537)  # Include a partial final data packet.
        for extra in ((), ("-receiver_cc_only",)):
            result = self.run_flow(size=None, granularity="packet", extra=extra)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            traces = [line for line in result.stdout.splitlines() if line.startswith("Messagelet route -")]
            self.assertEqual(len(traces), 17)
            self.assertEqual([int(re.search(r" - Index (\d+)", line).group(1)) for line in traces],
                             list(range(17)))
            self.assertTrue(all(" - Granularity packet" in line for line in traces))

    def test_conflicting_granularity_sizes_and_unknown_mode_are_rejected(self):
        for granularity, size in (("unknown", 0), ("flow", 8192),
                                  ("messagelet", 0), ("packet", 8192)):
            with self.subTest(granularity=granularity, size=size):
                result = self.run_flow(size=size, granularity=granularity)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("runtime granularity" if granularity == "unknown" else "runtime.granularity",
                              result.stderr)

    def test_existing_packet_ugal_mode_still_delivers(self):
        for group, switch in ((3, 0), (0, 2), (0, 0)):
            result = self.run_flow(dst_group=group, dst_switch=switch,
                                   strategy="none", routing="UGAL_L")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn(" - policy ugal_l", result.stdout)

    def test_transparent_matches_native_switch_hops_and_completion(self):
        for routing in ("MINIMAL", "UGAL_L", "VALIANT"):
            for extra in ((), ("-receiver_cc_only",)):
                with self.subTest(routing=routing, extra=extra):
                    native = self.run_flow(size=0, strategy="none", routing=routing,
                                           extra=extra, trace_hops=True)
                    native_metrics = {path.name: path.read_bytes() for path in
                                      (self.last_run / "output_metrics").glob("*.csv")}
                    transparent = self.run_flow(size=0, strategy="none", routing=routing,
                                                mode="transparent", extra=extra, trace_hops=True)
                    for result in (native, transparent):
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    hops = lambda result: [line for line in result.stdout.splitlines()
                                           if line.startswith("PacketHop ")]
                    self.assertGreater(len(hops(native)), 0)
                    self.assertEqual(hops(native), hops(transparent))
                    self.assertNotIn("Messagelet route -", transparent.stdout)
                    for name, content in native_metrics.items():
                        self.assertEqual(content, (self.last_run / "output_metrics" / name).read_bytes(), name)

    def test_transparent_rejects_source_runtime_policy_and_unknown_mode(self):
        result = self.run_flow(size=0, strategy="none", mode="transparent")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Transparent runtime requires", result.stderr)
        result = self.run_flow(size=0, mode="transparent", routing="UGAL_L")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires routing.network_decision = 'none'", result.stderr)
        result = self.run_flow(size=0, strategy="none", mode="unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported runtime mode", result.stderr)

    def test_invalid_strategy_and_non_source_are_rejected(self):
        result = self.run_flow(strategy="unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported network decision strategy", result.stderr)
        result = self.run_flow(routing="UGAL_L")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires -routing SOURCE and -goal", result.stderr)

    def test_seed_flags_preserve_defaults_repeat_and_change_routing(self):
        outputs = []
        for extra in ((), ("-seed", "13", "-routing_seed", "33"),
                      ("-seed", "13", "-routing_seed", "33"),
                      ("-seed", "14", "-routing_seed", "34")):
            result = self.run_flow(extra=extra)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            outputs.append(([line for line in result.stdout.splitlines() if line.startswith("Messagelet route -")],
                            {path.name: path.read_bytes() for path in (self.last_run / "output_metrics").glob("*.csv")}))
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[1], outputs[2])
        self.assertNotEqual(outputs[1][0], outputs[3][0])
        self.assertIn(b"14,34", outputs[3][1]["randomSeeds.csv"])

    def test_seed_flags_reject_invalid_values(self):
        for option, value in (("-seed", "-1"), ("-routing_seed", "4294967296"), ("-seed", "13x")):
            result = self.run_flow(extra=(option, value))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Seed must be an unsigned 32-bit integer", result.stderr)
        result = self.run_flow(extra=("-routing_seed",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing value for seed option", result.stderr)

    def test_manual_routing_conflict_is_explicit(self):
        metadata = json.loads(self.workload.with_suffix(".meta.json").read_text())
        routing = self.root / "routing.json"
        routing.write_text(json.dumps(build_config(metadata, "minimum", {})))
        result = self.run_flow(extra=("-routing_config", str(routing)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be combined", result.stderr)


if __name__ == "__main__":
    unittest.main()

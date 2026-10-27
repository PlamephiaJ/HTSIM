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
                 routing_mode=None, dag_id=0, soft_claim=None, beta=None, trace_decisions=True):
        placement = self.root / "placement.json"
        placement.write_text(json.dumps({"schema_version": 1, "placements": [
            {"rank": 0, "group": 0, "switch": 0, "host": 0},
            {"rank": 1, "group": dst_group, "switch": dst_switch, "host": 1}]}))
        config = self.root / "messagelet.json"
        settings = {"routing": {"network_decision": strategy},
                    "logging": {"trace_decisions": trace_decisions}}
        if alpha is not None:
            settings["routing"]["commitment_alpha"] = alpha
        if soft_claim is not None:
            settings["routing"]["soft_claim"] = {"enabled": soft_claim, "policy": "dag_priority"}
        if beta is not None:
            settings["routing"].setdefault("soft_claim", {})["beta"] = beta
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

    def test_soft_claim_runtime_ready_and_tail(self):
        self.write_workload(65537)
        for granularity, size, expected in (
                ("messagelet", 8321, [12288] * 5 + [4097]),
                ("flow", 0, [65537]),
                ("packet", 0, [4096] * 16 + [1])):
            with self.subTest(granularity=granularity):
                result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH",
                                       strategy="commitment_aware_ugal", soft_claim=True,
                                       granularity=granularity, size=size, dag_id=7)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                metrics = self.last_run / "output_metrics"
                with (metrics / "entry_decisions.csv").open() as stream:
                    decisions = list(csv.DictReader(stream))
                with (metrics / "gra_soft_claims.csv").open() as stream:
                    claims = list(csv.DictReader(stream))
                with (metrics / "gra_events.csv").open() as stream:
                    events = list(csv.DictReader(stream))
                ready = [r for r in claims if r["action"] == "ready"]
                self.assertEqual([int(r["ready_payload_bytes"]) for r in ready], expected)
                self.assertEqual([int(r["messagelet_index"]) for r in ready], list(range(len(expected))))
                self.assertEqual({r["claim_dag_id"] for r in ready}, {"7"})
                self.assertTrue(all(int(r["priority"]) > 0 for r in ready))
                for decision, report in zip(decisions, ready):
                    self.assertLess(int(report["time_ps"]), int(decision["time_ps"]))
                    state = dict(item.split("=", 1) for item in decision["policy_state"].split(";") if item)
                    if state.get("compared") == "true":
                        self.assertEqual(state["soft_claim_min_bytes"], "0")
                        self.assertEqual(state["soft_claim_val_bytes"], "0")
                self.assertEqual(claims[-1]["pending_count"], "0")
                self.assertEqual(claims[-1]["claim_flow_id"], "")
                commits = [r for r in events if r["action"] == "commit"]
                admissions = [r for r in events if r["action"] == "first_admission"]
                self.assertEqual([int(r["payload_bytes"]) for r in commits], expected)
                self.assertEqual(sum(int(r["payload_bytes"]) for r in admissions), 65537)
                if granularity != "flow":
                    with (metrics / "gra_pending_preferences.csv").open() as stream:
                        preferences = list(csv.DictReader(stream))
                    self.assertTrue(any(r["claim_status"] == "equal_priority_tiebreak" for r in preferences))

    def test_soft_claim_disabled_preserves_baseline(self):
        outputs = []
        for enabled in (None, False):
            result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH",
                                   strategy="commitment_aware_ugal", soft_claim=enabled)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            metrics = self.last_run / "output_metrics"
            self.assertFalse((metrics / "gra_soft_claims.csv").exists())
            outputs.append(tuple((metrics / name).read_bytes() for name in
                                 ("entry_decisions.csv", "gra_events.csv", "flow_dag_info.csv")))
        self.assertEqual(outputs[0], outputs[1])
        for routing, routing_mode, strategy in (("SOURCE", "SOURCE", "commitment_aware_ugal"),
                                               ("ENTRY_SWITCH", "ENTRY_SWITCH", "ugal_l")):
            result = self.run_flow(routing=routing, routing_mode=routing_mode,
                                   strategy=strategy, soft_claim=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Soft claim requires", result.stderr)

    def test_soft_claim_ready_competition_from_goal(self):
        spec = {"schema_version": 1, "kind": "flow", "name": "competition", "world_size": 2,
                "nodes": [
                    {"id": "start", "op": "compute", "ranks": "all", "duration_ns": 2000},
                    {"id": "low", "op": "flow", "flow_id": 20, "src": 0, "dst": 1,
                     "bytes": 8192, "tag": 20, "depends_on": ["start"]},
                    {"id": "high", "op": "flow", "flow_id": 10, "src": 0, "dst": 1,
                     "bytes": 8192, "tag": 10, "depends_on": ["start"]},
                    {"id": "tail", "op": "compute", "ranks": [1], "duration_ns": 100000,
                     "depends_on": ["high"]}]}
        path = self.root / "competition.json"
        path.write_text(json.dumps(spec))
        subprocess.run(["python3", str(REPO_ROOT / "workload_gen/generate.py"), str(path),
                        "--output-root", str(self.root)], check=True, capture_output=True, text=True)
        self.workload = self.root / "competition/workload.bin"
        for alpha, beta in ((1.0, None), (0.25, None), (0.25, 2.0), (0.25, 0.0), (0.0, None)):
            with self.subTest(alpha=alpha, beta=beta):
                effective_beta = alpha if beta is None else beta
                result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH",
                                       strategy="commitment_aware_ugal", soft_claim=True,
                                       granularity="flow", size=0, alpha=alpha, beta=beta)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                metrics = self.last_run / "output_metrics"
                with (metrics / "entry_decisions.csv").open() as stream:
                    decisions = list(csv.DictReader(stream))
                self.assertEqual([r["flow_id"] for r in decisions], ["20", "10"])
                low_state = dict(item.split("=", 1) for item in decisions[0]["policy_state"].split(";") if item)
                self.assertEqual(low_state["soft_claim_min_bytes"], "8192")
                self.assertEqual(low_state["r_min_bytes"], "0")
                self.assertEqual(float(low_state["beta"]), effective_beta)
                self.assertEqual(float(low_state["pressure_min"]), 8192 * effective_beta)
                self.assertEqual(low_state["soft_claim_min_reason"], "applied" if effective_beta > 0 else "beta_zero")
                self.assertEqual(low_state["soft_claim_min_owner_flow_id"], "10")
                self.assertEqual(low_state["soft_claim_val_reason"], "no_claim")
                self.assertEqual(float(low_state["soft_claim_min_pressure"]), 8192 * effective_beta)
                self.assertEqual(decisions[0]["route"], "valiant" if effective_beta > 0 else "minimum")
                if effective_beta > 0:
                    self.assertEqual(decisions[1]["route"], "minimum")
                with (metrics / "flow_dag_info.csv").open() as stream:
                    tasks = list(csv.DictReader(stream))
                self.assertTrue(all(r["finishTimeNs"] for r in tasks))
                with (metrics / "gra_pending_preferences.csv").open() as stream:
                    preferences = list(csv.DictReader(stream))
                snapshots = {}
                for row in preferences:
                    snapshots.setdefault((row["group"], row["snapshot_id"]), []).append(row)
                for snapshot in snapshots.values():
                    count = int(snapshot[0]["pending_count"])
                    self.assertEqual(len(snapshot), max(1, count))
                    self.assertEqual(len({(r["dag_id"], r["flow_id"], r["messagelet_index"])
                                          for r in snapshot}), max(1, count))
                for decision in decisions:
                    state = dict(item.split("=", 1) for item in decision["policy_state"].split(";") if item)
                    snapshot = snapshots[(str(int(decision["entry_switch"]) // 4), state["soft_claim_snapshot_id"])]
                    self.assertEqual({r["refresh_reason"] for r in snapshot}, {"decision"})
                    self.assertEqual({r["trigger_flow_id"] for r in snapshot}, {decision["flow_id"]})
                    self.assertEqual({r["time_ps"] for r in snapshot}, {decision["time_ps"]})
                low_snapshot = snapshots[("0", low_state["soft_claim_snapshot_id"])]
                self.assertEqual({r["flow_id"] for r in low_snapshot}, {"10", "20"})
                by_flow = {r["flow_id"]: r for r in low_snapshot}
                self.assertEqual(by_flow["10"]["claim_status"], "owner")
                self.assertEqual(by_flow["20"]["claim_status"], "higher_priority_owner")
                self.assertEqual(by_flow["20"]["claim_flow_id"], "10")
                self.assertEqual(by_flow["20"]["preferred_egress_switch"], low_state["min_egress_switch"])
                self.assertEqual(preferences[-1]["pending_count"], "0")
                self.assertEqual(preferences[-1]["claim_status"], "empty")

    def test_soft_claim_logging_does_not_change_execution(self):
        outputs = []
        for trace in (False, True):
            result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH",
                                   strategy="commitment_aware_ugal", soft_claim=True,
                                   trace_decisions=trace)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            metrics = self.last_run / "output_metrics"
            self.assertEqual((metrics / "gra_pending_preferences.csv").exists(), trace)
            outputs.append(tuple((metrics / name).read_bytes() for name in
                                 ("flow_dag_info.csv", "flowsInfo.csv")))
        self.assertEqual(outputs[0], outputs[1])

    def test_soft_claim_beta_rejects_invalid_values(self):
        for beta in (-1, "nan", "inf"):
            with self.subTest(beta=beta):
                result = self.run_flow(routing="ENTRY_SWITCH", routing_mode="ENTRY_SWITCH",
                                       strategy="commitment_aware_ugal", soft_claim=True, beta=beta)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("routing.soft_claim.beta", result.stderr)


if __name__ == "__main__":
    unittest.main()

"""Online prediction integration checks using the existing collective workload."""

import csv
import json
import math
import os
import subprocess
import tempfile
import unittest
from collections import defaultdict
from pathlib import Path

from test_gra_start_dependencies import write_goal


REPO = Path(__file__).resolve().parents[1]
BINARY = REPO / "htsim/sim/build/datacenter/htsim_uec_df"
WORKLOAD = REPO / "workload/exp2s/exp2a1_collective_internal_slack/workload.bin"
TOPOLOGY = REPO / "htsim/sim/datacenter/topologies/dragonfly/p2a4h2"


def read_csv(run, name):
    with (run / "output_metrics" / f"{name}.csv").open() as stream:
        return list(csv.DictReader(stream))


def run_collective(run, *, metadata=None, trace=True):
    run.mkdir()
    config = run / "online.json"
    config.write_text(json.dumps({
        "runtime": {"routing_mode": "ENTRY_SWITCH", "dag_id": 7},
        "routing": {"network_decision": "commitment_aware_ugal",
                    "soft_claim": {"enabled": True, "priority": "global_slack_online"}},
        "logging": {"trace_decisions": trace}}))
    placement = run / "placement.json"
    placement.write_text(json.dumps({"schema_version": 1, "placements": [
        {"rank": rank, "group": rank // 2, "switch": 0, "host": rank % 2}
        for rank in range(12)]}))
    extra = ["-flow_metadata", str(metadata)] if metadata else []
    return subprocess.run([
        str(BINARY), "-basepath", str(TOPOLOGY), "-goal", str(WORKLOAD),
        "-routing", "ENTRY_SWITCH", "-messagelet_config", str(config),
        "-rank_placement", str(placement), *extra], cwd=run, capture_output=True, text=True,
        timeout=30, env={**os.environ, "HTSIM_TRACE_PACKET_HOPS": "0", "HTSIM_TRACE_GOAL_SEND_GATE": "0"})


@unittest.skipUnless(BINARY.is_file(), "build htsim_uec_df first")
class OnlineFctPredictorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="htsim-online-fct-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_collective_runtime_samples_and_dynamic_estimates(self):
        run = self.root / "smoke"
        result = run_collective(run)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        metadata = json.loads(WORKLOAD.with_suffix(".meta.json").read_text())
        flows = metadata["flows"]
        network = read_csv(run, "flowsInfo")
        self.assertEqual(len(network), len(flows))
        lifecycle = read_csv(run, "flow_dag_info")
        self.assertTrue(all(row["finishTimeNs"] for row in lifecycle))
        self.assertEqual(len(lifecycle), metadata["workload"]["tasks"])

        # Match actual transport IDs through the existing entry decision log,
        # independently of GOAL's matched SEND/RECV FINISH timestamps.
        transport_to_dag = {}
        for row in read_csv(run, "entry_decisions"):
            transport = row["transport_flow_id"]
            self.assertEqual(transport_to_dag.setdefault(transport, row["flow_id"]), row["flow_id"])
        history = defaultdict(list)
        for row in network:  # CSV preserves completion callback order, including time ties.
            src, dst, transport = row["srcNode_dstNode_flowId"].split("_")
            flow = flows[transport_to_dag[transport]]
            self.assertEqual(int(src), (flow["src"] // 2) * 8 + flow["src"] % 2)
            self.assertEqual(int(dst), (flow["dst"] // 2) * 8 + flow["dst"] % 2)
            history[flow["collective_id"]].append((round(float(row["endTimeNs"]) * 1000), float(row["fctNs"])))

        rows = [row for row in read_csv(run, "gra_pending_preferences") if row["flow_id"]]
        self.assertTrue(rows)
        self.assertEqual({row["priority_strategy"] for row in rows}, {"global_slack_online"})
        self.assertEqual({row["dag_id"] for row in rows}, {"7"})
        self.assertTrue(any(row["nominal_fallback"] == "true" for row in rows))
        self.assertTrue(any(row["nominal_fallback"] == "false" for row in rows))
        self.assertTrue(any(row["refresh_reason"] == "fct_completion" for row in rows))
        self.assertTrue(any(row["fct_sample_count"] == "8" for row in rows))
        self.assertTrue(any(float(row["dynamic_tail_ns"]) != float(row["nominal_tail_ns"]) for row in rows))
        speed = float(read_csv(run, "globalInfo")[0]["linkSpeedGbps"])
        for row in rows:
            flow = flows[row["flow_id"]]
            self.assertEqual(int(row["collective_id"]), flow["collective_id"])
            now_ps = int(row["time_ps"])
            self.assertEqual(int(row["prediction_time_ns"]), now_ps // 1000)
            samples = history[flow["collective_id"]]
            earlier = sum(end < now_ps for end, _ in samples)
            visible = sum(end <= now_ps for end, _ in samples)
            count = int(row["fct_sample_count"])
            predicted = float(row["predicted_fct_ns"])
            # Several callbacks may share a timestamp; the log row must match
            # a prefix of completions already delivered at that event time.
            candidates = []
            for prefix in range(earlier, visible + 1):
                window = samples[max(0, prefix - 8):prefix]
                if len(window) != count:
                    continue
                candidates.append(sum(fct for _, fct in window) / len(window) if window
                                  else math.ceil(flow["bytes"] * 8 / speed))
            self.assertTrue(any(abs(predicted - candidate) < 1e-6 for candidate in candidates), row)
            self.assertEqual(row["nominal_fallback"], "true" if count == 0 else "false")
            if row["priority_scope"] != "completed":
                self.assertAlmostEqual(float(row["global_slack_ns"]),
                    float(row["estimated_workload_end_ns"]) - float(row["estimated_finish_time_ns"]) -
                    float(row["dynamic_tail_ns"]), places=6)

    def test_missing_collective_metadata_is_rejected(self):
        metadata = json.loads(WORKLOAD.with_suffix(".meta.json").read_text())
        del metadata["flows"]["0"]["collective_id"]
        path = self.root / "incomplete.meta.json"
        path.write_text(json.dumps(metadata))
        result = run_collective(self.root / "missing", metadata=path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("global_slack_online requires collective_id metadata for DAG 7 flow 0", result.stdout + result.stderr)

    def test_network_fct_excludes_delayed_goal_receive(self):
        run = self.root / "delayed-receive"
        run.mkdir()
        goal = run / "workload.bin"
        write_goal(goal, [[(1, 1, 2500, 1, (1,), ()), (1, 1, 2500, 2, (), ())],
                          [(3, 0, 100000, 0, (1,), ()), (2, 0, 2500, 1, (), ()),
                           (2, 0, 2500, 2, (), ())]])
        flows = {str(flow_id): {"flow_id": flow_id, "collective_id": 77, "src": 0, "dst": 1,
                 "bytes": 2500, "tag": index + 1, "send_task": {"rank": 0, "task_id": index},
                 "recv_task": {"rank": 1, "task_id": index + 1}}
                 for index, flow_id in enumerate((901, 17))}
        goal.with_suffix(".meta.json").write_text(json.dumps(
            {"schema_version": 1, "workload": {"p2p_flows": 2}, "flows": flows}))
        config = run / "online.json"
        config.write_text(json.dumps({"runtime": {"routing_mode": "ENTRY_SWITCH", "dag_id": 7},
            "routing": {"network_decision": "commitment_aware_ugal",
                        "soft_claim": {"enabled": True, "priority": "global_slack_online"}},
            "logging": {"trace_decisions": True}}))
        placement = run / "placement.json"
        placement.write_text(json.dumps({"schema_version": 1, "placements": [
            {"rank": 0, "group": 0, "switch": 0, "host": 0},
            {"rank": 1, "group": 3, "switch": 0, "host": 1}]}))
        result = subprocess.run([str(BINARY), "-basepath", str(TOPOLOGY), "-goal", str(goal),
                                 "-routing", "ENTRY_SWITCH", "-messagelet_config", str(config),
                                 "-rank_placement", str(placement)],
                                cwd=run, capture_output=True, text=True, timeout=10,
                                env={**os.environ, "HTSIM_TRACE_PACKET_HOPS": "0"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        network = read_csv(run, "flowsInfo")
        self.assertEqual(len(network), 2)
        fct = float(network[0]["fctNs"])
        self.assertLess(fct, 100000)
        send = next(row for row in read_csv(run, "flow_dag_info") if row["op"] == "send")
        self.assertEqual(int(send["finishTimeNs"]) - int(send["issueTimeNs"]), 100000)
        rows = [row for row in read_csv(run, "gra_pending_preferences") if row["flow_id"] == "17"]
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row["collective_id"], "77")
            self.assertEqual(row["fct_sample_count"], "1")
            self.assertAlmostEqual(float(row["predicted_fct_ns"]), fct, places=6)

    def test_logging_does_not_change_online_execution(self):
        for trace in (True, False):
            run = self.root / str(trace)
            result = run_collective(run, trace=trace)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for metric in ("flowsInfo", "flow_dag_info"):
            self.assertEqual(read_csv(self.root / "True", metric), read_csv(self.root / "False", metric))
        self.assertFalse((self.root / "False/output_metrics/gra_pending_preferences.csv").exists())


if __name__ == "__main__":
    unittest.main()

"""Small GOAL START/FINISH integration cases; build htsim_uec_df first."""

import csv
import json
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


BINARY = Path(__file__).resolve().parents[1] / "htsim/sim/build/datacenter/htsim_uec_df"


def write_goal(path, ranks):
    """Write raw GOAL tasks, including START edges absent from the JSON generator."""
    blobs = []
    for tasks in ranks:
        indegree = [0] * len(tasks)
        for _, _, _, _, finish, start in tasks:
            for target in (*finish, *start):
                indegree[target] += 1
        roots = [i for i, count in enumerate(indegree) if count == 0]
        blob = bytearray(struct.pack("<II", len(tasks), len(roots)))
        for root in roots:
            blob.extend(struct.pack("<I", root))
        appendix = []
        for i, (op, peer, size, tag, finish, start) in enumerate(tasks):
            blob.extend(struct.pack("<IBIQIBBIIII", indegree[i], op, peer, size, tag, 0, 0,
                                    len(finish), len(appendix), len(start), len(appendix) + len(finish)))
            appendix.extend((*finish, *start))
        for target in appendix:
            blob.extend(struct.pack("<I", target))
        blobs.append(blob)
    header = bytearray(struct.pack("<QIBB", 4223, len(ranks), 0, 0))
    offset = 6 + len(ranks) * 16
    for blob in blobs:
        header.extend(struct.pack("<QQ", offset, offset + len(blob)))
        offset += len(blob)
    path.write_bytes(header + b"".join(blobs))


@unittest.skipUnless(BINARY.is_file(), "build htsim_uec_df first")
class GraStartDependenciesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="htsim-gra-start-")
        self.root = Path(self.temp.name)
        self.topology = self.root / "topology"
        self.topology.mkdir()
        (self.topology / "dragonfly.topo").write_text("p 2\na 4\nh 2\n")

    def tearDown(self):
        self.temp.cleanup()

    def run_goal(self, ranks, placement=None, soft_claim_priority=None):
        run = self.root / f"run-{len(list(self.root.iterdir()))}"
        run.mkdir()
        workload = run / "workload.bin"
        write_goal(workload, ranks)
        extra = []
        if placement is not None:
            placement_file = run / "placement.json"
            placement_file.write_text(json.dumps({"schema_version": 1, "placements": placement}))
            extra = ["-rank_placement", str(placement_file)]
        if soft_claim_priority is not None:
            flows = {}
            for rank, tasks in enumerate(ranks):
                for offset, (op, peer, size, tag, _, _) in enumerate(tasks):
                    if op != 1:
                        continue
                    recv = next(i for i, task in enumerate(ranks[peer]) if task[:4] == (2, rank, size, tag))
                    flow_id = (rank << 32) | offset
                    flows[str(flow_id)] = {"flow_id": flow_id, "src": rank, "dst": peer, "bytes": size, "tag": tag,
                                           "send_task": {"rank": rank, "task_id": offset},
                                           "recv_task": {"rank": peer, "task_id": recv}}
            workload.with_suffix(".meta.json").write_text(json.dumps(
                {"schema_version": 1, "workload": {"p2p_flows": len(flows)}, "flows": flows}))
            config = run / "messagelet.json"
            config.write_text(json.dumps({"runtime": {"routing_mode": "ENTRY_SWITCH"},
                "routing": {"network_decision": "commitment_aware_ugal",
                            "soft_claim": {"enabled": True, "priority": soft_claim_priority}},
                "logging": {"trace_decisions": True}}))
            extra += ["-messagelet_config", str(config)]
        result = subprocess.run([str(BINARY), "-basepath", str(self.topology), "-goal", str(workload),
                                 "-routing", "ENTRY_SWITCH" if soft_claim_priority is not None else "SOURCE", *extra],
                                cwd=run, capture_output=True, text=True,
                                timeout=10, env={**os.environ, "HTSIM_TRACE_PACKET_HOPS": "0",
                                                "HTSIM_TRACE_GOAL_SEND_GATE": "0",
                                                "HTSIM_TRACE_GRA_PROGRESS": "1"})
        return result, run

    def completed(self, result, run, count):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with (run / "output_metrics/flow_dag_info.csv").open() as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), count)
        self.assertTrue(all(row["finishTimeNs"] for row in rows), rows)
        tasks = {(int(row["rank"]), int(row["nodeOffset"])): row for row in rows}
        with (run / "output_metrics/gra_progress_reports.csv").open() as stream:
            reports = list(csv.DictReader(stream))
        self.assertEqual(len(reports), 2 * count)
        versions = {}
        seen = set()
        for report in reports:
            rank, offset = int(report["rank"]), int(report["task_offset"])
            versions[rank] = versions.get(rank, 0) + 1
            self.assertEqual(int(report["version"]), versions[rank])
            identity = rank, offset, report["event"]
            self.assertNotIn(identity, seen)
            seen.add(identity)
            time_field = "issueTimeNs" if report["event"] == "START" else "finishTimeNs"
            self.assertEqual(report["event_time_ns"], tasks[rank, offset][time_field])
        # RECEIVE FINISH reports consume versions without duplicating COMM FINISH.
        event_count = sum(3 if row["op"] == "send" else 2 if row["op"] == "calc" else 0 for row in rows)
        self.assertEqual(int(reports[-1]["confirmed_events"]), event_count)
        self.assertEqual(reports[-1]["frontier"], "")
        return tasks

    def test_crossed_sends_with_start_and_mixed_dependencies(self):
        for first_start, second_start in ((True, True), (True, False), (False, True)):
            with self.subTest(first_start=first_start, second_start=second_start):
                a = (1, 1, 2500, 1, () if first_start else (1,), (1,) if first_start else ())
                b = (1, 0, 2500, 2, () if second_start else (1,), (1,) if second_start else ())
                result, run = self.run_goal([[a, (2, 1, 2500, 2, (), ())],
                                             [b, (2, 0, 2500, 1, (), ())]])
                rows = self.completed(result, run, 4)
                for rank, is_start in enumerate((first_start, second_start)):
                    source, target = rows[rank, 0], rows[rank, 1]
                    self.assertEqual(target["readyTimeNs"],
                                     source["issueTimeNs"] if is_start else source["finishTimeNs"])

    def test_crossed_finish_dependencies_remain_a_deadlock(self):
        result, _ = self.run_goal([
            [(1, 1, 2500, 1, (1,), ()), (2, 1, 2500, 2, (), ())],
            [(1, 0, 2500, 2, (1,), ()), (2, 0, 2500, 1, (), ())],
        ])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency cycle", result.stderr)

    def test_send_and_receive_start_release_work_before_completion(self):
        result, run = self.run_goal([
            [(1, 1, 2500, 1, (), (1,)), (3, 0, 1000, 0, (), ())],
            [(2, 0, 2500, 1, (), (1,)), (3, 0, 2000, 0, (), ())],
        ])
        rows = self.completed(result, run, 4)
        for rank in (0, 1):
            comm, comp = rows[rank, 0], rows[rank, 1]
            self.assertEqual(comp["readyTimeNs"], comm["issueTimeNs"])
            self.assertLess(int(comp["issueTimeNs"]), int(comm["finishTimeNs"]))

    def test_progress_reports_follow_rank_placement(self):
        result, run = self.run_goal([
            [(1, 1, 2500, 1, (), ())], [(2, 0, 2500, 1, (), ())],
        ], placement=[{"rank": 0, "group": 0, "switch": 0, "host": 0},
                      {"rank": 1, "group": 3, "switch": 0, "host": 1}])
        self.completed(result, run, 2)
        with (run / "output_metrics/gra_progress_reports.csv").open() as stream:
            for report in csv.DictReader(stream):
                self.assertEqual(int(report["group"]), 0 if report["rank"] == "0" else 3)

    def test_join_slack_protects_nearest_join_instead_of_start_only_d(self):
        # Long A START-only compute makes D(A) larger, but B's longer transfer
        # limits the shared receive-side Join. Both sends become READY together.
        ranks = [
            [(1, 1, 65536, 10, (), (2,)), (1, 1, 262144, 20, (), ()), (3, 0, 100000, 0, (), ())],
            [(2, 0, 65536, 10, (2,), ()), (2, 0, 262144, 20, (2,), ()), (3, 0, 0, 0, (), ())]]
        placement = [{"rank": 0, "group": 0, "switch": 0, "host": 0},
                     {"rank": 1, "group": 3, "switch": 0, "host": 1}]
        for strategy, expected_owner, expected_route in (("join_slack", "1", "valiant"),
                                                          ("dag_downstream", "0", "minimum")):
            with self.subTest(strategy=strategy):
                result, run = self.run_goal(ranks, placement, strategy)
                self.completed(result, run, 6)
                with (run / "output_metrics/entry_decisions.csv").open() as stream:
                    decisions = list(csv.DictReader(stream))
                first = decisions[0]
                self.assertEqual(first["flow_id"], "0")
                self.assertEqual(first["route"], expected_route)
                state = dict(item.split("=", 1) for item in first["policy_state"].split(";") if item)
                self.assertEqual(state["soft_claim_min_owner_flow_id"], expected_owner)
                with (run / "output_metrics/gra_pending_preferences.csv").open() as stream:
                    snapshots = list(csv.DictReader(stream))
                snapshot = [r for r in snapshots if r["group"] == "0" and
                            r["snapshot_id"] == state["soft_claim_snapshot_id"]]
                self.assertEqual({r["priority_strategy"] for r in snapshot}, {strategy})
                self.assertEqual({r["claim_flow_id"] for r in snapshot}, {expected_owner})
                if strategy == "join_slack":
                    self.assertEqual({r["priority_scope"] for r in snapshot}, {"join"})
                    self.assertTrue(any(r["refresh_reason"] == "progress" for r in snapshots))
                    by_flow = {r["flow_id"]: r for r in snapshot}
                    self.assertGreater(float(by_flow["0"]["slack_ns"]), 0)
                    self.assertLess(float(by_flow["1"]["slack_ns"]), 0)

    def test_global_slack_sees_past_a_local_join(self):
        # A limits a short Join; B has local compute-peer slack, but the work
        # after B's Join determines final completion. Global priority protects B.
        ranks = [
            [(1, 1, 65536, 10, (), ()), (1, 1, 65536, 20, (), ())],
            [(2, 0, 65536, 10, (4,), ()), (2, 0, 65536, 20, (5,), ()),
             (3, 0, 100, 0, (4,), ()), (3, 0, 10000, 0, (5,), ()),
             (3, 0, 0, 0, (), ()), (3, 0, 100000, 0, (), ())]]
        placement = [{"rank": 0, "group": 0, "switch": 0, "host": 0},
                     {"rank": 1, "group": 3, "switch": 0, "host": 1}]
        for strategy, owner, route in (("global_slack", "1", "valiant"), ("join_slack", "0", "minimum")):
            with self.subTest(strategy=strategy):
                result, run = self.run_goal(ranks, placement, strategy)
                self.completed(result, run, 8)
                with (run / "output_metrics/entry_decisions.csv").open() as stream:
                    decision = next(csv.DictReader(stream))
                self.assertEqual(decision["flow_id"], "0")
                self.assertEqual(decision["route"], route)
                state = dict(item.split("=", 1) for item in decision["policy_state"].split(";") if item)
                self.assertEqual(state["soft_claim_min_owner_flow_id"], owner)
                self.assertEqual(state["r_min_bytes"], "0")
                with (run / "output_metrics/gra_pending_preferences.csv").open() as stream:
                    snapshot = [r for r in csv.DictReader(stream) if r["group"] == "0" and
                                r["snapshot_id"] == state["soft_claim_snapshot_id"]]
                by_flow = {r["flow_id"]: r for r in snapshot}
                if strategy == "global_slack":
                    self.assertEqual({r["priority_scope"] for r in snapshot}, {"global"})
                    self.assertEqual(float(by_flow["0"]["nominal_tail_ns"]), 0)
                    self.assertEqual(float(by_flow["1"]["nominal_tail_ns"]), 100000)
                    self.assertLess(float(by_flow["1"]["global_slack_ns"]), float(by_flow["0"]["global_slack_ns"]))
                    for row in snapshot:
                        self.assertEqual(float(row["global_slack_ns"]), float(row["estimated_workload_end_ns"]) -
                                         (float(row["estimated_finish_time_ns"]) + float(row["nominal_tail_ns"])))
                else:
                    self.assertLess(float(by_flow["0"]["slack_ns"]), 0)
                    self.assertGreater(float(by_flow["1"]["slack_ns"]), 0)


if __name__ == "__main__":
    unittest.main()

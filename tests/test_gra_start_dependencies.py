"""Small GOAL START/FINISH integration cases; build htsim_uec_df first."""

import csv
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

    def run_goal(self, ranks):
        run = self.root / f"run-{len(list(self.root.iterdir()))}"
        run.mkdir()
        workload = run / "workload.bin"
        write_goal(workload, ranks)
        result = subprocess.run([str(BINARY), "-basepath", str(self.topology), "-goal", str(workload),
                                 "-routing", "SOURCE"], cwd=run, capture_output=True, text=True,
                                timeout=10, env={**os.environ, "HTSIM_TRACE_PACKET_HOPS": "0",
                                                "HTSIM_TRACE_GOAL_SEND_GATE": "0"})
        return result, run

    def completed(self, result, run, count):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with (run / "output_metrics/flow_dag_info.csv").open() as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), count)
        self.assertTrue(all(row["finishTimeNs"] for row in rows), rows)
        return {(int(row["rank"]), int(row["nodeOffset"])): row for row in rows}

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


if __name__ == "__main__":
    unittest.main()

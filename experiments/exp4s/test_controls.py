"""Integration checks of the causal audit using the completed exp4 artifacts."""

import copy
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from compare import CASES, ROOT, compare, read_run, validate_inputs, validate_network_pairs


@unittest.skipUnless(all((ROOT / f"exp4.{key}/artifacts/latest").exists() for key, _, _ in CASES),
                     "run experiments/exp4s/run_all.sh first")
class CausalControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runs = {key: read_run(ROOT / f"exp4.{key}/artifacts/latest", dag, target)
                    for key, dag, target in CASES}

    def test_complete_experiment_passes(self):
        with tempfile.TemporaryDirectory(prefix="exp4-report-test-") as directory:
            self.assertTrue(compare(self.runs, Path(directory)))

    def test_identical_fct_cannot_hide_changed_hop_event(self):
        runs = copy.deepcopy(self.runs)
        runs["3"]["hops"][0] = runs["3"]["hops"][0].replace("PacketHop 2000 ", "PacketHop 2001 ")
        with self.assertRaisesRegex(ValueError, "packet-hop or queue-state trace differs"):
            validate_network_pairs(runs)

    def test_other_dag_changes_are_rejected(self):
        runs = copy.deepcopy(self.runs)
        runs["3"]["template"]["defaults"]["nic"] = 1
        with self.assertRaisesRegex(ValueError, "DAG differs beyond tails"):
            validate_inputs(runs)

    def test_binary_cannot_disagree_with_readable_dag(self):
        with tempfile.TemporaryDirectory(prefix="exp4-input-test-") as directory:
            artifact = Path(directory) / "artifact"
            shutil.copytree(self.runs["1"]["path"], artifact)
            binary = artifact / "config_snapshot/workload.bin"
            data = bytearray(binary.read_bytes())
            data[-1] ^= 1
            binary.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "executed workload.bin differs"):
                read_run(artifact, 1, 0)

    def test_packet_tracing_does_not_change_execution(self):
        for key in ("1", "2"):
            run = self.runs[key]
            command = shlex.split((run["path"] / "command.txt").read_text())
            with tempfile.TemporaryDirectory(prefix="exp4-trace-off-") as directory:
                env = {**os.environ, "HTSIM_TRACE_PACKET_HOPS": "0", "HTSIM_TRACE_FLOW_COMPLETIONS": "1"}
                result = subprocess.run(command, cwd=directory, env=env, capture_output=True,
                                        text=True, check=True, timeout=30)
                self.assertNotIn("PacketHop ", result.stdout)
                for name in ("flowsInfo.csv", "flowRouting.csv", "flow_dag_info.csv"):
                    self.assertEqual((Path(directory) / "output_metrics" / name).read_bytes(),
                                     (run["path"] / "output_metrics" / name).read_bytes())
                self.assertIn(f"Htsim time {run['cct']}", result.stdout)


if __name__ == "__main__":
    unittest.main()

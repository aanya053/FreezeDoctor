"""Test real process lifecycle and report integrity without assuming host timing."""
import csv
import json
import math
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'build/freezedoctor'


class FreezeDoctorTests(unittest.TestCase):
    def test_invalid_inputs(self):
        for args in [('--seconds', 'nan'), ('--seconds', '1'),
                     ('--workers', '0'), ('--workers', '17'),
                     ('--cpu', '-1'), ('--cpu', '999999'),
                     ('--seconds', '2oops'), ('--report', 'same', '--csv', 'same')]:
            with self.subTest(args=args):
                self.assertEqual(subprocess.run([BIN, *args], capture_output=True).returncode, 2)

    def run_experiment(self, extra=()):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / 'report.json'
            samples = Path(directory) / 'samples.csv'
            run = subprocess.run([BIN, '--seconds', '2', '--report', report,
                                  '--csv', samples, *extra], text=True,
                                 capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)
            data = json.loads(report.read_text())
            with samples.open() as stream:
                rows = list(csv.DictReader(stream))
            for phase, key in [('baseline', 'baseline'), ('contention', 'before'),
                               ('verification', 'after')]:
                values = sorted(float(r['response_ms']) for r in rows if r['phase'] == phase)
                self.assertGreater(len(values), 0)
                self.assertEqual(len(values), data[key]['samples'])
                self.assertAlmostEqual(sum(values) / len(values), data[key]['mean_ms'], places=5)
                self.assertAlmostEqual(values[math.ceil(.95 * len(values)) - 1], data[key]['p95_ms'], places=5)
            for evidence in data['workload_evidence']:
                self.assertGreaterEqual(evidence['cpu_ticks_after'], evidence['cpu_ticks_before'])
                self.assertFalse(Path(f"/proc/{evidence['pid']}").exists(), 'workload leaked')
            self.assertFalse(Path(f"/proc/{data['probe_pid']}").exists(), 'probe leaked')
            return data

    def test_measured_experiment(self):
        data = self.run_experiment()
        if data['recovery_verified']:
            self.assertTrue(data['contention_detected'])
            self.assertTrue(data['priority_adjustment_applied'])
            self.assertGreater(data['improvement_percent'], 10)
            self.assertLess(data['after']['p95_ms'], data['before']['p95_ms'])
        if data['priority_adjustment_applied']:
            self.assertTrue(all(w['nice_after'] == 19 for w in data['workload_evidence']))
        # Hosts need not reproduce scheduling degradation; never fabricate success.

    def test_control_cannot_claim_recovery(self):
        data = self.run_experiment(('--no-recovery',))
        self.assertFalse(data['recovery_verified'])
        self.assertFalse(data['priority_adjustment_applied'])
        self.assertEqual(data['intervention_monotonic_ns'], 0)
        for worker in data['workload_evidence']:
            self.assertEqual(worker['nice_before'], worker['nice_after'])

    def test_interrupt_reaps_children(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = subprocess.Popen([BIN, '--seconds', '10', '--report', f'{directory}/r.json',
                                     '--csv', f'{directory}/s.csv'], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            pids = []
            try:
                # Wait for a real child, not just for the parent to start.
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    pids = []
                    for path in Path('/proc').glob('[0-9]*/stat'):
                        try:
                            fields = path.read_text().rsplit(')', 1)[1].split()
                            if int(fields[1]) == proc.pid:
                                pids.append(path.parent.name)
                        except (OSError, IndexError, ValueError):
                            continue
                    if pids:
                        break
                    time.sleep(.02)
                self.assertTrue(pids)
                proc.send_signal(signal.SIGINT)
                proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 130)
                for pid in pids:
                    self.assertFalse(Path(f'/proc/{pid}').exists(), 'child survived interruption')
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()


if __name__ == '__main__':
    unittest.main()

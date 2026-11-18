"""Tests for compensation/ech_meter_calibration.py on synthetic shots (no MDS, no hardware).

    python3 -m unittest discover -s compensation/tests
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "ech_meter_calibration.py")
sys.path.insert(0, os.path.join(HERE, ".."))

try:
    import numpy as np
    import ech_meter_calibration as mc
except ImportError:
    np = mc = None


@unittest.skipIf(mc is None, "numpy not installed")
class TestCalibration(unittest.TestCase):
    def shots(self, K, lag=0.0, **kw):
        return [mc.synthetic("dummy-load", K, 1), mc.synthetic("vessel", K, 2, lag_ms=lag, **kw),
                mc.synthetic("dummy-load", K, 3), mc.synthetic("vessel", K, 4, lag_ms=lag, **kw)]

    def test_recovers_the_ratio_and_the_timing(self):
        for K, lag in ((1.0, 0.0), (1.08, 0.04), (0.91, -0.06)):
            r = mc.calibrate(self.shots(K, lag))
            self.assertAlmostEqual(r["power_scale"], K, delta=0.005)
            self.assertAlmostEqual(r["lag_ms"], lag, delta=0.015)
            self.assertAlmostEqual(r["fit_slope"], K, delta=0.02)
            self.assertLess(r["v_mismatch_pct"], 0.5)

    def test_baselines_are_removed(self):
        # the synthetic meters read +3 kW with no power; the ratio must not include it
        r = mc.calibrate(self.shots(1.05))
        for b in r["baselines_kw"].values():
            self.assertAlmostEqual(b, 3.0, delta=0.3)
        self.assertAlmostEqual(r["power_scale"], 1.05, delta=0.005)

    def test_a_power_dependent_ratio_shows_as_an_offset(self):
        r = mc.calibrate(self.shots(1.0, offset_kw=15.0))
        self.assertGreater(r["fit_offset_kw"], 8.0)
        self.assertTrue(any("offset" in n for n in r["notes"]))
        ratios = [b["ratio"] for b in r["bins"]]
        self.assertGreater(ratios[0], ratios[-1])                    # +15 kW weighs more at low power

    def test_refuses_a_gyrotron_that_ran_differently(self):
        with self.assertRaises(mc.CalError):
            mc.calibrate(self.shots(1.0, v_shift=0.03))
        r = mc.calibrate(self.shots(1.0, v_shift=0.03), force=True)
        self.assertTrue(any("ran differently" in n for n in r["notes"]))

    def test_needs_both_targets(self):
        with self.assertRaises(mc.CalError):
            mc.calibrate([mc.synthetic("dummy-load", 1.0, 1), mc.synthetic("dummy-load", 1.0, 2)])

    def test_command_line_on_csv_files(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for k, s in enumerate(self.shots(1.04)):
                p = os.path.join(d, f"s{k}.csv")
                mc.save_csv(s, p)
                paths.append(p)
            out = subprocess.run([sys.executable, SCRIPT, *paths, "--out", os.path.join(d, "cal"), "--no-plot"],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("status=ok", out.stdout)
            k = float(out.stdout.split("power_scale=")[1].split()[0])
            self.assertAlmostEqual(k, 1.04, delta=0.003)
            self.assertIn(f"--power-scale {k:.4f}", out.stderr)
            self.assertTrue(os.path.exists(os.path.join(d, "cal.json")))


if __name__ == "__main__":
    unittest.main()

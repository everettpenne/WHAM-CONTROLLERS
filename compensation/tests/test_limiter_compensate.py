"""Tests for compensation/limiter_compensate.py on a simulated capacitor bank (no MDS, no hardware).

    python3 -m unittest discover -s compensation/tests
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "limiter_compensate.py")
sys.path.insert(0, os.path.join(HERE, ".."))

try:
    import numpy as np
    import limiter_compensate as lc
except ImportError:                                   # numpy missing: nothing here can run
    np = lc = None


def profile(rows, phases=1):
    """rows: (t_us, carrier_hz, duty or (dU, dV, dW)); demand == commanded."""
    t = [r[0] for r in rows]
    c = [r[1] for r in rows]
    d = [list(r[2]) if phases == 3 else [r[2]] for r in rows]
    return lc.Profile(t, c, d, d)


def iterate(p, shots, **sim):
    errs = []
    r = None
    for i in range(shots):
        t_ms, v = lc.simulate(p, seed=i, **sim)
        r = lc.compensate(p, t_ms, v)
        errs.append(r.rms_pct)
        p = r.profile
    return errs, r


@unittest.skipIf(lc is None, "numpy not installed")
class TestAnalysis(unittest.TestCase):
    def test_alignment_sign_and_offset(self):
        p = lc.example_profile()
        for sign, t0 in ((1, 3.217), (-1, -1.5), (1, 12.0)):
            t_ms, v = lc.simulate(p, sign=sign, t0_ms=t0, offset=4.0, post_ms=20.0)
            r = lc.compensate(p, t_ms, v)
            self.assertEqual(r.sign, sign)
            self.assertAlmostEqual(r.t0_ms, t0, delta=0.03)          # the 10 us measurement filter delays it a little
            self.assertAlmostEqual(r.baseline, 4.0, delta=2.0)

    def test_bank_fit_tracks_true_droop(self):
        p = lc.example_profile()
        t_ms, v = lc.simulate(p, noise=0.002)
        r = lc.compensate(p, t_ms, v)
        # the simulated bank: 900 V, drained by the output; rebuild it to compare
        tau = np.arange(0.0, p.duration_us, 2.0)
        d, on = lc.eval_profile(p.t_us, p.carrier_hz, p.commanded, tau)
        x = 2 * d[:, 0] / 100 - 1
        vc, true = 900.0, []
        for xi in x:
            true.append(vc)
            vc -= (vc * xi) ** 2 / (0.02 * vc) * 2e-6
        true = np.interp(r.tau_ms, tau / 1000, true)
        inner = r.use
        self.assertLess(np.max(np.abs(r.vc[inner] - true[inner])) / 900.0, 0.01)
        self.assertGreater(r.droop_pct, 5.0)

    def test_iteration_converges(self):
        errs, r = iterate(lc.example_profile(), 4)
        self.assertGreater(errs[0], 2.0)                             # the droop is visible uncorrected
        self.assertLess(errs[-1], 1.0)
        for a, b in zip(errs, errs[1:]):
            self.assertLessEqual(b, a + 0.1)
        self.assertEqual(r.profile.demand.shape, r.profile.commanded.shape)

    def test_negative_sign_converges(self):
        errs, r = iterate(lc.example_profile(), 3, sign=-1)
        self.assertEqual(r.sign, -1)
        self.assertLess(errs[-1], 1.0)

    def test_gap_kept_and_converges(self):
        p = profile([(0, 100000, 80), (3000, 100000, 80), (4000, 0, 0), (6000, 100000, 25),
                     (9000, 100000, 25), (10000, 100000, 25)])
        errs, r = iterate(p, 3)
        self.assertLess(errs[-1], 1.0)
        q = r.profile
        off = q.t_us[q.carrier_hz == 0]
        self.assertEqual(list(off), [4000.0])
        self.assertTrue(np.all(q.commanded[q.carrier_hz == 0] == 0))
        self.assertIn(6000.0, list(q.t_us))
        lc.check_profile(q)

    def test_per_phase(self):
        p = profile([(0, 100000, (80, 75, 85)), (5000, 100000, (70, 70, 70)), (10000, 100000, (75, 80, 70))], 3)
        errs, r = iterate(p, 3)
        self.assertLess(errs[-1], 1.0)
        self.assertEqual(r.profile.commanded.shape[1], 3)
        # the correction pushes every phase further from 50 % as the bank droops
        self.assertTrue(np.all(r.profile.commanded[-1] > r.profile.demand[-1]))

    def test_unreachable_demand_is_clamped_and_reported(self):
        p = profile([(0, 100000, 94), (20000, 100000, 94)])
        t_ms, v = lc.simulate(p, rc=0.01)
        r = lc.compensate(p, t_ms, v)
        self.assertTrue(r.unreachable)
        self.assertLessEqual(r.profile.commanded.max(), lc.LIM_DUTY_MAX_PCT)
        lc.check_profile(r.profile)

    def test_wrong_shot_is_refused(self):
        p = lc.example_profile()
        t = np.arange(0, 40, 0.002)
        noise = np.random.default_rng(3).standard_normal(len(t))
        with self.assertRaises(lc.CompError):
            lc.compensate(p, t, noise)

    def test_setpoint_count_stays_bounded(self):
        p = lc.example_profile()
        n = []
        for i in range(5):
            t_ms, v = lc.simulate(p, seed=i)
            p = lc.compensate(p, t_ms, v).profile
            n.append(len(p.t_us))
        self.assertLess(max(n), 40)                                  # rows from earlier iterations are not piled up

    def test_gain(self):
        p = lc.example_profile()
        t_ms, v = lc.simulate(p)
        full = lc.compensate(p, t_ms, v).profile.commanded[-1, 0]
        half = lc.compensate(p, t_ms, v, gain=0.5).profile.commanded[-1, 0]
        self.assertAlmostEqual(half - 30.0, (full - 30.0) / 2, delta=0.05)


@unittest.skipIf(lc is None, "numpy not installed")
class TestProfileRules(unittest.TestCase):
    def bad(self, rows):
        with self.assertRaises(lc.CompError):
            lc.check_profile(profile(rows))

    def test_rules(self):
        lc.check_profile(profile([(0, 100000, 50), (1000, 100000, 50)]))
        self.bad([(0, 100000, 50)])                                              # one setpoint
        self.bad([(10, 100000, 50), (1000, 100000, 50)])                         # not at 0
        self.bad([(0, 100000, 50), (0, 100000, 50)])                             # not increasing
        self.bad([(0, 100000, 50), (1000, 100000, 96)])                          # duty high
        self.bad([(0, 100000, 4), (1000, 100000, 50)])                           # duty low
        self.bad([(0, 70000, 50), (1000, 100000, 50)])                           # carrier low
        self.bad([(0, 100000, 50), (1000, 130000, 50)])                          # carrier high
        self.bad([(0, 0, 0), (1000, 100000, 50), (2000, 100000, 50)])            # starts off
        self.bad([(0, 100000, 50), (1000, 100000, 50), (2000, 0, 0)])            # ends off
        self.bad([(0, 100000, 50), (1000, 0, 0), (2000, 100000, 50)])            # last two not both on
        self.bad([(0, 100000, 50), (2_000_000, 100000, 50)])                     # too long
        self.bad([(i * 10, 100000, 50) for i in range(501)])                     # too many

    def test_interpolation_matches_firmware_rules(self):
        p = profile([(0, 100000, 20), (1000, 100000, 80), (2000, 100000, 80), (3000, 0, 0),
                     (4000, 100000, 50), (5000, 100000, 50)])
        d, on = lc.eval_profile(p.t_us, p.carrier_hz, p.demand, np.array([500.0, 2500.0, 3500.0, 4500.0, 5000.0]))
        self.assertAlmostEqual(d[0, 0], 50.0)       # linear ramp
        self.assertAlmostEqual(d[1, 0], 80.0)       # on -> off holds
        self.assertFalse(on[2])                     # inside the gap
        self.assertAlmostEqual(d[3, 0], 50.0)
        self.assertFalse(on[4])                     # the shot is over at the last setpoint's time


@unittest.skipIf(lc is None, "numpy not installed")
class TestCommandLine(unittest.TestCase):
    def test_round_trip_through_files(self):
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "shot.csv")
            with open(csv, "w") as f:
                f.write("# operator=test\ntime_us,carrier_hz,duty_pct\n0,100000,80\n4000,100000,80\n"
                        "6000,100000,30\n10000,100000,30\n")
            p = lc.read_profile(csv)
            t_ms, v = lc.simulate(p)
            data = os.path.join(d, "meas.csv")
            np.savetxt(data, np.column_stack([t_ms, v]), delimiter=",", header="time_ms,volts", comments="")
            out = subprocess.run([sys.executable, SCRIPT, csv, "--data", data, "--no-plot"],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("RESULT status=ok", out.stdout)
            nxt = os.path.join(d, "shot_iter1.csv")
            q = lc.read_profile(nxt)
            self.assertEqual(q.meta["iteration"], "1")
            self.assertEqual(q.meta["operator"], "test")
            lc.check_profile(q)
            np.testing.assert_allclose(lc.eval_profile(q.t_us, q.carrier_hz, q.demand, np.arange(0, 10000, 50.0))[0],
                                       lc.eval_profile(p.t_us, p.carrier_hz, p.demand, np.arange(0, 10000, 50.0))[0],
                                       atol=0.01)                     # the demand is unchanged
            # the second pass reads the commanded columns back and names the next file _iter2
            t_ms, v = lc.simulate(q, seed=5)
            np.savetxt(data, np.column_stack([t_ms, v]), delimiter=",", header="time_ms,volts", comments="")
            out = subprocess.run([sys.executable, SCRIPT, nxt, "--data", data, "--no-plot"],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertTrue(os.path.exists(os.path.join(d, "shot_iter2.csv")))

    def test_bad_csv_reports_error(self):
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "bad.csv")
            with open(csv, "w") as f:
                f.write("time_us,carrier_hz,duty_pct\n0,100000,99\n1000,100000,50\n")
            out = subprocess.run([sys.executable, SCRIPT, csv, "--data", csv], capture_output=True, text=True)
            self.assertEqual(out.returncode, 1)
            self.assertIn("RESULT status=error", out.stdout)


if __name__ == "__main__":
    unittest.main()

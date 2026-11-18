"""Tests for compensation/ech_compensate.py against a simulated ECH supply (no MDS, no hardware).

    python3 -m unittest discover -s compensation/tests
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "ech_compensate.py")
sys.path.insert(0, os.path.join(HERE, ".."))

try:
    import numpy as np
    import ech_compensate as ec
except ImportError:
    np = ec = None


def two_pulse(kw=200.0):
    return ec.picker_like([(0, kw), (4, kw), (9, kw), (13, kw)], gaps=(1,))


def commanded(rows, mode="power", demand=None):
    """rows: (t_us, carrier_hz); demand defaults to 200 kW (power) or the carrier (frequency); 0 Hz = off."""
    t = [r[0] for r in rows]
    c = [r[1] for r in rows]
    if demand is None:
        demand = [(200.0 if x > 0 else float("nan")) if mode == "power" else (x if x > 0 else float("nan")) for x in c]
    return ec.Profile(t, demand, c, [50.0 if x > 0 else 0.0 for x in c], mode)


def iterate(p, bank, shots, **kw):
    out = []
    for i in range(shots):
        t, V, P = ec.simulate(p, bank, seed=i)
        r = ec.compensate(p, t, V, P, bank, v_ref=(out[-1].v_ref if out else None), **kw)
        out.append(r)
        p = r.profile
    return out


@unittest.skipIf(ec is None, "numpy not installed")
class TestRules(unittest.TestCase):
    def bad(self, p):
        with self.assertRaises(ec.CompError):
            ec.check_profile(p)

    def test_rules(self):
        ec.check_profile(commanded([(0, 20000), (10000, 19800)]))
        self.bad(commanded([(0, 21100), (10000, 19800)]))                      # above the 21 kHz ceiling
        self.bad(commanded([(0, 20000), (10000, 18600)]))                      # below the 18.7 kHz floor
        self.bad(commanded([(0, 20000), (16000, 19800)]))                      # longer than 15 ms
        self.bad(commanded([(0, 0), (1000, 20000), (2000, 20000)]))            # starts off
        self.bad(commanded([(0, 20000), (1000, 20000), (2000, 0)]))            # ends off
        self.bad(commanded([(0, 20000), (1000, 0), (2000, 20000)]))            # last two not both on
        p = commanded([(0, 20000), (10000, 19800)])
        p.duty[0] = 65
        self.bad(p)                                                            # duty outside 40-60
        p = commanded([(0, 20000), (4000, 20000), (5000, 0), (9000, 20000), (13000, 20000)])
        p.demand[2] = 200.0
        self.bad(p)                                                            # gaps must match the demand's

    def test_interpolation_like_the_firmware(self):
        p = commanded([(0, 20000), (2000, 19800), (4000, 19800), (5000, 0), (9000, 19700), (13000, 19700)])
        f, on = ec.eval_rows(p.t_us, np.where(p.carrier > 0, p.carrier, np.nan), p.carrier > 0,
                             np.array([1000.0, 4500.0, 6000.0, 12999.0, 13000.0]))
        self.assertAlmostEqual(f[0], 19900.0)          # ramp
        self.assertAlmostEqual(f[1], 19800.0)          # on -> off holds
        self.assertFalse(on[2])                        # in the gap
        self.assertTrue(on[3])
        self.assertFalse(on[4])                        # the shot is over at the last row's time


@unittest.skipIf(ec is None, "numpy not installed")
class TestFirstShot(unittest.TestCase):
    def test_picker_demand_to_first_profile(self):
        q, info = ec.initial_profile(two_pulse(), 530)
        ec.check_profile(q)
        self.assertTrue(np.all((q.carrier[q.carrier > 0] >= ec.F_FLOOR_HZ) & (q.carrier[q.carrier > 0] <= ec.F_CEIL_HZ)))
        self.assertEqual(list(q.t_us[q.carrier == 0]), [4010.0])               # the gap is kept
        # the second pulse starts lower than the first (droop and the pulse-2 deficit)
        f1 = q.carrier[q.t_us == 0][0]
        f2 = q.carrier[q.t_us == 9000][0]
        self.assertLess(f2, f1 - 100)

    def test_unreachable_and_too_much(self):
        _, info = ec.initial_profile(two_pulse(400), 400)
        self.assertTrue(any("not reachable" in n or "capped" in n for n in info["notes"]))
        _, info = ec.initial_profile(two_pulse(20), 700)
        self.assertTrue(any("lower the bank charge" in n for n in info["notes"]))

    def test_start_of_pulse_follows_the_flat_top(self):
        q, info = ec.initial_profile(two_pulse(), 530)
        g, f = info["grid"], info["f_new"]
        for k, (a, b) in enumerate(g.pulses):
            rise = (g.pidx == k) & (g.since < ec.SETTLE_MS)
            first = np.nonzero(g.ctrl & (g.pidx == k))[0][0]
            np.testing.assert_allclose(f[rise], f[first])


@unittest.skipIf(ec is None, "numpy not installed")
class TestCorrection(unittest.TestCase):
    def test_power_target_converges(self):
        q, _ = ec.initial_profile(two_pulse(), 530)
        rs = iterate(q, 530, 6)
        errs = [max(r.rms_pct) for r in rs]
        self.assertGreater(errs[0], 8.0)
        self.assertLess(errs[-1], 3.0)

    def test_frequency_profile_converges(self):
        p = ec.Profile([0, 2000, 10000], [20000.0] * 3, None, None, "frequency")
        q, _ = ec.initial_profile(p, 500)
        rs = iterate(q, 500, 5)
        self.assertLess(rs[-1].rms_pct[0], 1.0)
        self.assertLess(rs[-1].rms_pct[0], rs[0].rms_pct[0])

    def test_step_limit_holds_every_shot(self):
        q, _ = ec.initial_profile(two_pulse(), 530)
        for r in iterate(q, 530, 4, max_step_hz=30.0):
            self.assertLessEqual(r.max_step, 30.0 + 5.0 + 0.5)          # + the setpoint fitting tolerance (no ceiling here)

    def test_voltage_ceiling(self):
        q, _ = ec.initial_profile(two_pulse(300), 530, max_kv=70)
        rs = iterate(q, 530, 5, max_kv=63.0, max_step_hz=200.0)
        self.assertTrue(any(r.n_vcap > 0 for r in rs))
        for r in rs:
            self.assertLessEqual(r.max_v_pred, 63.0 + 0.05)

    def test_floor_holds(self):
        q, _ = ec.initial_profile(two_pulse(300), 470)
        for r in iterate(q, 470, 4, max_step_hz=400.0, max_kv=90.0):
            c = r.profile.carrier[r.profile.carrier > 0]
            self.assertGreaterEqual(c.min(), ec.F_FLOOR_HZ)

    def test_next_bank_rescales(self):
        q, _ = ec.initial_profile(two_pulse(), 530)
        t, V, P = ec.simulate(q, 530)
        same = ec.compensate(q, t, V, P, 530)
        lower = ec.compensate(q, t, V, P, 530, next_bank_v=510)
        higher = ec.compensate(q, t, V, P, 530, next_bank_v=545)
        on = same.profile.carrier > 0
        self.assertTrue(np.all(lower.profile.carrier[on] < same.profile.carrier[on] + 0.2))
        self.assertTrue(np.all(higher.profile.carrier[on] >= same.profile.carrier[on] - 0.2))
        self.assertTrue(any("rescaled" in n for n in lower.notes))

    def test_wrong_shot_is_refused(self):
        q, _ = ec.initial_profile(two_pulse(), 530)
        one = commanded([(0, 20000), (10000, 19800)])
        t, V, P = ec.simulate(one, 530)
        with self.assertRaises(ec.CompError):
            ec.compensate(q, t, V, P, 530)                           # one measured pulse, two in the profile

    def test_ramped_frequency_demand_warns(self):
        rows = [(0, 20000), (2000, 20000), (10000, 19450)]
        p = commanded(rows, mode="frequency")
        t, V, P = ec.simulate(p, 500)
        r = ec.compensate(p, t, V, P, 500)
        self.assertTrue(any("power target instead" in n for n in r.notes))

    def test_target_names(self):
        self.assertTrue(ec.target_names_match("dummy-load", "Dummy_Load"))
        self.assertFalse(ec.target_names_match("vessel", "Dummy_Load"))
        self.assertIsNone(ec.target_names_match("vessel", None))


@unittest.skipIf(ec is None, "numpy not installed")
class TestLearning(unittest.TestCase):
    def setUp(self):
        ec.PLANT.reset()

    def tearDown(self):
        ec.PLANT.reset()

    def run_pairs(self, kappa_true, shots, learn=True, gain_fn=None, demand=100, bank=520):
        p, _ = ec.initial_profile(ec.picker_like([(0, demand), (15, demand)]), bank)
        prev, rs = None, []
        for i in range(shots):
            t, V, P = ec.simulate(p, bank, seed=i, kappa=kappa_true, gain_fn=gain_fn)
            r = ec.compensate(p, t, V, P, bank, prev=prev if learn else None)
            rs.append(r)
            prev = {"profile": p, "t_ms": t, "v_kv": V, "p_kw": P, "bank": bank}
            p = r.profile
        return rs

    def test_learning_converges_faster(self):
        slow = [r.rms_pct[0] for r in self.run_pairs(6e-4, 5, learn=False)]
        ec.PLANT.reset()
        fast = [r.rms_pct[0] for r in self.run_pairs(6e-4, 5, learn=True)]
        self.assertLess(fast[3], 0.5 * slow[3])

    def test_learned_kappa_moves_towards_the_truth(self):
        before = ec.PLANT.kappa.copy()
        self.run_pairs(2.5e-4, 5)
        seen = ec.PLANT.kw > 0
        self.assertTrue(seen.any(), "nothing was learned")
        err_before = np.abs(before[seen] - 2.5e-4).mean()
        err_after = np.abs(ec.PLANT.kappa[seen] - 2.5e-4).mean()
        self.assertLess(err_after, 0.6 * err_before)

    def test_guard_stops_lowering_at_the_gain_peak(self):
        # a supply whose output stops rising below 19.25 kHz; a demand it cannot reach pushes the tail down to there
        flat = lambda f: 1 + 4.4e-4 * (20000 - max(f, 19250))              # noqa: E731
        rs = self.run_pairs(4.4e-4, 6, gain_fn=flat, demand=220, bank=520)
        guarded = [r for r in rs if r.n_guard > 0]
        self.assertTrue(guarded, "the guard never tripped")
        r = guarded[0]
        self.assertTrue(any("gain peak" in n for n in r.notes))
        on = r.grid.on
        held = r.grid.ctrl & (r.w_meas >= 0.4) & (r.k_meas < 2e-4)
        self.assertTrue(np.all(r.f_new[held & on] >= r.f_cmd[held & on] - 0.1))   # nothing lowered where it tripped

    def test_rise_shaping_removes_the_overshoot_without_going_below_the_flat_top(self):
        kw = dict(p_decay=0.6, p_decay_ms=1.5, lag_ms=0.4)             # a gyrotron that overshoots like the real one
        res = {}
        for shape in (False, True):
            ec.PLANT.reset()
            p, _ = ec.initial_profile(ec.picker_like([(0, 120), (15, 120)]), 560)
            prev = None
            for i in range(7):
                t, V, P = ec.simulate(p, 560, seed=i, **kw)
                r = ec.compensate(p, t, V, P, 560, prev=prev, shape_rise=shape)
                prev = {"profile": p, "t_ms": t, "v_kv": V, "p_kw": P, "bank": 560}
                p = r.profile
                if shape:                                               # the rise never goes below the flat top's start
                    g = r.grid
                    start = r.f_new[np.nonzero(g.ctrl)[0][0]]
                    rise = g.on & (g.since < ec.SETTLE_MS)
                    self.assertTrue(np.all(r.f_new[rise] >= start - 0.5))
            res[shape] = r.overshoot_pct[0]
        self.assertGreater(res[False], 8.0)
        self.assertLess(res[True], 0.4 * res[False])

    def test_different_bank_learns_nothing(self):
        p, _ = ec.initial_profile(ec.picker_like([(0, 100), (10, 100)]), 520)
        t, V, P = ec.simulate(p, 520)
        r = ec.compensate(p, t, V, P, 520, prev={"profile": p, "t_ms": t, "v_kv": V, "p_kw": P, "bank": 510})
        self.assertEqual(r.learned["pairs"], 0)
        self.assertTrue(any("nothing learned" in n for n in r.notes))

    def test_plant_file_round_trip(self):
        self.run_pairs(3e-4, 3)
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, "ech_plant.json")
            ec.PLANT.save(f)
            k = ec.PLANT.kappa.copy()
            ec.PLANT.reset()
            ec.PLANT.load(f)
            np.testing.assert_allclose(ec.PLANT.kappa, k, rtol=1e-6)

    def test_gain_follows_the_learned_sensitivity(self):
        g1 = float(ec.gain(19000.0))
        ec.PLANT.kappa[:] = 4e-4
        ec.PLANT._build()
        self.assertGreater(float(ec.gain(19000.0)), g1)                 # more sensitivity -> more gain at 19 kHz
        self.assertAlmostEqual(float(ec.freq_for_gain(ec.gain(19321.0))), 19321.0, delta=1.0)


@unittest.skipIf(ec is None, "numpy not installed")
class TestFixedTimes(unittest.TestCase):
    def setUp(self):
        ec.PLANT.reset()

    def test_exactly_the_fixed_times_and_the_limits_hold(self):
        T = ec.parse_fixed_times("0:15:1")
        self.assertEqual(list(T), [i * 1000.0 for i in range(16)])
        dem = ec.picker_like([(0, 200), (15, 200)])
        p, _ = ec.initial_profile(dem, 545, fixed_times_us=T)
        self.assertEqual(list(p.t_us), list(T))
        prev = None
        for i in range(4):
            t, V, P = ec.simulate(p, 545, seed=i, p_decay=0.6, p_decay_ms=1.5, lag_ms=0.4)
            r = ec.compensate(p, t, V, P, 545, prev=prev, fixed_times_us=T, max_step_hz=60.0)
            q = r.profile
            self.assertEqual(list(q.t_us), list(T))
            ec.check_profile(q)
            self.assertLessEqual(q.max_dip_hz, ec.FIXED_DIP_TOL_HZ + 0.1)
            g = r.grid
            fq = ec.eval_rows(q.t_us, q.carrier, q.carrier > 0, g.tau)[0]
            self.assertTrue(np.all(fq[g.on] >= ec.F_FLOOR_HZ))
            self.assertLessEqual(float(np.nanmax((r.f_cmd - fq)[g.on])), 60.0 + ec.FIXED_DIP_TOL_HZ + 0.5)
            start = float(r.f_new[np.nonzero(g.ctrl)[0][0]])
            self.assertGreaterEqual(float(q.carrier[0]), start - 0.5)      # the pulse never starts below its flat top
            prev = {"profile": p, "t_ms": t, "v_kv": V, "p_kw": P, "bank": 545}
            p = q

    def test_a_demand_corner_off_the_grid_is_refused(self):
        dem = ec.picker_like([(0, 200), (7.5, 150), (15, 150)])
        with self.assertRaises(ec.CompError):
            ec.initial_profile(dem, 545, fixed_times_us=ec.parse_fixed_times("0:15:1"))


@unittest.skipIf(ec is None, "numpy not installed")
class TestCommandLine(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, SCRIPT, *args, "--no-plot"], capture_output=True, text=True)

    def test_picker_csv_through_two_iterations(self):
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "shot.csv")
            with open(csv, "w") as f:
                f.write("# tool=power_waveform_picker\n# mode=power\n# target=dummy-load\n# bank_volts=530\n"
                        "time_us,demand_kw\n0,200.0\n4000,200.0\n4010,off\n9000,200.0\n13000,200.0\n")
            out = self.run_cli(csv)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("kind=initial", out.stdout)
            it0 = os.path.join(d, "shot_iter0.csv")
            q = ec.read_profile(it0)
            ec.check_profile(q)
            self.assertEqual(q.meta["target"], "dummy-load")
            for n in (1, 2):
                prev = os.path.join(d, f"shot_iter{n - 1}.csv")
                t, V, P = ec.simulate(ec.read_profile(prev), 530, seed=n)
                data = os.path.join(d, "meas.csv")
                np.savetxt(data, np.column_stack([t, V, P]), delimiter=",", header="time_ms,gyrotron_kv,power_kw", comments="")
                out = self.run_cli(prev, "--data", data)
                self.assertEqual(out.returncode, 0, out.stderr)
                self.assertIn("kind=correction", out.stdout)
                self.assertTrue(os.path.exists(os.path.join(d, f"shot_iter{n}.csv")))

    def test_flatten_aims_at_the_measured_level(self):
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "shot.csv")
            with open(csv, "w") as f:
                f.write("# mode=power\n# target=dummy-load\n# bank_volts=530\ntime_us,demand_kw\n0,200.0\n12000,200.0\n")
            self.assertEqual(self.run_cli(csv).returncode, 0)
            it0 = os.path.join(d, "shot_iter0.csv")
            p0 = ec.read_profile(it0)
            t, V, P = ec.simulate(p0, 500, seed=3)                      # a lower bank than planned: well below 200 kW
            data = os.path.join(d, "meas.csv")
            np.savetxt(data, np.column_stack([t, V, P]), delimiter=",", header="time_ms,gyrotron_kv,power_kw", comments="")
            g = ec.Grid(p0, ec.SETTLE_MS)
            level = float(np.mean(ec.resample(t, P, g.tau / 1000.0)[g.ctrl]))
            self.assertLess(level, 190.0)
            out = self.run_cli(it0, "--data", data, "--bank-volts", "500", "--flatten")
            self.assertEqual(out.returncode, 0, out.stderr)
            q = ec.read_profile(os.path.join(d, "shot_iter1.csv"))
            dem = q.demand[np.isfinite(q.demand)]
            np.testing.assert_allclose(dem, level, rtol=1e-3)         # the level is carried in the demand itself
            self.assertAlmostEqual(float(q.meta["flatten_level"]), level, delta=0.1)

    def test_previous_bank_is_the_bank_it_ran_at(self):
        # the previous CSV was planned for 560 V but its shot ran at 530 V (recorded as analysed_bank_volts in the
        # next CSV): the pair must still be learned from
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "shot.csv")
            with open(csv, "w") as f:
                f.write("# mode=power\n# target=dummy-load\n# bank_volts=560\ntime_us,demand_kw\n0,200.0\n12000,200.0\n")
            self.assertEqual(self.run_cli(csv).returncode, 0)
            it0 = os.path.join(d, "shot_iter0.csv")
            meas = []
            for n, prof in enumerate((it0, os.path.join(d, "shot_iter1.csv"))):
                t, V, P = ec.simulate(ec.read_profile(prof), 530, seed=n)
                meas.append(os.path.join(d, f"meas{n}.csv"))
                np.savetxt(meas[-1], np.column_stack([t, V, P]), delimiter=",", header="time_ms,gyrotron_kv,power_kw",
                           comments="")
                args = [prof, "--data", meas[-1], "--bank-volts", "530", "--plant", os.path.join(d, "plant.json")]
                if n:
                    args += ["--prev-data", meas[0]]
                out = self.run_cli(*args)
                self.assertEqual(out.returncode, 0, out.stderr)
            self.assertNotIn("nothing learned from the pair", out.stderr)
            self.assertIn("learned_from_previous=yes", out.stdout)

    def test_power_scale_works_in_experimental_meter_units(self):
        K = 1.05
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "shot.csv")
            with open(csv, "w") as f:
                f.write("# mode=power\n# target=dummy-load\n# bank_volts=530\ntime_us,demand_kw\n0,200.0\n12000,200.0\n")
            self.assertEqual(self.run_cli(csv).returncode, 0)
            it0 = os.path.join(d, "shot_iter0.csv")
            t, V, P = ec.simulate(ec.read_profile(it0), 530, seed=5)
            meas, meas_v = os.path.join(d, "meas.csv"), os.path.join(d, "meas_vessel.csv")
            np.savetxt(meas, np.column_stack([t, V, P]), delimiter=",", header="time_ms,gyrotron_kv,power_kw", comments="")
            np.savetxt(meas_v, np.column_stack([t, V, P * K]), delimiter=",", header="time_ms,gyrotron_kv,power_kw",
                       comments="")
            # the same profile with its demand written in experimental-meter kW (200 kW on the dummy-load meter = 210)
            it0k = os.path.join(d, "shotk_iter0.csv")
            with open(it0) as f, open(it0k, "w") as g:
                g.write(f.read().replace(",200.00,", f",{200 * K:.2f},"))
            plant = os.path.join(d, "plant.json")
            common = ["--no-learn", "--plant", plant, "--bank-volts", "530"]
            runs = {"plain": [it0, "--data", meas], "scaled": [it0k, "--data", meas, "--power-scale", str(K)],
                    "vessel": [it0k, "--data", meas_v, "--power-scale", str(K), "--target", "vessel"]}
            prof = {}
            for name, args in runs.items():
                out_csv = os.path.join(d, f"{name}.csv")
                out = self.run_cli(*args, *common, "--out", out_csv)
                self.assertEqual(out.returncode, 0, out.stderr)
                if name != "plain":
                    self.assertIn("power_scale=1.0500", out.stdout)
                prof[name] = ec.read_profile(out_csv)
            for name in ("scaled", "vessel"):
                np.testing.assert_allclose(prof[name].carrier, prof["plain"].carrier, atol=0.15)
                on = np.isfinite(prof[name].demand)
                np.testing.assert_allclose(prof[name].demand[on], 200 * K, atol=0.01)   # written back in the CSV's units
                self.assertEqual(prof[name].meta["power_scale"], "1.0500")
            # the scale is carried to the next iteration without the option
            t2, V2, P2 = ec.simulate(prof["scaled"], 530, seed=6)
            np.savetxt(meas, np.column_stack([t2, V2, P2]), delimiter=",", header="time_ms,gyrotron_kv,power_kw", comments="")
            out = self.run_cli(os.path.join(d, "scaled.csv"), "--data", meas, *common)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("power_scale=1.0500", out.stdout)

    def test_power_scale_out_of_range_is_refused(self):
        with self.assertRaises(ec.CompError):
            ec.check_power_scale("5")
        self.assertEqual(ec.check_power_scale(None), 1.0)

    def test_target_is_required(self):
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "x.csv")
            with open(csv, "w") as f:
                f.write("time_us,demand_kw\n0,200\n10000,200\n")
            out = self.run_cli(csv, "--bank-volts", "530")
            self.assertEqual(out.returncode, 1)
            self.assertIn("--target", out.stderr)


if __name__ == "__main__":
    unittest.main()

"""The practice controller's 0 Hz (output-off gap) setpoints, firmware 2026-11-16: accepted like the firmware does,
and the simulated PFM capture shows the gap as one long period."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "operator_ui"))
import wham_sim as w  # noqa: E402


class SimGapTests(unittest.TestCase):
    def setUp(self):
        self.c = w.SimController()
        self.c.sim.mode = "HVPS"
        self.c.shot_begin([])

    def test_first_setpoint_cannot_be_off(self):
        self.assertTrue(self.c.shot_step(["0", "0", "0"]).startswith("ERR 4"))

    def test_shot_cannot_end_in_a_gap(self):
        for a in (["0", "20000", "50"], ["1000", "20000", "50"], ["2000", "0", "0"]):
            self.assertEqual(self.c.shot_step(a), "OK")
        self.assertTrue(self.c.shot_end([]).startswith("ERR 4"))

    def test_gap_is_one_long_period_in_the_capture(self):
        for a in (["0", "20000", "50"], ["1000", "0", "0"], ["3000", "25000", "50"], ["4000", "25000", "50"]):
            self.assertEqual(self.c.shot_step(a), "OK")
        self.assertEqual(self.c.shot_end([]), "OK 4 4000")
        s = self.c.sim
        s.pfm_arm, s.pfm_modes = 200, ["R"] * 6
        self.c.make_pfm()
        periods = s.pfm[1][1]
        long = [p for p in periods if p > 20000]
        self.assertEqual(len(long), 1)
        self.assertAlmostEqual(long[0] / 170.0, 2000 + 50, delta=60)        # the gap + one 20 kHz period, in us
        self.assertEqual(periods[0], 8500)
        self.assertEqual(periods[-1], 6800)


if __name__ == "__main__":
    unittest.main()

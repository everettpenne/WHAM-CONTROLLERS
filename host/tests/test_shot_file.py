"""Tests of operator_ui/shot_file.py: reading an HBRIDGE shot from a file or pasted text. Run: python3 -m unittest tests.test_shot_file"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "operator_ui"))
import shot_file as sf

ECH = """# written by ech_compensate.py
# iteration=3
# from_shot=261118012
# bank_volts=520
# power_scale=1.08
time_us,demand_kw,carrier_hz,duty_pct
0,200,20086.0,50
1000,200,20117.5,50
5000,0,0,0
6500,200,20200.0,50
15000,200,19018.0,50
"""

LIMITER = """time_us,carrier_hz,demand_duty_u_pct,demand_duty_v_pct,demand_duty_w_pct,commanded_duty_u_pct,commanded_duty_v_pct,commanded_duty_w_pct
0,100000,20,20,20,21.5,19.25,20
5000,100000,40,40,40,42,38,40.5
"""


class ParseTests(unittest.TestCase):
    def test_ech_profile_with_a_gap_and_its_notes(self):
        p = sf.parse(ECH)
        self.assertEqual(p["kind"], "ECH profile (ech_compensate.py)")
        self.assertEqual(len(p["rows"]), 5)
        self.assertEqual(p["rows"][1], {"t": 1.0, "f": 20.1175, "u": 50.0, "v": 50.0, "w": 50.0, "off": False})
        self.assertEqual(p["rows"][2], {"t": 5.0, "f": 0.0, "u": 0.0, "v": 0.0, "w": 0.0, "off": True})
        self.assertEqual(p["meta"]["iteration"], "3")
        self.assertTrue(any("output-off" in n for n in p["notes"]))
        self.assertTrue(any("power scale of 1.08" in n for n in p["notes"]))
        text = sf.describe(p, "ech_flat_iter3.csv")
        for part in ("ech_flat_iter3.csv", "5 setpoints, 15 ms", "iteration 3", "shot 261118012", "520 V"):
            self.assertIn(part, text)

    def test_limiter_profile_loads_the_commanded_duty_not_the_demand(self):
        p = sf.parse(LIMITER)
        self.assertEqual(p["kind"], "Limiter profile (limiter_compensate.py)")
        self.assertEqual((p["rows"][0]["u"], p["rows"][0]["v"], p["rows"][0]["w"]), (21.5, 19.25, 20.0))
        self.assertEqual(p["rows"][1]["f"], 100.0)
        self.assertEqual(p["notes"], [])

    def test_a_limiter_file_without_commanded_columns_loads_the_demand_and_says_so(self):
        p = sf.parse("time_us,carrier_hz,demand_duty_pct\n0,100000,20\n5000,100000,40\n")
        self.assertEqual(p["rows"][1]["w"], 40.0)
        self.assertTrue(any("DEMAND duty" in n for n in p["notes"]))

    def test_other_units_in_the_header(self):
        p = sf.parse("time_ms,freq_khz,duty_pct\n0,20,50\n2.5,20.5,55\n")
        self.assertEqual(p["rows"][1], {"t": 2.5, "f": 20.5, "u": 55.0, "v": 55.0, "w": 55.0, "off": False})
        p = sf.parse("t_s,f_hz,duty_u_pct,duty_v_pct,duty_w_pct\n0,20000,50,51,52\n0.01,21000,50,51,52\n")
        self.assertEqual((p["rows"][1]["t"], p["rows"][1]["f"], p["rows"][1]["v"]), (10.0, 21.0, 51.0))

    def test_shot_step_lines_as_in_a_logbook(self):
        text = "> SHOT:BEGin\nSHOT:STEP 0 20086 50\n  SHOT:STEP 1000 20117.5 50 -> OK\nshot:step 4000 0 0\nSHOT:STEP 8000 20000 40 50 60\nSHOT:STEP 9000 20000 40 50 60\nSHOT:END\n"
        p = sf.parse(text)
        self.assertEqual(p["kind"], "SHOT:STEP lines")
        self.assertEqual([r["t"] for r in p["rows"]], [0.0, 1.0, 4.0, 8.0, 9.0])
        self.assertTrue(p["rows"][2]["off"])
        self.assertEqual((p["rows"][3]["u"], p["rows"][3]["v"], p["rows"][3]["w"]), (40.0, 50.0, 60.0))

    def test_bare_rows_of_numbers_with_spaces_or_commas(self):
        for text in ("0 20000 50\n1000 20500 50\n", "0,20000,50\n1000,20500,50\n", "0\t20000\t50\r\n1000\t20500\t50\r\n"):
            p = sf.parse(text)
            self.assertEqual([(r["t"], r["f"]) for r in p["rows"]], [(0.0, 20.0), (1.0, 20.5)], text)

    def test_a_first_setpoint_that_is_not_at_zero_is_noted(self):
        p = sf.parse("100 20000 50\n1000 20500 50\n")
        self.assertTrue(any("needs it at 0" in n for n in p["notes"]))

    def test_what_is_refused_and_why(self):
        cases = [("", "empty"), ("   \n\n", "empty"), ("# only a comment\n", "no rows"),
                 ("hello world", "does not look like a shot"),
                 ("time_us,demand_kw\n0,200\n1000,200\n", "no frequencies yet"),
                 ("time_us,carrier_hz\n0,20000\n1000,20000\n", "no duty column"),
                 ("carrier_hz,duty_pct\n20000,50\n20000,50\n", "no time column"),
                 ("0 20000 50\n", "at least two"),
                 ("0 20000 50\n0 20000 50\n", "times must increase"),
                 ("0 20000 50\n1000 abc 50\n", "line 2: a value is not a number"),
                 ("0 20000 50\n1000 20000\n", "line 2"),
                 ("time_us,carrier_hz,duty_pct\n0,20000,50\n1000,x,50\n", "line 3: the frequency is not a number"),
                 ("0 20000 -5\n1000 20000 50\n", "negative"),
                 ("SHOT:STEP 0 20000\nSHOT:STEP 1000 20000 50\n", "one or three duties")]
        for text, want in cases:
            with self.assertRaises(ValueError, msg=text) as cm:
                sf.parse(text)
            self.assertIn(want, str(cm.exception), text)

    def test_too_many_setpoints_and_too_large_a_file(self):
        rows = "\n".join(f"{i * 100} 20000 50" for i in range(sf.MAX_ROWS + 1))
        with self.assertRaises(ValueError) as cm:
            sf.parse(rows)
        self.assertIn("at most 500", str(cm.exception))
        self.assertEqual(len(sf.parse("\n".join(f"{i * 100} 20000 50" for i in range(sf.MAX_ROWS)))["rows"]), 500)
        with self.assertRaises(ValueError) as cm:
            sf.parse("0 20000 50\n" * 60000)
        self.assertIn("too large", str(cm.exception))

    def test_the_example_profiles_in_the_repository_load(self):
        folder = os.path.join(HERE, "..", "..", "compensation", "examples")
        names = [n for n in sorted(os.listdir(folder)) if n.endswith(".csv")] if os.path.isdir(folder) else []
        loaded = 0
        for n in names:
            with open(os.path.join(folder, n)) as f:
                text = f.read()
            if "carrier_hz" not in text:
                continue                                    # a demand-only file: nothing to upload yet
            p = sf.parse(text)
            self.assertGreaterEqual(len(p["rows"]), 2, n)
            loaded += 1
        if names:
            self.assertGreater(loaded, 0, "no example profile with frequencies was found")


if __name__ == "__main__":
    unittest.main()

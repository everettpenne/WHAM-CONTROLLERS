"""shot_data.py: parsing the controllers' logs, the numbers, the files. Pure functions; no hardware."""
import json
import math
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "operator_ui"))
import shot_data as sd


def page(rows, start=0, count=None, rate=10000):
    count = len(rows) if count is None else count
    nums = [x for r in rows[start:start + 40] for x in r]
    return f"OK {count} {rate} SUM={sum(x & 0xFFFFFFFF for x in nums) & 0xFFFFFFFF} " + " ".join(map(str, nums))


def synthetic(product="xrex", n=200, rate=100.0, calib=True, lag=0.0):
    sp = [100000.0 * min(i, 100) / 100 for i in range(n)]
    pv = [(sp[max(0, i - int(lag))]) for i in range(n)]
    ch = {"n": 1, "name": sd.channel_name(product, 1), "units": {"sp": "Hz", "pv": "Hz", "out": "Hz"} if product == "xrex" else {"sp": "V", "pv": "V", "out": "%"},
          "sp": sp, "pv": pv, "out": [v + 5 for v in sp]}
    return {"version": 1, "product": product, "controller": {"serial": "WB-0001-8", "label": "t", "name": "n", "board": "B", "fw": "v1"},
            "started": "2026-10-30T10:11:12+00:00", "outcome": "completed", "rate_hz": rate, "decim": 10, "count": n, "duration_s": n / rate,
            "mode": None, "calib": {"turnon_hz": 5000.0, "max_hz": 100000.0, "max_amps": {"1": 6000.0}} if calib else {}, "meta": {}, "notes": [], "channels": [ch]}


class ParseTests(unittest.TestCase):
    def test_xrex_log(self):
        c, r, s = sd.parse_xrex_log("OK 2 333 1 2 3 4 5 6")
        self.assertEqual((c, r, s), (2, 333.0, [[1, 4], [2, 5], [3, 6]]))

    def test_xrex_short_reply_is_refused(self):
        with self.assertRaises(ValueError):
            sd.parse_xrex_log("OK 2 333 1 2 3 4 5")
        with self.assertRaises(ValueError):
            sd.parse_xrex_log("ERR 12 nope")

    def test_hbridge_page_checks_sum_and_length(self):
        rows = [(1, -2, 3), (4, 5, 6), (-7, 8, 9)]
        c, r, s = sd.parse_hbridge_page(page(rows), 0, 40)
        self.assertEqual((c, s[0], s[1]), (3, [1, 4, -7], [-2, 5, 8]))
        bad = page(rows).replace(" 5 ", " 6 ")
        with self.assertRaises(ValueError):
            sd.parse_hbridge_page(bad, 0, 40)                 # a character changed on the link
        with self.assertRaises(ValueError):
            sd.parse_hbridge_page(page(rows)[:-2], 0, 40)     # a number lost
        c, r, s = sd.parse_hbridge_page("OK 0 10000 SUM=0", 0, 40)
        self.assertEqual(c, 0)

    def test_decimation_covers_the_shot(self):
        decim, rate, note = sd.choose_decimation(3.0, 1000, 1000)
        self.assertLessEqual(3.0 * 1.0 * 1000 / decim, 1000)
        self.assertEqual(rate, 1000 / decim)
        self.assertIsNone(note)
        d, _, note = sd.choose_decimation(100000.0, 1000, 1000)
        self.assertEqual(d, 10000)
        self.assertIn("not recorded", note)
        self.assertEqual(sd.choose_decimation(0.001, 1000, 1000)[0], 1)


class RecordingTests(unittest.TestCase):
    def test_units_hbridge_hvps_and_limiter(self):
        raw = {1: [[1000, 2000], [900, 2100], [50_000_000, 60_000_000]]}
        hv = sd.build_recording("hbridge", {"serial": "S"}, "completed", 100.0, 100, raw, mode="HVPS")
        self.assertEqual(hv["channels"][0]["sp"], [1.0, 2.0])
        self.assertEqual(hv["channels"][0]["units"]["out"], "kHz")
        self.assertEqual(hv["channels"][0]["out"], [50.0, 60.0])        # milli-Hz -> kHz
        lim = sd.build_recording("hbridge", {"serial": "S"}, "completed", 100.0, 100, {1: [[0, 0], [0, 0], [50_000, 60_000]]}, mode="LIMITER")
        self.assertEqual(lim["channels"][0]["out"], [50.0, 60.0])       # milli-% -> %
        self.assertEqual(lim["channels"][0]["units"]["out"], "%")

    def test_amps_conversion_endpoints(self):
        rec = synthetic()
        self.assertAlmostEqual(sd.amps_from_hz(rec, 1, [5000.0])[0], 0.0)
        self.assertAlmostEqual(sd.amps_from_hz(rec, 1, [100000.0])[0], 6000.0)
        self.assertIsNone(sd.amps_from_hz(synthetic(calib=False), 1, [5000.0]))

    def test_stats_for_a_perfect_follow_and_for_a_lag(self):
        rows, raw = sd.stats(synthetic(lag=0), 1)
        self.assertAlmostEqual(raw["rms_error"], 0.0)
        self.assertAlmostEqual(raw["overshoot_pct"], 0.0)
        rows2, raw2 = sd.stats(synthetic(lag=5), 1)
        self.assertGreater(raw2["rms_error"], 1000)
        self.assertLess(raw2["mean_error"], 0)                           # measured behind a rising setpoint
        self.assertTrue(any("Flat top" in a for a, _ in rows))

    def test_diagnose_finds_a_flat_output_and_a_disabled_channel(self):
        flat = sd.build_recording("xrex", {"serial": "S"}, "completed", 100.0, 10,
                                  {1: [[5000 + 500 * i for i in range(100)], [3000] * 100, [3000] * 100], 2: [[0] * 100, [0] * 100, [0] * 100]},
                                  meta={"channels": {"1": {"in_shot": True, "loop": "closed", "gains_kp_ki_kd": [0, 0, 0]}, "2": {"in_shot": False}}})
        text = " ".join(flat["notes"])
        self.assertIn("did not follow the setpoint", text)
        self.assertIn("all PID gains zero", text)
        self.assertIn("Channel 2 was not enabled", text)
        good = sd.build_recording("xrex", {"serial": "S"}, "completed", 100.0, 10,
                                  {1: [[5000 + 500 * i for i in range(100)], [5000 + 500 * i for i in range(100)], [5000 + 500 * i for i in range(100)]]})
        self.assertEqual(good["notes"], [])
        stuck_fb = sd.build_recording("xrex", {"serial": "S"}, "completed", 100.0, 10,
                                      {1: [[5000 + 500 * i for i in range(100)], [3000] * 100, [5000 + 500 * i for i in range(100)]]})
        self.assertIn("feedback barely moved", " ".join(stuck_fb["notes"]))

    def test_svg_and_reports_are_well_formed(self):
        import xml.dom.minidom
        rec = synthetic()
        svg = sd.plot_svg(rec, 1, "A")
        xml.dom.minidom.parseString(svg)
        self.assertIn("<polyline", svg)
        hb = synthetic("hbridge")
        self.assertEqual(sd.plot_svg(hb, 1).count("<polyline"), 3)       # target+measured panel, output panel
        html = sd.to_html(rec, "A")
        self.assertIn("<svg", html)
        self.assertIn("Setpoint peak", sd.to_markdown(rec, "A"))
        csv = sd.to_csv(rec).splitlines()
        self.assertEqual(csv[0].split(",")[:4], ["t_s", "ch1_setpoint_Hz", "ch1_measured_Hz", "ch1_output_Hz"])
        self.assertIn("ch1_setpoint_A", csv[0])
        self.assertEqual(len(csv), 201)

    def test_save_load_list_and_path_safety(self):
        with tempfile.TemporaryDirectory() as root:
            rec = synthetic()
            sid = sd.save(root, rec)
            sid2 = sd.save(root, rec)
            self.assertNotEqual(sid, sid2)                                # never overwrites
            self.assertEqual(sd.load(root, sid)["count"], 200)
            self.assertEqual(len(sd.list_recordings(root, rec["controller"])), 2)
            self.assertTrue(sd.read_file(root, sid, "shot.csv").startswith(b"t_s,"))
            for bad in ("../x", "WB-0001-8/../../etc", "/etc/passwd", "WB-0001-8/20261030-101112/../x", "a/b"):
                with self.assertRaises(KeyError):
                    sd.load(root, bad)
            with self.assertRaises(KeyError):
                sd.read_file(root, sid, "../../secret")
            d = sd.describe(sd.load(root, sid), sid, "A")
            self.assertTrue(d["can_amps"])
            self.assertEqual(d["unit"], "A")
            json.dumps(d)

    def test_controller_dir_for_unassigned_board(self):
        self.assertTrue(sd.controller_dir({"serial": "UNSET", "name": "my board"}).startswith("unassigned-my-board"))
        self.assertEqual(sd.controller_dir({"serial": "WB-0001-8"}), "WB-0001-8")


if __name__ == "__main__":
    unittest.main()

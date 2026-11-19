"""host/operator_ui/pfm_capture.py: the PFMIN reply formats, the both-edge decoder, the numbers, and the programmed-shot comparison."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "operator_ui"))
import pfm_capture as pc


def data_reply(vals, start=0, count=100, total=None, overcap=0):
    page = vals[start:start + count]
    return f"OK {len(vals) if total is None else total} OVERCAP={overcap} SUM={sum(page) & 0xFFFFFFFF} " + " ".join(map(str, page))


def raw_values(cycles, t0=1000):
    """[t0, d1, d2 ...] for rising, falling, rising ...: high, low, high, low differences."""
    v = [t0]
    for per, high in cycles:
        v += [high, per - high]
    return v


class Pages(unittest.TestCase):
    def test_a_good_data_page_parses(self):
        vals = [1700] * 150
        self.assertEqual(pc.parse_data_page(data_reply(vals, 0), 0), (150, 0, vals[:100]))
        self.assertEqual(pc.parse_data_page(data_reply(vals, 100), 100), (150, 0, vals[100:]))

    def test_damage_is_detected_not_guessed(self):
        vals = [1700] * 20
        good = data_reply(vals)
        for bad in (good.replace("1700", "170", 1),                    # a lost digit: sum mismatch
                    good.rsplit(" ", 1)[0],                           # a lost number: wrong length
                    good.replace("SUM=", "SUN=", 1),                  # a damaged field name
                    "ERR 13 not in rising mode", "", "OK"):
            with self.assertRaises(ValueError, msg=bad):
                pc.parse_data_page(bad, 0)

    def test_raw_page_carries_the_level(self):
        v = raw_values([(1700, 680)] * 5)
        reply = f"OK {len(v)} OVERCAP=2 LEVEL=1 SUM={sum(v)} " + " ".join(map(str, v))
        total, oc, level, nums = pc.parse_raw_page(reply, 0)
        self.assertEqual((total, oc, level, nums), (len(v), 2, 1, v))

    def test_an_empty_capture_is_a_valid_page(self):
        self.assertEqual(pc.parse_data_page("OK 0 OVERCAP=0 SUM=0", 0), (0, 0, []))


class Decode(unittest.TestCase):
    def test_cycles_from_alternating_edges(self):
        cyc = pc.decode_cycles(0, raw_values([(1700, 680), (1700, 850), (1700, 1020)]))
        self.assertEqual(cyc, [(0, 1700, 680), (1700, 1700, 850), (3400, 1700, 1020)])

    def test_pin_high_before_the_capture_starts_with_a_falling_edge(self):
        # level 1: the first edge is falling, so the first complete cycle starts at the second edge
        v = [500, 300, 680, 1020, 680]                                # fall, rise, fall, rise, ... as differences
        cyc = pc.decode_cycles(1, v)
        self.assertEqual(cyc[0][0], 300)                              # first rising edge is edge index 1
        self.assertEqual(cyc[0][1:], (1700, 680))

    def test_build_input_duty_frequency_and_flags(self):
        inp = pc.build_input(1, "both", 0, raw_values([(1700, 340)] + [(1700, 850)] * 9 + [(3400, 850)]), 0)
        self.assertEqual(len(inp["t_s"]), 11)
        self.assertEqual(inp["flag"][0], "startup")
        self.assertEqual(inp["flag"][-1], "odd period")                # a merged cycle (an edge lost)
        self.assertAlmostEqual(inp["duty_pct"][1], 50.0)
        self.assertAlmostEqual(inp["freq_hz"][1], 100000.0)
        self.assertAlmostEqual(inp["t_s"][1], 1700 / 170e6)

    def test_rising_mode_has_no_duty(self):
        inp = pc.build_input(2, "rising", 0, [1700] * 10)
        self.assertIsNone(inp["duty_pct"])
        self.assertEqual(inp["flag"], [""] * 10)
        self.assertAlmostEqual(inp["freq_hz"][0], 100000.0)


class Programmed(unittest.TestCase):
    POINTS = [[0.0, 100000.0, 20.0, 50.0, 80.0], [10000.0, 100000.0, 80.0, 50.0, 20.0]]

    def test_points_from_shot_replies(self):
        self.assertEqual(pc.parse_points(["OK 0 100000 20", "OK 10000 100000 80 50 20", "ERR 12 no such setpoint", None]),
                         [[0.0, 100000.0, 20.0, 20.0, 20.0], [10000.0, 100000.0, 80.0, 50.0, 20.0]])

    def test_interpolation_holds_flat_outside(self):
        self.assertEqual(pc.interp([0, 1], [0, 10], 0.25), 2.5)
        self.assertEqual(pc.interp([0, 1], [0, 10], -5), 0)
        self.assertEqual(pc.interp([0, 1], [0, 10], 5), 10)
        self.assertIsNone(pc.interp([], [], 1))

    def test_phase_mapping_and_unwired_inputs(self):
        self.assertEqual(pc.programmed(self.POINTS, 3)[1], [80.0, 20.0])
        self.assertIsNone(pc.programmed(self.POINTS, 4))

    def test_error_against_the_programmed_duty(self):
        cycles = [(1700, round(1700 * (0.20 + 0.60 * i / 99 - 0.02))) for i in range(100)]      # a 20 -> 80 % ramp, 2 points low
        inp = pc.build_input(1, "both", 0, raw_values(cycles), 0)
        inp["t_s"] = [i * 1e-4 for i in range(len(inp["t_s"]))]                                   # spread over the 10 ms shot
        rows, raw = pc.input_stats(inp, self.POINTS)
        self.assertAlmostEqual(raw["duty_error_mean"], -2.0, delta=0.5)
        self.assertTrue(any(k == "Duty, measured minus programmed" for k, _ in rows))
        rows2, raw2 = pc.input_stats(inp, None)
        self.assertNotIn("duty_error_mean", raw2)

    def test_csv_has_one_row_per_cycle_and_the_overlay(self):
        inp = pc.build_input(1, "both", 0, raw_values([(1700, 850)] * 4), 0)
        csv_text = pc.to_csv({"inputs": [inp], "programmed": self.POINTS})
        lines = csv_text.strip().split("\n")
        self.assertEqual(len(lines), 1 + 4)
        self.assertTrue(lines[0].startswith("input,mode,cycle,t_s"))
        self.assertIn("startup", lines[1])


class Sizing(unittest.TestCase):
    def test_a_short_shot_is_covered(self):
        m, note = pc.auto_cycles(0.010, 100000)
        self.assertGreaterEqual(m, 1000)
        self.assertLess(m, 1100)
        self.assertIsNone(note)

    def test_a_long_shot_is_capped_with_an_explanation(self):
        m, note = pc.auto_cycles(0.100, 125000)
        self.assertEqual(m, pc.MAX_CYCLES)
        self.assertIn("30.4 ms", note)


if __name__ == "__main__":
    unittest.main()

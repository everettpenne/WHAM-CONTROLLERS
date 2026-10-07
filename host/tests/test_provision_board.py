"""Unit tests for the pure parts of provision_board.py: serial numbers and the registry."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import provision_board as pb


class SerialTests(unittest.TestCase):
    def test_luhn_known_values(self):
        self.assertEqual(pb.luhn_digit("7992739871"), "3")     # the classic Luhn example
        self.assertEqual(pb.luhn_digit("0001"), "8")

    def test_make_and_check(self):
        for n in (1, 7, 42, 9999):
            s = pb.make_serial(n)
            self.assertTrue(pb.serial_ok(s), s)
            self.assertEqual(pb.serial_number(s), n)
            self.assertTrue(len(s) <= 16)

    def test_check_digit_catches_a_typo_and_a_swap(self):
        s = pb.make_serial(1234)                      # WB-1234-4 style
        body, chk = s[3:7], s[8]
        bad = f"WB-{body[:3]}{(int(body[3]) + 1) % 10}-{chk}"
        self.assertFalse(pb.serial_ok(bad))
        swapped = f"WB-{body[1]}{body[0]}{body[2:]}-{chk}"
        if swapped != s:
            self.assertFalse(pb.serial_ok(swapped) and body[0] != body[1])

    def test_format_rules_match_the_firmware(self):
        for good in ("ABC", "TEST-0001", "0123456789ABCDEF"):
            self.assertTrue(pb.serial_ok(good))
        for bad in ("AB", "wb-0001", "WB 0001", "UNSET", "0123456789ABCDEFG", ""):
            self.assertFalse(pb.serial_ok(bad))


class RegistryTests(unittest.TestCase):
    def test_next_serial_and_lookup(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "devices.csv")
            self.assertEqual(pb.load_registry(path), [])
            self.assertEqual(pb.next_serial([]), pb.make_serial(1))
            pb.append_registry(path, {"serial": pb.make_serial(1), "uid": "AA", "flashuid": "BB", "product": "P"})
            pb.append_registry(path, {"serial": "CUSTOM-9", "uid": "CC", "flashuid": "DD"})
            rows = pb.load_registry(path)
            self.assertEqual(len(rows), 2)
            self.assertEqual(pb.next_serial(rows), pb.make_serial(2))    # non-WB serials do not count
            self.assertEqual(pb.find_by_hardware(rows, "AA", "BB")[0]["serial"], pb.make_serial(1))
            self.assertEqual(pb.find_by_hardware(rows, "AA", "XX"), [])

    def test_parse_id(self):
        d = pb.parse_id("OK serial=WB-0001-8 uid=AB flashuid=CD product=X hw=REVA fw=v1 commit=abc-dirty label=- verified=1 identity=ok")
        self.assertEqual(d["serial"], "WB-0001-8")
        self.assertEqual(d["identity"], "ok")
        with self.assertRaises(ValueError):
            pb.parse_id("ERR 1 Unknown command")


if __name__ == "__main__":
    unittest.main()

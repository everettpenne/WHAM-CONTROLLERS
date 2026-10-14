"""dangerous_commands.py: matching rules, the argument rules, the real command tables, and the console's older list."""
import importlib.util
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dangerous_commands as dc

FIRMWARE = os.path.join(HERE, "..", "..", "firmware")


def table_patterns(product):
    path = os.path.join(FIRMWARE, product, "src", "app", "commands", "command_table.c")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return re.findall(r'^\s*\{\s*"([^"]+)"\s*,', f.read(), re.M)


class MatchingTests(unittest.TestCase):
    def test_short_and_long_forms_like_the_firmware(self):
        for cmd in ("SOURce:RUN", "SOUR:RUN", "sour:run", "SOURCE:RUN", ":SOUR:RUN", "Source:Run"):
            self.assertEqual(dc.classify(cmd).pattern, "SOURce:RUN", cmd)
        for cmd in ("SOURc:RUN", "SOURCE:RU", "SOU:RUN", "SOURce", "SOURce:RUN:X"):
            self.assertIsNone(dc.classify(cmd), cmd)

    def test_queries_are_never_dangerous(self):
        for cmd in ("FIRE?", "ARM?", "SHOT:STARt?", "EXTernal:ENAble?", "SYS:ID?", "STATE?", "*IDN?"):
            self.assertIsNone(dc.classify(cmd), cmd)

    def test_plain_commands(self):
        self.assertEqual(dc.classify("FIRE").category, "output")
        self.assertEqual(dc.classify("fire").category, "output")
        self.assertEqual(dc.classify("ARM").category, "arm")
        self.assertEqual(dc.classify("BOOT").category, "firmware")
        self.assertEqual(dc.classify("SHOT:STAR").category, "output")
        self.assertEqual(dc.classify("FAULT:CLE").category, "interlock")
        self.assertEqual(dc.classify("SYS:ID:CLE CONFIRM").category, "identity")
        self.assertEqual(dc.classify("GENERAL:TEST:FAULT").category, "fault-inject")

    def test_the_safe_direction_is_not_listed(self):
        for cmd in ("SOURce:ENAble 1 0", "XREX:CHANnel:ENAOut 2 0", "PHASe:ENAble UW 0", "DAC:OUTPut 1 0",
                    "DAC:OUTPut 2 0.0", "DEBUG:FAULT:BYPASS 0", "EXTernal:ENAble 1", "DIAG:GPO 0"):
            self.assertIsNone(dc.classify(cmd), cmd)
        for cmd in ("SOURce:ENAble 1 1", "SOUR:ENA 3 1", "XREX:CHANnel:ENAOut 2 1", "XREX:CHAN:CONT 1 1", "PHASe:ENAble UW 1",
                    "DAC:OUTPut 1 2.5", "DEBUG:FAULT:BYPASS 1", "EXTernal:ENAble 0", "EXT:ENA 0", "DIAGnostic:GPOut11 1"):
            self.assertIsNotNone(dc.classify(cmd), cmd)

    def test_missing_or_garbled_direction_counts_as_dangerous(self):
        for cmd in ("SOURce:ENAble", "SOURce:ENAble 1 on", "EXTernal:ENAble", "EXTernal:ENAble off", "DAC:OUTPut 1 x"):
            self.assertIsNotNone(dc.classify(cmd), cmd)

    def test_firmware_update_is_announced_once_not_per_data_line(self):
        for cmd in ("FWUPdate:BEGin 94752 DEADBEEF", "FWUPdate:SWAP", "FWUPdate:ROLLback", "FWUP:SWAP"):
            self.assertEqual(dc.classify(cmd).category, "firmware", cmd)
        for cmd in ("FWUPdate:DATA 0 AABB", "FWUPdate:END", "FWUPdate:ABORt", "FWUPdate:STATus?"):
            self.assertIsNone(dc.classify(cmd), cmd)

    def test_ordinary_commands_are_not_flagged(self):
        for cmd in ("STATE?", "DISARM", "SOURce:STOP", "SOURce:SETpoint 1 500", "PID:GAINS 1 1 0 0", "*SAV 3", "*RCL 3",
                    "LOG:DATA?", "SYS:LABel x", "TABle:BEGin 10", "CONFig:PROFile:CLEar 2", "SHOT:BEGin", "QSPI:SELFtest"):
            self.assertIsNone(dc.classify(cmd), cmd)


class RealTableTests(unittest.TestCase):
    def test_every_rule_names_a_real_command(self):
        tables = [t for t in (table_patterns("xrex"), table_patterns("hbridge")) if t]
        if not tables:
            self.skipTest("firmware sources not next to host/")
        known = set().union(*tables)
        for pattern, _, _ in dc.RULES:
            self.assertIn(pattern, known, f"{pattern} is in no command_table.c (a typo, or a retired command)")

    def test_a_rule_matches_its_own_table_entry(self):
        for pattern, when, _ in dc.RULES:
            probe = pattern + (" 1 1" if when == dc.ON else " 1 0" if when == dc.OFF else "")
            self.assertEqual(dc.classify(probe).pattern, pattern)


class ConsoleListTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("serial"), "pyserial is not installed (wham_console imports it)")
    def test_superset_of_the_consoles_older_list(self):
        sys.path.insert(0, os.path.join(FIRMWARE, "xrex", "python"))
        import wham_console
        corpus = ["FIRE", "BOOT", "SOURce:RUN", "sour:run", "SOUR:ENAble 1 1", "SOURce:ENAble 2 1", "SHOT:STARt", "shot:star",
                  "SOURce:ENAble 1 0", "STATE?", "ARM", "FAULT?", "SOURce:STOP", "PID:GAINS 1 1 1 1", "SHOT:TIMing 1 2"]
        for cmd in corpus:
            if wham_console._is_dangerous(cmd):
                self.assertTrue(dc.is_dangerous(cmd), f"the console flags {cmd!r} but dangerous_commands does not")


if __name__ == "__main__":
    unittest.main()

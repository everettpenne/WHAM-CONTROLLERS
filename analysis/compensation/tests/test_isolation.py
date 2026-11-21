"""The compensation tools talk to MDSplus (read only) and the terminal that runs them, and to nothing else: no
controller, no hub, no other network service, no other program. This test keeps it that way.

    python3 -m unittest discover -s analysis/compensation/tests
"""
import ast
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = ["ech_compensate.py", "ech_meter_calibration.py", "limiter_compensate.py"]

# every module a tool may import (top-level name). Anything that can reach another machine or start a program --
# socket, ssl, http, urllib, requests, ftplib, telnetlib, smtplib, xmlrpc, asyncio, subprocess, multiprocessing,
# serial, paramiko, websocket, ... -- is deliberately absent.
ALLOWED = {"argparse", "math", "os", "re", "sys", "json", "numpy", "MDSplus", "matplotlib"}
FORBIDDEN_CALLS = {"system", "popen", "spawnl", "spawnv", "execv", "execl", "startfile", "urlopen", "create_connection"}


def source(name):
    with open(os.path.join(HERE, "..", name)) as f:
        return f.read()


class TestIsolation(unittest.TestCase):
    def test_imports_are_only_mds_numpy_matplotlib_and_the_standard_basics(self):
        for name in TOOLS:
            tree = ast.parse(source(name))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    mods = [a.name for a in node.names]
                elif isinstance(node, ast.ImportFrom):
                    mods = [node.module or ""]
                else:
                    continue
                for m in mods:
                    self.assertIn(m.split(".")[0], ALLOWED, f"{name} imports {m}")

    def test_no_dynamic_imports_or_program_starts(self):
        for name in TOOLS:
            tree = ast.parse(source(name))
            for node in ast.walk(tree):
                if isinstance(node, ast.Call):
                    f = node.func
                    called = f.attr if isinstance(f, ast.Attribute) else (f.id if isinstance(f, ast.Name) else "")
                    self.assertNotIn(called, FORBIDDEN_CALLS, f"{name} calls {called}()")
                    self.assertNotEqual(called, "__import__", f"{name} imports dynamically")
                    self.assertNotEqual(called, "import_module", f"{name} imports dynamically")

    def test_mds_is_opened_read_only(self):
        opened = 0
        for name in TOOLS:
            for node in ast.walk(ast.parse(source(name))):
                if not isinstance(node, ast.Call):
                    continue
                f = node.func
                called = f.attr if isinstance(f, ast.Attribute) else (f.id if isinstance(f, ast.Name) else "")
                if called != "Tree":
                    continue
                opened += 1
                modes = [a.value for a in node.args + [k.value for k in node.keywords] if isinstance(a, ast.Constant)]
                self.assertIn("readonly", [str(m).lower() for m in modes],
                              f"{name} line {node.lineno}: an MDS tree opened without 'readonly'")
        self.assertGreater(opened, 0)


if __name__ == "__main__":
    unittest.main()

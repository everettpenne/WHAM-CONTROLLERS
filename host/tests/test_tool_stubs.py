"""The four tools hoisted from firmware/*/python to host/ (2026-11-20) left stubs at the old paths: each stub must be the
same in both products and must run the host/ tool with the caller's arguments."""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
TOOLS = ["net_client", "net_broker", "net_terminal", "dsl_viewer"]
PRODUCTS = ["hbridge", "xrex"]


def run(path, *args):
    return subprocess.run([sys.executable, path, *args], capture_output=True, text=True, timeout=30)


class ToolStubTests(unittest.TestCase):
    def test_the_tool_is_in_host_and_the_stubs_match(self):
        for t in TOOLS:
            self.assertTrue(os.path.isfile(os.path.join(ROOT, "host", t + ".py")), t)
            stubs = [open(os.path.join(ROOT, "firmware", p, "python", t + ".py")).read() for p in PRODUCTS]
            self.assertEqual(stubs[0], stubs[1], t)
            self.assertIn("runpy", stubs[0], t)
            self.assertLess(len(stubs[0].splitlines()), 15, t)

    def test_a_stub_gives_the_same_help_as_the_tool(self):
        for t in ("net_client", "net_broker", "dsl_viewer"):          # net_terminal has no --help
            want = run(os.path.join(ROOT, "host", t + ".py"), "--help")
            self.assertEqual(want.returncode, 0, t)
            for p in PRODUCTS:
                got = run(os.path.join(ROOT, "firmware", p, "python", t + ".py"), "--help")
                self.assertEqual(got.returncode, 0, (t, p, got.stderr))
                self.assertEqual(got.stdout.split("\n", 1)[1:], want.stdout.split("\n", 1)[1:], (t, p))

    def test_a_stub_that_cannot_find_host_says_where_the_tool_is(self):
        import shutil, tempfile
        with tempfile.TemporaryDirectory() as tmp:
            lone = os.path.join(tmp, "net_client.py")
            shutil.copy(os.path.join(ROOT, "firmware", "xrex", "python", "net_client.py"), lone)
            r = run(lone, "--help")
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("host/net_client.py", r.stderr)


if __name__ == "__main__":
    unittest.main()

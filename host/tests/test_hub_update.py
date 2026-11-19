"""Tests of host/deploy/hub_update.py (the server's self-update) and of the hub's .local.yaml site settings.

Real git repositories in a temporary folder stand in for GitHub and the server's checkout; a fake stands in for systemd and
ps; the quiet check runs against a real hub (wham_hub.Hub) and the fake controller of test_wham_hub."""
import asyncio
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "deploy"))
import hub_update as hu
import wham_hub as wh
from test_wham_hub import Client, FakeController, HubBase

STUB_HUB = '''import sys
cfg = sys.argv[sys.argv.index("--config") + 1]
text = open(cfg).read()
sys.exit(1 if "broken" in text else 0)
'''
PASSING = "import unittest\nclass T(unittest.TestCase):\n    def test_ok(self):\n        pass\n"
FAILING = "import unittest\nclass T(unittest.TestCase):\n    def test_bad(self):\n        self.fail('deliberately broken')\n"


def git(cwd, *args):
    r = subprocess.run(["git", "-C", cwd] + list(args), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


class FakeServices:
    """Two hubs, a and b. stop() 'restarts' a hub the way systemd would: a new pid, started now."""

    def __init__(self, started, come_back=True):
        self.pids = {"a": 101, "b": 102}
        self.start = {101: started, 102: started}
        self.stopped = []
        self.come_back = come_back

    def names(self):
        return ["a", "b"]

    def pid(self, name):
        return self.pids[name]

    def started(self, pid):
        return self.start.get(pid)

    def stop(self, pid):
        self.stopped.append(pid)
        name = next(n for n, p in self.pids.items() if p == pid)
        if self.come_back:
            self.pids[name] = pid + 1000
            self.start[pid + 1000] = time.time()

    def port(self, name):
        return {"a": 6001, "b": 6002}[name]


class UpdaterBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        t = self.tmp.name
        self.origin = os.path.join(t, "origin.git")
        self.work = os.path.join(t, "work")          # where commits are made (the developer)
        self.live = os.path.join(t, "live")          # the server's checkout
        git(t, "init", "--quiet", "--bare", self.origin)
        git(t, "init", "--quiet", "-b", "main", self.work)
        git(self.work, "config", "user.email", "t@example.invalid")
        git(self.work, "config", "user.name", "t")
        self.v1 = self.commit({"host/wham_hub.py": STUB_HUB, "host/tests/__init__.py": "", "host/tests/test_dummy.py": PASSING,
                               "host/config/a.yaml": "name: a\nallow_from: [127.0.0.1]\n", "host/config/b.yaml": "name: b\n"}, "v1")
        git(self.work, "remote", "add", "origin", self.origin)
        git(self.work, "push", "--quiet", "origin", "main", "main:deploy")
        git(t, "clone", "--quiet", "-b", "main", self.origin, self.live)
        self.state = os.path.join(t, "state")
        self.lines = []
        self.svc = FakeServices(started=time.time() - 3600)
        self.health = ["# hub name=a target=x:5000 connected=1 serial=WB-0001-8"]

    def commit(self, files, msg):
        for path, text in files.items():
            full = os.path.join(self.work, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w") as f:
                f.write(text)
        git(self.work, "add", "-A")
        git(self.work, "commit", "--quiet", "-m", msg)
        return git(self.work, "rev-parse", "HEAD")

    def release(self, files, msg):
        sha = self.commit(files, msg)
        git(self.work, "push", "--quiet", "origin", "HEAD:deploy")
        return sha

    def updater(self, quiet=(True, "nobody"), **kw):
        ask = lambda port, lines, want, timeout=8.0: self.health
        return hu.Updater(repo=self.live, state_dir=self.state, services=self.svc, tests=["tests.test_dummy"], health_s=2.0,
                          quiet_fn=lambda port: quiet, ask_fn=ask, out=self.lines.append, **kw)

    def text(self):
        return "\n".join(self.lines)


class UpdateTests(UpdaterBase):
    def test_nothing_to_do_when_the_checkout_is_already_at_deploy(self):
        u = self.updater()
        u.update()
        self.assertIn("up to date", self.text())
        self.assertEqual(self.svc.stopped, [])

    def test_a_push_to_main_alone_changes_nothing(self):
        self.commit({"host/wham_hub.py": STUB_HUB + "# newer\n"}, "work in progress")
        git(self.work, "push", "--quiet", "origin", "main")
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), self.v1)
        self.assertIn("up to date", self.text())

    def test_a_tested_release_is_switched_to_and_the_hubs_restarted_one_by_one(self):
        v2 = self.release({"host/wham_hub.py": STUB_HUB + "# v2\n"}, "v2")
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), v2)
        self.assertEqual(git(self.live, "rev-parse", "--abbrev-ref", "HEAD"), "deploy")
        st = u.load()
        self.assertEqual((st["previous"], st["current"]), (self.v1, v2))
        self.assertEqual(self.svc.stopped, [101, 102])
        self.assertIn("tests passed", self.text())
        self.assertIn("a: back up, controller connected=1 serial=WB-0001-8", self.text())
        self.lines.clear()
        u.restart()                                   # both run the new code now: nothing pending
        self.assertEqual(self.svc.stopped, [101, 102])
        self.assertEqual(u.pending(), [])

    def test_site_settings_and_local_edits_survive_an_update(self):
        with open(os.path.join(self.live, "host", "config", "a.local.yaml"), "w") as f:
            f.write("allow_from: [127.0.0.1, 192.0.2.7]\n")
        with open(os.path.join(self.live, "host", "config", "b.yaml"), "a") as f:
            f.write("expect_serial: WB-0002-6\n")
        v2 = self.release({"host/wham_hub.py": STUB_HUB + "# v2\n"}, "v2")
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), v2)
        self.assertTrue(os.path.exists(os.path.join(self.live, "host", "config", "a.local.yaml")))
        self.assertIn("WB-0002-6", open(os.path.join(self.live, "host", "config", "b.yaml")).read())

    def test_a_local_edit_that_would_be_overwritten_stops_the_switch(self):
        with open(os.path.join(self.live, "host", "config", "b.yaml"), "a") as f:
            f.write("expect_serial: WB-0002-6\n")
        self.release({"host/config/b.yaml": "name: b\nport: 5000\n"}, "v2 changes b.yaml")
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), self.v1)
        self.assertIn("could not switch", self.text())
        self.assertEqual(self.svc.stopped, [])

    def test_failing_tests_keep_the_old_code_and_the_commit_is_not_retried(self):
        bad = self.release({"host/tests/test_dummy.py": FAILING}, "breaks a test")
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), self.v1)
        self.assertIn("NOT updating", self.text())
        self.assertIn("deliberately broken", self.text())
        self.assertIn(bad, u.load()["bad"])
        self.lines.clear()
        u.update()
        self.assertIn("failed before", self.text())
        self.assertNotIn("testing it", self.text())
        fixed = self.release({"host/tests/test_dummy.py": PASSING}, "fixed")
        u.update()
        self.assertEqual(u.head(), fixed)

    def test_a_config_the_new_code_refuses_keeps_the_old_code(self):
        self.release({"host/config/a.yaml": "name: a\nbroken: yes\n"}, "bad config")
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), self.v1)
        self.assertIn("--check failed", self.text())

    def test_a_busy_hub_waits_and_is_restarted_later(self):
        self.release({"host/wham_hub.py": STUB_HUB + "# v2\n"}, "v2")
        u = self.updater(quiet=(False, "1 other client(s) connected (labview@192.0.2.5)"))
        u.update()
        self.assertEqual(self.svc.stopped, [])
        self.assertIn("restart waits: 1 other client(s) connected (labview@192.0.2.5)", self.text())
        self.assertEqual([n for n, _ in u.pending()], ["a", "b"])
        u2 = self.updater()
        u2.restart()
        self.assertEqual(self.svc.stopped, [101, 102])

    def test_a_hub_that_does_not_come_back_rolls_the_checkout_back(self):
        v2 = self.release({"host/wham_hub.py": STUB_HUB + "# v2\n"}, "v2")
        self.health = None                            # the restarted hub never answers
        u = self.updater()
        u.update()
        self.assertEqual(u.head(), self.v1)
        self.assertIn("ROLLED BACK", self.text())
        self.assertIn(v2, u.load()["bad"])
        self.assertEqual(self.svc.stopped, [101], "the second hub is not touched once the first failed")

    def test_rollback_by_hand(self):
        v2 = self.release({"host/wham_hub.py": STUB_HUB + "# v2\n"}, "v2")
        u = self.updater()
        u.update()
        self.assertTrue(u.rollback())
        self.assertEqual(u.head(), self.v1)
        self.assertIn(v2, u.load()["bad"])
        self.assertFalse(u.rollback(), "only one step back is recorded")

    def test_status_names_pending_hubs_and_local_edits(self):
        self.release({"host/wham_hub.py": STUB_HUB + "# v2\n"}, "v2")
        u = self.updater(quiet=(False, "busy"))
        u.update()
        with open(os.path.join(self.live, "host", "config", "b.yaml"), "a") as f:
            f.write("expect_serial: WB-0002-6\n")
        self.lines.clear()
        u.status()
        self.assertIn("restart pending", self.text())
        self.assertIn("local edits to tracked files", self.text())

    def test_the_cron_lines(self):
        lines = hu.cron_lines("/usr/bin/python3", "/opt/x/host/deploy/hub_update.py", "/var/lib/wham/hub/update")
        self.assertEqual(lines[0].split()[:5], ["30", "3", "*", "*", "*"])
        self.assertIn(" update >> /var/lib/wham/hub/update/cron.log 2>&1", lines[0])
        self.assertEqual(lines[1].split()[:5], ["*/15", "*", "*", "*", "*"])
        self.assertTrue(all(l.endswith(hu.CRON_MARK) for l in lines))


class LocalSettingsTests(unittest.TestCase):
    def test_a_local_file_overrides_the_tracked_config(self):
        with tempfile.TemporaryDirectory() as d:
            cfg = os.path.join(d, "ech.yaml")
            with open(cfg, "w") as f:
                f.write("name: ech\nhost: 192.0.2.1\nlisten_port: 5103\nallow_from: [127.0.0.1]\n")
            self.assertNotIn("192.0.2.7", str(wh.load_config(cfg)["allow_from"]))
            with open(os.path.join(d, "ech.local.yaml"), "w") as f:
                f.write("allow_from: [127.0.0.1, 192.0.2.7]\nexpect_serial: WB-0001-8\n")
            c = wh.load_config(cfg)
            self.assertIn("192.0.2.7", str(c["allow_from"]))
            self.assertEqual(c["expect_serial"], "WB-0001-8")
            self.assertEqual(c["listen_port"], 5103)
            self.assertEqual(wh.local_config_path(cfg), os.path.join(d, "ech.local.yaml"))


class ArmedController(FakeController):
    async def answer(self, cmd, writer):
        if cmd == "STATE?":
            self.say(writer, "OK ARMED\r\n")
        else:
            await super().answer(cmd, writer)


class QuietCheckTests(HubBase):
    """hub_update.quiet() against a real hub."""

    async def test_only_the_updater_and_an_idle_controller_is_quiet(self):
        await self.start_hub()
        ok, why = await asyncio.to_thread(hu.quiet, self.listen)
        self.assertTrue(ok, why)
        self.assertIn("OK IDLE", why)

    async def test_another_client_makes_it_wait(self):
        await self.start_hub()
        await Client.connect(self.listen, "labview", quiet=True)
        await asyncio.sleep(0.2)
        ok, why = await asyncio.to_thread(hu.quiet, self.listen)
        self.assertFalse(ok)
        self.assertIn("labview", why)

    async def test_an_armed_controller_makes_it_wait(self):
        await self.ctl.stop()
        self.ctl = ArmedController()
        self.ctl_port = await self.ctl.start()
        await self.start_hub()
        ok, why = await asyncio.to_thread(hu.quiet, self.listen)
        self.assertFalse(ok)
        self.assertIn("ARMED", why)

    async def test_no_hub_listening(self):
        ok, why = await asyncio.to_thread(hu.quiet, self.listen)
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()

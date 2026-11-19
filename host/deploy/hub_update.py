#!/usr/bin/env python3
"""hub_update.py -- keep the lab server's hub services (wham_hub.py) on the repository's `deploy` branch.

Runs on the server, as the account the hubs run as (whamdata), from that account's crontab:

    30 3 * * *    hub_update.py update      once a day: fetch the deploy branch, test it, switch the checkout to it
    */15 * * * *  hub_update.py restart     restart a hub that runs older code, but only when nobody is using it

`hub_update.py install-cron` writes those two lines (and `uninstall-cron` removes them); `status` shows where things stand;
`rollback` goes back to the commit before the last update.

What moves the server: only the `deploy` branch on GitHub. Pushing to main does nothing here; to release, move deploy to the
commit you mean (`git push origin <commit>:deploy`). Anyone who can push to the repository therefore decides what runs on
the server the next morning.

update
  1. fetch origin/deploy. Nothing to do when the checkout is already there, or when that commit failed before (it is kept
     in a list of bad commits until deploy moves on).
  2. test it BEFORE touching the live checkout: the commit is unpacked into a temporary folder, this site's
     host/config/*.local.yaml files are copied in, and there (a) the hub tests run (test_wham_hub, test_dangerous_commands,
     test_hub_update) and (b) every running hub's config is loaded with the new code (wham_hub.py --check). Any failure:
     logged, the commit is marked bad, the checkout is left alone.
  3. switch: `git checkout -B deploy <commit>`. Local edits to tracked files are carried over; one that would be
     overwritten stops the switch (logged) -- keep site settings in the .local.yaml files instead.
  4. then the restart step below.
restart
  A hub whose process started before the checkout last changed runs old code. It is restarted only when it is quiet: the
  updater connects to it as a quiet client and asks `!hub status` and `STATE?`; it goes ahead only when it is the only
  client and the controller is not ARMED or FIRING (IDLE, FAULT, or not connected). Otherwise it waits for the next run.
  The restart is a SIGTERM to the hub process (same account, no sudo); systemd (Restart=always) starts it again on the new
  code. Restarting a hub never touches its controller: a running shot carries on; only the hub's connections drop.
  Afterwards the hub must listen and answer `!hub status` within 60 s; if it does not, the checkout goes back to the
  previous commit (systemd's next restart then runs that), the commit is marked bad, and the hubs already restarted are
  restarted back the same quiet way.

Everything is logged to <state-dir>/update.log (default /var/lib/wham/hub/update). Standard library only; needs git,
systemctl and ps on the PATH. Does not need or use sudo.
"""
import argparse
import datetime
import fcntl
import glob
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TESTS = ["tests.test_wham_hub", "tests.test_dangerous_commands", "tests.test_hub_update"]
# what a test of a commit needs from the tree: the host code and the firmware command tables the danger list is checked against
TEST_PATHS = ["host", "firmware/xrex/src/app/commands", "firmware/hbridge/src/app/commands", "firmware/xrex/python",
              "firmware/hbridge/python"]
CRON_MARK = "# wham hub_update"
UNIT = "wham-hub@{}.service"


def now():
    return time.time()


def stamp(t=None):
    return datetime.datetime.fromtimestamp(t if t is not None else now()).strftime("%Y-%m-%d %H:%M:%S")


class Services:
    """The running hubs, through systemd and ps. Replaced by a fake in the tests."""

    def __init__(self, repo):
        self.repo = repo

    def _show(self, name, prop):
        out = subprocess.run(["systemctl", "show", UNIT.format(name), "-p", prop, "--value"], capture_output=True, text=True)
        return out.stdout.strip()

    def names(self):
        """The hubs this checkout configures that systemd is running (or restarting): one per host/config/<name>.yaml."""
        found = []
        for path in sorted(glob.glob(os.path.join(self.repo, "host", "config", "*.yaml"))):
            name = os.path.basename(path)[:-5]
            if name == "example" or name.endswith(".local"):
                continue
            if self._show(name, "ActiveState") in ("active", "activating", "reloading"):
                found.append(name)
        return found

    def pid(self, name):
        try:
            return int(self._show(name, "MainPID") or 0)
        except ValueError:
            return 0

    def started(self, pid):
        """When the process started (epoch seconds), or None."""
        out = subprocess.run(["ps", "-o", "etimes=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
        return now() - int(out) if out.isdigit() else None

    def stop(self, pid):
        os.kill(pid, signal.SIGTERM)

    def port(self, name):
        sys.path.insert(0, os.path.join(self.repo, "host"))
        try:
            import wham_hub
            cfg = wham_hub.load_config(os.path.join(self.repo, "host", "config", name + ".yaml"))
            return int(cfg.get("listen_port") or 0)
        finally:
            sys.path.pop(0)


def ask_hub(port, lines, want, timeout=8.0):
    """Connect to a hub on this host as a quiet client, send `lines`, and return the reply lines, one per entry of `want`
    (a predicate on a line). None when it cannot be reached or does not answer in time."""
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except OSError:
        return None
    try:
        sock.sendall(("@quiet hub-updater\n" + "".join(l + "\n" for l in lines)).encode())
        buf, got, end = b"", [], time.monotonic() + timeout
        while len(got) < len(want) and time.monotonic() < end:
            sock.settimeout(max(0.1, end - time.monotonic()))
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and len(got) < len(want):
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line and want[len(got)](line):
                    got.append(line)
        return got if len(got) == len(want) else None
    finally:
        sock.close()


def hub_fields(status_line):
    return dict(w.split("=", 1) for w in status_line.split()[2:] if "=" in w)


def quiet(port):
    """(True, why) when the hub may be restarted now: this updater is its only client and the controller is not ARMED or
    FIRING. (False, why) otherwise."""
    status = ask_hub(port, ["!hub status"], [lambda l: l.startswith("# hub name=")])
    if not status:
        return False, "it does not answer !hub status"
    f = hub_fields(status[0])
    clients = int(f.get("clients", "0") or 0)
    if clients > 1:
        others = [w for w in f.get("who", "").split(",") if w and not w.startswith("hub-updater")]
        return False, f"{clients - 1} other client(s) connected ({', '.join(others) or '?'})"
    if f.get("connected") != "1":
        return True, "its controller is not connected"
    state = ask_hub(port, ["STATE?"], [lambda l: l.startswith(("OK", "ERR"))])
    if not state:
        return False, "STATE? got no answer"
    words = state[0].split()
    if words[0] == "OK" and len(words) > 1 and words[1] in ("ARMED", "FIRING"):
        return False, f"its controller is {words[1]}"
    return True, f"no other client, controller says {state[0][:40]}"


class Updater:
    def __init__(self, repo=REPO, remote="origin", branch="deploy", state_dir="/var/lib/wham/hub/update", services=None,
                 python=sys.executable, tests=None, test_timeout=900, health_s=60.0, quiet_fn=quiet, ask_fn=ask_hub,
                 out=print):
        self.repo, self.remote, self.branch, self.state_dir = repo, remote, branch, state_dir
        self.svc = services or Services(repo)
        self.python, self.tests, self.test_timeout, self.health_s = python, tests or TESTS, test_timeout, health_s
        self.quiet, self.ask, self.out = quiet_fn, ask_fn, out
        os.makedirs(state_dir, exist_ok=True)
        self.state_file = os.path.join(state_dir, "state.json")
        self.log_file = os.path.join(state_dir, "update.log")

    # -- bookkeeping
    def log(self, msg):
        line = f"{stamp()} {msg}"
        self.out(line)
        with open(self.log_file, "a") as f:
            f.write(line + "\n")

    def load(self):
        try:
            with open(self.state_file) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def save(self, st):
        tmp = self.state_file + ".tmp"
        with open(tmp, "w") as f:
            json.dump(st, f, indent=1)
        os.replace(tmp, self.state_file)

    def git(self, *args, check=True):
        r = subprocess.run(["git", "-C", self.repo] + list(args), capture_output=True, text=True)
        if check and r.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
        return r.stdout.strip()

    def head(self):
        return self.git("rev-parse", "HEAD")

    def short(self, sha):
        return (sha or "?")[:7]

    # -- update
    def fetch(self):
        self.git("fetch", "--quiet", self.remote, f"+refs/heads/{self.branch}:refs/remotes/{self.remote}/{self.branch}")
        return self.git("rev-parse", f"{self.remote}/{self.branch}")

    def test_commit(self, sha):
        """Unpack the commit beside the live checkout and run the hub's tests and a config check there. (ok, output)."""
        tmp = tempfile.mkdtemp(prefix="hub-update-")
        try:
            archive = os.path.join(tmp, "tree.tar")
            paths = [p for p in TEST_PATHS if self.git("ls-tree", "--name-only", sha, "--", p, check=False)]
            if "host" not in paths:
                return False, f"{self.short(sha)} has no host/ folder"
            with open(archive, "wb") as f:
                r = subprocess.run(["git", "-C", self.repo, "archive", sha, "--"] + paths, stdout=f, stderr=subprocess.PIPE)
            if r.returncode != 0:
                return False, "git archive: " + r.stderr.decode(errors="replace").strip()
            tree = os.path.join(tmp, "tree")
            with tarfile.open(archive) as t:
                try:
                    t.extractall(tree, filter="data")
                except TypeError:                                   # Python before 3.12 has no filter argument
                    t.extractall(tree)
            host = os.path.join(tree, "host")
            for local in glob.glob(os.path.join(self.repo, "host", "config", "*.local.yaml")):
                shutil.copy(local, os.path.join(host, "config"))
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            steps = [[self.python, "-m", "unittest"] + self.tests]
            for name in self.svc.names():
                steps.append([self.python, "wham_hub.py", "--config", os.path.join("config", name + ".yaml"), "--check"])
            output = []
            for cmd in steps:
                try:
                    r = subprocess.run(cmd, cwd=host, env=env, capture_output=True, text=True, timeout=self.test_timeout)
                except subprocess.TimeoutExpired:
                    return False, f"{' '.join(cmd[1:])}: no result within {self.test_timeout} s"
                output.append((r.stdout + r.stderr).strip())
                if r.returncode != 0:
                    return False, f"{' '.join(cmd[1:])} failed:\n" + "\n".join(output[-1].splitlines()[-25:])
            return True, output[0].splitlines()[-1] if output and output[0] else "ok"
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def switch(self, sha):
        self.git("checkout", "--quiet", "-B", self.branch, sha)

    def update(self):
        st = self.load()
        try:
            target = self.fetch()
        except RuntimeError as e:
            self.log(f"fetch failed, nothing changed: {e}")
            return self.restart()
        st["last_fetch"] = now()
        here = self.head()
        if target == here:
            self.save(st)
            self.log(f"up to date at {self.short(here)} ({self.branch})")
            return self.restart()
        if target in st.get("bad", []):
            self.save(st)
            self.log(f"{self.remote}/{self.branch} is {self.short(target)}, which failed before: staying at {self.short(here)}")
            return self.restart()
        self.log(f"{self.remote}/{self.branch} moved to {self.short(target)} ({self.git('log', '-1', '--format=%s', target)[:70]}); "
                 f"testing it before switching from {self.short(here)}")
        ok, text = self.test_commit(target)
        if not ok:
            st.setdefault("bad", []).append(target)
            self.save(st)
            self.log(f"NOT updating: {self.short(target)} failed its tests. Staying at {self.short(here)}.\n{text}")
            return self.restart()
        self.log(f"tests passed ({text})")
        try:
            self.switch(target)
        except RuntimeError as e:
            self.save(st)
            self.log(f"NOT updating: the checkout could not switch to {self.short(target)} (a local edit in the way? keep site "
                     f"settings in host/config/<name>.local.yaml): {e}")
            return self.restart()
        st.update(previous=here, current=target, updated_at=now())
        self.save(st)
        self.log(f"checkout switched {self.short(here)} -> {self.short(target)}; the hubs are restarted as each becomes quiet")
        return self.restart()

    # -- restart
    def pending(self):
        """[(name, pid)] of the hubs that started before the checkout last changed."""
        changed = self.load().get("updated_at")
        out = []
        for name in self.svc.names():
            pid = self.svc.pid(name)
            started = self.svc.started(pid) if pid else None
            if pid and changed and started is not None and started < changed - 1:
                out.append((name, pid))
        return out

    def healthy(self, name, old_pid):
        end = time.monotonic() + self.health_s
        while time.monotonic() < end:
            pid = self.svc.pid(name)
            port = self.svc.port(name)
            if pid and pid != old_pid and port:
                status = self.ask(port, ["!hub status"], [lambda l: l.startswith("# hub name=")], 3.0)
                if status:
                    return hub_fields(status[0])
            time.sleep(1.0)
        return None

    def restart(self):
        restarted = 0
        for name, pid in self.pending():
            port = self.svc.port(name)
            ok, why = self.quiet(port) if port else (False, "no listen_port in its config")
            if not ok:
                self.log(f"{name}: runs older code, restart waits: {why}")
                continue
            self.log(f"{name}: restarting on the new code ({why})")
            try:
                self.svc.stop(pid)
            except OSError as e:
                self.log(f"{name}: could not stop process {pid}: {e}")
                continue
            f = self.healthy(name, pid)
            if f is None:
                self.log(f"{name}: did not come back within {self.health_s:.0f} s on {self.short(self.head())}: rolling back")
                self.rollback(reason=f"{name} did not come back")
                return restarted
            restarted += 1
            self.log(f"{name}: back up, controller connected={f.get('connected')} serial={f.get('serial')}")
        return restarted

    def rollback(self, reason="by hand"):
        st = self.load()
        prev, cur = st.get("previous"), self.head()
        if not prev or prev == cur:
            self.log(f"rollback ({reason}): no earlier commit recorded; nothing changed")
            return False
        self.switch(prev)
        st.setdefault("bad", []).append(cur)
        st.update(previous=None, current=prev, updated_at=now())
        self.save(st)
        self.log(f"ROLLED BACK {self.short(cur)} -> {self.short(prev)} ({reason}); {self.short(cur)} will not be tried again "
                 f"until {self.branch} moves. A hub that is restarting picks up the old code by itself; the others follow when quiet.")
        return True

    def status(self):
        st = self.load()
        lines = [f"checkout {self.repo}: {self.git('rev-parse', '--abbrev-ref', 'HEAD')} at {self.short(self.head())} "
                 f"({self.git('log', '-1', '--format=%cs %s')[:70]})"]
        ref = self.git("rev-parse", "--verify", "--quiet", f"{self.remote}/{self.branch}", check=False)
        lines.append(f"{self.remote}/{self.branch} as last fetched: {self.short(ref) if ref else 'never fetched'}"
                     + (f" at {stamp(st['last_fetch'])}" if st.get("last_fetch") else ""))
        if st.get("updated_at"):
            lines.append(f"last change of the checkout: {stamp(st['updated_at'])}, previous commit {self.short(st.get('previous'))}")
        if st.get("bad"):
            lines.append("commits that failed (not retried): " + ", ".join(self.short(b) for b in st["bad"]))
        waiting = dict(self.pending())
        for name in self.svc.names():
            pid = self.svc.pid(name)
            started = self.svc.started(pid) if pid else None
            lines.append(f"  {name}: pid {pid}, started {stamp(started) if started else '?'}"
                         + (" -- runs older code, restart pending" if name in waiting else ""))
        dirty = self.git("status", "--porcelain", "--untracked-files=no")
        if dirty:
            lines.append("local edits to tracked files (move site settings into host/config/<name>.local.yaml):\n  "
                         + dirty.replace("\n", "\n  "))
        for line in lines:
            self.out(line)


def cron_lines(python, script, state_dir):
    log = os.path.join(state_dir, "cron.log")
    return [f"30 3 * * * {python} {script} update >> {log} 2>&1 {CRON_MARK}",
            f"*/15 * * * * {python} {script} restart >> {log} 2>&1 {CRON_MARK}"]


def set_cron(lines):
    cur = subprocess.run(["crontab", "-l"], capture_output=True, text=True)
    keep = [l for l in (cur.stdout.splitlines() if cur.returncode == 0 else []) if CRON_MARK not in l]
    new = "\n".join(keep + lines) + "\n"
    subprocess.run(["crontab", "-"], input=new, text=True, check=True)
    return new


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["update", "restart", "status", "rollback", "install-cron", "uninstall-cron"])
    ap.add_argument("--repo", default=REPO, help=f"the live checkout (default {REPO})")
    ap.add_argument("--remote", default="origin")
    ap.add_argument("--branch", default="deploy", help="the branch the server follows (default deploy)")
    ap.add_argument("--state-dir", default="/var/lib/wham/hub/update", help="state and log folder (default /var/lib/wham/hub/update)")
    args = ap.parse_args(argv)
    os.makedirs(args.state_dir, exist_ok=True)
    if args.action in ("install-cron", "uninstall-cron"):
        lines = cron_lines(sys.executable, os.path.abspath(__file__), args.state_dir) if args.action == "install-cron" else []
        print(set_cron(lines), end="")
        return 0
    lock = open(os.path.join(args.state_dir, "lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print(f"{stamp()} another hub_update.py is running; skipped")
        return 0
    u = Updater(repo=args.repo, remote=args.remote, branch=args.branch, state_dir=args.state_dir)
    if args.action == "update":
        u.update()
    elif args.action == "restart":
        u.restart()
    elif args.action == "rollback":
        u.rollback()
    else:
        u.status()
    return 0


if __name__ == "__main__":
    sys.exit(main())

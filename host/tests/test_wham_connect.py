"""host/wham_connect.py: device lookup, the identity check, the poll filter, one-shot through a stand-in hub, and the tunnel step."""
import io
import os
import socket
import subprocess
import sys
import threading
import time
import unittest
from contextlib import redirect_stdout, redirect_stderr

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "operator_ui"))
import wham_connect as wc
import wham_ui


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


class StandInHub:
    """Answers *IDN? with a given serial and STATE? with OK IDLE, one reply per command line, like a quiet hub client sees."""

    def __init__(self, serial="WB-0002-6", product="WHAM-HBRIDGE-PFMG474", idn_reply=None):
        self.idn = idn_reply or f"OK {product} REVA v0.11 abc1234 SN={serial} UID=1"
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0)); self.srv.listen(5)
        self.port = self.srv.getsockname()[1]
        self.received = []
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        buf = b""
        while True:
            try:
                d = c.recv(1024)
            except OSError:
                return
            if not d:
                return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = line.decode().strip()
                self.received.append(t)
                if t.startswith("@"):
                    continue
                if t.startswith("!hub"):
                    c.sendall(b"# hub: ok\r\n")
                elif t == "*IDN?":
                    c.sendall(self.idn.encode() + b"\r\n")
                elif t.endswith("?"):
                    c.sendall(b"OK IDLE\r\n")
                else:
                    c.sendall(b"OK\r\n")

    def close(self):
        self.srv.close()


class Lookup(unittest.TestCase):
    def test_names_aliases_serials_and_ports_all_find_the_same_device(self):
        for w in ("limiter", "LMTR", "lmtr-ctrl", "LMTR-CTRL", "WB-0002-6", "wb-0002-6", "5102"):
            self.assertEqual(wc.find_device(w)["port"], 5102, w)
        self.assertEqual(wc.find_device("xrex")["serial"], "WB-0003-4")
        self.assertEqual(wc.find_device("ech")["port"], 5103)
        self.assertIsNone(wc.find_device("bogus"))

    def test_ports_and_serials_match_the_hub_configs_and_the_registry(self):
        host = os.path.join(HERE, "..")
        with open(os.path.join(host, "devices.csv")) as f:
            reg = f.read()
        for d in wc.DEVICES:
            self.assertIn(d["serial"], reg)
            with open(os.path.join(host, "config", d["label"].lower() + ".yaml")) as f:
                self.assertIn(f"listen_port: {d['port']}", f.read())


class PollFilterTests(unittest.TestCase):
    def test_hides_a_clients_state_polls_and_their_replies_only(self):
        f = wc.PollFilter(True)
        self.assertTrue(f.hide("[14:00:00.000] [bob] > STATE?"))
        self.assertTrue(f.hide("[14:00:00.010] [bob] < OK IDLE"))
        self.assertFalse(f.hide("[14:00:01.000] [bob] > FAULT?"))
        self.assertFalse(f.hide("[14:00:01.010] [bob] < OK 0"))
        self.assertFalse(f.hide("[14:00:02.000] [bob] > ARM"))
        self.assertFalse(f.hide("--- bob left ---"))
        self.assertEqual(f.hidden, 1)

    def test_a_reply_from_another_client_is_not_swallowed(self):
        f = wc.PollFilter(True)
        f.hide("[amy] > STATE?")
        self.assertFalse(f.hide("[bob] < OK ARMED"))
        self.assertTrue(f.hide("[amy] < OK IDLE"))

    def test_off_by_default_hides_nothing(self):
        self.assertFalse(wc.PollFilter(False).hide("[bob] > STATE?"))


class Styling(unittest.TestCase):
    def test_dangerous_joins_and_commands_are_highlighted_and_plain_without_colour(self):
        self.assertIn(wc.RED, wc.style_line("--- hub: DANGEROUS [output] bob: FIRE ---", True))
        self.assertIn(wc.YELLOW, wc.style_line("[14:00:00.000] --- bob (127.0.0.1) joined ---", True))
        self.assertIn(wc.BOLD, wc.style_line("[bob] > ARM", True))
        self.assertEqual(wc.style_line("--- bob left ---", False), "--- bob left ---")


class Identity(unittest.TestCase):
    def test_right_board_is_accepted(self):
        h = StandInHub(); self.addCleanup(h.close)
        ok, msg = wc.check_identity("127.0.0.1", h.port, "WB-0002-6", "WHAM-HBRIDGE-PFMG474")
        self.assertTrue(ok, msg)

    def test_a_different_board_on_the_port_is_refused(self):
        h = StandInHub(serial="WB-0003-4", product="WHAM-XREX-PFMG474"); self.addCleanup(h.close)
        ok, msg = wc.check_identity("127.0.0.1", h.port, "WB-0002-6", "WHAM-HBRIDGE-PFMG474")
        self.assertFalse(ok)
        self.assertIn("wrong board", msg)

    def test_a_hub_whose_controller_is_unreachable_is_reported(self):
        h = StandInHub(idn_reply="ERR hub the controller is not connected"); self.addCleanup(h.close)
        ok, msg = wc.check_identity("127.0.0.1", h.port, "WB-0002-6")
        self.assertFalse(ok)
        self.assertIn("not connected", msg)

    def test_nothing_listening_is_reported_not_raised(self):
        ok, msg = wc.check_identity("127.0.0.1", free_port(), "WB-0002-6")
        self.assertFalse(ok)


class Main(unittest.TestCase):
    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = wc.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_one_shot_through_a_stand_in_hub_without_a_tunnel(self):
        h = StandInHub(); self.addCleanup(h.close)
        code, out, _ = self.run_main("limiter", "--port", str(h.port), "--serial", "WB-0002-6", "--no-tunnel",
                                     "--send", "STATE?", "--wait", "0.3", "--name", "tester")
        self.assertEqual(code, 0, out)
        self.assertIn("identity: OK", out)
        self.assertIn("OK IDLE", out)
        self.assertIn("STATE?", h.received)

    def test_a_wrong_board_stops_before_any_command_is_sent_and_force_goes_on(self):
        h = StandInHub(serial="WB-0003-4", product="WHAM-XREX-PFMG474"); self.addCleanup(h.close)
        code, out, _ = self.run_main("--port", str(h.port), "--serial", "WB-0002-6", "--no-tunnel", "--send", "ARM", "--wait", "0.2")
        self.assertEqual(code, 1)
        self.assertNotIn("ARM", h.received)
        code, out, _ = self.run_main("--port", str(h.port), "--serial", "WB-0002-6", "--no-tunnel", "--force", "--send", "STATE?", "--wait", "0.2")
        self.assertEqual(code, 0)

    def test_unknown_device_and_watch_with_send_are_usage_errors(self):
        self.assertEqual(self.run_main("nonesuch")[0], 2)
        self.assertEqual(self.run_main("limiter", "--watch", "--send", "ARM")[0], 2)
        self.assertEqual(self.run_main()[0], 2)


class TunnelStep(unittest.TestCase):
    FAKE = "import socket,sys,time\ns=socket.socket(); s.bind(('127.0.0.1',int(sys.argv[1]))); s.listen(1); time.sleep(60)\n"

    def setUp(self):
        self.port = free_port()
        self._cmd = wham_ui.Tunnel.command
        wham_ui.Tunnel.command = lambda t, ports: [sys.executable, "-c", self.FAKE, str(ports[0])]
        self.addCleanup(lambda: setattr(wham_ui.Tunnel, "command", self._cmd))

    def test_starts_a_tunnel_it_then_owns_and_closes(self):
        t, state, msg = wc.open_tunnel("127.0.0.1", self.port, "lab", ssh="x")
        self.assertEqual(state, "open", msg)
        self.assertIsNotNone(t)
        self.assertTrue(wham_ui.port_in_use(self.port))
        t.stop()
        self.assertFalse(wham_ui.port_in_use(self.port))

    def test_a_tunnel_already_up_is_reused_and_not_owned(self):
        outside = subprocess.Popen([sys.executable, "-c", self.FAKE, str(self.port)])
        self.addCleanup(lambda: (outside.kill(), outside.wait()))
        end = time.time() + 5
        while time.time() < end and not wham_ui.port_in_use(self.port):
            time.sleep(0.05)
        t, state, msg = wc.open_tunnel("127.0.0.1", self.port, "lab", ssh="x")
        self.assertEqual((t, state), (None, "open-elsewhere"))

    def test_a_failing_ssh_is_reported_with_its_own_message(self):
        wham_ui.Tunnel.command = lambda t, ports: [sys.executable, "-c", "import sys; sys.stderr.write('Permission denied (publickey).\\n'); sys.exit(255)"]
        t, state, msg = wc.open_tunnel("127.0.0.1", self.port, "lab", ssh="x")
        self.assertEqual((t, state), (None, "closed"))
        self.assertIn("Permission denied", msg)


if __name__ == "__main__":
    unittest.main()

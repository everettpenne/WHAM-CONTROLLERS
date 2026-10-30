"""The operator web UI's server (host/operator_ui/wham_ui.py): over HTTP, against the simulated controller (direct mode)
and against the real hub service with a fake controller behind it (hub mode)."""
import asyncio
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "operator_ui"))
import wham_hub as wh
import wham_sim
import wham_ui
from test_wham_hub import FakeController, free_port


class Http:
    """A tiny client for the page's API, with the page's own token."""

    def __init__(self, app, server):
        self.app, self.base = app, f"http://127.0.0.1:{server.server_address[1]}"

    def call(self, path, body=None, token=True, host=None):
        req = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                                     method="GET" if body is None else "POST")
        if token:
            req.add_header("X-WHAM-Token", self.app.token)
        if host:
            req.add_header("Host", host)
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def wait(self, cid, pred, what, secs=8):
        end = time.time() + secs
        while time.time() < end:
            _, st = self.call("/api/state")
            for c in st["connections"]:
                if c["id"] == cid and pred(c):
                    return c
            time.sleep(0.1)
        self.last = st
        raise AssertionError(f"timed out waiting for {what}: {st}")


class UiBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.app = wham_ui.App(presets_file=os.path.join(self.tmp.name, "controllers.json"), shots_dir=os.path.join(self.tmp.name, "shots"))
        self.server = wham_ui.make_server(self.app, "127.0.0.1", 0)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.app.shutdown)
        self.addCleanup(self.server.shutdown)
        self.http = Http(self.app, self.server)


class DirectModeTests(UiBase):
    def setUp(self):
        super().setUp()
        self.sim = wham_sim.SimController("hbridge", 0, label="t", min_fire_s=0.5)
        self.sim.start()
        self.addCleanup(self.sim.stop)
        code, st = self.http.call("/api/connect", {"mode": "direct", "host": "127.0.0.1", "port": self.sim.port, "name": "sim"})
        self.assertEqual(code, 200, st)
        self.cid = st["id"]
        self.http.wait(self.cid, lambda c: c["identified"] and c["state"], "identity and state")

    def test_identified_as_hbridge(self):
        _, st = self.http.call("/api/state")
        c = st["connections"][0]
        self.assertEqual(c["product"], "hbridge")
        self.assertEqual(c["state"], "IDLE")
        self.assertTrue(c["link_ok"])

    def test_pages_and_security(self):
        code, _ = self.http.call("/api/state", token=False)
        self.assertEqual(code, 403)
        code, _ = self.http.call("/api/state", host="evil.example")          # DNS-rebinding style request
        self.assertEqual(code, 403)
        code, _ = self.http.call("/api/send", {"id": self.cid, "command": "STATE?"}, token=False)
        self.assertEqual(code, 403)
        with urllib.request.urlopen(self.http.base + "/") as r:
            page = r.read().decode()
        self.assertIn(self.app.token, page)
        self.assertNotIn("__WHAM_TOKEN__", page)
        for f in ("app.js", "style.css"):
            with urllib.request.urlopen(f"{self.http.base}/static/{f}") as r:
                self.assertEqual(r.status, 200)
        with self.assertRaises(urllib.error.HTTPError):
            urllib.request.urlopen(self.http.base + "/static/../wham_ui.py")

    def test_plain_command_and_error_reply(self):
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "STATE?"})
        self.assertEqual((code, r["reply"]), (200, "OK IDLE"))
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "NOSUCH"})
        self.assertEqual(code, 200)
        self.assertFalse(r["ok"])
        self.assertTrue(r["reply"].startswith("ERR 1"))

    def test_dangerous_command_needs_confirmation_server_side(self):
        for cmd in ("ARM", "FIRE", "FAULT:CLEar", "CHARGE:FREQuency 1000", "arm"):
            code, r = self.http.call("/api/send", {"id": self.cid, "command": cmd})
            self.assertEqual(code, 409, cmd)
            self.assertTrue(r["needs_confirm"])
        self.assertEqual(self.sim.sim.state if hasattr(self.sim.sim, "state") else "IDLE", "IDLE")
        _, r = self.http.call("/api/send", {"id": self.cid, "command": "STATE?"})
        self.assertEqual(r["reply"], "OK IDLE")                              # nothing was sent for the refused ones
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "ARM", "confirmed": True})
        self.assertEqual((code, r["reply"]), (200, "OK"))
        _, r = self.http.call("/api/send", {"id": self.cid, "command": "STATE?"})
        self.assertEqual(r["reply"], "OK ARMED")

    def test_query_endpoint_refuses_anything_but_queries(self):
        code, r = self.http.call("/api/query", {"id": self.cid, "commands": ["STATE?", "ARM", "CONFig:MODE LIMiter", "*IDN?"]})
        res = r["results"]
        self.assertEqual(res[0]["reply"], "OK IDLE")
        self.assertIsNone(res[1]["reply"])
        self.assertEqual(res[1]["error"], "not a query")
        self.assertEqual(res[2]["error"], "not a query")
        self.assertTrue(res[3]["reply"].startswith("OK WHAM-HBRIDGE"))
        _, r = self.http.call("/api/send", {"id": self.cid, "command": "STATE?"})
        self.assertEqual(r["reply"], "OK IDLE")

    def test_a_click_is_not_queued_behind_a_long_refresh(self):
        import threading, time
        slow = threading.Thread(target=lambda: self.http.call("/api/query", {"id": self.cid, "commands": ["STATE?"] * 60}))
        slow.start()
        time.sleep(0.02)
        t0 = time.time()
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "STATE?"})
        waited = time.time() - t0
        slow.join()
        self.assertEqual(code, 200)
        self.assertLess(waited, 5.0)

    def test_hub_lines_are_refused_outside_hub_mode(self):
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "!hub status"})
        self.assertEqual(code, 400)

    def test_shot_upload_and_readback_sequence(self):
        self.http.call("/api/send", {"id": self.cid, "command": "CONFig:MODE LIMiter"})
        for cmd in ("SHOT:BEGin", "SHOT:STEP 0 100000 20 80 50", "SHOT:STEP 10000 100000 80 20 50", "SHOT:END"):
            _, r = self.http.call("/api/send", {"id": self.cid, "command": cmd})
            self.assertTrue(r["ok"], (cmd, r))
        self.assertEqual(r["reply"], "OK 2 10000")
        _, r = self.http.call("/api/query", {"id": self.cid, "commands": ["SHOT?", "SHOT:POINt? 1"]})
        self.assertEqual(r["results"][1]["reply"], "OK 10000 100000 80 20 50")
        _, r = self.http.call("/api/send", {"id": self.cid, "command": "SHOT:STEP 0 50 20 80 50"})   # outside an upload
        self.assertFalse(r["ok"])

    def test_events_stream_shows_what_was_sent_and_received(self):
        _, ev = self.http.call(f"/api/events?id={self.cid}&since=0&wait=0")
        base = ev["seq"]
        self.http.call("/api/send", {"id": self.cid, "command": "FAULT?"})
        _, ev = self.http.call(f"/api/events?id={self.cid}&since={base}&wait=3")
        texts = [(e["dir"], e["text"]) for e in ev["events"] if not e["poll"]]
        self.assertEqual(texts[0], ("tx", "FAULT?"))
        self.assertEqual(texts[1][0], "rx")
        self.assertTrue(texts[1][1].startswith("OK"))

    def test_fault_and_clear_follow_the_state(self):
        self.http.call("/api/send", {"id": self.cid, "command": "GENERAL:TEST:FAULT", "confirmed": True})
        c = self.http.wait(self.cid, lambda c: c["state"] == "FAULT", "FAULT state")
        self.assertTrue(c["fault"])
        code, _ = self.http.call("/api/send", {"id": self.cid, "command": "FAULT:CLEar"})
        self.assertEqual(code, 409)

    def test_presets_round_trip_and_validation(self):
        items = [{"name": "A", "mode": "hub", "host": "127.0.0.1", "port": "5101"}, {"name": "B", "mode": "serial", "device": "/dev/x"}]
        code, r = self.http.call("/api/presets", {"presets": items})
        self.assertEqual(code, 200)
        self.assertEqual(r["presets"], items)
        _, st = self.http.call("/api/state")
        self.assertEqual(st["presets"], items)
        code, _ = self.http.call("/api/presets", {"presets": [{"name": "bad", "mode": "ftp"}]})
        self.assertEqual(code, 400)

    def test_disconnect_removes_the_connection(self):
        self.http.call("/api/disconnect", {"id": self.cid})
        _, st = self.http.call("/api/state")
        self.assertEqual(st["connections"], [])


class ConnectErrorTests(UiBase):
    def test_unreachable_address_gives_a_readable_error(self):
        code, r = self.http.call("/api/connect", {"mode": "direct", "host": "127.0.0.1", "port": free_port()})
        self.assertEqual(code, 502)
        self.assertIn("127.0.0.1", r["error"])

    def test_missing_fields(self):
        code, r = self.http.call("/api/connect", {"mode": "hub", "host": "", "port": ""})
        self.assertEqual(code, 400)
        code, r = self.http.call("/api/connect", {"mode": "serial"})
        self.assertEqual(code, 400)
        code, r = self.http.call("/api/connect", {"mode": "carrier-pigeon"})
        self.assertEqual(code, 400)

    def test_demo_mode(self):
        self.app.demo = True
        code, st = self.http.call("/api/connect", {"mode": "demo", "product": "xrex"})
        self.assertEqual(code, 200, st)
        c = self.http.wait(st["id"], lambda c: c["identified"], "demo identity")
        self.assertEqual(c["product"], "xrex")


class HubModeTests(UiBase):
    """The page's server in front of the real hub service, which has a fake controller behind it."""

    def setUp(self):
        super().setUp()
        self.loop = asyncio.new_event_loop()
        self.tmp2 = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp2.cleanup)
        ready = threading.Event()
        self.listen = free_port()

        def run():
            asyncio.set_event_loop(self.loop)
            self.ctl = FakeController()
            port = self.loop.run_until_complete(self.ctl.start())
            cfg = dict(wh.DEFAULTS)
            cfg.update(name="t1", host="127.0.0.1", port=port, listen_host="127.0.0.1", listen_port=self.listen,
                       log_dir=os.path.join(self.tmp2.name, "logs"), registry=os.path.join(self.tmp2.name, "devices.csv"),
                       reconnect_s=0.2, confirm_dangerous=True)
            self.hub = wh.Hub(cfg)
            self.task = self.loop.create_task(self.hub.run())
            ready.set()
            self.loop.run_forever()
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        ready.wait(5)
        self.addCleanup(self.stop_hub)
        code, st = self.http.call("/api/connect", {"mode": "hub", "host": "127.0.0.1", "port": self.listen, "name": "hub"})
        self.assertEqual(code, 200, st)
        self.cid = st["id"]
        self.http.wait(self.cid, lambda c: c["identified"] and c["state"] and c["hub"], "identity, state and hub status", 12)

    def stop_hub(self):
        async def shutdown():
            self.hub.stop.set()
            await asyncio.wait_for(self.task, 3)
            await self.ctl.stop()
            others = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
            for t in others:
                t.cancel()
            await asyncio.gather(*others, return_exceptions=True)
        asyncio.run_coroutine_threadsafe(shutdown(), self.loop).result(6)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(3)
        self.loop.close()

    def test_status_carries_hub_info_and_identity(self):
        _, st = self.http.call("/api/state")
        c = st["connections"][0]
        self.assertTrue(c["hub"]["connected"])
        self.assertEqual(c["hub"]["clients"], 1)
        self.assertEqual(c["ident"]["serial"], "WB-0001-8")
        self.assertEqual(c["state"], "IDLE")

    def test_second_connection_is_seen_and_commands_are_echoed(self):
        code, st2 = self.http.call("/api/connect", {"mode": "hub", "host": "127.0.0.1", "port": self.listen, "name": "second"})
        self.assertEqual(code, 200)
        self.http.wait(self.cid, lambda c: c["hub"] and c["hub"]["clients"] == 2, "two clients")
        _, ev = self.http.call(f"/api/events?id={self.cid}&since=0&wait=0")
        base = ev["seq"]
        self.http.call("/api/send", {"id": st2["id"], "command": "STATE?"})
        _, ev = self.http.call(f"/api/events?id={self.cid}&since={base}&wait=3")
        self.assertTrue(any(e["dir"] == "other_tx" and "STATE?" in e["text"] for e in ev["events"]), ev["events"])

    def test_dangerous_command_through_a_confirming_hub(self):
        code, _ = self.http.call("/api/send", {"id": self.cid, "command": "ARM"})
        self.assertEqual(code, 409)
        self.assertNotIn("ARM", self.ctl.received)
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "ARM", "confirmed": True})
        self.assertEqual(code, 200)
        self.assertNotIn("ERR hub", r["reply"] or "")
        self.assertIn("ARM", self.ctl.received)

    def test_hub_status_line_is_allowed(self):
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "!hub status"})
        self.assertEqual(code, 200)
        self.assertIn("serial=WB-0001-8", r["reply"])


class RecordingTests(UiBase):
    """Shot data: arm the controller's log, run a shot, and get a saved, reported recording."""

    def connect(self, product):
        self.sim = wham_sim.SimController(product, 0, label="t", min_fire_s=0.6)
        self.sim.start()
        self.addCleanup(self.sim.stop)
        code, st = self.http.call("/api/connect", {"mode": "direct", "host": "127.0.0.1", "port": self.sim.port, "name": "sim"})
        self.cid = st["id"]
        self.http.wait(self.cid, lambda c: c["identified"] and c["state"], "identity")

    def send(self, cmd, confirmed=True):
        code, r = self.http.call("/api/send", {"id": self.cid, "command": cmd, "confirmed": confirmed})
        self.assertEqual(code, 200, (cmd, r))
        self.assertTrue(r["ok"], (cmd, r))

    def wait_rec(self, states, secs=15):
        return self.http.wait(self.cid, lambda c: c["rec"] and c["rec"]["state"] in states, f"recording in {states}", secs)["rec"]

    def test_xrex_shot_is_recorded_reported_and_downloadable(self):
        self.connect("xrex")
        self.send("SHOT:TIMing 0.5 0.5 0.5")
        self.send("SHOT:CURRent 1 3000")
        self.send("ARM")
        code, r = self.http.call("/api/record/arm", {"id": self.cid, "channels": [1, 2], "duration_s": 1.5, "meta": {"what": "test"}})
        self.assertEqual(code, 200, r)
        self.assertEqual(r["rec"]["state"], "armed")
        self.assertEqual(r["rec"]["decim"], 2)                   # 1.5 s * 1.15 * 1000 Hz over 1000 samples
        self.send("SHOT:STARt")
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        sid = rec["sid"]
        code, d = self.http.call(f"/api/shot?sid={sid}&unit=A")
        self.assertEqual(code, 200)
        self.assertEqual([c["name"] for c in d["channels"]], ["Channel 1", "Channel 2"])
        self.assertTrue(d["can_amps"])
        self.assertIn("<polyline", d["channels"][0]["svg"])
        self.assertGreater(d["channels"][0]["raw"]["setpoint_peak"], 2000)       # amps, near the 3000 A demand
        code, d2 = self.http.call(f"/api/shot?sid={sid}&unit=native")
        self.assertEqual(d2["unit"], "native")
        self.assertGreater(d2["channels"][0]["raw"]["setpoint_peak"], 50000)     # Hz
        _, lst = self.http.call(f"/api/shots?id={self.cid}")
        self.assertEqual([x["sid"] for x in lst["shots"]], [sid])
        for name in ("shot.csv", "report.html", "report.md", "shot.json"):
            req = urllib.request.Request(f"{self.http.base}/api/shot/file?sid={sid}&name={name}", headers={"X-WHAM-Token": self.app.token})
            with urllib.request.urlopen(req) as resp:
                self.assertIn("attachment", resp.headers["Content-Disposition"])
                self.assertGreater(len(resp.read()), 100)
        code, _ = self.http.call(f"/api/shot/file?sid={sid}&name=../../x")
        self.assertEqual(code, 404)
        code, _ = self.http.call("/api/shot?sid=../../etc/passwd")
        self.assertEqual(code, 404)
        code, _ = self.http.call(f"/api/shot?sid={sid}", token=False)
        self.assertEqual(code, 403)
        with open(os.path.join(self.tmp.name, "shots", sid, "shot.json")) as f:
            saved = json.load(f)
        self.assertEqual(saved["meta"], {"what": "test"})
        self.assertEqual(saved["controller"]["serial"], "WB-SIM1-0")

    def test_xrex_zero_gains_shot_is_flagged_and_gains_fix_it(self):
        self.connect("xrex")
        self.send("SHOT:TIMing 0.5 0.5 0.5")
        self.send("SHOT:CURRent 1 3000")
        _, r = self.http.call("/api/query", {"id": self.cid, "commands": ["PID:GAINS? 1", "PID:LOOPMODE? 1"]})
        self.assertEqual([x["reply"] for x in r["results"]], ["OK 0 0 0", "OK 1"])     # the controller's boot state
        meta = {"channels": {"1": {"in_shot": True, "loop": "closed", "gains_kp_ki_kd": [0, 0, 0]}}}
        for gains, flagged in (("0 0 0", True), ("1 10 0", False)):
            self.send(f"PID:GAINS 1 {gains}")
            self.send("ARM")
            self.http.call("/api/record/arm", {"id": self.cid, "channels": [1], "duration_s": 1.5, "meta": meta})
            self.send("SHOT:STARt")
            rec = self.wait_rec(("done", "failed", "empty"))
            self.assertEqual(rec["state"], "done", rec)
            _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
            self.assertEqual(any("did not follow" in n for n in d["notes"]), flagged, d["notes"])
            self.http.wait(self.cid, lambda c: c["state"] == "IDLE", "IDLE")
            self.http.call("/api/record/cancel", {"id": self.cid})

    def test_hbridge_closed_loop_shot_is_read_back_in_checked_pages(self):
        self.connect("hbridge")
        self.send("CONFig:MODE LIMiter")
        for c in ("SHOT:BEGin", "SHOT:STEP 0 100000 20 20 20", "SHOT:STEP 100000 100000 80 80 80", "SHOT:END", "PID:LOOPMODE 1 1"):
            self.send(c)
        self.send("ARM")
        code, r = self.http.call("/api/record/arm", {"id": self.cid, "channels": [1], "duration_s": 0.5})
        self.assertEqual(code, 200, r)
        self.assertEqual(r["rec"]["warnings"], [])
        self.send("FIRE")
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        self.assertEqual(d["unit"], "native")
        self.assertFalse(d["can_amps"])
        self.assertEqual(len(d["channels"]), 1)
        self.assertGreater(d["summary"]["count"], 40)                          # more than one 40-sample page
        self.assertEqual(d["header"][3][0], "Supply")

    def test_hbridge_open_loop_warns_and_records_nothing(self):
        self.connect("hbridge")
        self.send("CONFig:MODE LIMiter")
        for c in ("SHOT:BEGin", "SHOT:STEP 0 100000 20 20 20", "SHOT:STEP 100000 100000 80 80 80", "SHOT:END", "ARM"):
            self.send(c)
        code, r = self.http.call("/api/record/arm", {"id": self.cid, "channels": [1, 2, 3], "duration_s": 0.5})
        self.assertEqual(code, 200)
        self.assertIn("open loop", " ".join(r["rec"]["warnings"]))
        self.assertEqual(r["rec"]["decim"], 18)                                # 0.5 s * 1.15 * 10 kHz over 333 samples = 17.3 -> 18
        self.send("FIRE")
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "empty")
        self.assertIn("open-loop", rec["message"])

    def test_disarm_before_the_shot_gives_an_empty_recording_not_a_crash(self):
        self.connect("xrex")
        self.send("SHOT:TIMing 0.5 0.5 0.5")
        self.send("ARM")
        self.http.call("/api/record/arm", {"id": self.cid, "channels": [1], "duration_s": 1.5})
        self.send("DISARM")
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "empty")

    def test_validation_and_cancel(self):
        self.connect("xrex")
        for body in ({"channels": [], "duration_s": 1}, {"channels": [9], "duration_s": 1}, {"channels": [1], "duration_s": 0},
                     {"channels": [1], "duration_s": "abc"}, {"channels": [1], "duration_s": 99999}):
            code, r = self.http.call("/api/record/arm", {"id": self.cid, **body})
            self.assertEqual(code, 409, body)
        code, r = self.http.call("/api/record/arm", {"id": self.cid, "channels": [1], "duration_s": 2})
        self.assertEqual(code, 200)
        code, r = self.http.call("/api/record/arm", {"id": self.cid, "channels": [1], "duration_s": 2})
        self.assertEqual(code, 409)                                            # one at a time
        code, r = self.http.call("/api/record/cancel", {"id": self.cid})
        self.assertEqual(r["rec"]["state"], "cancelled")
        code, r = self.http.call("/api/record/arm", {"id": self.cid, "channels": [1], "duration_s": 2})
        self.assertEqual(code, 200)

    def test_collect_now_reads_a_log_the_page_did_not_watch(self):
        self.connect("xrex")
        self.send("SHOT:TIMing 0.3 0.3 0.3")
        self.send("LOG:ARM 0 1000 1")
        self.send("ARM")
        self.send("SHOT:STARt")
        time.sleep(1.2)
        code, r = self.http.call("/api/record/collect", {"id": self.cid, "channels": [1]})
        self.assertEqual(code, 200, r)
        self.assertEqual(r["rec"]["state"], "done")


@unittest.skipUnless(hasattr(os, "openpty"), "needs a pseudo-terminal")
class SerialModeTests(UiBase):
    """A USB-serial cable, imitated with a pseudo-terminal that answers like the simulated controller."""

    def setUp(self):
        try:
            import serial  # noqa: F401
        except ImportError:
            self.skipTest("pyserial is not installed")
        super().setUp()
        self.sim = wham_sim.SimController("hbridge", 0, label="cable")
        self.master, slave = os.openpty()
        self.slave_name = os.ttyname(slave)
        self.addCleanup(os.close, slave)
        self.stop = False
        threading.Thread(target=self.pump, daemon=True).start()
        self.addCleanup(lambda: setattr(self, "stop", True))

    def pump(self):
        import select
        buf = b""
        while not self.stop:
            if not select.select([self.master], [], [], 0.1)[0]:
                continue
            try:
                buf += os.read(self.master, 256)
            except OSError:
                return
            while b"\n" in buf or b"\r" in buf:
                line, buf = buf.replace(b"\r", b"\n").split(b"\n", 1)
                if line.strip():
                    os.write(self.master, self.sim.dispatch(line.decode().strip()).encode() + b"\r\n")

    def test_identity_state_and_a_command_over_a_cable(self):
        code, st = self.http.call("/api/connect", {"mode": "serial", "device": self.slave_name, "name": "cable"})
        self.assertEqual(code, 200, st)
        c = self.http.wait(st["id"], lambda c: c["identified"] and c["state"], "identity and state over serial")
        self.assertEqual(c["product"], "hbridge")
        code, r = self.http.call("/api/send", {"id": st["id"], "command": "FAULT?"})
        self.assertEqual(r["reply"], "OK 0")
        code, r = self.http.call("/api/send", {"id": st["id"], "command": "ARM"})
        self.assertEqual(code, 409)


if __name__ == "__main__":
    unittest.main()

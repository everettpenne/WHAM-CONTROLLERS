"""The operator web UI's server (host/operator_ui/wham_ui.py): over HTTP, against the simulated controller (direct mode)
and against the real hub service with a fake controller behind it (hub mode)."""
import asyncio
import json
import os
import subprocess
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

    def test_host_log_is_for_hub_connections_only(self):
        code, r = self.http.call(f"/api/hostlog?id={self.cid}")
        self.assertEqual(code, 400)
        self.assertIn("through the host", r["error"])

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

    def send(self, cmd, confirmed=True):
        return self.http.call("/api/send", {"id": self.cid, "command": cmd, "confirmed": confirmed})[1]

    def ask(self, cmd):
        return self.http.call("/api/query", {"id": self.cid, "commands": [cmd]})[1]["results"][0]["reply"]

    def test_disconnect_all_closes_every_connection_and_leaves_the_controllers_alone(self):
        self.send("ARM")
        other = wham_sim.SimController("xrex", 0, label="t2")
        other.start()
        self.addCleanup(other.stop)
        code, st = self.http.call("/api/connect", {"mode": "direct", "host": "127.0.0.1", "port": other.port, "name": "sim2"})
        self.assertEqual(code, 200, st)
        code, r = self.http.call("/api/disconnect_all", {})
        self.assertEqual((code, r["closed"]), (200, 2))
        _, st = self.http.call("/api/state")
        self.assertEqual(st["connections"], [])
        self.assertEqual(self.sim.sim.state, "ARMED", "disconnecting must not change what the controller is doing")
        code, r = self.http.call("/api/disconnect_all", {})
        self.assertEqual((code, r["closed"]), (200, 0))
        code, _ = self.http.call("/api/disconnect_all", {}, token=False)
        self.assertEqual(code, 403)

    def test_a_shot_file_is_parsed_for_the_table_and_a_bad_one_is_explained(self):
        code, r = self.http.call("/api/shot/parse", {"text": "SHOT:STEP 0 20086 50\nSHOT:STEP 1000 0 0\nSHOT:STEP 2000 20100 50\nSHOT:STEP 3000 20100 50\n", "name": "../x<y>.csv"})
        self.assertEqual(code, 200, r)
        self.assertEqual(len(r["rows"]), 4)
        self.assertTrue(r["rows"][1]["off"])
        self.assertIn("4 setpoints, 3 ms", r["summary"])
        self.assertNotIn("<", r["summary"])
        self.assertNotIn("/", r["summary"].split(":")[0])
        code, r = self.http.call("/api/shot/parse", {"text": "hello world"})
        self.assertEqual(code, 422)
        self.assertIn("does not look like a shot", r["error"])
        code, r = self.http.call("/api/shot/parse", {"text": "0 20000 50\n"}, token=False)
        self.assertEqual(code, 403)
        _, r = self.http.call("/api/query", {"id": self.cid, "commands": ["SHOT?"]})
        self.assertTrue(r["results"][0]["reply"].startswith("OK 0 "), "parsing a file sends nothing to the controller")

    def test_help_pages_are_listed_and_served(self):
        code, r = self.http.call("/api/help", token=False)
        self.assertEqual(code, 200)
        for page in r["pages"]:
            self.assertTrue(page["title"])
            with urllib.request.urlopen(f"{self.http.base}/static/help/{page['file']}") as resp:
                self.assertEqual(resp.status, 200)
                self.assertIn("text/html", resp.headers["Content-Type"])
                self.assertIn(b"<svg", resp.read())
        self.assertEqual(sorted(p["file"] for p in r["pages"]), sorted(wham_ui.HELP_TITLES), "the console ships both command flowcharts")

    def test_an_off_setpoint_uploads_and_reads_back(self):
        self.send("CONFig:MODE HVPS")
        for cmd in ("SHOT:BEGin", "SHOT:STEP 0 20000 50", "SHOT:STEP 4000 20000 50", "SHOT:STEP 4010 0 0", "SHOT:STEP 8000 21000 50", "SHOT:STEP 12000 21000 50"):
            self.assertTrue(self.send(cmd)["ok"], cmd)
        self.assertEqual(self.send("SHOT:END")["reply"], "OK 5 12000")
        self.assertEqual(self.ask("SHOT:POINt? 2").split()[:3], ["OK", "4010", "0"])
        self.send("SHOT:BEGin")
        self.assertFalse(self.send("SHOT:STEP 0 0 0")["ok"], "the first setpoint cannot be an off one")

    def test_fault_inputs_latch_a_fault_and_say_which(self):
        self.assertEqual(self.ask("FAULT:SOURces?"), "OK PC10=0 GATEDRV=0 TABLE=0 TEST=0 OVP=0 FB=0 SPARK=0")
        self.assertTrue(self.ask("GDS?").startswith("OK 01=HIGH 02=HIGH"))
        self.sim.set_pin(3, 0)                                               # normally HIGH: a LOW input is a fault
        c = self.http.wait(self.cid, lambda c: c["state"] == "FAULT", "FAULT from a gate-driver input")
        self.assertIn("GATEDRV=1", self.ask("FAULT:SOURces?"))
        self.assertIn("03=LOW", self.ask("GDS?"))
        self.assertEqual(self.ask("GDS:LAST?").split()[1], "FFB")            # the levels at the fault: every input HIGH but input 3
        self.assertFalse(self.send("FAULT:CLEar")["reply"] == "OK" and self.ask("STATE?") == "OK IDLE", "a fault that is still there does not clear")
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "GDS:ENAble 3 0"})
        self.assertEqual(code, 409, "switching a fault input off must ask first")
        self.assertEqual(r["category"], "interlock")
        self.assertTrue(self.send("GDS:ENAble 3 0")["ok"])
        self.assertEqual(self.ask("GDS:ENAble?").split()[3], "03=0")
        self.send("FAULT:CLEar")
        self.http.wait(self.cid, lambda c: c["state"] == "IDLE", "IDLE once the input is switched off")
        self.assertTrue(self.send("GDS:NICKname 3 PhaseU_Hi")["ok"])
        self.assertIn("03=PhaseU_Hi", self.ask("GDS:NICKname?"))
        self.assertFalse(self.send("GDS:NICKname 3 a_name_longer_than_15")["ok"])

    def test_the_spark_gap_input_and_the_polarity(self):
        self.sim.set_pin("spark", 0)
        self.http.wait(self.cid, lambda c: c["state"] == "FAULT", "FAULT from the spark-gap input")
        self.assertIn("SPARK=1", self.ask("FAULT:SOURces?"))
        self.assertEqual(self.ask("SPARKgap:INPut?"), "OK 0")
        self.assertTrue(self.send("SPARKgap:ENAble 0")["ok"])
        self.send("FAULT:CLEar")
        self.http.wait(self.cid, lambda c: c["state"] == "IDLE", "IDLE with the spark-gap input off")
        self.assertEqual(self.ask("GDS:POLarity?"), "OK NORMALLY_HIGH")
        code, _ = self.http.call("/api/send", {"id": self.cid, "command": "GDS:POLarity NORMALLY_LOW"})
        self.assertEqual(code, 409)
        self.send("GDS:POLarity NORMALLY_LOW")                               # every input reads HIGH, so each is now in fault
        self.http.wait(self.cid, lambda c: c["state"] == "FAULT", "FAULT after the polarity change")

    def test_the_bypass_keeps_a_bad_input_from_latching(self):
        code, _ = self.http.call("/api/send", {"id": self.cid, "command": "DEBUG:FAULT:BYPASS 1"})
        self.assertEqual(code, 409)
        self.send("DEBUG:FAULT:BYPASS 1")
        self.sim.set_pin(1, 0)
        time.sleep(0.3)
        self.assertEqual(self.ask("STATE?"), "OK IDLE")
        self.assertEqual(self.ask("DEBUG:FAULT:BYPASS?"), "OK 1")
        self.send("DEBUG:FAULT:BYPASS 0", confirmed=False)                   # turning the protection back on needs no confirmation
        self.http.wait(self.cid, lambda c: c["state"] == "FAULT", "FAULT once the bypass is off")

    def test_dead_time_is_confirmed_and_read_back_as_the_timer_holds_it(self):
        self.assertEqual(self.ask("CONFig:DEADtime?"), "OK 100")
        code, r = self.http.call("/api/send", {"id": self.cid, "command": "CONFig:DEADtime 150"})
        self.assertEqual((code, r["category"]), (409, "limits"))
        self.assertTrue(self.send("CONFig:DEADtime 150")["ok"])
        self.assertEqual(self.ask("CONFig:DEADtime?"), "OK 153")
        for bad in ("49", "3001", "abc"):
            self.assertFalse(self.send(f"CONFig:DEADtime {bad}")["ok"], bad)

    def test_saved_profiles_round_trip(self):
        self.assertEqual(self.ask("CONFig:PROFile:CATalog?").split()[1:3], ["1=empty", "2=empty"])
        self.send("CONFig:MODE HVPS")
        for cmd in ("SHOT:BEGin", "SHOT:STEP 0 20000 50", "SHOT:STEP 5000 20000 50", "SHOT:END"):
            self.send(cmd)
        self.assertEqual(self.send("*SAV 2 two-point")["reply"], "OK")
        self.assertIn("2=two-point/", self.ask("CONFig:PROFile:CATalog?"))
        self.assertEqual(self.ask("CONFig:PROFile:DIFF? 2"), "OK SAME")
        self.send("CONFig:DEADtime 200")
        self.send("GDS:ENAble 5 0")
        diff = self.ask("CONFig:PROFile:DIFF? 2")
        self.assertIn("DEADtime=200>100", diff)
        self.assertIn("GDS:ENAble=", diff)
        reply = self.send("*RCL 2")["reply"]
        self.assertTrue(reply.startswith("OK APPLIED="), reply)
        self.assertIn("FAILED=0", reply)
        self.assertEqual(self.ask("CONFig:DEADtime?"), "OK 100")
        self.assertEqual(self.ask("GDS:ENAble?").split()[5], "05=1")
        self.assertEqual(self.ask("CONFig:PROFile:DIFF? 2"), "OK SAME")
        self.send("GDS:ENAble 5 0")
        self.send("*SAV 3")
        self.send("GDS:ENAble 5 1")
        self.assertIn("GDSOFF=1", self.send("*RCL 3")["reply"])              # a recall that leaves an input off says so
        self.send("ARM")
        self.assertTrue(self.send("*RCL 2")["reply"].startswith("ERR 13"))   # only while IDLE
        self.send("DISARM")
        self.assertTrue(self.send("CONFig:PROFile:CLEar 2")["ok"])
        self.assertTrue(self.ask("CONFig:PROFile:DIFF? 2").startswith("ERR 18"))
        self.assertFalse(self.send("*SAV 9")["ok"])
        self.assertFalse(self.send("*SAV 1 a_label_longer_than_15")["ok"])

    def test_start_latency_is_reported_after_a_shot(self):
        self.assertTrue(self.ask("EXTernal:TRIGger:LATency?").startswith("ERR 13"))
        self.send("CONFig:MODE HVPS")
        for cmd in ("SHOT:BEGin", "SHOT:STEP 0 20000 50", "SHOT:STEP 5000 20000 50", "SHOT:END", "ARM", "FIRE"):
            self.assertTrue(self.send(cmd)["ok"], cmd)
        kv = dict(t.split("=") for t in self.ask("EXTernal:TRIGger:LATency?").split()[1:])
        self.assertEqual(kv["prepared"], "1")
        self.assertEqual(kv["total_us"], "NA")                               # started by FIRE, not by the trigger edge
        self.assertGreater(float(kv["start_us"]), 0)


class QuitTests(UiBase):
    def test_quit_answers_then_stops_serving(self):
        code, _ = self.http.call("/api/quit", {}, token=False)
        self.assertEqual(code, 403)
        code, r = self.http.call("/api/quit", {})
        self.assertEqual((code, r["ok"]), (200, True))
        end = time.time() + 5
        while time.time() < end:
            try:
                self.http.call("/api/state")
            except (urllib.error.URLError, ConnectionError, OSError):
                return
            time.sleep(0.1)
        self.fail("the console kept serving after Quit")


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

    def test_host_log_shows_recent_activity_and_filters(self):
        self.http.call("/api/send", {"id": self.cid, "command": "ECHO findme"})
        code, r = self.http.call(f"/api/hostlog?id={self.cid}&n=50")
        self.assertEqual(code, 200)
        lines = [x["line"] for x in r["rows"]]
        self.assertIn("ECHO findme", lines)
        self.assertEqual(r["info"]["dev"], "WB-0001-8")
        self.assertEqual(r["info"]["rows"], len(r["rows"]))
        code, r = self.http.call(f"/api/hostlog?id={self.cid}&kind=cmd&grep=FINDME")
        self.assertEqual([x["line"] for x in r["rows"]], ["ECHO findme"])
        self.assertTrue(r["rows"][0]["client"])
        code, r = self.http.call(f"/api/hostlog?id={self.cid}&grep=zzz-not-there")
        self.assertEqual((code, r["rows"]), (200, []))

    def test_host_log_arguments_are_checked_before_they_reach_the_host(self):
        for bad in ("since=soon", "kind=cmd%0A!hub%20confirm", "client=a%20b", "n=ten", "before=x"):
            code, r = self.http.call(f"/api/hostlog?id={self.cid}&{bad}")
            self.assertEqual(code, 400, bad)
        self.assertNotIn("!hub confirm", self.ctl.received)

    def test_host_log_arguments_are_composed_in_the_order_the_host_wants(self):
        q = {"n": ["100"], "since": ["2h"], "kind": ["cmd,reply"], "client": ["bob"], "all": ["1"], "grep": ["a b\nc"]}
        self.assertEqual(wham_ui.host_log_args(q), "100 since=2h kind=cmd,reply client=bob all grep=a b c")


class PfmRecordingTests(UiBase):
    """Shot data with the PFM_Input capture: armed before the shot, read back after, open loop or closed, over a lossy link."""
    SHOT = ("SHOT:BEGin", "SHOT:STEP 0 100000 20 50 80", "SHOT:STEP 10000 100000 80 50 20", "SHOT:END")

    def connect(self, product="hbridge"):
        self.sim = wham_sim.SimController(product, 0, label="t", min_fire_s=0.4)
        self.sim.start()
        self.addCleanup(self.sim.stop)
        code, st = self.http.call("/api/connect", {"mode": "direct", "host": "127.0.0.1", "port": self.sim.port, "name": "sim"})
        self.cid = st["id"]
        self.http.wait(self.cid, lambda c: c["identified"] and c["state"], "identity")

    def send(self, cmd):
        code, r = self.http.call("/api/send", {"id": self.cid, "command": cmd, "confirmed": True})
        self.assertEqual(code, 200, (cmd, r))
        self.assertTrue(r["ok"], (cmd, r))

    def upload(self):
        self.send("CONFig:MODE LIMiter")
        for c in self.SHOT:
            self.send(c)

    def wait_rec(self, states, secs=20):
        return self.http.wait(self.cid, lambda c: c["rec"] and c["rec"]["state"] in states, f"recording in {states}", secs)["rec"]

    def arm_record(self, **kw):
        body = {"id": self.cid, "channels": [], "duration_s": 0.01, "pfm": {"inputs": [1, 2, 3], "edges": "both", "cycles": None}}
        body.update(kw)
        return self.http.call("/api/record/arm", body)

    def shot(self):
        self.send("ARM")
        self.http.wait(self.cid, lambda c: c["rec"] and c["rec"]["state"] == "armed", "recording armed")      # as an operator would: the page sees ARMED first
        self.send("FIRE")

    def file(self, sid, name):
        req = urllib.request.Request(f"{self.http.base}/api/shot/file?sid={sid}&name={name}", headers={"X-WHAM-Token": self.app.token})
        with urllib.request.urlopen(req) as resp:
            return resp.read().decode()

    def test_open_loop_both_edge_capture_is_recorded_reported_and_the_modes_put_back(self):
        self.connect()
        self.upload()
        code, r = self.arm_record()
        self.assertEqual(code, 200, r)
        self.assertTrue(r["rec"]["pfm"])
        self.assertEqual(self.sim.sim.pfm_modes[:3], ["B", "B", "B"])
        self.assertTrue(1000 < r["rec"]["pfm_cycles"] < 1100)
        self.shot()
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        self.assertIn("PFM capture of inputs 1, 2, 3", rec["message"])
        self.assertEqual(self.sim.sim.pfm_modes, ["R"] * 6, "the capture modes this page changed are put back")
        code, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        self.assertEqual(d["channels"], [])
        self.assertEqual([i["n"] for i in d["pfm"]["inputs"]], [1, 2, 3])
        self.assertIn("pfm.csv", d["files"])
        self.assertNotIn("shot.csv", d["files"])
        self.assertIn("<svg", d["pfm"]["svg_duty"])
        self.assertIn("<svg", d["pfm"]["svg_freq"])
        stats = dict(d["pfm"]["inputs"][0]["stats"])
        self.assertIn("measured minus programmed", " ".join(stats))
        err = stats["Duty, measured minus programmed"]
        self.assertTrue(err.startswith("mean -2.0") or err.startswith("mean -1.9"), err)
        self.assertTrue(stats["Cycles"].startswith("1001 over"), stats["Cycles"])
        csv_text = self.file(rec["sid"], "pfm.csv")
        self.assertEqual(len(csv_text.strip().split("\n")), 1 + 3 * 1001)
        self.assertIn("PFM input capture", self.file(rec["sid"], "report.html"))
        self.assertIn("PFM Input 3 (phase W)", self.file(rec["sid"], "report.md"))
        with open(os.path.join(self.tmp.name, "shots", rec["sid"], "summary.json")) as f:
            self.assertEqual(json.load(f)["pfm_inputs"], [1, 2, 3])
        code, _ = self.http.call(f"/api/shot/file?sid={rec['sid']}&name=shot.csv")
        self.assertEqual(code, 404)                                                # no waveform log, so no such file

    def test_rising_edge_capture_gives_carrier_only(self):
        self.connect()
        self.upload()
        self.assertEqual(self.arm_record(pfm={"inputs": [1], "edges": "rising", "cycles": 500})[0], 200)
        self.assertEqual(self.sim.sim.pfm_modes[0], "R")
        self.shot()
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        inp = d["pfm"]["inputs"][0]
        stats = dict(inp["stats"])
        self.assertIn("rising", stats["Capture"])
        self.assertNotIn("Duty (measured)", stats)
        self.assertEqual(d["pfm"]["svg_duty"], "")
        self.assertTrue(stats["Cycles"].startswith("500 over"))
        self.assertIn("the capture filled", " ".join(d["notes"] + d["pfm"]["notes"]))        # 500 cycles asked, the shot is longer

    def test_lost_characters_on_the_link_are_asked_again(self):
        self.connect()
        self.upload()
        self.sim.sim.pfm_glitch_every = 4                                            # every 4th page reply loses a character
        self.arm_record()
        self.shot()
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        self.assertTrue(dict(d["pfm"]["inputs"][2]["stats"])["Cycles"].startswith("1001 over"))

    def test_waveform_log_and_pfm_in_one_recording(self):
        self.connect()
        self.upload()
        self.send("PID:LOOPMODE 1 1")
        self.assertEqual(self.arm_record(channels=[1], pfm={"inputs": [1], "edges": "both", "cycles": None})[0], 200)
        self.shot()
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        self.assertEqual(len(d["channels"]), 1)
        self.assertEqual(len(d["pfm"]["inputs"]), 1)
        self.assertIn("shot.csv", d["files"])
        self.assertIn("pfm.csv", d["files"])

    def test_open_loop_log_is_empty_but_the_pfm_capture_is_kept(self):
        self.connect()
        self.upload()
        code, r = self.arm_record(channels=[1])
        self.assertIn("open loop", " ".join(r["rec"]["warnings"]))
        self.assertIn("recorded anyway", " ".join(r["rec"]["warnings"]))
        self.shot()
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        self.assertEqual(d["channels"], [])
        self.assertIn("waveform log is empty", " ".join(d["notes"]))

    def test_a_mode_change_while_armed_is_refused_with_the_reason(self):
        self.connect()
        self.upload()
        self.send("ARM")
        code, r = self.arm_record()
        self.assertEqual(code, 409)
        self.assertIn("DISARM", r["error"])
        self.assertEqual(self.sim.sim.pfm_modes, ["R"] * 6)

    def test_inputs_already_in_the_wanted_mode_need_no_change_while_armed(self):
        self.connect()
        self.upload()
        self.send("PFMIN:EDGes 1 BOTH")
        self.send("ARM")
        code, r = self.arm_record(pfm={"inputs": [1], "edges": "both", "cycles": 300})
        self.assertEqual(code, 200, r)
        self.assertEqual(self.sim.sim.pfm_modes[0], "B")                              # left as the operator had set it

    def test_cancelling_puts_the_modes_back(self):
        self.connect()
        self.upload()
        self.arm_record()
        self.assertEqual(self.sim.sim.pfm_modes[:3], ["B"] * 3)
        code, r = self.http.call("/api/record/cancel", {"id": self.cid})
        self.assertEqual(r["rec"]["state"], "cancelled")
        self.assertEqual(self.sim.sim.pfm_modes, ["R"] * 6)

    def test_disarming_before_the_shot_gives_an_empty_recording(self):
        self.connect()
        self.upload()
        self.arm_record()
        self.send("ARM")
        self.http.wait(self.cid, lambda c: c["rec"] and c["rec"]["state"] == "armed", "armed recording")
        self.send("DISARM")
        rec = self.wait_rec(("empty", "done", "failed"))
        self.assertEqual(rec["state"], "empty", rec)
        self.assertIn("PFM capture is empty", rec["message"])
        self.assertEqual(self.sim.sim.pfm_modes, ["R"] * 6)

    def test_bad_requests_are_explained(self):
        self.connect()
        self.upload()
        for kw, word in (({"pfm": {"inputs": [], "edges": "both"}}, "at least one PFM input"),
                         ({"pfm": {"inputs": [9], "edges": "both"}}, "PFM input"),
                         ({"pfm": {"inputs": [1], "edges": "falling"}}, "edges"),
                         ({"pfm": {"inputs": [1], "edges": "both", "cycles": 99999}}, "3800"),
                         ({"pfm": None}, "at least one phase")):
            code, r = self.arm_record(**kw)
            self.assertEqual(code, 409, (kw, r))
            self.assertIn(word, r["error"])
        self.assertEqual(self.sim.sim.pfm_modes, ["R"] * 6, "a refused request leaves the modes alone")

    def test_xrex_has_no_pfm_capture(self):
        self.connect("xrex")
        code, r = self.arm_record(channels=[1])
        self.assertEqual(code, 409)
        self.assertIn("HBRIDGE", r["error"])

    def test_read_the_capture_now_for_a_shot_this_page_did_not_arm(self):
        self.connect()
        self.upload()
        self.send("PFMIN:EDGes 2 BOTH")
        self.send("PFMIN:CAPTURE 300")
        self.send("ARM")
        self.send("FIRE")
        time.sleep(0.2)
        self.http.wait(self.cid, lambda c: c["state"] == "IDLE", "shot over")
        code, r = self.http.call("/api/record/collect", {"id": self.cid, "channels": [], "pfm": {"inputs": [2], "edges": "both"}})
        self.assertEqual(code, 200, r)
        rec = self.wait_rec(("done", "failed", "empty"))
        self.assertEqual(rec["state"], "done", rec)
        _, d = self.http.call(f"/api/shot?sid={rec['sid']}")
        self.assertEqual([i["n"] for i in d["pfm"]["inputs"]], [2])
        self.assertTrue(dict(d["pfm"]["inputs"][0]["stats"])["Cycles"].startswith("300 over"))


class TunnelTests(UiBase):
    """The start screen's tunnel button. A stand-in for ssh (a Python one-liner that listens on the ports) is used, so no
    network or key is needed; the real command line is checked separately."""

    FAKE = ("import socket,sys,time\nss=[]\nfor p in map(int,sys.argv[1:]):\n s=socket.socket(); s.bind(('127.0.0.1',p)); s.listen(1); ss.append(s)\ntime.sleep(60)\n")

    def setUp(self):
        super().setUp()
        self.ports = [free_port(), free_port()]
        self.app.tunnel.ports_fn = lambda: self.ports
        self.app.tunnel.ssh = "x"

    def fake(self, ports):
        return [sys.executable, "-c", self.FAKE] + [str(p) for p in ports]

    def test_the_real_command_line_forwards_each_port_without_asking_questions(self):
        t = wham_ui.Tunnel(host="lab", ports_fn=lambda: [5101, 5102], ssh="/usr/bin/ssh")
        cmd = t.command([5101, 5102])
        self.assertEqual(cmd[0], "/usr/bin/ssh")
        self.assertIn("BatchMode=yes", cmd)
        self.assertIn("ExitOnForwardFailure=yes", cmd)
        self.assertEqual(cmd[-1], "lab")
        self.assertEqual([cmd[i + 1] for i, a in enumerate(cmd) if a == "-L"], ["5101:127.0.0.1:5101", "5102:127.0.0.1:5102"])

    def test_open_then_close_from_the_page_and_state_reports_it(self):
        self.app.tunnel.command = self.fake
        code, st = self.http.call("/api/state")
        self.assertEqual(st["tunnel"]["state"], "closed")
        code, r = self.http.call("/api/tunnel", {"on": True})
        self.assertEqual((code, r["tunnel"]["state"]), (200, "open"), r)
        code, st = self.http.call("/api/state")
        self.assertEqual(st["tunnel"]["state"], "open")
        self.assertEqual(st["tunnel"]["ports"], sorted(self.ports))
        code, r = self.http.call("/api/tunnel", {"on": False})
        self.assertEqual(r["tunnel"]["state"], "closed")
        self.assertFalse(any(wham_ui.port_in_use(p) for p in self.ports))

    def test_a_tunnel_that_is_already_up_is_recognised_and_not_closed_by_the_page(self):
        outside = subprocess.Popen(self.fake(self.ports))
        self.addCleanup(lambda: (outside.kill(), outside.wait()))
        end = time.time() + 5
        while time.time() < end and not all(wham_ui.port_in_use(p) for p in self.ports):
            time.sleep(0.05)
        _, st = self.http.call("/api/state")
        self.assertEqual(st["tunnel"]["state"], "open-elsewhere")
        self.http.call("/api/tunnel", {"on": False})
        self.assertTrue(all(wham_ui.port_in_use(p) for p in self.ports), "a tunnel the page did not start must stay up")

    def test_a_failing_ssh_reports_its_own_message(self):
        self.app.tunnel.command = lambda ports: [sys.executable, "-c", "import sys; sys.stderr.write('Permission denied (publickey).\\n'); sys.exit(255)"]
        _, r = self.http.call("/api/tunnel", {"on": True})
        self.assertEqual(r["tunnel"]["state"], "closed")
        self.assertIn("Permission denied (publickey)", r["tunnel"]["error"])

    def test_a_port_held_by_something_else_is_explained(self):
        import socket as _s
        blocker = _s.socket(); blocker.bind(("127.0.0.1", self.ports[0])); blocker.listen(1)
        self.addCleanup(blocker.close)
        self.app.tunnel.command = self.fake
        _, r = self.http.call("/api/tunnel", {"on": True})
        self.assertEqual(r["tunnel"]["state"], "partial")                  # one of the two ports answers: not a whole tunnel
        self.assertEqual(r["tunnel"]["listening"], [self.ports[0]])
        self.assertIn("already used", r["tunnel"]["error"])
        self.assertFalse(wham_ui.port_in_use(self.ports[1]), "nothing was started")

    def outside(self, ports):
        """A tunnel started outside the console (the ssh stand-in as a separate process), listening on the given ports."""
        proc = subprocess.Popen(self.fake(ports))
        self.addCleanup(lambda: (proc.kill(), proc.wait()))
        end = time.time() + 5
        while time.time() < end and not all(wham_ui.port_in_use(p) for p in ports):
            time.sleep(0.05)
        return proc

    def test_a_tunnel_opened_elsewhere_is_closed_only_on_an_explicit_request_and_only_if_it_is_ssh(self):
        proc = self.outside(self.ports)
        ended = []
        self.app.tunnel.listeners_fn = lambda ports: {p: (proc.pid, "ssh") for p in ports}
        self.app.tunnel.end_fn = lambda pid: (ended.append(pid), proc.terminate(), True)[-1]
        _, st = self.http.call("/api/state")
        t = st["tunnel"]
        self.assertEqual((t["state"], t["closable"]), ("open-elsewhere", True))
        self.assertEqual(t["foreign"], [{"pid": proc.pid, "name": "ssh", "ports": sorted(self.ports), "ssh": True}])
        self.http.call("/api/tunnel", {"on": False})                        # the ordinary Close: leaves it
        self.assertEqual(ended, [])
        _, r = self.http.call("/api/tunnel", {"on": False, "force": True})
        self.assertEqual(ended, [proc.pid])
        self.assertEqual((r["tunnel"]["state"], r["tunnel"]["error"]), ("closed", None), r)
        self.assertFalse(any(wham_ui.port_in_use(p) for p in self.ports))

    def test_a_port_held_by_another_program_is_named_and_left_alone(self):
        proc = self.outside(self.ports)
        ended = []
        self.app.tunnel.listeners_fn = lambda ports: {p: (proc.pid, "nginx") for p in ports}
        self.app.tunnel.end_fn = lambda pid: ended.append(pid) or True
        _, st = self.http.call("/api/state")
        self.assertFalse(st["tunnel"]["closable"])
        _, r = self.http.call("/api/tunnel", {"on": False, "force": True})
        self.assertEqual(ended, [], "a program that is not ssh must never be stopped")
        self.assertEqual(r["tunnel"]["state"], "open-elsewhere")
        self.assertIn("nginx", r["tunnel"]["error"])
        self.assertIn("not ssh", r["tunnel"]["error"])
        self.assertTrue(all(wham_ui.port_in_use(p) for p in self.ports))

    def test_a_tunnel_for_one_controller_is_reported_as_partial_and_can_be_closed(self):
        proc = self.outside(self.ports[:1])
        self.app.tunnel.listeners_fn = lambda ports: {p: (proc.pid, "ssh") for p in ports}
        self.app.tunnel.end_fn = lambda pid: (proc.terminate(), True)[-1]
        _, st = self.http.call("/api/state")
        t = st["tunnel"]
        self.assertEqual((t["state"], t["listening"], t["closable"]), ("partial", self.ports[:1], True))
        _, r = self.http.call("/api/tunnel", {"on": False, "force": True})
        self.assertEqual(r["tunnel"]["state"], "closed")

    def test_an_unknown_holder_is_reported_not_guessed(self):
        self.outside(self.ports)
        self.app.tunnel.listeners_fn = lambda ports: {}                     # the system tools found nothing (no lsof, another user's process)
        self.app.tunnel.end_fn = lambda pid: self.fail("nothing may be stopped")
        _, r = self.http.call("/api/tunnel", {"on": False, "force": True})
        self.assertEqual(r["tunnel"]["state"], "open-elsewhere")
        self.assertIn("could not find which program", r["tunnel"]["error"])

    def test_the_listener_lookup_finds_this_computers_own_listening_process(self):
        proc = self.outside(self.ports[:1])
        found = wham_ui.find_listeners(self.ports)
        if not found:
            self.skipTest("no lsof/ss/netstat on this computer")
        self.assertEqual(list(found), self.ports[:1])
        self.assertEqual(found[self.ports[0]][0], proc.pid)
        self.assertFalse(wham_ui.is_ssh(found[self.ports[0]][1]), found)     # it is a Python process, so the page would not close it
        for name, want in (("ssh", True), ("ssh.exe", True), ("/usr/bin/ssh", True), ("sshd", False), ("python3", False), ("", False)):
            self.assertEqual(wham_ui.is_ssh(name), want, name)

    def test_the_ports_follow_the_saved_hub_entries_and_the_host_is_not_settable_from_the_page(self):
        app = wham_ui.App(presets_file=os.path.join(self.tmp.name, "p2.json"), shots_dir=self.tmp.name, tunnel_host="labhost")
        self.assertEqual(app.tunnel.ports(), [5101, 5102, 5103])
        app.save_presets([{"name": "a", "mode": "hub", "host": "127.0.0.1", "port": 6001}, {"name": "b", "mode": "hub", "host": "10.0.0.5", "port": 6002}])
        self.assertEqual(app.tunnel.ports(), [6001])
        self.assertEqual(app.tunnel.host, "labhost")
        code, r = self.http.call("/api/tunnel", {"on": False, "host": "evil"})
        self.assertEqual(code, 200)


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

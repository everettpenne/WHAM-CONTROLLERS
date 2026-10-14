"""wham_hub.py against a fake controller (an asyncio TCP server that answers like the firmware)."""
import asyncio
import datetime
import json
import os
import socket
import sys
import tempfile
import time
import unittest
import unittest.mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import wham_hub as wh
from test_dangerous_commands import table_patterns


class FakeController:
    """Answers like the firmware: one reply line per command, nothing for a blank line, `ECHO x` after delays[x] seconds."""

    def __init__(self, serial="WB-0001-8", uid="0055002B4232500220343558", identity="ok", verified="1"):
        self.serial, self.uid, self.identity, self.verified = serial, uid, identity, verified
        self.received = []
        self.timeline = []           # (monotonic time, "rx" | "tx", first line)
        self.delays = {}
        self.mute = False            # swallow every command, keep the TCP connection open
        self.silent = set()          # commands swallowed the same way
        self.server = None
        self.writers = []

    def idline(self):
        return (f"OK serial={self.serial} uid={self.uid} flashuid=E4653C34CB172E22 product=WHAM-FAKE hw=REVA fw=v1 "
                f"commit=abc label=- verified={self.verified} identity={self.identity}")

    async def start(self, port=0):
        self.server = await asyncio.start_server(self.handle, "127.0.0.1", port)
        return self.server.sockets[0].getsockname()[1]

    async def stop(self):
        self.server.close()
        for w in self.writers:
            w.close()
        await self.server.wait_closed()

    noise = ""
    stray = ""                       # sent just before the *IDN? reply: leftover text on the link
    resets_on_idn = 0                # this many *IDN? get a !BOOT banner instead of a reply

    def banner(self):
        return (f"{self.noise}!BOOT WHAM-FAKE v1 abc BANK=1 STATE=IDLE tick=4 SN={self.serial} UID={self.uid}\r\n"
                "!BOOT diag boot=1\r\n!BOOT Rise and shine\r\n")

    def say(self, writer, text):
        self.timeline.append((time.monotonic(), "tx", text.split("\r\n")[0]))
        writer.write(text.encode())

    async def push(self, text):
        """An unsolicited line to every connected hub."""
        for w in self.writers:
            self.say(w, text + "\r\n")
            await w.drain()

    async def handle(self, reader, writer):
        self.writers.append(writer)
        inbox = asyncio.Queue()

        async def rx():
            while True:
                line = await reader.readline()
                if not line:
                    inbox.put_nowait(None)
                    return
                for piece in line.decode().rstrip("\r\n").split("\r"):     # a lone CR ends a line, as in the firmware
                    cmd = piece.strip()
                    self.received.append(cmd)
                    self.timeline.append((time.monotonic(), "rx", cmd))
                    inbox.put_nowait(cmd)
        rx_task = asyncio.ensure_future(rx())
        try:
            while True:
                cmd = await inbox.get()
                if cmd is None:
                    break
                if cmd and not self.mute and cmd not in self.silent:
                    await self.answer(cmd, writer)
                    await writer.drain()
        except ConnectionError:
            pass                                     # the hub closed the link
        finally:
            rx_task.cancel()

    async def answer(self, cmd, writer):
        say = lambda t: self.say(writer, t)
        if cmd.upper() == "*IDN?" and self.resets_on_idn:
            self.resets_on_idn -= 1
            say(self.banner())                       # resets instead of replying
        elif cmd.upper() == "*IDN?":                 # mnemonics ignore case, as in the firmware
            await asyncio.sleep(self.delays.get("*IDN?", 0))
            say(self.stray)
            say(f"OK WHAM-FAKE REVA v1 abc SN={self.serial} UID={self.uid}\r\n")
        elif cmd == "SYS:ID?":
            say(self.idline() + "\r\n")
        elif cmd == "STATE?":
            say("OK IDLE\r\n")
        elif cmd == "REBOOT":
            say("OK\r\n" + self.banner())
        elif cmd == "DIE":
            await asyncio.sleep(self.delays.get("DIE", 0))
            say(self.banner())                       # resets without replying
        elif cmd == "HANGUP":
            writer.close()
        elif cmd == "DIEHANG":
            say(self.banner())
            await writer.drain()
            writer.close()
        elif cmd == "DUMP?":
            say("OK 3\r\n1 2 3\r\n")
        elif cmd == "SYS:EVLOG?":
            say("OK 2\r\n")
            for ev in ("!EVT 1 STATE IDLE", "!EVT 2 STATE ARMED"):
                await asyncio.sleep(0.1)
                say(ev + "\r\n")
        elif cmd == "SYS:EVLOG? reset":
            say("OK 3\r\n!EVT 1 STATE IDLE\r\n" + self.banner())
        elif cmd == "SYS:EVLOG? short":
            say("OK 5\r\n!EVT 1 STATE IDLE\r\n")
        elif cmd == "CONFig:PROFile:CATalog?":
            say("OK " + " ".join(f"{i}=empty" for i in range(1, 9)) + "\r\n")
        elif cmd == "CONFig:PROFile:CATalog? broken":
            say("OK 1=empty 2=x/y\r\nERR 24 slot 3: flash read failed\r\n")
        elif cmd.startswith("LOG:DATA?"):
            await asyncio.sleep(self.delays.get("LOG:DATA?", 0))
            say("OK 3 1 2 3\r\n")
        elif cmd == "GARBLE?":
            say("O\r\n")                            # a byte lost on the bridge
        elif cmd == "NOREPLY":
            pass
        elif cmd.startswith("ECHO "):
            what = cmd[5:]
            await asyncio.sleep(self.delays.get(what, 0))
            say(f"OK {what}\r\n")
        elif cmd.startswith("BIG "):
            for i in range(int(cmd[4:])):
                say("!EVT 1 BIGLINE " + "x" * 1000 + "\r\n")
                await writer.drain()
            say("OK\r\n")
        else:
            say("ERR 1 Unknown command\r\n")


class Client:
    opened = []

    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        Client.opened.append(self)

    @classmethod
    async def connect(cls, port, name, quiet=False):
        r, w = await asyncio.open_connection("127.0.0.1", port)
        c = cls(r, w)
        w.write(f"@{'quiet' if quiet else 'name'} {name}\r\n".encode())
        return c

    async def send(self, text):
        self.writer.write((text + "\r\n").encode())
        await self.writer.drain()

    async def quiet(self, wait=0.25):
        """Everything that arrives until the line goes quiet for `wait` seconds."""
        out = []
        try:
            while True:
                out.append((await asyncio.wait_for(self.reader.readline(), wait)).decode().strip())
        except asyncio.TimeoutError:
            return out

    async def lines_until(self, predicate, timeout=3.0):
        out = []
        end = asyncio.get_running_loop().time() + timeout
        while True:
            left = end - asyncio.get_running_loop().time()
            if left <= 0:
                raise AssertionError(f"timed out; saw {out}")
            line = (await asyncio.wait_for(self.reader.readline(), left)).decode().strip()
            out.append(line)
            if predicate(line):
                return out


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class HubBase(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.ctl = FakeController()
        self.ctl_port = await self.ctl.start()
        self.listen = free_port()

    async def asyncTearDown(self):
        for c in Client.opened:
            c.writer.close()
        Client.opened.clear()
        await asyncio.sleep(0.05)
        await self.ctl.stop()

    async def shutdown(self, hub, task):
        hub.stop.set()
        await asyncio.wait_for(task, 3)

    def cfg(self, **kw):
        c = dict(wh.DEFAULTS)
        c.update(name="t1", host="127.0.0.1", port=self.ctl_port, listen_host="127.0.0.1", listen_port=self.listen,
                 log_dir=os.path.join(self.tmp.name, "logs"), registry=os.path.join(self.tmp.name, "devices.csv"),
                 reconnect_s=0.2)
        c.update(kw)
        return c

    async def start_hub(self, **kw):
        hub = wh.Hub(self.cfg(**kw))
        task = asyncio.ensure_future(hub.run())
        self.addAsyncCleanup(self.shutdown, hub, task)
        for _ in range(50):
            await asyncio.sleep(0.05)
            if hub.identity:
                break
        return hub

    def log_records(self, hub):
        recs = []
        for root, _, files in os.walk(hub.cfg["log_dir"]):
            for f in files:
                with open(os.path.join(root, f)) as fh:
                    recs += [json.loads(l) for l in fh]
        return sorted(recs, key=lambda r: r["t"])


class HubTests(HubBase):
    async def test_handshake_binds_identity_and_logs_under_the_serial(self):
        hub = await self.start_hub()
        self.assertEqual(hub.serial(), "WB-0001-8")
        self.assertEqual(self.ctl.received[:2], ["*IDN?", "SYS:ID?"])
        c = await Client.connect(self.listen, "alice")
        await c.send("!hub status")
        out = await c.lines_until(lambda l: l.startswith("# hub"))
        self.assertIn("serial=WB-0001-8", out[-1])
        self.assertIn("match=1", out[-1])
        self.assertTrue(os.path.isdir(os.path.join(hub.cfg["log_dir"], "WB-0001-8")))

    async def test_stray_text_on_the_link_is_not_taken_for_the_identity(self):
        self.ctl.stray = "\x00\x7f\r\nO\r\n"
        hub = await self.start_hub()
        self.assertEqual(hub.serial(), "WB-0001-8")
        self.assertTrue(hub.identity["idn"].startswith("OK WHAM-FAKE"))
        self.assertIsNone(hub.mismatch)
        self.assertIn("O", [r["line"] for r in self.log_records(hub) if r["kind"] == "data"])

    async def test_client_command_reply_and_echo_to_others(self):
        hub = await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        b = await Client.connect(self.listen, "bob")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await b.lines_until(lambda l: l.startswith("--- connected"))
        await a.send("STATE?")
        got_a = await a.lines_until(lambda l: l == "OK IDLE")
        got_b = await b.lines_until(lambda l: l == "[alice] < OK IDLE")
        self.assertIn("[alice] > STATE?", got_b)            # others see who sent what, and the reply marked as alice's
        self.assertNotIn("[alice] > STATE?", got_a)
        self.assertNotIn("OK serial", " ".join(got_a))      # the service's own handshake replies stay hidden
        recs = self.log_records(hub)
        cmd = [r for r in recs if r["kind"] == "cmd"]
        self.assertEqual(cmd[-1]["line"], "STATE?")
        self.assertEqual(cmd[-1]["client"], "alice")
        self.assertEqual(cmd[-1]["serial"], "WB-0001-8")
        self.assertEqual([r["line"] for r in recs if r["kind"] == "reply"][-1], "OK IDLE")

    async def test_wrong_board_is_refused(self):
        hub = await self.start_hub(expect_serial="WB-0002-6")
        self.assertIsNotNone(hub.mismatch)
        c = await Client.connect(self.listen, "alice")
        await c.send("STATE?")
        out = await c.lines_until(lambda l: l.startswith("ERR hub identity mismatch"))
        self.assertIn("expected serial WB-0002-6", out[-1])
        self.assertNotIn("STATE?", self.ctl.received)       # nothing reached the controller

    async def test_an_unidentified_board_is_refused_when_a_serial_is_expected(self):
        self.ctl.silent.add("SYS:ID?")                           # the handshake fails part way
        hub = await self.start_hub(expect_serial="WB-0001-8", heartbeat_s=60)
        for _ in range(80):
            await asyncio.sleep(0.05)
            if hub.identity_settled:
                break
        self.assertEqual(hub.identity, {})
        c = await Client.connect(self.listen, "alice", quiet=True)
        await c.send("STATE?")
        out = await c.lines_until(lambda l: l.startswith(("OK", "ERR")))
        self.assertTrue(out[-1].startswith("ERR hub identity mismatch"), out)
        self.assertNotIn("STATE?", self.ctl.received)

    async def test_a_failed_identification_is_retried_on_the_heartbeat_schedule(self):
        self.ctl.silent.add("SYS:ID?")
        hub = await self.start_hub(expect_serial="WB-0001-8", heartbeat_s=0.5)
        for _ in range(80):
            await asyncio.sleep(0.05)
            if hub.identity_settled:
                break
        self.assertIsNotNone(hub.mismatch)                       # unidentified: refused
        self.ctl.silent.clear()
        for _ in range(60):
            await asyncio.sleep(0.05)
            if hub.serial() == "WB-0001-8":
                break
        self.assertEqual(hub.serial(), "WB-0001-8")
        self.assertIsNone(hub.mismatch)
        self.assertEqual(hub.session, 1)                         # the same connection, nobody typed !hub identify
        c = await Client.connect(self.listen, "alice", quiet=True)
        await c.send("STATE?")
        self.assertEqual(await c.lines_until(lambda l: l.startswith(("OK", "ERR"))), ["OK IDLE"])

    async def test_a_malformed_registry_does_not_skip_the_serial_check(self):
        reg = os.path.join(self.tmp.name, "bad.csv")
        with open(reg, "w") as f:
            f.write("sn,hw\nWB-0001-8,x\n\n")
        hub = await self.start_hub(expect_serial="WB-0002-6", registry=reg)
        self.assertIsNotNone(hub.mismatch)
        self.assertIsNone(hub.registered)
        notes = [r["line"] for r in self.log_records(hub) if r["kind"] == "hub"]
        self.assertTrue(any(n.startswith("identity WB-0001-8") for n in notes), notes)     # the handshake finished
        c = await Client.connect(self.listen, "alice", quiet=True)
        await c.send("STATE?")
        out = await c.lines_until(lambda l: l.startswith(("OK", "ERR")))
        self.assertTrue(out[-1].startswith("ERR hub identity mismatch"), out)

    async def test_an_unwritable_log_does_not_stop_the_service(self):
        bad = os.path.join(self.tmp.name, "not-a-dir")
        open(bad, "w").close()                                   # the log directory cannot be created
        hub = await self.start_hub(log_dir=bad)
        self.assertEqual(hub.serial(), "WB-0001-8")
        c = await Client.connect(self.listen, "alice", quiet=True)
        await c.send("STATE?")
        self.assertEqual(await c.lines_until(lambda l: l.startswith(("OK", "ERR"))), ["OK IDLE"])

    async def test_records_reach_the_disk_while_the_controller_is_down_at_start(self):
        hub = wh.Hub(self.cfg(port=free_port()))                 # nothing listens there
        task = asyncio.ensure_future(hub.run())
        self.addAsyncCleanup(self.shutdown, hub, task)
        await asyncio.sleep(0.3)
        c = await Client.connect(self.listen, "alice", quiet=True)
        await c.send("STATE?")
        await c.lines_until(lambda l: l.startswith("ERR hub"))
        self.assertEqual(hub.pending, [])
        self.assertIn(("refused", "STATE?"), [(r["kind"], r["line"]) for r in self.log_records(hub)])

    async def test_boot_starts_a_new_epoch_and_reidentifies(self):
        hub = await self.start_hub(expect_serial="WB-0001-8")
        c = await Client.connect(self.listen, "alice")
        await c.lines_until(lambda l: l.startswith("--- connected"))
        self.ctl.serial = "WB-0009-8"                       # the board that comes back is a different one
        await c.send("REBOOT")
        await c.lines_until(lambda l: l.startswith("!BOOT Rise"))
        for _ in range(60):
            await asyncio.sleep(0.05)
            if hub.mismatch:
                break
        self.assertEqual(hub.epoch, 1)
        self.assertIsNotNone(hub.mismatch)
        self.assertEqual(hub.serial(), "WB-0009-8")
        boots = [r for r in self.log_records(hub) if r["kind"] == "boot"]
        self.assertEqual(len(boots), 3)                    # the banner's three lines
        self.assertEqual(boots[0]["epoch"], 1)

    async def test_banner_with_a_stray_byte_in_front_still_counts(self):
        hub = await self.start_hub()
        c = await Client.connect(self.listen, "alice")
        await c.lines_until(lambda l: l.startswith("--- connected"))
        self.ctl.noise = "\x00\xff"
        await c.send("REBOOT")
        await c.lines_until(lambda l: l.startswith("!BOOT Rise"))
        self.assertEqual(hub.epoch, 1)

    async def test_long_lines_are_truncated_in_the_log_but_not_on_the_wire(self):
        hub = await self.start_hub()
        c = await Client.connect(self.listen, "alice")
        await c.lines_until(lambda l: l.startswith("--- connected"))
        await c.send("LONG " + "A" * 2000)                 # the fake answers ERR for anything unknown
        await c.lines_until(lambda l: l.startswith("ERR"))
        rec = [r for r in self.log_records(hub) if r["kind"] == "cmd"][-1]
        self.assertEqual(len(rec["line"]), wh.MAX_LOG_LINE)
        self.assertEqual(rec["truncated"], len("LONG " + "A" * 2000) - wh.MAX_LOG_LINE)
        self.assertIn("LONG " + "A" * 2000, self.ctl.received)    # the controller got all of it

    async def test_firmware_image_data_is_not_logged_in_full(self):
        hub = await self.start_hub()
        c = await Client.connect(self.listen, "alice")
        await c.lines_until(lambda l: l.startswith("--- connected"))
        await c.send("FWUPdate:DATA 0 " + "AB" * 128)
        await c.lines_until(lambda l: l.startswith("ERR"))
        rec = [r for r in self.log_records(hub) if r["kind"] == "cmd"][-1]
        self.assertEqual(rec["line"], ("FWUPdate:DATA 0 " + "AB" * 128)[:24])
        self.assertGreater(rec["truncated"], 200)

    async def test_events_before_the_first_handshake_are_logged_under_the_serial(self):
        hub = await self.start_hub()
        self.assertFalse(os.path.exists(os.path.join(hub.cfg["log_dir"], "unidentified-t1")))
        first = [r for r in self.log_records(hub)][0]
        self.assertEqual(first["serial"], "WB-0001-8")
        self.assertIn("connected to the controller", first["line"])

    async def test_unprovisioned_board_logs_under_its_hardware_id(self):
        self.ctl.serial, self.ctl.identity, self.ctl.verified = "UNSET", "unset", "0"
        hub = await self.start_hub()
        self.assertEqual(hub.dev_key(), "UNSET-34203558".replace("34203558", "20343558"))
        self.assertTrue(os.path.isdir(os.path.join(hub.cfg["log_dir"], hub.dev_key())))

    async def test_waits_for_the_controller_and_reconnects(self):
        await self.ctl.stop()
        hub = wh.Hub(self.cfg(port=free_port()))
        task = asyncio.ensure_future(hub.run())
        self.addAsyncCleanup(self.shutdown, hub, task)
        await asyncio.sleep(0.4)
        c = await Client.connect(self.listen, "alice")
        await c.send("STATE?")
        out = await c.lines_until(lambda l: "not connected" in l)
        self.assertFalse(hub.connected)
        # now the controller appears on the port the hub is trying
        hub.cfg["port"] = await self.ctl.start()
        for _ in range(60):
            await asyncio.sleep(0.1)
            if hub.identity:
                break
        self.assertTrue(hub.connected)
        self.assertEqual(hub.serial(), "WB-0001-8")
        await c.send("STATE?")
        await c.lines_until(lambda l: l == "OK IDLE")

    async def test_multi_line_reply_reaches_the_client_unchanged(self):
        await self.start_hub()
        c = await Client.connect(self.listen, "alice")
        await c.lines_until(lambda l: l.startswith("--- connected"))
        await c.send("DUMP?")
        out = await c.lines_until(lambda l: l == "1 2 3")
        self.assertEqual(out[-2:], ["OK 3", "1 2 3"])


class VisibilityTests(HubBase):
    """Several operators on one controller: who is connected, and dangerous commands made visible to everyone."""

    async def two(self, **kw):
        hub = await self.start_hub(**kw)
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        b = await Client.connect(self.listen, "bob")
        await b.lines_until(lambda l: l.startswith("--- connected"))
        return hub, a, b

    async def test_clients_command_lists_everyone(self):
        hub, a, b = await self.two()
        await a.lines_until(lambda l: "bob (127.0.0.1) joined" in l)      # joins show the address
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")
        await b.send("!hub clients")
        out = await b.lines_until(lambda l: l.strip().startswith("# alice") or l.strip().startswith("#   alice"), 3)
        text = "\n".join(out)
        self.assertIn("# hub clients: 2", text)
        row = [l for l in out if "alice" in l and "127.0.0.1" in l][0]
        self.assertIn("cmds 1", row)
        self.assertIn("last: STATE?", row)
        await b.send("!hub status")
        st = (await b.lines_until(lambda l: l.startswith("# hub name=")))[-1]
        self.assertIn("who=alice@127.0.0.1,bob@127.0.0.1", st)

    async def test_leaving_is_announced_and_logged_with_counts(self):
        hub, a, b = await self.two()
        await b.send("ARM")
        await b.lines_until(lambda l: l.startswith("ERR"))
        b.writer.close()
        await a.lines_until(lambda l: l == "--- bob left ---")
        left = [r for r in self.log_records(hub) if r["kind"] == "hub" and "bob left" in r["line"]][-1]
        self.assertIn("1 commands, 1 dangerous", left["line"])

    async def test_dangerous_command_is_announced_to_the_others_and_logged(self):
        hub, a, b = await self.two()
        await b.send("SOURce:ENAble 1 1")                  # the unknown-to-fake command still goes through
        got_a = await a.lines_until(lambda l: "DANGEROUS" in l)
        self.assertIn("--- hub: DANGEROUS [output] bob@127.0.0.1: SOURce:ENAble 1 1 ---", got_a)
        await b.lines_until(lambda l: l.startswith("ERR"))
        self.assertIn("SOURce:ENAble 1 1", self.ctl.received)             # not blocked
        rec = [r for r in self.log_records(hub) if r["kind"] == "cmd" and r["line"].startswith("SOURce")][-1]
        self.assertEqual(rec["danger"], "output")
        self.assertEqual(rec["client"], "bob")
        # the sender is not told about their own command, and a harmless one is not announced at all
        await b.send("STATE?")
        got_a = await a.lines_until(lambda l: l == "[bob] < OK IDLE")
        self.assertFalse(any("DANGEROUS" in l for l in got_a))
        self.assertFalse(any("DANGEROUS" in l for l in await b.quiet()))

    async def test_confirm_mode_holds_until_confirmed(self):
        hub, a, b = await self.two(confirm_dangerous=True)
        await b.send("ARM")
        told = await b.lines_until(lambda l: "not sent yet" in l)
        self.assertIn("[arm]", told[-1])
        about = await a.lines_until(lambda l: "about to send" in l)
        self.assertIn("bob@127.0.0.1 is about to send: ARM", about[-1])
        await asyncio.sleep(0.2)
        self.assertNotIn("ARM", self.ctl.received)                         # held: nothing reached the controller
        await b.send("STATE?")                                              # harmless commands are not held
        await b.lines_until(lambda l: l == "OK IDLE")
        await b.send("!hub confirm")
        await b.lines_until(lambda l: l.startswith("ERR"))                  # now the fake answers it
        self.assertIn("ARM", self.ctl.received)
        kinds = [r["kind"] for r in self.log_records(hub) if r["line"] == "ARM"]
        self.assertEqual(kinds[:2], ["danger-held", "cmd"])

    async def test_cancel_and_expiry_and_only_the_sender_can_confirm(self):
        hub, a, b = await self.two(confirm_dangerous=True, confirm_timeout_s=0.3)
        await b.send("FIRE")
        await b.lines_until(lambda l: "not sent yet" in l)
        await a.send("!hub confirm")                                        # someone else's held command is not theirs
        self.assertIn("nothing is waiting", (await a.lines_until(lambda l: l.startswith("# hub")))[-1])
        await b.send("!hub cancel")
        self.assertIn("cancelled", (await b.lines_until(lambda l: l.startswith("# hub")))[-1])
        await b.send("FIRE")
        await b.lines_until(lambda l: "not sent yet" in l)
        await asyncio.sleep(0.5)
        await b.send("!hub confirm")
        self.assertIn("too late", (await b.lines_until(lambda l: l.startswith("# hub")))[-1])
        self.assertNotIn("FIRE", self.ctl.received)


class UnsolicitedTests(HubBase):
    async def test_events_reach_every_client_and_are_logged(self):
        hub = await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        b = await Client.connect(self.listen, "bob")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await b.lines_until(lambda l: l.startswith("--- connected"))
        await self.ctl.push("!EVT 9 STATE FIRING")
        await a.lines_until(lambda l: l == "!EVT 9 STATE FIRING")
        await b.lines_until(lambda l: l == "!EVT 9 STATE FIRING")
        self.assertEqual([r["line"] for r in self.log_records(hub) if r["kind"] == "event"], ["!EVT 9 STATE FIRING"])

    async def test_blank_lines_are_passed_on_and_do_not_stall_the_next_command(self):
        await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await a.send("")
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")
        self.assertIn("", self.ctl.received)


class PipelineTests(HubBase):
    """One command in flight to the controller; each reply goes to the client that sent the command."""

    async def two(self, **kw):
        hub = await self.start_hub(**kw)
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        b = await Client.connect(self.listen, "bob")
        await b.lines_until(lambda l: l.startswith("--- connected"))
        return hub, a, b

    def times(self, kind, text):
        return [t for t, k, x in self.ctl.timeline if k == kind and x == text]

    async def test_two_clients_at_once_each_get_only_their_own_reply(self):
        hub, a, b = await self.two()
        self.ctl.delays["slow"] = 0.3
        await a.send("ECHO slow")
        await b.send("ECHO fast")
        got_a = await a.lines_until(lambda l: l == "OK slow")
        got_b = await b.lines_until(lambda l: l == "OK fast")
        self.assertNotIn("OK fast", got_a + await a.quiet())
        self.assertNotIn("OK slow", [l for l in got_b + await b.quiet() if not l.startswith("[")])
        self.assertGreaterEqual(self.times("rx", "ECHO fast")[0], self.times("tx", "OK slow")[0])   # one in flight
        recs = [r for r in self.log_records(hub) if r["kind"] == "reply"]
        self.assertEqual([(r["client"], r["line"]) for r in recs], [("alice", "OK slow"), ("bob", "OK fast")])

    async def test_observers_see_other_clients_replies_marked(self):
        hub, a, b = await self.two()
        await a.send("STATE?")
        got_a = await a.lines_until(lambda l: l == "OK IDLE")
        got_b = await b.lines_until(lambda l: l == "[alice] < OK IDLE")
        self.assertIn("[alice] > STATE?", got_b)
        self.assertNotIn("OK IDLE", got_b)                         # bob never sees a bare line that is alice's
        self.assertFalse(any(l.startswith("[alice] <") for l in got_a))

    async def test_identify_by_another_client_does_not_steal_a_reply(self):
        hub, a, b = await self.two()
        self.ctl.delays["slow"] = 0.3
        await a.send("ECHO slow")
        await asyncio.sleep(0.05)
        await b.send("!hub identify")
        await a.lines_until(lambda l: l == "OK slow")
        await b.lines_until(lambda l: l.startswith("# hub: identifying"))
        for _ in range(60):
            await asyncio.sleep(0.05)
            if self.ctl.received.count("SYS:ID?") >= 2:
                break
        self.assertEqual(self.ctl.received.count("*IDN?"), 2)       # the hub's second handshake did run, after the reply
        self.assertEqual(self.ctl.received.count("SYS:ID?"), 2)
        self.assertGreaterEqual(self.times("rx", "*IDN?")[1], self.times("tx", "OK slow")[0])

    async def test_a_command_after_a_reboot_gets_its_own_reply_not_the_handshakes(self):
        hub, a, b = await self.two()
        await a.send("REBOOT")
        await a.lines_until(lambda l: l.startswith("!BOOT Rise"))
        await a.send("*IDN?")
        got = await a.lines_until(lambda l: l.startswith("OK WHAM-FAKE"))
        self.assertEqual(got[-1], "OK WHAM-FAKE REVA v1 abc SN=WB-0001-8 UID=0055002B4232500220343558")
        self.assertEqual(await a.quiet(0.5), [])                    # and no second, stolen or duplicated, reply

    async def test_a_carriage_return_inside_a_line_is_two_commands_each_answered(self):
        hub = await self.start_hub()
        p = await Client.connect(self.listen, "prog", quiet=True)
        q = await Client.connect(self.listen, "other", quiet=True)
        await asyncio.sleep(0.1)
        p.writer.write(b"ECHO one\rECHO two\r\n")
        await q.send("STATE?")
        self.assertEqual(await p.lines_until(lambda l: l == "OK two"), ["OK one", "OK two"])
        self.assertEqual(await q.lines_until(lambda l: l.startswith("OK")), ["OK IDLE"])
        recs = [r for r in self.log_records(hub) if r["kind"] == "reply"]
        self.assertEqual([(r["client"], r["line"]) for r in recs],
                         [("prog", "OK one"), ("prog", "OK two"), ("other", "OK IDLE")])

    async def test_a_late_reply_is_not_handed_to_the_next_command(self):
        hub = await self.start_hub(command_timeout_s=0.3)
        self.ctl.delays["slow"] = 0.6
        a = await Client.connect(self.listen, "prog", quiet=True)
        b = await Client.connect(self.listen, "other", quiet=True)
        w = await Client.connect(self.listen, "watcher")
        await asyncio.sleep(0.1)
        await a.send("ECHO slow")
        await asyncio.sleep(0.1)
        await b.send("ECHO b")
        got_a = await a.lines_until(lambda l: l.startswith("ERR hub timeout"))
        self.assertEqual(await b.lines_until(lambda l: l.startswith("OK") or l.startswith("ERR")), ["OK b"])
        self.assertIn("OK slow", await w.lines_until(lambda l: l == "OK slow"))     # interactive: shown, owned by nobody
        self.assertEqual(got_a + await a.quiet(0.3), [got_a[0]])                    # quiet: never a second reply
        self.assertEqual(await b.quiet(0.3), [])
        recs = [(r.get("client"), r["line"]) for r in self.log_records(hub) if r["kind"] == "reply"]
        self.assertIn((None, "OK slow"), recs)
        self.assertIn(("other", "OK b"), recs)

    async def test_a_late_idn_reply_is_told_apart_by_a_different_query(self):
        hub = await self.start_hub(command_timeout_s=0.3)
        self.ctl.delays["*IDN?"] = 0.6
        a = await Client.connect(self.listen, "prog", quiet=True)
        b = await Client.connect(self.listen, "other", quiet=True)
        await asyncio.sleep(0.1)
        await a.send("*idn?")
        await asyncio.sleep(0.1)
        await b.send("STATE?")
        self.assertEqual(await b.lines_until(lambda l: l.startswith(("OK", "ERR"))), ["OK IDLE"])
        self.assertEqual(self.ctl.received[-3:], ["*idn?", "SYS:ID?", "STATE?"])

    async def test_a_long_command_that_times_out_gets_as_long_to_resync(self):
        hub = await self.start_hub(long_command_timeout_s=0.6, heartbeat_timeout_s=0.1, heartbeat_s=60)
        self.ctl.delays["LOG:DATA?"] = 0.9                       # after the 0.6 s timeout, well before the 1.2 s resync deadline
        a = await Client.connect(self.listen, "prog", quiet=True)
        await asyncio.sleep(0.1)
        await a.send("LOG:DATA? 1")
        await a.lines_until(lambda l: l.startswith("ERR hub timeout"))
        await asyncio.sleep(0.9)
        self.assertEqual(hub.session, 1)                         # still the same healthy link
        await a.send("STATE?")
        self.assertEqual(await a.lines_until(lambda l: l.startswith(("OK", "ERR"))), ["OK IDLE"])

    async def test_no_answer_after_a_timeout_reopens_the_connection(self):
        hub = await self.start_hub(command_timeout_s=0.3, heartbeat_timeout_s=0.3, heartbeat_s=60)
        a = await Client.connect(self.listen, "prog", quiet=True)
        await asyncio.sleep(0.1)
        self.ctl.mute = True
        await a.send("STATE?")
        await a.lines_until(lambda l: l.startswith("ERR hub timeout"))
        await a.send("ECHO queued")                              # waits behind the resync that will fail
        await asyncio.sleep(0.05)
        self.assertIn("ECHO queued", [c.text for c in hub.queue])   # waiting, not refused
        for _ in range(40):
            await asyncio.sleep(0.05)
            if hub.session == 2:
                break
        self.assertEqual(hub.session, 2)
        self.assertNotIn("ECHO queued", self.ctl.received)       # never sent on the link being closed
        notes = [r["line"] for r in self.log_records(hub) if r["kind"] == "hub"]
        self.assertEqual(sum("resync after STATE?: no answer to *IDN? within 0.3 s; closing the connection" in n for n in notes), 1, notes)

    async def test_a_reply_already_read_when_the_link_closes_sends_nothing_more(self):
        hub = await self.start_hub(command_timeout_s=0.3, heartbeat_timeout_s=5, heartbeat_s=60)
        a = await Client.connect(self.listen, "prog", quiet=True)
        await asyncio.sleep(0.1)
        self.ctl.mute = True
        await a.send("STATE?")
        await a.lines_until(lambda l: l.startswith("ERR hub timeout"))
        self.assertEqual(hub.inflight.text, "*IDN?")             # the resync
        await a.send("ECHO queued")
        await asyncio.sleep(0.05)
        self.assertIn("ECHO queued", [c.text for c in hub.queue])   # waiting, not refused
        hub.on_timeout(hub.inflight)                             # its timer fires with its reply already read
        hub.on_line("OK WHAM-FAKE REVA v1 abc SN=WB-0001-8 UID=0")
        await asyncio.sleep(0.3)
        self.assertNotIn(("cmd", "ECHO queued"), [(r["kind"], r["line"]) for r in self.log_records(hub)])

    async def test_repeated_identify_during_a_long_command_sends_one_handshake(self):
        hub = await self.start_hub()
        self.ctl.delays["slow"] = 0.5
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await a.send("ECHO slow")
        await asyncio.sleep(0.1)
        for _ in range(3):
            await a.send("!hub identify")
            await asyncio.sleep(0.05)                            # each handshake starts and queues its *IDN?
        await asyncio.sleep(1.0)
        after = self.ctl.received[self.ctl.received.index("ECHO slow") + 1:]
        self.assertEqual(after, ["*IDN?", "SYS:ID?"])

    async def raw(self, first):
        r, w = await asyncio.open_connection("127.0.0.1", self.listen)
        c = Client(r, w)
        w.write(first)
        await w.drain()
        return c

    async def test_a_client_ending_lines_with_cr_only_is_served(self):
        hub = await self.start_hub()
        c = await self.raw(b"@quiet lv\rSTATE?\r")
        self.assertEqual(await c.lines_until(lambda l: l.startswith("OK")), ["OK IDLE"])
        self.assertEqual([i.name for i in hub.clients.values()], ["lv"])

    async def test_a_command_on_the_first_line_after_the_name_is_sent(self):
        hub = await self.start_hub()
        c = await self.raw(b"@quiet lv\r*IDN?\r\n")
        self.assertTrue((await c.lines_until(lambda l: l.startswith("OK")))[-1].startswith("OK WHAM-FAKE"))
        self.assertEqual([i.name for i in hub.clients.values()], ["lv"])

    async def test_a_line_cut_off_by_a_disconnect_is_not_sent(self):
        hub = await self.start_hub()
        c = await self.raw(b"@quiet lv\r\nECHO whole\r\nECHO frag")
        await c.lines_until(lambda l: l == "OK whole")
        c.writer.close()
        await asyncio.sleep(0.3)
        self.assertIn("ECHO whole", self.ctl.received)
        self.assertNotIn("ECHO frag", self.ctl.received)          # the firmware never runs a line with no end
        refused = [r for r in self.log_records(hub) if r["kind"] == "refused"]
        self.assertEqual([(r["client"], r["line"]) for r in refused], [("lv", "ECHO frag")])

    async def test_a_cut_off_hub_command_is_not_logged_as_a_refused_controller_command(self):
        hub = await self.start_hub()
        c = await self.raw(b"@quiet lv\r\n!hub sta")
        await asyncio.sleep(0.1)
        c.writer.close()
        await asyncio.sleep(0.3)
        self.assertEqual([r for r in self.log_records(hub) if r["kind"] == "refused"], [])

    async def test_a_bare_quiet_line_turns_quiet_mode_on(self):
        hub = await self.start_hub()
        c = await self.raw(b"@quiet\r\nSTATE?\r\n")
        self.assertEqual(await c.lines_until(lambda l: l.startswith(("OK", "ERR"))), ["OK IDLE"])
        self.assertEqual([(i.name, i.quiet) for i in hub.clients.values()], [("client", True)])
        self.assertNotIn("@quiet", self.ctl.received)

    async def test_a_cr_lf_split_across_reads_is_one_line_end(self):
        hub = await self.start_hub()
        c = await self.raw(b"@quiet lv\r\n")
        await asyncio.sleep(0.1)
        c.writer.write(b"STATE?\r")
        await c.writer.drain()
        await asyncio.sleep(0.1)
        c.writer.write(b"\nECHO x\r\n")
        await c.writer.drain()
        self.assertEqual(await c.lines_until(lambda l: l == "OK x"), ["OK IDLE", "OK x"])
        self.assertEqual(self.ctl.received[-2:], ["STATE?", "ECHO x"])          # no blank line in between

    async def test_commands_queued_before_a_restart_are_answered_not_sent(self):
        hub = await self.start_hub()
        self.ctl.delays["DIE"] = 0.3
        a = await Client.connect(self.listen, "prog", quiet=True)
        b = await Client.connect(self.listen, "other", quiet=True)
        await asyncio.sleep(0.1)
        await a.send("DIE")
        await asyncio.sleep(0.1)
        await b.send("ECHO b")
        out = await b.lines_until(lambda l: l.startswith(("OK", "ERR")))
        self.assertTrue(out[-1].startswith("ERR hub the controller restarted"), out)
        await asyncio.sleep(1.0)                                 # past the handshake after the banner
        self.assertNotIn("ECHO b", self.ctl.received)
        self.assertEqual([l for l in await b.quiet(0.2) if not l.startswith("!")], [])     # one answer, nothing more

    async def test_commands_held_by_a_handshake_are_answered_when_the_board_restarts(self):
        hub = await self.start_hub(heartbeat_s=60)
        a = await Client.connect(self.listen, "prog", quiet=True)
        await asyncio.sleep(0.1)
        self.ctl.silent.add("SYS:ID?")
        await a.send("!hub identify")                            # this handshake will wait on SYS:ID?
        for _ in range(40):
            await asyncio.sleep(0.05)
            if hub.inflight is not None and hub.inflight.text == "SYS:ID?":
                break
        await a.send("ECHO held")
        await asyncio.sleep(0.1)
        self.assertEqual([t for _, t in hub.held], ["ECHO held"])
        self.ctl.silent.clear()
        await self.ctl.push(self.ctl.banner().rstrip("\r\n"))
        out = await a.lines_until(lambda l: l.startswith(("OK", "ERR")))
        self.assertTrue(out[-1].startswith("ERR hub the controller restarted"), out)
        await asyncio.sleep(1.0)
        self.assertNotIn("ECHO held", self.ctl.received)

    async def test_a_restart_part_way_through_a_dump_adds_no_second_reply(self):
        hub = await self.start_hub()
        a = await Client.connect(self.listen, "prog", quiet=True)
        await asyncio.sleep(0.1)
        await a.send("SYS:EVLOG? reset")
        out = await a.lines_until(lambda l: l.startswith("!BOOT Rise"))
        await asyncio.sleep(1.0)
        await a.send("STATE?")
        out += await a.lines_until(lambda l: l == "OK IDLE")
        self.assertEqual([l for l in out if not l.startswith("!")], ["OK 3", "OK IDLE"])
        notes = [r["line"] for r in self.log_records(hub) if r["kind"] == "hub"]
        self.assertTrue(any("cut short by the restart, 2 line(s) missing" in n for n in notes), notes)

    async def test_a_command_with_no_reply_times_out_and_frees_the_queue(self):
        hub, a, b = await self.two(command_timeout_s=0.3)
        await a.send("NOREPLY")
        await b.send("STATE?")
        out = await a.lines_until(lambda l: l.startswith("ERR hub timeout"))
        self.assertIn("NOREPLY", out[-1])
        await b.lines_until(lambda l: l == "OK IDLE")

    async def test_a_garbled_reply_still_ends_the_command(self):
        await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await a.send("GARBLE?")
        await a.lines_until(lambda l: l == "O")
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")

    async def test_a_reboot_with_no_reply_is_answered(self):
        hub, a, b = await self.two()
        await a.send("DIE")
        out = await a.lines_until(lambda l: l.startswith("ERR hub"))
        self.assertIn("restarted", out[-1])
        await b.lines_until(lambda l: l.startswith("!BOOT Rise"))

    async def test_losing_the_controller_answers_the_command_in_flight_and_the_queue(self):
        hub, a, b = await self.two()
        await a.send("HANGUP")
        await b.send("STATE?")
        out = await a.lines_until(lambda l: l.startswith("ERR hub"))
        self.assertIn("lost", out[-1])
        await b.lines_until(lambda l: "not connected" in l or l.startswith("ERR hub"))

    async def test_event_dump_holds_the_queue_until_its_last_line(self):
        hub, a, b = await self.two()
        await a.send("SYS:EVLOG?")
        await b.send("STATE?")
        got_a = await a.lines_until(lambda l: l == "!EVT 2 STATE ARMED")
        self.assertEqual([l for l in got_a if l.startswith(("OK", "!EVT"))], ["OK 2", "!EVT 1 STATE IDLE", "!EVT 2 STATE ARMED"])
        await b.lines_until(lambda l: l == "OK IDLE")
        self.assertGreaterEqual(self.times("rx", "STATE?")[0], self.times("tx", "!EVT 2 STATE ARMED")[0])

    async def test_a_catalog_that_fails_part_way_keeps_its_error_line_with_its_sender(self):
        hub, a, b = await self.two()
        await a.send("CONFig:PROFile:CATalog? broken")
        await b.send("STATE?")
        got_a = await a.lines_until(lambda l: l.startswith("ERR 24"))
        self.assertEqual(got_a[-2], "OK 1=empty 2=x/y")
        got_b = await b.lines_until(lambda l: l == "OK IDLE")
        self.assertFalse([l for l in got_b if l.startswith("ERR 24")])
        await a.send("CONFig:PROFile:CATalog?")
        await a.lines_until(lambda l: l.startswith("OK 1=empty"))
        await a.send("STATE?")
        out = await a.lines_until(lambda l: l == "OK IDLE")
        self.assertEqual(out, ["OK IDLE"])                      # a whole catalog needs no second line

    async def test_a_dump_cut_short_releases_the_queue_after_its_timeout(self):
        hub, a, b = await self.two(long_command_timeout_s=0.5)
        await a.send("SYS:EVLOG? short")                         # a header promising 5 lines, then 1
        await b.send("STATE?")
        await b.lines_until(lambda l: l == "OK IDLE", 3)
        self.assertGreaterEqual(self.times("rx", "STATE?")[0] - self.times("rx", "SYS:EVLOG? short")[0], 0.4)

    async def test_commands_of_a_client_that_left_are_not_sent(self):
        hub, a, b = await self.two()
        self.ctl.delays["slow"] = 0.3
        await a.send("ECHO slow")
        await b.send("ECHO ghost")
        await asyncio.sleep(0.05)
        b.writer.close()
        await a.lines_until(lambda l: l == "OK slow")
        await asyncio.sleep(0.3)
        self.assertNotIn("ECHO ghost", self.ctl.received)

    async def test_the_reply_to_a_client_that_left_is_not_shown_to_the_others(self):
        hub, a, b = await self.two()
        self.ctl.delays["slow"] = 0.3
        await b.send("ECHO slow")
        await asyncio.sleep(0.05)
        b.writer.close()
        await asyncio.sleep(0.6)
        self.assertFalse([l for l in await a.quiet(0.2) if "OK slow" in l])
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")

    async def test_the_queue_is_bounded(self):
        hub, a, b = await self.two(queue_max=2)
        self.ctl.delays["slow"] = 0.5
        for _ in range(4):
            await a.send("ECHO slow")
        out = await a.lines_until(lambda l: l.startswith("ERR hub queue full"))
        self.assertTrue(out)

    async def test_commands_while_the_controller_is_down_get_an_answer(self):
        await self.ctl.stop()
        hub = wh.Hub(self.cfg(port=free_port()))
        task = asyncio.ensure_future(hub.run())
        self.addAsyncCleanup(self.shutdown, hub, task)
        await asyncio.sleep(0.3)
        a = await Client.connect(self.listen, "alice")
        await a.send("STATE?")
        out = await a.lines_until(lambda l: "not connected" in l)
        self.assertEqual(out[-1], "--- hub: the controller is not connected ---")

    async def test_commands_after_a_reboot_wait_for_the_handshake(self):
        hub, a, b = await self.two()
        await a.send("DIE")
        await b.lines_until(lambda l: l.startswith("!BOOT Rise"))
        await b.send("STATE?")
        await b.lines_until(lambda l: l == "OK IDLE", 5)
        self.assertGreaterEqual(self.times("rx", "STATE?")[0], self.times("rx", "SYS:ID?")[1])   # after the second handshake

    async def test_losing_the_link_during_the_handshake_window_leaves_nothing_stuck(self):
        hub, a, b = await self.two()
        await a.send("DIEHANG")
        await b.send("STATE?")
        for _ in range(100):
            await asyncio.sleep(0.05)
            if hub.session == 2 and hub.identity and not hub.busy:
                break
        self.assertEqual(hub.session, 2)
        self.assertFalse(hub.busy)
        self.assertEqual(hub.hub_queue.__len__(), 0)
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")

    async def test_a_command_waiting_for_the_handshake_is_answered_after_it(self):
        hub, a, b = await self.two()
        await a.send("REBOOT")
        await a.lines_until(lambda l: l.startswith("!BOOT Rise"))
        await asyncio.sleep(0.7)                                # inside the handshake window
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")


class ReplyTableTests(unittest.TestCase):
    def test_which_commands_are_long_and_which_dump_lines(self):
        self.assertEqual(wh.reply_spec("SYS:EVLOG?"), (True, "evlog"))
        self.assertEqual(wh.reply_spec("sys:evlog?"), (True, "evlog"))
        self.assertEqual(wh.reply_spec("CONFig:PROFile:CATalog?"), (True, "catalog"))
        for cmd in ("LOG:DATA? 2", "PFMIN:DATA? 1", "PFMIN:RAW? 1", "PFM:GAPLOG?", "FWUPdate:BEGin 100 DEADBEEF", "FWUP:BEG 100 DEADBEEF", "FWUP:END"):
            self.assertEqual(wh.reply_spec(cmd), (True, None), cmd)
        self.assertEqual(wh.reply_spec("CONF:PROF:CAT?"), (True, "catalog"))     # short form of a query
        self.assertEqual(wh.reply_spec(":conf:profile:cat?"), (True, "catalog"))
        self.assertEqual(wh.reply_spec("PFM:GAPL?"), (False, None))                # neither short nor long form
        self.assertEqual(wh.reply_spec("STATE?"), (False, None))
        self.assertEqual(wh.reply_spec("SYS:EVLOG"), (False, None))        # not the query
        self.assertEqual(wh.reply_spec(""), (False, None))

    def test_every_table_entry_names_a_real_command(self):
        tables = [t for t in (table_patterns("xrex"), table_patterns("hbridge")) if t]
        if not tables:
            self.skipTest("firmware sources not next to host/")
        known = set().union(*tables)
        for pattern, _ in wh.REPLY_TABLE:
            self.assertIn(pattern, known, pattern)


class FramingFactsTests(unittest.TestCase):
    def test_the_slot_count_is_the_firmwares(self):
        found = False
        for product in ("xrex", "hbridge"):
            path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "firmware", product, "src", "app",
                                "settings", "profile_store.h")
            if os.path.exists(path):
                found = True
                with open(path) as f:
                    self.assertIn(f"#define PROFILE_SLOT_COUNT    ({wh.PROFILE_SLOTS}U)", f.read(), product)
        if not found:
            self.skipTest("firmware sources not next to host/")


class QuietModeTests(HubBase):
    """A machine client: replies to its own commands and unsolicited controller output, nothing else."""

    async def trio(self, **kw):
        hub = await self.start_hub(**kw)
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        q = await Client.connect(self.listen, "labview", quiet=True)
        await a.lines_until(lambda l: "labview" in l and "joined" in l)
        return hub, a, q

    async def test_a_quiet_client_gets_no_greeting_notices_or_echoes(self):
        hub, a, q = await self.trio()
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")
        await a.send("!hub identify")
        await asyncio.sleep(0.4)
        await a.send("ARM")
        await a.lines_until(lambda l: l.startswith("ERR"))
        self.assertEqual(await q.quiet(0.4), [])

    async def test_a_quiet_client_gets_its_own_replies_and_unsolicited_lines(self):
        hub, a, q = await self.trio()
        await q.send("STATE?")
        self.assertEqual(await q.quiet(0.4), ["OK IDLE"])
        await self.ctl.push("!EVT 9 STATE FIRING")
        self.assertEqual(await q.quiet(0.3), ["!EVT 9 STATE FIRING"])
        await q.send("SYS:EVLOG?")
        self.assertEqual(await q.quiet(0.5), ["OK 2", "!EVT 1 STATE IDLE", "!EVT 2 STATE ARMED"])
        out = await a.quiet(0.3)
        self.assertIn("[labview] > STATE?", out)                     # interactive clients still see everyone
        self.assertIn("[labview] < OK IDLE", out)

    async def test_two_quiet_clients_do_not_see_each_others_replies(self):
        hub, a, q = await self.trio()
        q2 = await Client.connect(self.listen, "script", quiet=True)
        await asyncio.sleep(0.1)
        self.ctl.delays["slow"] = 0.2
        await q.send("ECHO slow")
        await q2.send("ECHO fast")
        self.assertEqual(await q.quiet(0.6), ["OK slow"])
        self.assertEqual(await q2.quiet(0.3), ["OK fast"])

    async def test_hub_command_switches_quiet_mode(self):
        hub = await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        b = await Client.connect(self.listen, "bob")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await b.lines_until(lambda l: l.startswith("--- connected"))
        await b.send("!hub quiet on")
        await b.lines_until(lambda l: l == "# hub: quiet on")
        await a.send("STATE?")
        await a.lines_until(lambda l: l == "OK IDLE")
        self.assertEqual(await b.quiet(0.3), [])
        await b.send("!hub quiet")
        await b.lines_until(lambda l: l == "# hub: quiet on")
        await b.send("!hub quiet off")
        await b.lines_until(lambda l: l == "# hub: quiet off")
        await a.send("STATE?")
        await b.lines_until(lambda l: l == "[alice] < OK IDLE")
        await b.send("!hub status")
        await b.lines_until(lambda l: l.startswith("# hub name="))

    async def test_refusals_to_a_quiet_client_are_error_replies(self):
        hub = await self.start_hub(expect_serial="WB-0002-6")
        q = await Client.connect(self.listen, "labview", quiet=True)
        await q.send("STATE?")
        self.assertTrue((await q.quiet(0.3))[0].startswith("ERR hub identity mismatch"))
        await self.ctl.stop()
        await asyncio.sleep(0.5)
        hub.mismatch = None
        await q.send("STATE?")
        self.assertEqual(await q.quiet(0.3), ["ERR hub the controller is not connected"])

    async def test_a_held_dangerous_command_is_an_error_reply_to_a_quiet_client(self):
        hub = await self.start_hub(confirm_dangerous=True)
        q = await Client.connect(self.listen, "labview", quiet=True)
        await asyncio.sleep(0.1)
        await q.send("ARM")
        out = await q.quiet(0.3)
        self.assertEqual(len(out), 1)
        self.assertTrue(out[0].startswith("ERR hub DANGEROUS [arm] not sent yet"), out)
        await q.send("!hub confirm")
        self.assertTrue((await q.quiet(0.3))[0].startswith("ERR 1"))     # the controller's reply to the confirmed ARM


class LinkLivenessTests(HubBase):
    """A dead bridge connection is noticed: TCP keepalive on the socket, and a heartbeat when the controller is silent."""

    def beat(self, **kw):
        return dict(heartbeat_s=0.3, heartbeat_timeout_s=0.3, **kw)

    async def test_keepalive_is_set_on_the_bridge_connection(self):
        hub = await self.start_hub(keepalive_idle_s=7, keepalive_interval_s=2, keepalive_count=4)
        sock = hub.up_writer.get_extra_info("socket")
        self.assertTrue(sock.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE))
        for opt, want in (("TCP_KEEPIDLE", 7), ("TCP_KEEPINTVL", 2), ("TCP_KEEPCNT", 4)):
            if hasattr(socket, opt):
                self.assertEqual(sock.getsockopt(socket.IPPROTO_TCP, getattr(socket, opt)), want, opt)

    async def test_keepalive_can_be_turned_off(self):
        hub = await self.start_hub(keepalive_idle_s=0)
        self.assertFalse(hub.up_writer.get_extra_info("socket").getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE))

    async def test_a_silent_controller_is_detected_and_the_connection_reopened(self):
        hub = await self.start_hub(**self.beat())
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        self.ctl.mute = True                                     # the TCP connection stays open, nothing answers
        out = await a.lines_until(lambda l: "no answer to" in l, 3)
        self.assertIn("heartbeat: no answer to *IDN? within 0.3 s; closing the connection", out[-1])
        self.ctl.mute = False
        for _ in range(60):
            await asyncio.sleep(0.05)
            if hub.session == 2 and hub.identity.get("serial"):
                break
        self.assertEqual(hub.session, 2)
        self.assertTrue(hub.connected)
        self.assertEqual(hub.serial(), "WB-0001-8")
        notes = [r["line"] for r in self.log_records(hub) if r["kind"] == "hub"]
        self.assertEqual(sum("no answer to *IDN?" in n for n in notes), 1, notes)

    async def test_a_heartbeat_timeout_sends_nothing_more_before_reconnecting(self):
        hub = await self.start_hub(**self.beat())
        a = await Client.connect(self.listen, "prog", quiet=True)
        await asyncio.sleep(0.1)
        self.ctl.mute = True
        start = len(self.ctl.received)
        for _ in range(60):
            await asyncio.sleep(0.02)
            if hub.inflight is not None and hub.inflight.text == "*IDN?":
                break
        self.assertEqual(hub.inflight.text, "*IDN?")             # the heartbeat is in flight
        await a.send("ECHO queued")                              # waits behind the heartbeat that will fail
        await asyncio.sleep(0.05)
        self.assertIn("ECHO queued", [c.text for c in hub.queue])   # waiting, not refused
        for _ in range(60):
            await asyncio.sleep(0.05)
            if hub.session == 2:
                break
        self.assertEqual(hub.session, 2)
        self.assertNotIn("ECHO queued", self.ctl.received)       # never sent on the link being closed
        self.ctl.mute = False
        await asyncio.sleep(0.2)
        self.assertEqual(self.ctl.received[start:start + 2], ["*IDN?", "*IDN?"])   # the heartbeat, then the new handshake

    async def test_an_idle_but_healthy_link_is_probed_quietly(self):
        hub = await self.start_hub(**self.beat())
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await asyncio.sleep(1.2)
        self.assertGreaterEqual(self.ctl.received.count("*IDN?"), 3)
        self.assertEqual(hub.session, 1)
        self.assertEqual(await a.quiet(0.2), [])                 # the clients never see the heartbeat or its reply

    async def test_the_heartbeat_keeps_going_after_a_reset_during_its_query(self):
        hub = await self.start_hub(heartbeat_s=0.3, heartbeat_timeout_s=1.0)
        self.ctl.resets_on_idn = 1
        for _ in range(40):
            await asyncio.sleep(0.05)
            if hub.epoch == 1:
                break
        self.assertEqual(hub.epoch, 1)
        await asyncio.sleep(0.8)                                 # the handshake after the banner, then idle again
        seen = self.ctl.received.count("*IDN?")
        await asyncio.sleep(1.0)
        self.assertGreater(self.ctl.received.count("*IDN?"), seen)

    async def test_a_talkative_link_is_not_probed(self):
        hub = await self.start_hub(heartbeat_s=0.5, heartbeat_timeout_s=0.3)
        for _ in range(12):
            await self.ctl.push("!EVT 1 STATE IDLE")
            await asyncio.sleep(0.1)
        self.assertEqual(self.ctl.received.count("*IDN?"), 1)    # the handshake's

    async def test_the_heartbeat_does_not_steal_a_clients_reply(self):
        hub = await self.start_hub(**self.beat())
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        self.ctl.delays["slow"] = 0.8
        await a.send("ECHO slow")
        await a.lines_until(lambda l: l == "OK slow", 3)
        await asyncio.sleep(0.5)
        beats = self.times("rx", "*IDN?")
        self.assertGreaterEqual(beats[-1], self.times("tx", "OK slow")[0])
        self.assertEqual(hub.session, 1)

    def times(self, kind, text):
        return [t for t, k, x in self.ctl.timeline if k == kind and x == text]

    async def test_heartbeat_off(self):
        hub = await self.start_hub(heartbeat_s=0)
        await asyncio.sleep(0.5)
        self.assertEqual(self.ctl.received.count("*IDN?"), 1)

    async def test_status_shows_how_long_the_controller_has_been_silent(self):
        hub = await self.start_hub(heartbeat_s=0)
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await asyncio.sleep(0.5)
        await a.send("!hub status")
        st = (await a.lines_until(lambda l: l.startswith("# hub name=")))[-1]
        age = float([t for t in st.split() if t.startswith("rx_age=")][0][7:])
        self.assertGreaterEqual(age, 0.4)


class SlowClientTests(HubBase):
    async def test_a_client_that_stops_reading_is_disconnected_not_buffered(self):
        hub = await self.start_hub()                             # the default limit: loopback buffers are small on macOS
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        sock = socket.socket()
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        sock.connect(("127.0.0.1", self.listen))
        sock.sendall(b"@name stuck\r\n")                         # and never reads
        self.addCleanup(sock.close)
        await a.lines_until(lambda l: "stuck" in l and "joined" in l)
        await a.send("BIG 12000")
        out = await a.lines_until(lambda l: l == "OK", 20)
        self.assertEqual(sum(1 for l in out if l.startswith("!EVT 1 BIGLINE")), 12000)    # everyone else kept getting data
        self.assertEqual([c.name for c in hub.clients.values()], ["alice"])
        notes = [r["line"] for r in self.log_records(hub) if r["kind"] == "hub"]
        self.assertTrue(any("stuck is not reading" in n for n in notes), notes)
        self.assertTrue(any(l.startswith("--- hub: stuck is not reading") for l in out))
        self.assertTrue(any("stuck left" in n for n in notes))

    async def test_a_dropped_client_is_disconnected_at_the_socket(self):
        hub = await self.start_hub(client_buffer_max=1000)
        b = await Client.connect(self.listen, "bob")
        await b.lines_until(lambda l: l.startswith("--- connected"))
        hub.broadcast("x" * 2000)

        async def until_closed():
            try:
                while await b.reader.read(65536):
                    pass
            except ConnectionError:
                pass
        await asyncio.wait_for(until_closed(), 2)
        await asyncio.sleep(0.1)
        self.assertEqual(hub.clients, {})

    async def test_a_dropped_clients_queued_commands_are_not_sent(self):
        hub = await self.start_hub()
        self.ctl.delays["slow"] = 0.3
        a = await Client.connect(self.listen, "alice", quiet=True)
        await asyncio.sleep(0.1)
        await a.send("ECHO slow")
        await a.send("ECHO second")
        await asyncio.sleep(0.1)
        hub.info_of("alice").dropped = True                      # being disconnected, its cleanup not yet run
        await asyncio.sleep(0.5)
        self.assertIn("ECHO slow", self.ctl.received)
        self.assertNotIn("ECHO second", self.ctl.received)

    async def test_a_reader_that_keeps_up_is_never_dropped(self):
        hub = await self.start_hub()                             # 3 MB through the default 1 MiB limit
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await a.send("BIG 3000")
        await a.lines_until(lambda l: l == "OK", 20)
        self.assertEqual(len(hub.clients), 1)


class AccessTests(HubBase):
    async def refused(self, port):
        r, w = await asyncio.open_connection("127.0.0.1", port)
        w.write(b"@name mallory\r\nSTATE?\r\n")
        try:
            return await asyncio.wait_for(r.read(1000), 2) == b""
        except ConnectionError:                                  # closed with our bytes unread: a reset
            return True
        finally:
            w.close()

    async def test_allow_from_refuses_other_addresses_and_logs_it(self):
        hub = await self.start_hub(allow_from=["10.0.0.0/8", "192.168.5.7"])
        self.assertTrue(await self.refused(self.listen))
        self.assertEqual(hub.clients, {})
        self.assertNotIn("STATE?", self.ctl.received)
        self.assertTrue(any("refused a connection from 127.0.0.1" in r["line"] for r in self.log_records(hub)))

    async def test_allow_from_admits_listed_addresses(self):
        for spec in (["127.0.0.1"], "10.1.1.1, 127.0.0.0/8", ["::1", "127.0.0.0/24"], "[127.0.0.1]"):
            hub = wh.Hub(self.cfg(allow_from=spec))
            task = asyncio.ensure_future(hub.run())
            for _ in range(50):
                await asyncio.sleep(0.05)
                if hub.identity:
                    break
            a = await Client.connect(self.listen, "alice")
            await a.send("STATE?")
            await a.lines_until(lambda l: l == "OK IDLE")
            a.writer.close()
            await self.shutdown(hub, task)

    async def test_no_allow_from_admits_everyone(self):
        await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        await a.lines_until(lambda l: l.startswith("--- connected"))


class AccessConfigTests(unittest.TestCase):
    def test_parse(self):
        nets = wh.parse_allow_from("10.0.0.0/24, 192.168.1.5  [fe80::1]")
        self.assertEqual([str(n) for n in nets], ["10.0.0.0/24", "192.168.1.5/32", "fe80::1/128"])
        self.assertEqual(wh.parse_allow_from(None), [])
        self.assertEqual(wh.parse_allow_from([]), [])
        with self.assertRaises(ValueError):
            wh.parse_allow_from("10.0.0.0/33")

    def test_flat_yaml_list(self):
        d = wh.parse_flat_yaml("allow_from: [127.0.0.1, 10.0.0.0/24]   # lab\n")
        self.assertEqual([str(n) for n in wh.parse_allow_from(d["allow_from"])], ["127.0.0.1/32", "10.0.0.0/24"])

    def test_the_example_config_builds_with_and_without_pyyaml(self):
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config", "example.yaml")
        args = type("A", (), {"config": path, **{k: None for k in wh.DEFAULTS}})()
        for yaml_module in (None, sys.modules.get("yaml")):
            with unittest.mock.patch.dict(sys.modules, {"yaml": yaml_module}):
                cfg = wh.build_config(args)
            self.assertEqual([str(n) for n in wh.parse_allow_from(cfg["allow_from"])], ["127.0.0.1/32"])
            self.assertEqual(cfg["listen_port"], 5101)
            self.assertIsNone(wh.listen_warning(cfg))

    def test_bad_allow_from_stops_the_start(self):
        base = {"config": None, **{k: None for k in wh.DEFAULTS}, "host": "h", "listen_port": 1, "allow_from": "nonsense"}
        with self.assertRaises(SystemExit):
            wh.build_config(type("A", (), base)())

    def test_startup_warning(self):
        def cfg(host, allow=None):
            return {"listen_host": host, "listen_port": 5101, "allow_from": allow}
        self.assertIn("allow_from", wh.listen_warning(cfg("0.0.0.0")))
        self.assertIn("0.0.0.0:5101", wh.listen_warning(cfg("0.0.0.0")))
        self.assertIsNotNone(wh.listen_warning(cfg("::")))
        self.assertIsNotNone(wh.listen_warning(cfg("192.168.130.5")))
        self.assertIsNone(wh.listen_warning(cfg("127.0.0.1")))
        self.assertIsNone(wh.listen_warning(cfg("localhost")))
        self.assertIsNone(wh.listen_warning(cfg("::1")))
        self.assertIsNone(wh.listen_warning(cfg("0.0.0.0", ["10.0.0.0/24"])))
        self.assertIsNone(wh.listen_warning(cfg("0.0.0.0", "10.0.0.0/24")))


class LogRetentionTests(unittest.TestCase):
    def test_old_days_are_deleted_when_a_new_file_opens(self):
        with tempfile.TemporaryDirectory() as tmp:
            dev = os.path.join(tmp, "WB-0001-8")
            os.makedirs(dev)
            today = datetime.date.today()
            names = {(today - datetime.timedelta(days=d)).isoformat() + ".jsonl": d for d in (0, 5, 29, 31, 400)}
            for n in list(names) + ["notes.txt", "garbage.jsonl"]:
                open(os.path.join(dev, n), "w").close()
            log = wh.DeviceLog(tmp, keep_days=30)
            log.write("WB-0001-8", {"x": 1})
            log.close()
            left = set(os.listdir(dev))
            self.assertEqual(left, {n for n, d in names.items() if d <= 30} | {"notes.txt", "garbage.jsonl"})

    def test_one_bad_entry_does_not_stop_the_pruning(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = (datetime.date.today() - datetime.timedelta(days=40)).isoformat() + ".jsonl"
            older = (datetime.date.today() - datetime.timedelta(days=41)).isoformat() + ".jsonl"
            for dev in ("A", "B"):
                os.makedirs(os.path.join(tmp, dev))
                for n in (old, older):
                    open(os.path.join(tmp, dev, n), "w").close()
            open(os.path.join(tmp, ".gitkeep"), "w").close()
            real_listdir, real_remove = os.listdir, os.remove
            gone = []

            def listdir(path):                                    # the plain file first, the rest in a fixed order
                names = sorted(real_listdir(path))
                return [".gitkeep"] + [n for n in names if n != ".gitkeep"] if path == tmp else names

            def remove(path):                                     # another process got to the first one
                if not gone:
                    gone.append(path)
                    real_remove(path)
                    raise FileNotFoundError(path)
                real_remove(path)
            with unittest.mock.patch.object(wh.os, "listdir", listdir), unittest.mock.patch.object(wh.os, "remove", remove):
                log = wh.DeviceLog(tmp, keep_days=30)
                log.write("A", {"x": 1})
                log.close()
            for dev in ("A", "B"):
                self.assertNotIn(old, os.listdir(os.path.join(tmp, dev)), dev)
                self.assertNotIn(older, os.listdir(os.path.join(tmp, dev)), dev)

    def test_zero_keeps_everything(self):
        with tempfile.TemporaryDirectory() as tmp:
            dev = os.path.join(tmp, "d")
            os.makedirs(dev)
            open(os.path.join(dev, "2001-01-01.jsonl"), "w").close()
            log = wh.DeviceLog(tmp)
            log.write("d", {"x": 1})
            log.close()
            self.assertIn("2001-01-01.jsonl", os.listdir(dev))


class ConfigTests(unittest.TestCase):
    def test_flat_yaml(self):
        d = wh.parse_flat_yaml("# comment\nname: magnet2\nhost: 192.168.130.138   # the bridge\nport: 5000\n"
                               "expect_serial: 'WB-0007-4'\nlog_dir: ~\n")
        self.assertEqual(d, {"name": "magnet2", "host": "192.168.130.138", "port": "5000",
                             "expect_serial": "WB-0007-4", "log_dir": None})

    def test_confirm_dangerous_parses_as_a_bool(self):
        base = {"config": None, **{k: None for k in wh.DEFAULTS}, "host": "h", "listen_port": 1}
        self.assertFalse(wh.build_config(type("A", (), base)())["confirm_dangerous"])
        for v in (True, "true", "yes", "1"):
            self.assertTrue(wh.build_config(type("A", (), {**base, "confirm_dangerous": v})())["confirm_dangerous"], v)

    def test_build_config_needs_a_host_and_a_listen_port(self):
        with self.assertRaises(SystemExit):
            wh.build_config(type("A", (), {"config": None, **{k: None for k in wh.DEFAULTS}})())


if __name__ == "__main__":
    unittest.main()

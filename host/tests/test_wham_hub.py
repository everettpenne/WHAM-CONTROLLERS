"""wham_hub.py against a fake controller (an asyncio TCP server that answers like the firmware)."""
import asyncio
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import wham_hub as wh


class FakeController:
    """Answers *IDN?, SYS:ID?, STATE?; REBOOT sends a !BOOT banner (and can change identity)."""

    def __init__(self, serial="WB-0001-8", uid="0055002B4232500220343558", identity="ok", verified="1"):
        self.serial, self.uid, self.identity, self.verified = serial, uid, identity, verified
        self.received = []
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

    def banner(self):
        return (f"{self.noise}!BOOT WHAM-FAKE v1 abc BANK=1 STATE=IDLE tick=4 SN={self.serial} UID={self.uid}\r\n"
                "!BOOT diag boot=1\r\n!BOOT Rise and shine\r\n")

    async def handle(self, reader, writer):
        self.writers.append(writer)
        while True:
            line = await reader.readline()
            if not line:
                break
            cmd = line.decode().strip()
            self.received.append(cmd)
            if cmd == "*IDN?":
                writer.write(f"OK WHAM-FAKE REVA v1 abc SN={self.serial} UID={self.uid}\r\n".encode())
            elif cmd == "SYS:ID?":
                writer.write((self.idline() + "\r\n").encode())
            elif cmd == "STATE?":
                writer.write(b"OK IDLE\r\n")
            elif cmd == "REBOOT":
                writer.write(b"OK\r\n" + self.banner().encode())
            elif cmd == "DUMP?":
                writer.write(b"OK 3\r\n1 2 3\r\n")
            else:
                writer.write(b"ERR 1 Unknown command\r\n")
            await writer.drain()


class Client:
    opened = []

    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        Client.opened.append(self)

    @classmethod
    async def connect(cls, port, name):
        r, w = await asyncio.open_connection("127.0.0.1", port)
        c = cls(r, w)
        w.write(f"@name {name}\r\n".encode())
        return c

    async def send(self, text):
        self.writer.write((text + "\r\n").encode())
        await self.writer.drain()

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


class HubTests(unittest.IsolatedAsyncioTestCase):
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
                recs += [json.loads(l) for l in open(os.path.join(root, f))]
        return sorted(recs, key=lambda r: r["t"])

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

    async def test_client_command_reply_and_echo_to_others(self):
        hub = await self.start_hub()
        a = await Client.connect(self.listen, "alice")
        b = await Client.connect(self.listen, "bob")
        await a.lines_until(lambda l: l.startswith("--- connected"))
        await b.lines_until(lambda l: l.startswith("--- connected"))
        await a.send("STATE?")
        got_a = await a.lines_until(lambda l: l == "OK IDLE")
        got_b = await b.lines_until(lambda l: l == "OK IDLE")
        self.assertIn("[alice] > STATE?", got_b)            # others see who sent what
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


class ConfigTests(unittest.TestCase):
    def test_flat_yaml(self):
        d = wh.parse_flat_yaml("# comment\nname: magnet2\nhost: 192.168.130.138   # the bridge\nport: 5000\n"
                               "expect_serial: 'WB-0007-4'\nlog_dir: ~\n")
        self.assertEqual(d, {"name": "magnet2", "host": "192.168.130.138", "port": "5000",
                             "expect_serial": "WB-0007-4", "log_dir": None})

    def test_build_config_needs_a_host_and_a_listen_port(self):
        with self.assertRaises(SystemExit):
            wh.build_config(type("A", (), {"config": None, **{k: None for k in wh.DEFAULTS}})())


if __name__ == "__main__":
    unittest.main()

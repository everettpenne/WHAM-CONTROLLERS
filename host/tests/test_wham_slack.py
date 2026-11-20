"""Tests of host/slack_bot/wham_slack.py. Slack itself is faked (its web API and its Socket Mode WebSocket); the hub is the
real wham_hub.Hub against the fake controller of test_wham_hub. Run: python3 -m unittest tests.test_wham_slack"""
import asyncio
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "slack_bot"))
import wham_slack as ws
from test_wham_hub import Client, HubBase


def cfg(port, **kw):
    c = json.loads(json.dumps(ws.DEFAULTS))
    c.update(app_token="xapp-test", bot_token="xoxb-test", allow_users=["U1", "UDANGER"], allow_channels=["CLAB"],
             danger_users=["UDANGER"], controllers={"ech": {"host": "127.0.0.1", "port": port, "aliases": ["hvps"]}})
    c.update(kw)
    return c


class RulesTests(unittest.TestCase):
    def test_a_one_off_question_only_reads(self):
        for ok in ("STATE?", "SHOT:POINt? 3", "*IDN?", "gds?"):
            self.assertIsNone(ws.refusal(ok), ok)
        for bad, word in (("ARM", "dangerous"), ("FIRE", "dangerous"), ("CONFig:MODE HVPS", "not a query"), ("SHOT:BEGin", "not a query"),
                          ("LOG:DATA? 1", "long dump"), ("SYS:EVLOG?", "long dump"), ("!hub confirm", "hub itself"),
                          ("@quiet x", "hub itself"), ("STATE?\nARM", "not one command"), ("", "not one command")):
            self.assertIn(word, ws.refusal(bad) or "", bad)

    def test_what_a_session_may_send(self):
        for ok in ("STATE?", "CONFig:MODE HVPS", "SHOT:STEP 0 20000 50", "DISARM", "GDS:ENAble 3 1", "CHARGE:FREQuency 0", "!hub status", "!hub log 5"):
            self.assertEqual(ws.session_check(ok)[0], "send", ok)
        for bad in ("ARM", "FIRE", "FAULT:CLEar", "GDS:ENAble 3 0", "DEBUG:FAULT:BYPASS 1", "CHARGE:FREQuency 1000", "CONFig:DEADtime 150",
                    "SOURce:RUN", "PHASe:SELect UVW", "arm"):
            verdict, why = ws.session_check(bad)
            self.assertEqual(verdict, "refuse", bad)
            self.assertIn("NOT sent", why)
            self.assertEqual(ws.session_check(bad, may_danger=True)[0], "confirm", bad)
        for never in ("FWUPdate:BEGin 10 0", "FWUPdate:SWAP", "BOOT", "SYS:SERial WB-1", "LOG:DATA? 1", "!hub confirm", "!hub quiet off", "@name x", ""):
            self.assertEqual(ws.session_check(never, may_danger=True)[0], "refuse", never)

    def test_controller_names_and_aliases(self):
        c = cfg(1)
        self.assertEqual(ws.find_controller(c, "ECH"), "ech")
        self.assertEqual(ws.find_controller(c, "hvps"), "ech")
        self.assertIsNone(ws.find_controller(c, "nope"))

    def test_tokens_come_from_the_environment_first_and_no_token_is_in_the_defaults(self):
        c = ws.load_config("/nonexistent", env={"SLACK_APP_TOKEN": "xapp-e", "SLACK_BOT_TOKEN": "xoxb-e"})
        self.assertEqual((c["app_token"], c["bot_token"]), ("xapp-e", "xoxb-e"))
        self.assertEqual((ws.DEFAULTS["app_token"], ws.DEFAULTS["bot_token"], ws.DEFAULTS["danger_users"]), ("", "", []))


class BotTests(HubBase):
    """The bot against a real hub. The bot's calls block, so they run in threads."""

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hub = await self.start_hub()
        self.bot = ws.Bot(cfg(self.listen))
        self.posts = []
        self.addCleanup(lambda: self.bot.close_all(None))

    async def say(self, text, user="U1", channel="D1"):
        return await asyncio.to_thread(self.bot.handle, user, channel, text, "ann", self.posts.append)

    async def posted(self, needle, secs=5.0):
        end = time.time() + secs
        while time.time() < end:
            if any(needle in p for p in self.posts):
                return next(p for p in self.posts if needle in p)
            await asyncio.sleep(0.05)
        self.fail(f"{needle!r} was never posted; got {self.posts}")

    async def test_someone_not_on_the_lists_gets_no_answer_and_nothing_is_sent(self):
        self.assertIsNone(await self.say("list", user="USTRANGER", channel="D9"))
        self.assertIsNone(await self.say("connect ech", user="USTRANGER", channel="D9"))
        self.assertEqual(self.bot.sessions, {})
        self.assertIsNotNone(await self.say("list", user="USTRANGER", channel="CLAB"))     # an allowed channel

    async def test_list_and_one_off_questions(self):
        out = await self.say("list")
        self.assertIn("*ech* (WHAM-FAKE WB-0001-8", out)
        self.assertIn("`IDLE`", out)
        self.assertIn("nobody else connected", out)
        self.assertIn("OK IDLE", await self.say("ech STATE?"))
        self.assertIn("NOT", (await self.say("ech ARM")).upper())
        self.assertNotIn("ARM", self.ctl.received)
        self.assertIn("do not know a controller", await self.say("nonsense"))
        self.assertIn("connect <controller>", await self.say("help"))
        self.assertIn("not connected", await self.say(".disconnect"))

    async def test_a_session_sends_what_is_typed_and_posts_the_replies(self):
        out = await self.say("connect ech")
        self.assertIn("Connected to *ech* as `slack-ann`", out)
        self.assertIsNone(await self.say("STATE?"))
        self.assertIn("OK IDLE", await self.posted("OK IDLE"))
        self.assertIsNone(await self.say("ECHO hello\nECHO again"))                        # several lines, an ordinary command
        await self.posted("OK hello")
        await self.posted("OK again")
        self.assertIn("ECHO hello", self.ctl.received)
        who = [c.name for c in self.hub.clients.values()]
        self.assertEqual(who, ["slack-ann"], "the session is a named hub client")
        recs = [r for r in self.log_records(self.hub) if r.get("client") == "slack-ann" and r.get("kind") == "cmd"]
        self.assertIn("STATE?", [r["line"] for r in recs], "the hub's log says who sent it")
        self.assertIn("connected to *ech*", await self.say(".list"))
        self.assertIn("Disconnected", await self.say(".disconnect"))
        await asyncio.sleep(0.3)
        self.assertEqual(len(self.hub.clients), 0)
        self.assertEqual(self.bot.sessions, {})

    async def test_dangerous_commands_are_refused_unless_on_the_danger_list_and_confirmed(self):
        await self.say("connect ech")
        out = await self.say("ARM")
        self.assertIn("NOT sent", out)
        out = await self.say("ECHO a\nFIRE\nECHO b")
        self.assertIn("NOT sent", out)
        self.assertIn("1 line(s) after it were not sent", out)
        await self.posted("OK a")
        self.assertNotIn("ECHO b", self.ctl.received)
        self.assertIn("never sent from Slack", await self.say("FWUPdate:SWAP"))
        # someone on the danger list: asked first, sent only after .confirm
        await self.say("connect ech", user="UDANGER", channel="D2")
        out = await self.say("ARM", user="UDANGER", channel="D2")
        self.assertIn("Reply `.confirm`", out)
        await asyncio.sleep(0.3)
        self.assertNotIn("ARM", self.ctl.received)
        self.assertIn("Sent `ARM`", await self.say(".confirm", user="UDANGER", channel="D2"))
        await asyncio.sleep(0.4)
        self.assertIn("ARM", self.ctl.received)
        self.assertIn("Nothing is waiting", await self.say(".confirm", user="UDANGER", channel="D2"))
        # typing anything else cancels; a late confirm is refused
        await self.say("FIRE", user="UDANGER", channel="D2")
        await self.say("STATE?", user="UDANGER", channel="D2")
        self.assertIn("Nothing is waiting", await self.say(".confirm", user="UDANGER", channel="D2"))
        self.bot.cfg["confirm_s"] = 0
        await self.say("FIRE", user="UDANGER", channel="D2")
        await asyncio.sleep(0.05)
        self.assertIn("Too late", await self.say(".confirm", user="UDANGER", channel="D2"))
        self.assertNotIn("FIRE", self.ctl.received)
        for bad in ("ARM", "FIRE", "FWUPdate:SWAP"):
            self.assertEqual(self.ctl.received.count(bad), 1 if bad == "ARM" else 0, bad)

    async def test_hublog_shows_other_clients_traffic_and_off_hides_it(self):
        await self.say("connect ech")
        other = await Client.connect(self.listen, "labview")
        await other.send("ECHO before")
        await asyncio.sleep(0.6)
        self.assertFalse(any("before" in p for p in self.posts), "quiet by default: other people's traffic is not shown")
        self.assertIn("Hub-log ON", await self.say(".hublog on"))
        await asyncio.sleep(0.3)
        await other.send("ECHO seen")
        await self.posted("[labview] > ECHO seen")
        await self.posted("[labview] < OK seen")
        self.assertFalse(any("# hub: quiet" in p for p in self.posts))
        self.assertIn("Hub-log off", await self.say(".hublog off"))
        await asyncio.sleep(0.3)
        await other.send("ECHO hidden")
        await asyncio.sleep(0.8)
        self.assertFalse(any("hidden" in p for p in self.posts))

    async def test_the_stored_log_and_who(self):
        await self.say("connect ech")
        await self.say("ECHO marker")
        await self.posted("OK marker")
        out = await self.say(".log 5 grep=marker")
        self.assertIn("hub log", out)
        self.assertIn("ECHO marker", out)
        self.assertIn("do not understand", await self.say(".log bogus=1"))
        self.assertIsNone(await self.say(".who"))
        await self.posted("hub clients: 1")

    async def test_an_idle_session_is_closed_and_says_so(self):
        self.bot.cfg["session_idle_min"] = 0.01                                           # 0.6 s
        await self.say("connect ech")
        await self.posted("nothing was typed", secs=4)
        await asyncio.sleep(0.2)
        self.assertEqual(self.bot.sessions, {})
        self.assertEqual(len(self.hub.clients), 0)

    async def test_a_hub_that_goes_away_closes_the_session(self):
        await self.say("connect ech")
        for w in list(self.hub.clients):
            w.close()
        await self.posted("the hub closed the connection")
        self.assertEqual(self.bot.sessions, {})

    async def test_too_many_messages_too_fast(self):
        self.bot.cfg["rate_per_minute"] = 2
        await self.say("help")
        await self.say("help")
        self.assertIn("Slow down", await self.say("help"))

    async def test_a_flood_is_cut_not_relayed_whole(self):
        await self.say("connect ech")
        await self.say(".hublog on")
        other = await Client.connect(self.listen, "labview", quiet=True)
        await other.send("BIG 400")                                                        # 400 unsolicited lines of 1000 characters
        await self.posted("more lines were not shown", secs=20)
        self.assertLess(sum(len(p) for p in self.posts), 400 * 1000)


class FakeWsServer:
    """One WebSocket connection: performs the handshake, sends text frames, collects what the client sends."""

    def __init__(self):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.got, self.conn = [], None
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        self.conn, _ = self.srv.accept()
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.conn.recv(4096)
        key = [l.split(":", 1)[1].strip() for l in head.decode().split("\r\n") if l.lower().startswith("sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.conn.sendall(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {acc}\r\n\r\n".encode())
        buf = b""
        while True:
            try:
                chunk = self.conn.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while len(buf) >= 2:
                n, off = buf[1] & 0x7F, 2
                if n == 126:
                    if len(buf) < 4:
                        break
                    n, off = struct.unpack(">H", buf[2:4])[0], 4
                if len(buf) < off + 4 + n:
                    break
                mask, data = buf[off:off + 4], buf[off + 4:off + 4 + n]
                self.got.append((buf[0] & 0x0F, bytes(b ^ mask[i % 4] for i, b in enumerate(data)).decode(errors="replace")))
                buf = buf[off + 4 + n:]

    def push(self, text, opcode=1, fin=True):
        data = text.encode()
        n = len(data)
        head = bytes([(0x80 if fin else 0) | opcode]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n))
        self.conn.sendall(head + data)

    def wait(self, pred, secs=5.0):
        end = time.time() + secs
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.02)
        return False


class WebSocketTests(unittest.TestCase):
    def test_text_fragments_ping_and_close(self):
        srv = FakeWsServer()
        w = ws.WebSocket(f"ws://127.0.0.1:{srv.port}/link?ticket=1")
        srv.push('{"type":"hello"}')
        self.assertEqual(w.recv(2.0), '{"type":"hello"}')
        srv.push("x" * 300)                                              # a 16-bit length
        self.assertEqual(w.recv(2.0), "x" * 300)
        srv.push("ab", fin=False)
        srv.push("cd", opcode=0)                                         # a continuation frame
        self.assertEqual(w.recv(2.0), "abcd")
        srv.push("are you there", opcode=9)
        self.assertIsNone(w.recv(0.4))                                   # nothing for the caller ...
        self.assertTrue(srv.wait(lambda: (10, "are you there") in srv.got), "... but the ping was answered")
        w.send("y" * 200)
        self.assertTrue(srv.wait(lambda: (1, "y" * 200) in srv.got), "client frames are masked and decoded by the server")
        srv.push("", opcode=8)
        with self.assertRaises(ConnectionError):
            w.recv(2.0)
        w.close()


class SlackLinkTests(HubBase):
    """The whole path: an event arrives on the socket, is acknowledged, and the answer is posted."""

    async def test_events_are_acknowledged_answered_and_not_repeated(self):
        await self.start_hub()
        srv, calls, logs = FakeWsServer(), [], []

        def api(method, token, payload=None, timeout=15.0):
            calls.append((method, token, payload))
            return {"auth.test": {"ok": True, "user_id": "UBOT", "user": "wham", "team": "lab"},
                    "apps.connections.open": {"ok": True, "url": f"ws://127.0.0.1:{srv.port}/"},
                    "users.info": {"ok": True, "user": {"name": "ann"}}}.get(method, {"ok": True})
        link = ws.SlackLink(ws.Bot(cfg(self.listen)), api=api, log=logs.append)
        t = threading.Thread(target=link.run, daemon=True)
        t.start()
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: srv.conn is not None))
        posts = lambda: [c[2] for c in calls if c[0] == "chat.postMessage"]

        def event(eid, ev, etype="events_api", extra=None):
            payload = {"event_id": eid, "event": ev} if etype == "events_api" else extra
            srv.push(json.dumps({"envelope_id": "env-" + eid, "type": etype, "payload": payload}))
        event("1", {"type": "message", "channel_type": "im", "user": "U1", "channel": "D1", "text": "connect ech", "ts": "1.0"})
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: any("Connected to *ech*" in p["text"] for p in posts())), posts())
        self.assertIn((1, '{"envelope_id": "env-1"}'), srv.got)
        self.assertEqual(calls[0][:2], ("auth.test", "xoxb-test"))
        self.assertIn(("apps.connections.open", "xapp-test", None), calls)
        event("2", {"type": "message", "channel_type": "im", "user": "U1", "channel": "D1", "text": "`STATE?`", "ts": "2.0"})
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: any("OK IDLE" in p["text"] and p["channel"] == "D1" for p in posts())), posts())
        n = len(posts())
        event("2", {"type": "message", "channel_type": "im", "user": "U1", "channel": "D1", "text": "STATE?", "ts": "2.0"})   # Slack retries
        event("3", {"type": "message", "channel_type": "im", "user": "U1", "channel": "D1", "text": "x", "bot_id": "B1"})      # a bot
        event("4", {"type": "message", "channel_type": "im", "user": "UBOT", "channel": "D1", "text": "STATE?"})                # itself
        event("5", {"type": "message", "channel_type": "channel", "user": "U1", "channel": "CLAB", "text": "STATE?"})           # no mention, no session there
        event("6", {"type": "message", "channel_type": "im", "user": "USTRANGER", "channel": "D7", "text": "list"})             # not allowed
        await asyncio.sleep(1.5)
        self.assertEqual(len(posts()), n, "a retry, a bot, the bot itself, an unmentioned channel message and a stranger get nothing")
        self.assertTrue(any("ignored a message from USTRANGER" in l for l in logs))
        event("7", {"type": "app_mention", "user": "U1", "channel": "CLAB", "text": "<@UBOT> ech STATE?", "ts": "7.0"})
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: any(p["channel"] == "CLAB" and "OK IDLE" in p["text"] for p in posts())), posts())
        event("8", None, etype="slash_commands", extra={"user_id": "U1", "channel_id": "D1", "text": ".disconnect", "command": "/wham"})
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: any("Disconnected" in p["text"] for p in posts())), posts())
        link.stop.set()
        srv.push(json.dumps({"type": "disconnect", "reason": "refresh_requested"}))
        await asyncio.to_thread(t.join, 5)
        self.assertFalse(t.is_alive())


if __name__ == "__main__":
    unittest.main()

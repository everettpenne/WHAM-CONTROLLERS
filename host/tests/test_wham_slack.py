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
import wham_hub as wh
from test_wham_hub import Client, FakeController, HubBase, free_port


def cfg(port, **kw):
    c = json.loads(json.dumps(ws.DEFAULTS))
    c.update(app_token="xapp-test", bot_token="xoxb-test", allow_users=["U1", "UDANGER"], allow_channels=["CLAB"],
             danger_users=["UDANGER"], controllers={"ech": {"host": "127.0.0.1", "port": port, "aliases": ["hvps"]}},
             discover=False)
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

    def test_an_edited_settings_file_changes_who_is_allowed_without_a_restart(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "s.json")
            with open(path, "w") as f:
                json.dump({"allow_users": ["U1"], "bot_token": "xoxb-a"}, f)
            bot = ws.Bot(ws.load_config(path, env={}), config_path=path)
            self.assertTrue(bot.allowed("U1", "D1"))
            self.assertFalse(bot.allowed("U2", "D2"))
            with open(path, "w") as f:
                json.dump({"allow_users": ["U2"], "danger_users": ["U2"], "bot_token": "xoxb-CHANGED"}, f)
            os.utime(path, (time.time() + 5, time.time() + 5))
            self.assertTrue(bot.allowed("U2", "D2"))
            self.assertFalse(bot.allowed("U1", "D1"))
            self.assertEqual(bot.cfg["danger_users"], ["U2"])
            self.assertEqual(bot.cfg["bot_token"], "xoxb-a", "tokens are read only at start")
            with open(path, "w") as f:
                f.write("{ not json")
            os.utime(path, (time.time() + 10, time.time() + 10))
            self.assertTrue(bot.allowed("U2", "D2"), "a broken file keeps the lists that were in force")

    def test_user_names_are_asked_for_the_way_slack_reads_them(self):
        seen = {}

        class Resp:
            def __enter__(self): return self
            def __exit__(self, *a): return False
            def read(self): return b'{"ok": true}'

        def fake_open(req, timeout=0):
            seen[req.full_url.rsplit("/", 1)[1]] = (req.data, req.get_header("Content-type"))
            return Resp()
        real, ws.urllib.request.urlopen = ws.urllib.request.urlopen, fake_open
        try:
            ws.slack_api("users.info", "xoxb-t", {"user": "U1"})
            ws.slack_api("chat.postMessage", "xoxb-t", {"channel": "D1", "text": "x"})
        finally:
            ws.urllib.request.urlopen = real
        self.assertEqual(seen["users.info"], (b"user=U1", "application/x-www-form-urlencoded"))
        self.assertTrue(seen["chat.postMessage"][1].startswith("application/json"))

    def test_hubs_configured_on_the_server_are_found_and_merged_with_the_named_ones(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            for name, text in (("ech-hvps.yaml", "name: ech-hvps\nhost: 192.0.2.1\nlisten_port: 5103\n"),
                               ("new-supply.yaml", "name: new-supply\nhost: 192.0.2.9\nlisten_host: 0.0.0.0\nlisten_port: 5107\n"),
                               ("new-supply.local.yaml", "allow_from: [127.0.0.1]\n"), ("example.yaml", "listen_port: 5001\n"),
                               ("broken.yaml", "host: 192.0.2.3\n"), ("notes.txt", "listen_port: 9\n")):
                with open(os.path.join(d, name), "w") as f:
                    f.write(text)
            found = ws.discover_hubs(d)
            self.assertEqual(found, {"ech-hvps": ("127.0.0.1", 5103), "new-supply": ("127.0.0.1", 5107)})
            named = {"ech": {"host": "127.0.0.1", "port": 5103, "aliases": ["hvps"]}}
            merged = ws.merge_controllers(named, found)
            self.assertEqual(list(merged), ["ech", "new-supply"])
            self.assertEqual(merged["ech"]["aliases"], ["hvps", "ech-hvps"], "a known controller keeps its short name; the hub's name is an alias")
            self.assertEqual((merged["new-supply"]["port"], merged["new-supply"]["discovered"]), (5107, True))
            self.assertEqual(named["ech"]["aliases"], ["hvps"], "the settings themselves are not changed")
            self.assertEqual(ws.discover_hubs(os.path.join(d, "missing")), {})
            c = {"controllers": merged}
            self.assertEqual(ws.find_controller(c, "new"), "new-supply")            # a unique beginning is enough
            self.assertEqual(ws.find_controller(c, "ech-hvps"), "ech")
            self.assertIsNone(ws.find_controller({"controllers": {"ab1": {"aliases": []}, "ab2": {"aliases": []}}}, "ab"), "not when two match")

    def test_a_stop_signal_ends_the_bot_through_its_normal_exit(self):
        import signal
        old = ws.stop_on_signals()
        try:
            with self.assertRaises(SystemExit):
                signal.raise_signal(signal.SIGTERM)
        finally:
            for sig, h in old.items():
                signal.signal(sig, h)

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

    async def test_help_always_answers_and_is_never_sent_to_the_controller(self):
        self.assertIn("connect <controller>", await self.say("help"))
        await self.say("connect ech")
        for word in ("help", "Help", ".help", "?"):
            self.assertIn("`.users`", await self.say(word), word)
        await asyncio.sleep(0.3)
        self.assertFalse([c for c in self.ctl.received if c.lower() in ("help", "?")], "help is the bot's, not a controller command")

    async def test_users_lists_who_is_authorized_by_name_without_mentions(self):
        self.bot.person = {"U1": "Ann Example", "UDANGER": "Dan Example"}.get
        out = await self.say("users")
        self.assertIn("*Authorized to use this bot* (2)", out)
        self.assertIn("• Ann Example", out)
        self.assertIn("• Dan Example  (may also send dangerous commands", out)
        self.assertNotIn("<@", out, "a mention would notify everyone on the list")
        self.assertIn("1 allowed channel(s): <#CLAB>", out)
        await self.say("connect ech")
        self.assertIn("Ann Example", await self.say(".users"))
        self.bot.cfg["danger_users"] = []
        self.assertIn("Nobody may send dangerous commands", await self.say(".users"))
        self.assertIsNone(await self.say("users", user="USTRANGER", channel="D9"), "only authorized people may ask")

    async def test_a_new_session_is_greeted_by_name_with_what_to_watch_out_for(self):
        self.bot.person = {"U1": "Ann Example"}.get
        out = await self.say("connect ech")
        self.assertIn("Hello Ann! Connected to *ech* as `slack-ann`.", out)
        self.assertIn("`IDLE`", out)
        self.assertNotIn(":warning:", out)
        await self.say(".disconnect")
        await Client.connect(self.listen, "labview", quiet=True)
        await asyncio.sleep(0.3)
        out = await self.say("connect ech")
        self.assertIn(":warning: Someone else is connected to it", out)
        self.assertIn("labview", out)
        ctl2, hub2, listen2 = await self.second_hub(ArmedController)
        self.bot.cfg["controllers"]["limiter"] = {"host": "127.0.0.1", "port": listen2, "aliases": []}
        out = await self.say("connect limiter", channel="D5")
        self.assertIn("`ARMED`", out)
        self.assertIn(":warning: The controller is not IDLE", out)

    async def test_stopping_the_bot_says_goodbye_to_every_open_session_and_watch(self):
        others = []
        await self.say("connect ech")
        await asyncio.to_thread(self.bot.handle, "UDANGER", "D2", "watch all", "dan", others.append)
        await asyncio.sleep(0.4)
        self.assertEqual(len(self.hub.clients), 2)
        told = await asyncio.to_thread(self.bot.goodbye)
        self.assertEqual(told, 2)
        bye = [p for p in self.posts if p.startswith("Goodbye")]
        self.assertEqual(len(bye), 1)
        self.assertIn("your session with *ech* is closed", bye[0])
        self.assertIn("`connect ech` again", bye[0])
        self.assertFalse(any("Session with" in p for p in self.posts), "said once, not twice")
        self.assertEqual(sum(p.startswith("Goodbye") and "watch is stopped" in p for p in others), 1)
        await asyncio.sleep(0.4)
        self.assertEqual(len(self.hub.clients), 0)
        self.assertEqual((self.bot.sessions, self.bot.watches), ({}, {}))
        self.assertEqual(await asyncio.to_thread(self.bot.goodbye), 0)

    async def test_the_diary_is_written_and_read_from_slack_with_date_time_and_name(self):
        import diary
        path = os.path.join(self.tmp.name, "diary.txt")
        self.bot.cfg["diary_file"] = path
        self.bot.person = {"U1": "Ann Example", "UDANGER": "Dan Example"}.get
        out = await self.say("diary write Bank raised to 540 V")
        self.assertIn("Written to the diary", out)
        self.assertRegex(out, r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \| Ann Example \(Slack\) \| general \| Bank raised to 540 V")
        await self.say("connect ech")
        self.assertIn("| ech | second entry / on two lines", await self.say("diary write second entry\non two lines"))
        self.assertIn("| Dan Example (Slack) | general | from Dan", await self.say(".diary write from Dan", user="UDANGER", channel="D2"))
        await asyncio.sleep(0.3)
        self.assertFalse([c for c in self.ctl.received if "diary" in c.lower()], "a diary line is never sent to the controller")
        out = await self.say("diary read")
        self.assertIn("Diary: 3 entries on ", out)
        self.assertEqual(len([l for l in out.splitlines() if l[:2] == "20" and " | " in l]), 3, "each line has its date, time and writer")
        self.assertIn("Dan Example (Slack)", out)
        self.assertIn("Diary: 1 entry containing", await self.say("diary search 540"))
        self.assertIn("the newest 1 of 3", await self.say("diary last 1"))
        self.assertIn("3 entries on 1 day", await self.say("diary dates"))
        self.assertIn("2 entries in the whole diary", await self.say("diary read all by=ann"))
        self.assertIn("no entries on 2020-01-01", await self.say("diary read 2020-01-01"))
        self.assertIn("the entry is empty", await self.say("diary write"))
        self.assertIn("I do not know 'diary erase'", await self.say("diary erase everything"))
        for i in range(60):
            diary.write(path, "x", f"filler {i}")
        out = await self.say("diary read all")
        self.assertIn("63 entries in the whole diary; the newest 40 are shown", out)
        self.assertLess(len(out), ws.MAX_TEXT + 60)
        self.assertIsNone(await self.say("diary read", user="USTRANGER", channel="D9"), "only authorized people")
        self.assertEqual(ws.diary_file({"diary_file": None, "discover_dir": "/nonexistent", "state_dir": "/var/lib/wham/hub/slack"}),
                         "/var/lib/wham/hub/diary.txt")

    async def second_hub(self, controller=FakeController):
        ctl = controller(serial="WB-0002-6")
        port = await ctl.start()
        self.addAsyncCleanup(ctl.stop)
        listen = free_port()
        hub = wh.Hub(self.cfg(name="t2", port=port, listen_port=listen))
        task = asyncio.ensure_future(hub.run())
        self.addAsyncCleanup(self.shutdown, hub, task)
        for _ in range(50):
            await asyncio.sleep(0.05)
            if hub.identity:
                break
        return ctl, hub, listen

    async def test_a_new_hub_config_appears_by_itself_in_list_connect_and_watch(self):
        ctl2, hub2, listen2 = await self.second_hub()
        folder = os.path.join(self.tmp.name, "config")
        os.makedirs(folder)
        self.bot = ws.Bot(cfg(self.listen, discover=True, discover_dir=folder))
        self.assertEqual(list(self.bot.cfg["controllers"]), ["ech"])
        with open(os.path.join(folder, "new-supply.yaml"), "w") as f:
            f.write(f"name: new-supply\nhost: 127.0.0.1\nport: 1\nlisten_host: 0.0.0.0\nlisten_port: {listen2}\n")
        self.bot.scanned = time.monotonic() - 60                                          # the next message looks again
        out = await self.say("list")
        self.assertIn("*new-supply* (WHAM-FAKE WB-0002-6", out)
        self.assertIn("*ech* (WHAM-FAKE WB-0001-8", out)
        self.assertIn("Connected to *new-supply*", await self.say("connect new"))
        await self.say("ECHO via-new")
        await self.posted("OK via-new")
        self.assertIn("ECHO via-new", ctl2.received)
        self.assertNotIn("ECHO via-new", self.ctl.received)
        await self.say(".disconnect")
        self.assertIn("Watching *ech, new-supply*", await self.say("watch all"))
        os.remove(os.path.join(folder, "new-supply.yaml"))
        self.bot.scanned = time.monotonic() - 60
        self.assertNotIn("new-supply", (await self.say("list")).split("You are watching")[0], "a removed hub is no longer offered")

    async def test_watch_all_shows_every_hub_marked_and_sends_nothing(self):
        ctl2, hub2, listen2 = await self.second_hub()
        self.bot.configured["limiter"] = {"host": "127.0.0.1", "port": listen2, "aliases": []}
        self.bot.cfg["controllers"]["limiter"] = self.bot.configured["limiter"]
        out = await self.say("watch all")
        self.assertIn("Watching *ech, limiter*", out)
        self.assertIn("nothing you type is sent", out)
        await asyncio.sleep(0.3)
        self.assertEqual([c.name for c in self.hub.clients.values()], ["slack-ann-watch"])
        a = await Client.connect(self.listen, "labview")
        b = await Client.connect(listen2, "console")
        await a.send("ECHO on-ech")
        await b.send("ECHO on-limiter")
        await self.posted("[ech] [labview] > ECHO on-ech")
        await self.posted("[ech] [labview] < OK on-ech")
        await self.posted("[limiter] [console] > ECHO on-limiter")
        self.assertFalse(any("connected as" in p for p in self.posts), "the hub's greeting is not relayed")
        before = (list(self.ctl.received), list(ctl2.received))
        self.assertIn("do not know a controller", await self.say("STATE?"))               # typed while only watching
        await asyncio.sleep(0.3)
        self.assertEqual((self.ctl.received, ctl2.received), before, "a watch never sends to a controller")
        self.assertIn("You are watching ech, limiter", await self.say("list"))
        # a session beside the watch: its lines go to its controller only
        await self.say("connect ech")
        await self.say("ECHO mine")
        await self.posted("[ech] [slack-ann] > ECHO mine")
        self.assertNotIn("ECHO mine", ctl2.received)
        self.assertIn("still connected to *ech*", await self.say("watch off"))
        await asyncio.sleep(0.4)
        self.assertEqual(self.bot.watches, {})
        self.assertEqual([c.name for c in hub2.clients.values()], ["console"])
        self.assertEqual(sorted(c.name for c in self.hub.clients.values()), ["labview", "slack-ann"])

    async def test_an_idle_watch_ends_once_and_a_lost_hub_is_named(self):
        ctl2, hub2, listen2 = await self.second_hub()
        self.bot.cfg["controllers"]["limiter"] = {"host": "127.0.0.1", "port": listen2, "aliases": []}
        await self.say("watch all")
        await asyncio.sleep(0.4)                                                          # the hubs have registered the watch
        self.assertEqual(len(hub2.clients), 1)
        for w in list(hub2.clients):
            w.close()
        await self.posted("*limiter*: the hub closed the connection. Still watching ech.")
        self.bot.cfg["session_idle_min"] = 0.01
        for w in self.bot.watches.values():
            for s in w.sessions.values():
                s.idle_s = 0.6
        await self.posted("Watching stopped: nothing was typed", secs=5)
        await asyncio.sleep(0.3)
        self.assertEqual(sum("Watching stopped" in p for p in self.posts), 1)
        self.assertEqual(self.bot.watches, {})
        self.assertEqual(len(self.hub.clients), 0)
        self.bot.cfg["controllers"] = {"gone": {"host": "127.0.0.1", "port": free_port(), "aliases": []}}
        self.assertIn("No hub could be reached", await self.say("watch all"))

    async def test_log_all_merges_the_hubs_logs_in_time_order(self):
        ctl2, hub2, listen2 = await self.second_hub()
        self.bot.cfg["controllers"]["limiter"] = {"host": "127.0.0.1", "port": listen2, "aliases": []}
        a = await Client.connect(self.listen, "labview")
        b = await Client.connect(listen2, "console")
        await a.send("ECHO first")
        await asyncio.sleep(0.3)
        await b.send("ECHO second")
        await asyncio.sleep(0.3)
        await a.send("ECHO third")
        await asyncio.sleep(0.4)
        out = await self.say("log all 20 kind=cmd grep=ECHO")
        self.assertIn("Hub logs of ech, limiter, merged", out)
        rows = [l for l in out.splitlines() if "ECHO" in l]
        self.assertEqual([(r.split()[2], r.split()[-1]) for r in rows], [("ech", "first"), ("limiter", "second"), ("ech", "third")])
        self.bot.cfg["controllers"]["dead"] = {"host": "127.0.0.1", "port": free_port(), "aliases": []}
        out = await self.say("log all 5 grep=ECHO")
        self.assertIn("*dead*: no answer from the hub", out)
        self.assertIn("third", out)
        await self.say("connect ech")
        self.assertIn("merged", await self.say("log all 5"))                              # no dot needed, never sent to the controller
        self.assertIn("*ech* hub log", await self.say(".log 5"))
        self.assertNotIn("log all 5", self.ctl.received)

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


class ArmedController(FakeController):
    async def answer(self, cmd, writer):
        if cmd == "STATE?":
            self.say(writer, "OK ARMED\r\n")
        else:
            await super().answer(cmd, writer)


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
        self.hub = await self.start_hub()
        srv, calls, logs = FakeWsServer(), [], []

        def api(method, token, payload=None, timeout=15.0):
            calls.append((method, token, payload))
            return {"auth.test": {"ok": True, "user_id": "UBOT", "user": "wham", "team": "lab"},
                    "apps.connections.open": {"ok": True, "url": f"ws://127.0.0.1:{srv.port}/"},
                    "users.info": {"ok": True, "user": {"name": "ann", "real_name": "Ann Example"}}}.get(method, {"ok": True})
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
        event("7b", {"type": "message", "channel_type": "im", "user": "U1", "channel": "D1", "text": ".users", "ts": "7.5"})
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: any("• Ann Example" in p["text"] for p in posts())), posts())
        self.assertTrue(any(r.get("client") == "slack-ann" for r in self.log_records(self.hub)), "the session is named after the Slack user")
        event("8", None, etype="slash_commands", extra={"user_id": "U1", "channel_id": "D1", "text": ".disconnect", "command": "/wham"})
        self.assertTrue(await asyncio.to_thread(srv.wait, lambda: any("Disconnected" in p["text"] for p in posts())), posts())
        link.stop.set()
        srv.push(json.dumps({"type": "disconnect", "reason": "refresh_requested"}))
        await asyncio.to_thread(t.join, 5)
        self.assertFalse(t.is_alive())


if __name__ == "__main__":
    unittest.main()

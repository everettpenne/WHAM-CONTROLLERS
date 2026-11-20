"""Tests of the hub's shot records: "!hub shotnumber <n>" files what the controller holds under a shot number in the hub
log, "!hub read shotnumber <n>" returns it. Run: python3 -m unittest tests.test_hub_shots"""
import asyncio
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import wham_hub as wh
from test_wham_hub import Client, FakeController, HubBase


class Board(FakeController):
    """A controller that identifies as a real product and answers that product's queries."""
    product = "WHAM-HBRIDGE-PFMG474"
    answers = {}

    def idline(self):
        return super().idline().replace("product=WHAM-FAKE", f"product={self.product}")

    async def answer(self, cmd, writer):
        if cmd in self.answers:
            self.say(writer, self.answers[cmd] + "\r\n")
        else:
            await super().answer(cmd, writer)


class Hbridge(Board):
    points = ["0 20086 50", "1000 20117.5 50", "5000 0 0", "6500 20200 40 50 60", "15000 19018 50"]
    answers = {"STATE?": "OK IDLE", "FAULT?": "OK 0", "FAULT:SOURces?": "OK PC10=0 GATEDRV=0 TABLE=0 TEST=0 OVP=0 FB=0 SPARK=0",
               "CONFig:MODE?": "OK HVPS", "CONFig:LIMits?": "OK HVPS FREQ 10000 30000 DUTY 40 60", "PHASe:ENAble?": "OK U=1 V=1 W=1",
               "CONFig:DEADtime?": "OK 100", "PID:LOOPMODE? 0": "OK 0 0 0", "TARGet?": "OK 0 0",
               "EXTernal:TRIGger:STATus?": "OK enabled=1 input=0 edges=3 started=3 last=FIRED",
               "EXTernal:TRIGger:LATency?": "OK edge_us=1 ring_us=2 preload_us=3 capture_us=4 pwm_us=5 start_us=39.5 total_us=41 prepared=1",
               "CHARGE:STATus?": "OK freq=0 running=0 actual=0", "GDS:ENAble?": "OK " + " ".join(f"{i:02d}=1" for i in range(1, 13)),
               "GDS:POLarity?": "OK NORMALLY_HIGH", "SPARKgap:ENAble?": "OK 1", "DEBUG:FAULT:BYPASS?": "OK 0",
               "SHOT?": "OK 5 15000 PROFILE"}

    async def answer(self, cmd, writer):
        if cmd.startswith("SHOT:POINt? "):
            self.say(writer, "OK " + self.points[int(cmd.split()[1])] + "\r\n")
        else:
            await super().answer(cmd, writer)


class Xrex(Board):
    product = "WHAM-XREX-PFMG474"
    answers = {"STATE?": "OK IDLE", "FAULT?": "OK 0", "SHOT:TIMing?": "OK 1.000 2.000 1.000", "EXTernal:ENAble?": "OK 1",
               "EXTernal:INPut?": "OK 1", "EXTernal:TRIGger?": "OK 0", "CONFig:DEADtime?": "OK 100", "DEBUG:FAULT:BYPASS?": "OK 0"}

    async def answer(self, cmd, writer):
        w = cmd.split()
        table = {"SOURce:ENAble?": lambda n: "1" if n in (1, 3) else "0", "SHOT:CURRent?": lambda n: f"{100 * n}.000",
                 "PID:LOOPMODE?": lambda n: "1", "PID:GAINS?": lambda n: "0.5 250 0", "CONFig:MAXCURRent?": lambda n: "800.000",
                 "XREX:CHANnel:ENAOut?": lambda n: "1", "XREX:CHANnel:CONTactOut?": lambda n: "1", "SOURce:STATus?": lambda n: "0 0 0 3000"}
        if len(w) == 2 and w[0] in table and w[1].isdigit():
            self.say(writer, "OK " + table[w[0]](int(w[1])) + "\r\n")
        else:
            await super().answer(cmd, writer)


class ShotBase(HubBase):
    board = Hbridge

    async def asyncSetUp(self):
        await super().asyncSetUp()
        await self.ctl.stop()
        self.ctl = self.board()
        self.ctl_port = await self.ctl.start()
        self.hub = await self.start_hub()

    async def talk(self, client, text, wait=0.8):
        await client.send(text)
        return await client.quiet(wait)


class HbridgeShotTests(ShotBase):
    async def test_a_shot_is_recorded_quietly_and_read_back_by_number(self):
        lv = await Client.connect(self.listen, "LVshot", quiet=True)
        watcher = await Client.connect(self.listen, "ann")
        await watcher.quiet()
        out = await self.talk(lv, "!hub shotnumber 261120015")
        self.assertEqual(out, ["# shot 261120015 recorded: HVPS, 5 setpoints, 15000 us; 22 values"])
        self.assertEqual(await watcher.quiet(), [], "nobody else is told, and the queries are not echoed")
        self.assertEqual([c for c in self.ctl.received if c.startswith("SHOT:POINt?")], [f"SHOT:POINt? {i}" for i in range(5)])
        self.assertFalse([c for c in self.ctl.received if not c.endswith("?") and "?" not in c], "only queries were sent")
        shots = [r for r in self.log_records(self.hub) if r["kind"] == "shot"]
        self.assertEqual(shots[0]["line"], "261120015 BEGIN product=WHAM-HBRIDGE-PFMG474 serial=WB-0001-8 fw=v1 hub=t1 by=LVshot")
        self.assertEqual(shots[0]["client"], "LVshot")
        self.assertIn("261120015 SHOT:POINt? 2 = OK 5000 0 0", [r["line"] for r in shots])
        self.assertEqual(shots[-1]["line"], "261120015 END items=22 unanswered=0")
        # read it back: to the one who asks, nobody else
        out = await self.talk(watcher, "!hub read shotnumber 261120015")
        self.assertRegex(out[0], r"^# shot 261120015 recorded \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}: product=WHAM-HBRIDGE-PFMG474 serial=WB-0001-8 fw=v1 hub=t1 by=LVshot$")
        self.assertIn("# shot CONFig:MODE? = OK HVPS", out)
        self.assertIn("# shot SHOT? = OK 5 15000 PROFILE", out)
        self.assertEqual([l for l in out if "SHOT:POINt?" in l],
                         [f"# shot SHOT:POINt? {i} = OK {p}" for i, p in enumerate(Hbridge.points)])
        self.assertIn("# shot EXTernal:TRIGger:LATency? = OK edge_us=1", " ".join(out))
        info = json.loads(out[-1].split(" ", 2)[2])
        self.assertEqual((info["shot"], info["values"], info["complete"], info["recordings"]), ("261120015", 22, True, 1))
        self.assertEqual(await lv.quiet(), [], "the read goes only to the client that asked")

    async def test_recording_again_keeps_both_and_reads_the_newest(self):
        c = await Client.connect(self.listen, "ann", quiet=True)
        await self.talk(c, "!hub shotnumber 7")
        self.ctl.answers = dict(Hbridge.answers, **{"CONFig:MODE?": "OK LIMITER"})
        self.assertIn("LIMITER", (await self.talk(c, "!hub shotnumber 7"))[0])
        out = await self.talk(c, "!hub read shotnumber 7")
        self.assertIn("# shot CONFig:MODE? = OK LIMITER", out)
        self.assertNotIn("# shot CONFig:MODE? = OK HVPS", out)
        self.assertEqual(json.loads(out[-1].split(" ", 2)[2])["recordings"], 2)
        await self.talk(c, "!hub shotnumber 8")
        out = await self.talk(c, "!hub read shotnumber")
        self.assertEqual(len([l for l in out if " product=" in l]), 3)
        self.assertRegex(out[-2], r"^# shot \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}  8 product=WHAM-HBRIDGE-PFMG474 .* by=ann$")
        self.assertEqual(json.loads(out[-1].split(" ", 2)[2])["recorded"], 3)

    async def test_what_is_refused_and_what_is_not_found(self):
        c = await Client.connect(self.listen, "ann", quiet=True)
        for bad in ("!hub shotnumber", "!hub shotnumber 1 2", "!hub shotnumber ../x", "!hub shotnumber " + "9" * 40):
            out = await self.talk(c, bad, 0.3)
            self.assertTrue(out[0].startswith("# shot: !hub shotnumber <number>"), (bad, out))
        self.assertFalse([r for r in self.log_records(self.hub) if r["kind"] == "shot"])
        out = await self.talk(c, "!hub read shotnumber 12345", 0.4)
        self.assertIn("nothing is recorded under that number", out[0])
        self.assertEqual(json.loads((await self.talk(c, "!hub read shotnumber", 0.4))[-1].split(" ", 2)[2])["recorded"], 0)

    async def test_a_table_upload_is_noted_not_read_back_and_a_silent_controller_is_marked_incomplete(self):
        c = await Client.connect(self.listen, "ann", quiet=True)
        self.ctl.answers = dict(Hbridge.answers, **{"SHOT?": "OK 300 5000 TABLE"})
        out = await self.talk(c, "!hub shotnumber T1")
        self.assertIn("300 setpoints", out[0])
        self.assertFalse([x for x in self.ctl.received if x.startswith("SHOT:POINt?")])
        self.assertIn("# shot NOTE the controller holds a TABle upload of 300 entries, not setpoints: not read back",
                      await self.talk(c, "!hub read shotnumber T1"))
        self.hub.cfg["command_timeout_s"] = 0.3
        self.ctl.answers = dict(Hbridge.answers)
        self.ctl.silent = {"SHOT:POINt? 1"}
        out = await self.talk(c, "!hub shotnumber S2", 2.5)
        self.assertRegex(out[0], r"^# shot S2 recorded INCOMPLETE: .*; 18 values, 1 not answered$")
        self.assertNotIn("SHOT:POINt? 2", self.ctl.received, "it does not wait for every remaining setpoint")
        out = await self.talk(c, "!hub read shotnumber S2")
        self.assertIn("# shot SHOT:POINt? 1 = (no answer: TimeoutError)", out)
        self.assertFalse(json.loads(out[-1].split(" ", 2)[2])["complete"])

    async def test_clients_commands_go_in_between_and_two_requests_do_not_mix(self):
        self.ctl.delays = {}
        a = await Client.connect(self.listen, "a", quiet=True)
        b = await Client.connect(self.listen, "b", quiet=True)
        await a.send("!hub shotnumber A1")
        await b.send("!hub shotnumber B1")
        await b.send("ECHO between")
        got_b = await b.quiet(1.5)
        self.assertIn("OK between", got_b)
        self.assertTrue(any(l.startswith("# shot B1 recorded:") for l in got_b))
        self.assertTrue(any(l.startswith("# shot A1 recorded:") for l in await a.quiet(0.5)))
        lines = [r["line"] for r in self.log_records(self.hub) if r["kind"] == "shot"]
        owners = [l.split()[0] for l in lines]                                # one block ends before the other begins
        self.assertEqual(len(owners), 48)
        self.assertIn(owners, (["A1"] * 24 + ["B1"] * 24, ["B1"] * 24 + ["A1"] * 24), "the two records are not mixed")


class XrexShotTests(ShotBase):
    board = Xrex

    async def test_an_xrex_shot_is_its_timing_and_each_channels_settings(self):
        c = await Client.connect(self.listen, "LVshot", quiet=True)
        out = await self.talk(c, "!hub shotnumber 261120016", 1.2)
        self.assertEqual(out, ["# shot 261120016 recorded: timing 1.000 2.000 1.000 s, channels in the shot: 1 3; 40 values"])
        self.assertFalse([x for x in self.ctl.received if x.startswith(("SHOT:POINt", "CONFig:MODE", "PHASe", "GDS"))],
                         "an XREX board is not asked HBRIDGE's questions")
        out = await self.talk(c, "!hub read shotnumber 261120016")
        for want in ("# shot SHOT:TIMing? = OK 1.000 2.000 1.000", "# shot SHOT:CURRent? 3 = OK 300.000", "# shot SOURce:ENAble? 2 = OK 0",
                     "# shot PID:GAINS? 4 = OK 0.5 250 0", "# shot XREX:CHANnel:CONTactOut? 1 = OK 1"):
            self.assertIn(want, out)
        self.assertIn("product=WHAM-XREX-PFMG474", out[0])


class OtherTests(HubBase):
    async def test_an_unknown_product_or_a_lost_controller_records_nothing(self):
        hub = await self.start_hub()                                          # the plain fake: product WHAM-FAKE
        c = await Client.connect(self.listen, "ann", quiet=True)
        await c.send("!hub shotnumber 5")
        out = await c.quiet(0.4)
        self.assertIn("NOT recorded: WHAM-FAKE is not a product whose shot is known here", out[0])
        await self.ctl.stop()
        await asyncio.sleep(0.5)
        await c.send("!hub shotnumber 6")
        self.assertIn("NOT recorded: the controller is not connected", (await c.quiet(0.4))[0])
        self.assertFalse([r for r in self.log_records(hub) if r["kind"] == "shot"])

    async def test_the_hub_gives_the_repository_link(self):
        await self.start_hub()
        c = await Client.connect(self.listen, "ann", quiet=True)
        await c.send("!hub repo")
        self.assertIn("https://github.com/everettpenne/WHAM-CONTROLLERS", (await c.quiet(0.4))[0])
        self.assertNotIn("!hub repo", self.ctl.received)

    def test_which_product_a_board_is(self):
        self.assertEqual([wh.product_family(p) for p in ("WHAM-HBRIDGE-PFMG474", "WHAM-XREX-PFMG474", "WHAM-XREX-PFMG474-SIM", "WHAM-FAKE", None)],
                         ["hbridge", "xrex", "xrex", None, None])


if __name__ == "__main__":
    unittest.main()

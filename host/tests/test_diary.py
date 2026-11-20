"""Tests of host/diary.py (the operators' diary) and of the hub's "!hub diary" command. Run: python3 -m unittest tests.test_diary"""
import asyncio
import datetime
import os
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import diary
import wham_hub as wh
from test_wham_hub import Client, FakeController, HubBase, free_port

T = datetime.datetime


class DiaryFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "sub", "diary.txt")
        self.today = datetime.date(2026, 11, 20)

    def fill(self):
        diary.write(self.path, "Ann Example", "Bank at 520 V", "ech", T(2026, 11, 18, 9, 5, 1))
        diary.write(self.path, "LVarm", "Shot 14 fine", "lmtr-ctrl", T(2026, 11, 19, 14, 30, 0))
        diary.write(self.path, "Dan Example (Slack)", "Water flow low on XR2", "general", T(2026, 11, 20, 8, 0, 0))
        diary.write(self.path, "Ann Example", "Flow fixed, valve was half shut", "xrex", T(2026, 11, 20, 8, 40, 9))

    def ask(self, text, **kw):
        return diary.answer(self.path, diary.parse_request(text, self.today), **kw)

    def test_an_entry_is_one_dated_line_with_its_writer_and_place(self):
        e = diary.write(self.path, "Ann Example", "Bank at 520 V", "ech", T(2026, 11, 18, 9, 5, 1))
        self.assertEqual(e, {"date": "2026-11-18", "time": "09:05:01", "who": "Ann Example", "where": "ech", "text": "Bank at 520 V"})
        with open(self.path) as f:
            self.assertEqual(f.read(), "2026-11-18 09:05:01 | Ann Example | ech | Bank at 520 V\n")
        self.assertEqual(diary.entries(self.path), [e])

    def test_text_is_made_one_safe_line(self):
        e = diary.write(self.path, "A | B\n", "first line\r\n  second | line\x07\tend", " ", T(2026, 1, 1))
        self.assertEqual((e["who"], e["where"], e["text"]), ("A / B", "general", "first line / second / line end"))
        self.assertEqual(len(diary.entries(self.path)), 1)
        for bad in ("", "   ", "\n\n"):
            with self.assertRaises(diary.DiaryError):
                diary.write(self.path, "x", bad)
        with self.assertRaises(diary.DiaryError) as cm:
            diary.write(self.path, "x", "y" * 1001)
        self.assertIn("at most 1000", str(cm.exception))
        diary.write(self.path, "x", "y" * 1000)

    def test_reading_by_day_all_last_and_search_always_shows_date_time_and_writer(self):
        self.fill()
        out = self.ask("read")
        self.assertEqual(out["lines"], ["2026-11-20 08:00:00 | Dan Example (Slack) | general | Water flow low on XR2",
                                        "2026-11-20 08:40:09 | Ann Example | xrex | Flow fixed, valve was half shut"])
        self.assertEqual(out["note"], "2 entries on 2026-11-20")
        self.assertEqual(self.ask("")["lines"], out["lines"])
        self.assertEqual(self.ask("read yesterday")["lines"], ["2026-11-19 14:30:00 | LVarm | lmtr-ctrl | Shot 14 fine"])
        self.assertEqual(len(self.ask("read 2026-11-18")["lines"]), 1)
        self.assertEqual(self.ask("read 2026-11-17"), {"lines": [], "note": "no entries on 2026-11-17"})
        self.assertEqual(len(self.ask("read all")["lines"]), 4)
        for line in self.ask("read all")["lines"]:
            self.assertRegex(line, r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \| [^|]+ \| [^|]+ \| ")
        self.assertEqual(self.ask("last 1")["lines"], ["2026-11-20 08:40:09 | Ann Example | xrex | Flow fixed, valve was half shut"])
        self.assertEqual(len(self.ask("last")["lines"]), 4)
        self.assertEqual([l[:10] for l in self.ask("search flow")["lines"]], ["2026-11-20", "2026-11-20"])
        self.assertEqual(len(self.ask("search ann")["lines"]), 2, "the writer is searched too")
        self.assertEqual(len(self.ask("read all by=ann")["lines"]), 2)
        self.assertEqual(len(self.ask("read all at=lmtr")["lines"]), 1)
        self.assertEqual(len(self.ask("search flow by=dan")["lines"]), 1)
        self.assertEqual(self.ask("dates")["lines"], ["2026-11-18  1 entry", "2026-11-19  1 entry", "2026-11-20  2 entries"])

    def test_the_index_lists_who_wrote_when_without_the_texts(self):
        self.fill()
        out = self.ask("index")
        self.assertEqual(out["lines"], ["2026-11-18 09:05:01 | Ann Example | ech", "2026-11-19 14:30:00 | LVarm | lmtr-ctrl",
                                        "2026-11-20 08:00:00 | Dan Example (Slack) | general", "2026-11-20 08:40:09 | Ann Example | xrex"])
        self.assertEqual(out["note"], "4 entries in the whole diary (who wrote when; the texts are left out)")
        for word in ("Bank", "Shot", "Water", "valve"):
            self.assertNotIn(word, "\n".join(out["lines"]))
        self.assertEqual(self.ask("index all")["lines"], out["lines"])
        self.assertEqual(self.ask("list")["lines"], out["lines"])
        self.assertEqual(len(self.ask("index today")["lines"]), 2)
        self.assertEqual(self.ask("index 2026-11-19")["lines"], ["2026-11-19 14:30:00 | LVarm | lmtr-ctrl"])
        self.assertEqual(len(self.ask("index by=ann")["lines"]), 2)
        self.assertEqual(self.ask("index 2026-11-17"), {"lines": [], "note": "no entries on 2026-11-17"})
        self.assertIn("the newest 2 are shown", self.ask("index", limit=2)["note"])
        with self.assertRaises(diary.DiaryError):
            diary.parse_request("index sometime", self.today)

    def test_too_many_entries_show_the_newest_and_say_so(self):
        for i in range(30):
            diary.write(self.path, "x", f"entry {i}", now=T(2026, 11, 20, 8, i, 0))
        out = self.ask("read all", limit=10)
        self.assertEqual(len(out["lines"]), 10)
        self.assertTrue(out["lines"][-1].endswith("entry 29"))
        self.assertIn("30 entries in the whole diary; the newest 10 are shown", out["note"])
        self.assertEqual(len(self.ask("last 500", limit=10)["lines"]), 10)

    def test_a_correction_is_a_new_entry_naming_the_old_one(self):
        self.fill()
        out = self.ask("correct 08:00 It was XR3, not XR2", who="Dan Example", now=T(2026, 11, 20, 9, 0, 0))
        self.assertEqual(out["lines"][0], "2026-11-20 09:00:00 | Dan Example | general | CORRECTION of 2026-11-20 08:00:00 (Dan Example (Slack)): It was XR3, not XR2")
        self.assertEqual(len(diary.entries(self.path)), 5, "nothing is edited or removed")
        out = self.ask("correct 2026-11-18 9:05 It was 540 V", who="Ann", now=T(2026, 11, 20, 9, 1, 0))
        self.assertIn("CORRECTION of 2026-11-18 09:05:01", out["lines"][0])
        with self.assertRaises(diary.DiaryError) as cm:
            self.ask("correct 07:00 nothing there", now=T(2026, 11, 20, 9, 2, 0))
        self.assertIn("no entry at 2026-11-20 07:00", str(cm.exception))

    def test_requests_that_make_no_sense_are_explained(self):
        for text, want in (("read tomorrow", "diary read ["), ("read 2026-13-40", "not a date that exists"), ("last many", "diary last [n]"),
                           ("search", "diary search <text>"), ("correct later text", "name the time"), ("delete 3", "I do not know 'diary delete'")):
            with self.assertRaises(diary.DiaryError, msg=text) as cm:
                diary.parse_request(text, self.today)
            self.assertIn(want, str(cm.exception), text)

    def test_an_empty_or_missing_diary_and_a_hand_written_line(self):
        self.assertEqual(self.ask("read all"), {"lines": [], "note": "no entries in the whole diary: the diary is empty"})
        self.assertEqual(self.ask("dates")["note"], "the diary is empty")
        os.makedirs(os.path.dirname(self.path))
        with open(self.path, "w") as f:
            f.write("a note someone typed in an editor\n\n")
        diary.write(self.path, "x", "after", now=T(2026, 11, 20, 8, 0, 0))
        self.assertEqual(self.ask("read all")["lines"], ["a note someone typed in an editor", "2026-11-20 08:00:00 | x | general | after"])

    def test_many_writers_at_once_lose_nothing(self):
        def work(k):
            for i in range(50):
                diary.write(self.path, f"writer{k}", f"entry {i} " + "x" * 200)
        threads = [threading.Thread(target=work, args=(k,)) for k in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        got = diary.entries(self.path)
        self.assertEqual(len(got), 400)
        self.assertTrue(all(e["date"] and e["text"].endswith("x" * 200) for e in got), "no line was torn or mixed")

    def test_the_default_file_is_beside_the_log_folder(self):
        self.assertEqual(diary.default_path("/var/lib/wham/hub/logs"), "/var/lib/wham/hub/diary.txt")


class HubDiaryTests(HubBase):
    async def test_write_and_read_through_a_hub_with_the_writer_recorded_and_the_others_told(self):
        hub = await self.start_hub()
        self.assertEqual(hub.diary_path(), os.path.join(self.tmp.name, "diary.txt"))
        ann = await Client.connect(self.listen, "ann")
        bob = await Client.connect(self.listen, "bob")
        quiet = await Client.connect(self.listen, "labview", quiet=True)
        await ann.quiet()
        await bob.quiet()
        await ann.send("!hub diary write Bank raised to 540 V | before lunch")
        out = await ann.quiet()
        self.assertTrue(any(l.startswith("# diary: written 20") and l.endswith("by ann") for l in out), out)
        told = await bob.quiet()
        self.assertTrue(any(l.startswith("--- diary: 20") and "| ann | t1 | Bank raised to 540 V / before lunch ---" in l for l in told), told)
        self.assertEqual(await quiet.quiet(), [], "a quiet client (a program) is not told")
        await bob.send("!hub diary read")
        out = await bob.quiet()
        self.assertRegex(out[0], r"^# diary \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \| ann \| t1 \| Bank raised to 540 V / before lunch$")
        self.assertEqual(out[-1], "# diary-end 1 entry on " + datetime.date.today().isoformat())
        await quiet.send("!hub diary write from the program")
        self.assertTrue(any("by labview" in l for l in await quiet.quiet()))
        await bob.send("!hub diary last 5 by=labview")
        out = await bob.quiet()
        self.assertEqual(len([l for l in out if l.startswith("# diary 20")]), 1)
        await bob.send("!hub diary read all")
        self.assertEqual(len([l for l in await bob.quiet() if l.startswith("# diary 20")]), 2)
        await bob.send("!hub diary index")
        out = await bob.quiet()
        self.assertRegex(out[0], r"^# diary \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \| ann \| t1$")
        self.assertFalse(any("540" in l or "from the program" in l for l in out), "the index has no texts")
        self.assertIn("2 entries in the whole diary (who wrote when", out[-1])
        for bad, want in (("!hub diary write", "the entry is empty"), ("!hub diary read never", "diary read ["), ("!hub diary frobnicate", "I do not know")):
            await bob.send(bad)
            out = await bob.quiet()
            self.assertTrue(any(l.startswith("# diary: ") and want in l for l in out), (bad, out))
        self.assertEqual(self.ctl.received.count("!hub diary read"), 0)
        self.assertFalse([c for c in self.ctl.received if "diary" in c.lower()], "nothing of the diary goes to the controller")
        recs = [r for r in self.log_records(hub) if r.get("kind") == "hub-cmd" and "diary write " in r.get("line", "")]
        self.assertEqual(len(recs), 2, "the hub's own log also shows who wrote")

    async def test_every_hub_on_the_host_shares_one_diary(self):
        await self.start_hub()
        ctl2 = FakeController(serial="WB-0002-6")
        port2 = await ctl2.start()
        self.addAsyncCleanup(ctl2.stop)
        listen2 = free_port()
        hub2 = wh.Hub(self.cfg(name="t2", port=port2, listen_port=listen2))
        task = asyncio.ensure_future(hub2.run())
        self.addAsyncCleanup(self.shutdown, hub2, task)
        await asyncio.sleep(0.5)
        a = await Client.connect(self.listen, "ann")
        b = await Client.connect(listen2, "bob")
        await a.send("!hub diary write written at the first hub")
        await a.quiet()
        await b.send("!hub diary write written at the second hub")
        await b.quiet()
        await b.send("!hub diary read")
        out = [l for l in await b.quiet() if l.startswith("# diary 20")]
        self.assertEqual([l.split(" | ")[1:] for l in out], [["ann", "t1", "written at the first hub"], ["bob", "t2", "written at the second hub"]])

    async def test_the_file_can_be_put_elsewhere(self):
        path = os.path.join(self.tmp.name, "elsewhere", "book.txt")
        await self.start_hub(diary_file=path)
        c = await Client.connect(self.listen, "ann")
        await c.send("!hub diary write here")
        await c.quiet()
        self.assertEqual(len(diary.entries(path)), 1)


if __name__ == "__main__":
    unittest.main()

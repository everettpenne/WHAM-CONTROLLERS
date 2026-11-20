"""diary.py -- the operators' diary: one plain text file on the host that everyone working with the controllers can read
and add to. The hubs (wham_hub.py, "!hub diary ...") and the Slack bot (slack_bot/wham_slack.py, "diary ...") both use
this module on the same file, so an entry written from a terminal, the operator console, LabVIEW or Slack lands in one
place.

The file (default <the hubs' log folder>/../diary.txt, on the lab server /var/lib/wham/hub/diary.txt; the hub config key
diary_file moves it) has one entry per line, readable and editable with any text tool:

    2026-11-20 11:02:13 | Everett Penne (Slack) | ech | Raised the bank charge to 540 V for the afternoon shots.

    time (the host's local clock) | who wrote it | where (the controller it was written at, or "general") | the text

Entries are only ever appended: the diary is a record, so there is no edit or delete command. A correction is a new entry
(the "correct" command writes one that names the time of the entry it corrects). Several programs append at once safely
(an exclusive lock around each write). A line someone adds by hand that does not have this shape is kept and shown as it is.

What can be asked (parse_request / answer; the hub and the bot only add their own framing):
    write <text>                         add an entry
    correct <time or date time> <text>   add an entry marked as a correction of an earlier one
    read                                 today's entries
    read <date>                          one day: 2026-11-20, today, yesterday
    read all                             everything (the newest are shown when there are too many; the reply says so)
    last [n]                             the newest n entries (default 10)
    search <text>                        entries containing the text (in the text, the writer or the place), newest last
    dates                                the days that have entries, with how many
    index [date|all]                     who wrote when and where, WITHOUT the texts: one line per entry (all days by default)
  read, last, search and index also take by=<who> and at=<where> to narrow them down.

Standard library only.
"""
import datetime
import fcntl
import os
import re

MAX_TEXT = 1000                     # characters of one entry
SEP = " | "
LINE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2}) \| (.*?) \| (.*?) \| (.*)$")
HELP = ("diary write <text>  |  diary read [today|yesterday|YYYY-MM-DD|all]  |  diary last [n]  |  diary search <text>  |  "
        "diary dates  |  diary index [date|all] (who wrote when, without the texts)  |  "
        "diary correct <HH:MM or YYYY-MM-DD HH:MM> <text>   (read, last, search, index also take by=<who> at=<where>)")


class DiaryError(ValueError):
    """Something about the request is wrong; the message is for the person who asked."""


def default_path(log_dir):
    return os.path.join(os.path.dirname(os.path.abspath(log_dir)), "diary.txt")


def clean(text, limit):
    """One line, no separator inside, no control characters."""
    text = re.sub(r"\s*[\r\n]+\s*", " / ", str(text).strip())
    text = "".join(c for c in text if c == "\t" or ord(c) >= 32).replace("\t", " ")
    return text.replace(SEP, " / ").strip()[:limit]


def write(path, who, text, where="general", now=None):
    """Append one entry; returns it as a dict. Raises DiaryError for an empty entry."""
    body = clean(text, MAX_TEXT + 1)
    if not body:
        raise DiaryError("the entry is empty: diary write <text>")
    if len(body) > MAX_TEXT:
        raise DiaryError(f"the entry is {len(clean(text, 100000))} characters; at most {MAX_TEXT} fit in one. Write it as two entries.")
    t = (now or datetime.datetime.now()).replace(microsecond=0)
    entry = {"date": t.strftime("%Y-%m-%d"), "time": t.strftime("%H:%M:%S"), "who": clean(who, 60) or "?",
             "where": clean(where, 30) or "general", "text": body}
    line = f"{entry['date']} {entry['time']}{SEP}{entry['who']}{SEP}{entry['where']}{SEP}{entry['text']}\n"
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        fcntl.flock(f, fcntl.LOCK_EX)               # the hubs and the bot are separate programs writing one file
        try:
            f.write(line)
            f.flush()
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)
    return entry


def entries(path):
    """Every entry, oldest first. A line that is not in the diary's shape is kept, with empty fields."""
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                if not line.strip():
                    continue
                m = LINE_RE.match(line)
                out.append({"date": m.group(1), "time": m.group(2), "who": m.group(3), "where": m.group(4), "text": m.group(5)}
                           if m else {"date": "", "time": "", "who": "", "where": "", "text": line})
    except FileNotFoundError:
        pass
    return out


def line_of(e):
    return f"{e['date']} {e['time']}{SEP}{e['who']}{SEP}{e['where']}{SEP}{e['text']}" if e["date"] else e["text"]


def index_line(e):
    """An entry without its text: when, who, where."""
    return f"{e['date']} {e['time']}{SEP}{e['who']}{SEP}{e['where']}" if e["date"] else "(a line without a date)"


def a_date(word, today=None):
    """today / yesterday / YYYY-MM-DD -> "YYYY-MM-DD", or None when the word is not a date."""
    today = today or datetime.date.today()
    w = word.lower()
    if w == "today":
        return today.isoformat()
    if w == "yesterday":
        return (today - datetime.timedelta(days=1)).isoformat()
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", w):
        try:
            return datetime.date.fromisoformat(w).isoformat()
        except ValueError:
            raise DiaryError(f"{word} is not a date that exists")
    return None


def parse_request(text, today=None):
    """The words after "diary" -> a request dict: {"do": write|correct|read|last|search|dates, ...}. Raises DiaryError."""
    words = text.strip().split()
    if not words:
        return {"do": "read", "date": a_date("today", today)}
    verb, rest = words[0].lower(), text.strip()[len(words[0]):].strip()
    if verb in ("write", "add", "w"):
        return {"do": "write", "text": rest}
    if verb in ("correct", "amend"):
        m = re.match(r"^((\d{4}-\d{2}-\d{2})\s+)?(\d{1,2}:\d{2}(:\d{2})?)\s+(.+)$", rest, re.S)
        if not m:
            raise DiaryError("diary correct <HH:MM or YYYY-MM-DD HH:MM> <text>: name the time of the entry you are correcting")
        return {"do": "correct", "ref": ((m.group(2) + " ") if m.group(2) else "") + m.group(3), "text": m.group(5)}
    req = {"do": verb, "by": None, "at": None}
    free = []
    for w in rest.split():
        m = re.fullmatch(r"(by|at)=(.+)", w, re.I)
        if m and verb != "search":
            req[m.group(1).lower()] = m.group(2).lower()
        elif m:
            req[m.group(1).lower()] = m.group(2).lower()
        else:
            free.append(w)
    if verb == "read":
        if not free:
            req["date"] = a_date("today", today)
        elif free[0].lower() == "all" and len(free) == 1:
            req["date"] = None
        else:
            d = a_date(free[0], today) if len(free) == 1 else None
            if d is None:
                raise DiaryError("diary read [today|yesterday|YYYY-MM-DD|all]")
            req["date"] = d
        return req
    if verb in ("last", "tail", "recent"):
        req["do"] = "last"
        if len(free) > 1 or (free and not free[0].isdigit()):
            raise DiaryError("diary last [n]")
        req["n"] = max(1, int(free[0])) if free else 10
        return req
    if verb in ("search", "find", "grep"):
        req["do"] = "search"
        if not free:
            raise DiaryError("diary search <text>")
        req["text"] = " ".join(free).lower()
        return req
    if verb in ("dates", "days"):
        return {"do": "dates"}
    if verb in ("index", "list", "entries", "names", "toc"):
        req["do"] = "index"
        if not free or (len(free) == 1 and free[0].lower() == "all"):
            req["date"] = None
        else:
            d = a_date(free[0], today) if len(free) == 1 else None
            if d is None:
                raise DiaryError("diary index [today|yesterday|YYYY-MM-DD|all]")
            req["date"] = d
        return req
    raise DiaryError("I do not know 'diary " + words[0][:20] + "'. " + HELP)


def answer(path, req, who="?", where="general", limit=200, now=None):
    """Carry a request out. Returns {"lines": [...], "note": text}: the lines are entries (or dates), oldest first; the
    note is one sentence about what was done or shown."""
    if req["do"] in ("write", "correct"):
        text = req["text"]
        if req["do"] == "correct":
            ref = req["ref"]
            date = ref.split()[0] if " " in ref else (now or datetime.datetime.now()).strftime("%Y-%m-%d")
            hm = ref.split()[-1]
            hit = [e for e in entries(path) if e["date"] == date and e["time"].startswith(hm if len(hm) > 5 else hm.zfill(5))]
            if not hit:
                raise DiaryError(f"there is no entry at {date} {hm} to correct (diary read {date} shows that day)")
            text = f"CORRECTION of {hit[-1]['date']} {hit[-1]['time']} ({hit[-1]['who']}): {text}"
        e = write(path, who, text, where, now)
        return {"lines": [line_of(e)], "note": f"written {e['date']} {e['time']} by {e['who']}"}
    all_ = entries(path)
    if req["do"] == "dates":
        counts = {}
        for e in all_:
            counts[e["date"] or "(no date)"] = counts.get(e["date"] or "(no date)", 0) + 1
        return {"lines": [f"{d}  {n} entr{'y' if n == 1 else 'ies'}" for d, n in sorted(counts.items())],
                "note": f"{len(all_)} entries on {len(counts)} day(s)" if all_ else "the diary is empty"}
    sel = all_
    if req.get("by"):
        sel = [e for e in sel if req["by"] in e["who"].lower()]
    if req.get("at"):
        sel = [e for e in sel if req["at"] in e["where"].lower()]
    if req["do"] in ("read", "index") and req.get("date"):
        sel = [e for e in sel if e["date"] == req["date"]]
        what = f"on {req['date']}"
    elif req["do"] in ("read", "index"):
        what = "in the whole diary"
    elif req["do"] == "search":
        sel = [e for e in sel if req["text"] in line_of(e).lower()]
        what = f"containing \"{req['text']}\""
    else:
        what = "in the diary"
    cap = min(limit, req["n"]) if req["do"] == "last" else limit
    shown = sel[-cap:]
    if not sel:
        note = f"no entries {what}" + ("" if all_ else ": the diary is empty")
    elif req["do"] == "last":
        note = f"the newest {len(shown)} of {len(sel)} entries"
    elif len(shown) < len(sel):
        note = f"{len(sel)} entries {what}; the newest {len(shown)} are shown (read one date, or search, for the rest)"
    else:
        note = f"{len(sel)} entr{'y' if len(sel) == 1 else 'ies'} {what}"
    if req["do"] == "index":
        return {"lines": [index_line(e) for e in shown], "note": note + " (who wrote when; the texts are left out)" if sel else note}
    return {"lines": [line_of(e) for e in shown], "note": note}

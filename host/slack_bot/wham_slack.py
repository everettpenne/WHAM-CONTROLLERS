#!/usr/bin/env python3
"""wham_slack.py -- a Slack bot that connects to the WHAM controllers through their hubs (wham_hub.py), as a text terminal.

In a direct message with the bot (or with /wham <words>, or by mentioning it):

    list                     the controllers: state, fault, who is connected
    connect ech              open a session to that controller through its hub
    STATE?                   ... while connected, every line you type is sent to the controller and its reply is posted
    .hublog on               show ALL traffic of that hub: other clients' commands and replies, joins, events (.hublog off)
    .log 20 since=1h         the hub's stored log (also grep=<text>, kind=cmd, client=<name>)
    .log file [date] [raw]   a whole day of that hub's log as a file to download (today by default; log all file: every hub,
                             merged; log <controller> file when not connected; log days lists the days kept)
    .disconnect              close the session
    .users                   who is authorized to use the bot
    repo                     the link to the project's public repository
    where                    what you are connected to right now, and where each kind of line you type will go

Telling things apart. Every line relayed from a session starts with its source: "[ech] STATE? -> OK IDLE" is the
controller's reply to your command, "[ech hub] ..." is the hub answering a !hub line, "[ech event] ..." is something
the controller said by itself, "[ech traffic] ..." is other people's traffic (hub-log). The bot also puts a reaction on
each message you send, showing where it went: gear = sent to the controller, file cabinet = sent to the hub, robot =
handled by the bot, hourglass = waiting for .confirm, no-entry = not sent. (Reactions need the app's reactions:write
permission; without it everything else works.)
    diary write <text>       add an entry to the operators' diary (the time and your name are recorded)
    diary read [date|all]    read it: today, a date, yesterday, or all; also diary last [n], diary search <text>, diary dates
    diary file               send the whole diary as a text file to download (needs the app's files:write permission)
    diary index              who wrote when and where, without the texts; diary index file sends that list as a file
    watch all                show the traffic of EVERY controller at once, each line marked with its controller (watch off)
    log all 20 since=1h      the stored logs of every hub, merged in time order
    help                     the list of commands (at any time)

While connected, a line that starts with a dot is for the bot; everything else goes to the controller. Not connected, the dot
is optional, and "ech state" or "ech SHOT?" asks one controller one thing without opening a session.

What may be sent
  * queries and ordinary commands: yes, from any allowed person, in a session;
  * a command on the project's dangerous list (dangerous_commands.py: ARM, FIRE, FAULT:CLEar, CHARGE, anything that weakens a
    protection): REFUSED, unless the sender's Slack user ID is listed in danger_users. Then the bot shows the command and its
    category and sends it only after the same person answers ".confirm" within 30 s. danger_users is empty by default: out
    of the box Slack cannot arm or fire anything. Firmware and identity commands are never sent, whoever asks;
  * the long dumps (LOG:DATA?, SYS:EVLOG?, ...) and lines for the hub's own bookkeeping: refused (use the operator console);
  * a mention or a one-off question outside a session only ever reads (first word must end in "?").

Who may use it: only people listed in allow_users, or messages in a channel listed in allow_channels (Slack IDs, U... and
C...). With both lists empty it answers nobody. A user is limited to rate_per_minute messages.

Which controllers: every hub this server runs. The bot reads the hub configs next to it (host/config/<name>.yaml, the files
the hub services are started from) and offers each one, so a controller that gets a hub appears in list, connect and
"watch all" by itself within a minute, under its hub's name. The "controllers" entries of the settings give the three
installed ones their short names. A controller with no hub cannot be reached: the bot never talks to a bridge directly.

A session is a real hub client named "slack-<user>": the hub's log records who sent what, the other clients see the commands
as they see anyone's, and "People on this controller" in the operator console lists it. Sessions and watches
stay open as long as you like during the day and are all closed once a night, at session_close_at (default 03:15, the
server's clock, just before the hubs' nightly update), each with a note saying so: a forgotten one therefore holds up a
hub update for a day at most. session_idle_min (0 = off, the default) can also close one after that long without a message.

How it talks to Slack: Socket Mode. The bot opens an outgoing connection to Slack; nothing listens on this machine and no
port is opened in the lab's network. It needs two tokens from a Slack app you create (docs/slack_bot.md): an app-level
token (xapp-..., scope connections:write) and a bot token (xoxb-..., scopes chat:write, im:history, app_mentions:read,
commands). They are read from the environment (SLACK_APP_TOKEN, SLACK_BOT_TOKEN) or from
host/slack_bot/slack_bot.local.json, which git ignores. Never put a token in a tracked file: the repository is public.

    python3 host/slack_bot/wham_slack.py run            # the bot (exits at once if another copy is running)
    python3 host/slack_bot/wham_slack.py check          # tokens, Slack reachable, each hub answers; changes nothing
    python3 host/slack_bot/wham_slack.py ask "ech state" # what the bot would answer to a one-off question, without Slack
    python3 host/slack_bot/wham_slack.py install-cron   # start at boot and restart within 5 minutes if it stops

It runs as the hubs' account with no sudo: cron is its supervisor. It stops by itself when the checkout it runs from moves
to another commit (hub_update.py) and no session is open, and cron starts it again on the new code. Standard library only.
"""
import argparse
import base64
import collections
import datetime
import fcntl
import hashlib
import json
import os
import re
import signal
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
HOST_DIR = os.path.dirname(HERE)
sys.path.insert(0, HOST_DIR)
import dangerous_commands as dc
import diary
import wham_hub

LOCAL_CONFIG = os.path.join(HERE, "slack_bot.local.json")
CRON_MARK = "# wham slack bot"
DEFAULTS = {
    "app_token": "", "bot_token": "", "allow_channels": [], "allow_users": [], "danger_users": [], "rate_per_minute": 30,
    "session_idle_min": 0,           # close a session or watch after this long without a message; 0 = never (the default)
    "session_close_at": "03:15",     # every night at this time (the server's clock) all sessions and watches are closed; "" = never
    "confirm_s": 30,
    "discover": True, "discover_dir": os.path.join(HOST_DIR, "config"),     # offer every hub configured on this server
    "diary_file": None,              # the operators' diary; default: the file the hubs use (beside their log folder)
    "state_dir": "/var/lib/wham/hub/slack",
    "controllers": {
        "xrex": {"host": "127.0.0.1", "port": 5101, "aliases": ["magnets", "transrex"]},
        "limiter": {"host": "127.0.0.1", "port": 5102, "aliases": ["lmtr", "lim"]},
        "ech": {"host": "127.0.0.1", "port": 5103, "aliases": ["hvps", "gyrotron"]},
    },
}
MAX_TEXT = 3500                     # a Slack message holds about 4000 characters
REPO_URL = "https://github.com/everettpenne/WHAM-CONTROLLERS"
REPO_TEXT = (f"*WHAM-CONTROLLERS*, the project's public repository: {REPO_URL}\n"
             "Firmware for both controllers, the board design files, the hubs, the operator console, this bot and the guides. "
             f"Start with the guides: {REPO_URL}/tree/main/docs (this bot: `docs/slack_bot.md`; the diary: `docs/diary.md`).")
HELP = """*WHAM-PFM-Controller-Link: the WHAM controllers, through their hubs*
*Any time* (with or without a dot)
\u2022 `help`  this list
\u2022 `where`  what you are connected to right now, and where each kind of line will go
\u2022 `list`  the controllers: state, fault, who is connected
\u2022 `connect <controller>`  open a session; then every line you type goes to the controller and its reply comes back
\u2022 `watch all` / `watch off`  the traffic of every controller at once, each line marked `[controller]` (watching only)
\u2022 `log all [n] [since=1h] [grep=text]`  the stored logs of every hub, merged in time order
\u2022 `diary write <text>`  add an entry to the operators' diary (time and your name are recorded)
\u2022 `diary read [today|yesterday|YYYY-MM-DD|all]`, `diary last [n]`, `diary search <text>`, `diary dates`  read it
\u2022 `diary index [date]`  who wrote when, without the texts; `diary file`, `diary index file`  as a file to download
\u2022 `repo`  the link to the project's public repository (code, board files, guides)
*While connected* (a line starting with a dot is for me; anything else is sent to the controller)
\u2022 `STATE?`, `SHOT?`, ...  any command for the controller
\u2022 `!hub status`, `!hub clients`, `!hub shotnumber <n>`, `!hub read shotnumber <n>`  lines for the hub
\u2022 `.hublog on` / `.hublog off`  show all traffic of that hub (other people's commands and replies, events)
\u2022 `.log [n] [since=1h] [grep=text] [kind=cmd]`  the hub's stored log (n up to 30)
\u2022 `.log file [today|yesterday|YYYY-MM-DD] [raw]`  a whole day of the hub's log as a file; `.log days`  the days kept
\u2022 `.who`  who is connected to that hub
\u2022 `.users`  who is authorized to use this bot
\u2022 `.disconnect`  close the session
*Not connected*
\u2022 `<controller> state`, `<controller> <QUERY?>`  ask one thing (read only)
\u2022 `log <controller> file [date]`, `log all file`, `log <controller> days`  hub logs as files
*Reading the answers*
\u2022 Controllers: {names}.
\u2022 Replies are labelled: `[ech] CMD \u2192 reply` controller, `[ech hub]` the hub, `[ech event]` the controller by itself, `[ech traffic]` other people.
\u2022 Reactions on your messages: :gear: sent to the controller \u00b7 :file_cabinet: sent to the hub \u00b7 :robot_face: handled by me \u00b7 :hourglass_flowing_sand: waiting for `.confirm` \u00b7 :no_entry_sign: not sent
\u2022 Dangerous commands (ARM, FIRE, FAULT:CLEar, CHARGE, protections) are refused unless you are on the danger list; then they need `.confirm`.
Project repository and guides: https://github.com/everettpenne/WHAM-CONTROLLERS"""
MAX_LINES = 20                      # lines of one Slack message sent to a controller


# ---- configuration ------------------------------------------------------------------------------------------------
def load_config(path=LOCAL_CONFIG, env=os.environ):
    cfg = json.loads(json.dumps(DEFAULTS))
    if path and os.path.exists(path):
        with open(path) as f:
            cfg.update(json.load(f))
    cfg["app_token"] = env.get("SLACK_APP_TOKEN") or cfg["app_token"]
    cfg["bot_token"] = env.get("SLACK_BOT_TOKEN") or cfg["bot_token"]
    return cfg


def find_controller(cfg, word):
    """The controller a word names: its name or an alias, else the one name or alias that starts with the word."""
    w = word.lower()
    starts = set()
    for name, c in cfg["controllers"].items():
        labels = [name.lower()] + [a.lower() for a in c.get("aliases", [])]
        if w in labels:
            return name
        if w and any(l.startswith(w) for l in labels):
            starts.add(name)
    return starts.pop() if len(starts) == 1 else None


def discover_hubs(folder):
    """{hub name: (host, port)} for every hub configured in host/config (the files the hub services are started from)."""
    found = {}
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        return found
    for f in names:
        if not f.endswith(".yaml") or f.endswith(".local.yaml") or f == "example.yaml":
            continue
        try:
            hc = wham_hub.load_config(os.path.join(folder, f))
            port = int(hc.get("listen_port") or 0)
        except (OSError, ValueError, TypeError):
            continue
        if port:
            host = str(hc.get("listen_host") or "0.0.0.0")
            found[f[:-5]] = ("127.0.0.1" if host in ("0.0.0.0", "::", "") else host, port)
    return found


def diary_file(cfg):
    """The diary the hubs on this server use: named in the settings, else taken from the first hub's configuration."""
    if cfg.get("diary_file"):
        return cfg["diary_file"]
    try:
        for f in sorted(os.listdir(cfg["discover_dir"])):
            if f.endswith(".yaml") and not f.endswith(".local.yaml") and f != "example.yaml":
                hc = wham_hub.load_config(os.path.join(cfg["discover_dir"], f))
                if hc.get("diary_file"):
                    return hc["diary_file"]
                if hc.get("log_dir"):
                    return diary.default_path(hc["log_dir"])
    except (OSError, ValueError):
        pass
    return os.path.join(os.path.dirname(os.path.abspath(cfg["state_dir"])), "diary.txt")


def merge_controllers(configured, found):
    """The controllers the bot offers: the configured ones (a hub found on the same port adds its name as an alias) plus
    every other hub found, under its hub name."""
    out = json.loads(json.dumps(configured))
    for hub, (host, port) in sorted(found.items()):
        same = next((n for n, c in out.items() if int(c["port"]) == port and c["host"] in (host, "127.0.0.1", "localhost")), None)
        if same is None and hub not in out:
            out[hub] = {"host": host, "port": port, "aliases": [], "discovered": True}
        elif same is not None and hub != same and hub not in out[same].setdefault("aliases", []):
            out[same]["aliases"].append(hub)
    return out


# ---- what may be sent ---------------------------------------------------------------------------------------------
def refusal(command):
    """None when the line may be sent to a controller (a short query), else why not, in words for the person who asked."""
    text = command.strip()
    if not text or "\n" in text or "\r" in text or len(text) > 200:
        return "That is not one command line."
    if text.startswith(("!", "@")):
        return "Lines for the hub itself are not passed on (use `who` or `log`)."
    head = text.split()[0]
    danger = dc.classify(text)
    if danger is not None:
        return (f"`{head}` is on the dangerous-command list ({danger.category}). This bot only reads: "
                "it is refused here and was not sent. Use the operator console.")
    if not head.endswith("?"):
        return f"`{head}` is not a query (it does not end in `?`). This bot only reads: it was not sent."
    if wham_hub.reply_spec(text)[0]:
        return (f"`{head}` is a long dump: it holds the controller's serial link for seconds and does not fit a Slack "
                "message. Use the operator console.")
    return None


def session_check(command, may_danger=False):
    """For a line typed in a session: (verdict, text). verdict "send": send it. "confirm": dangerous, the sender is on
    the danger list, ask first (text = the question). "refuse": text says why; nothing is sent."""
    text = command.strip()
    if not text or len(text) > 400:
        return "refuse", "That is not one command line."
    head = text.split()[0]
    if text.startswith("@"):
        return "refuse", "Lines starting with @ are for the hub's handshake and are not passed on."
    if text.startswith("!"):
        words = text.lower().split()
        if words[0] == "!hub" and words[1:2] and words[1] in ("status", "clients", "who", "identify", "log", "shotnumber"):
            return "send", ""
        if words[:3] == ["!hub", "read", "shotnumber"]:
            return "send", ""
        return "refuse", ("Of the hub's own commands only `!hub status`, `!hub clients`, `!hub identify` and `!hub log` are "
                          "passed on, with `!hub shotnumber <n>` and `!hub read shotnumber <n>` (use `.hublog` for the traffic view).")
    if wham_hub.reply_spec(text)[0]:
        return "refuse", (f"`{head}` is a long dump or a firmware step: it holds the controller's serial link for seconds and "
                          "does not fit Slack. Use the operator console or the firmware tools.")
    danger = dc.classify(text)
    if danger is None:
        return "send", ""
    if danger.category in ("firmware", "identity"):
        return "refuse", f"`{head}` ({danger.category}) is never sent from Slack. Use the firmware tools on a computer."
    if not may_danger:
        return "refuse", (f"`{head}` is on the dangerous-command list ({danger.category}): it was NOT sent. "
                          "Your Slack user is not on this bot's danger list; use the operator console.")
    return "confirm", f"`{text}` is dangerous ({danger.category})."


class Outbox:
    """Lines on their way to Slack: batched into code blocks, at most one post a second and three per batch; a flood is
    counted, not relayed."""

    def __init__(self, post):
        self.post, self.lines, self.skipped, self.first, self.last_add = post, [], 0, None, 0.0
        self.lock, self.stop = threading.Lock(), threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def add(self, text):
        with self.lock:
            self.lines.append(text if len(text) <= 300 else text[:300] + f" … ({len(text)} characters)")
            self.last_add = time.monotonic()
            self.first = self.first or self.last_add
            if len(self.lines) > 300:
                self.skipped += len(self.lines) - 100
                self.lines = self.lines[-100:]

    def flush(self):
        with self.lock:
            lines, skipped, self.lines, self.skipped, self.first = self.lines, self.skipped, [], 0, None
        posts = 0
        while lines:
            if posts == 3:                         # at most three messages per batch: the rest is counted, not relayed
                skipped += len(lines)
                break
            posts += 1
            chunk, size = [], 0
            while lines and len(chunk) < 60 and size + len(lines[0]) < 2900:
                size += len(lines[0]) + 1
                chunk.append(lines.pop(0))
            self.post("```\n" + "\n".join(chunk).replace("```", "` ` `") + "\n```")
            if lines and posts < 3:
                time.sleep(1.0)
        if skipped:
            self.post(f"… {skipped} more lines were not shown (too much traffic for Slack; `.log` has them).")

    def run(self):
        last_post = 0.0
        while not self.stop.wait(0.2):
            now = time.monotonic()
            if self.lines and now - last_post >= 1.0 and (now - self.last_add > 0.35 or now - self.first > 1.5):
                self.flush()
                last_post = time.monotonic()
        self.flush()

    def close(self):
        self.stop.set()
        if threading.current_thread() is not self.thread:
            self.thread.join(5)


class Session:
    """One connection to one hub for a Slack conversation: lines typed go out, every line the hub sends goes to the
    outbox (its own, or the one a watch shares, with a prefix naming the controller)."""

    def __init__(self, name, host, port, who, post, idle_s=1800.0, on_close=lambda s, why: None, outbox=None, prefix="",
                 quiet=True, announce=True):
        self.name, self.who, self.post, self.idle_s, self.on_close = name, who, post, idle_s, on_close
        self.prefix, self.announce = prefix, announce
        self.own_outbox = outbox is None
        self.outbox = outbox or Outbox(post)
        try:
            self.sock = socket.create_connection((host, port), timeout=5.0)
            self.sock.sendall(f"@{'quiet' if quiet else 'name'} {who}\n".encode())
        except OSError:
            if self.own_outbox:
                self.outbox.close()
            raise
        self.hublog = not quiet
        self.sent = collections.deque(maxlen=300)   # commands waiting for the controller's reply, oldest first
        self.confirm = None          # (command, time) waiting for .confirm
        self.last_input = time.monotonic()
        self.closed = False
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.reader, daemon=True)
        self.thread.start()

    def send(self, line):
        self.last_input = time.monotonic()
        with self.lock:
            if not line.startswith("!"):
                self.sent.append(line)             # the hub answers each command with exactly one line, in order
            self.sock.sendall((line + "\n").encode())

    def label(self, text):
        """A line from the hub with its source in front, so a controller's reply, the hub's own answer, an event and
        other people's traffic cannot be mistaken for one another (or for something the bot said)."""
        if self.prefix:
            return self.prefix + text              # a watch: everything is that controller's traffic
        if text.startswith("#"):
            return f"[{self.name} hub] {text}"
        if text.startswith("!"):
            return f"[{self.name} event] {text}"
        if text.startswith(("--- ", "[")):
            return f"[{self.name} traffic] {text}"
        with self.lock:
            cmd = self.sent.popleft() if self.sent else None
        return f"[{self.name}] {cmd} \u2192 {text}" if cmd else f"[{self.name}] {text}"

    def set_hublog(self, on):
        self.hublog = bool(on)
        self.send("!hub quiet " + ("off" if on else "on"))

    def reader(self):
        buf, why = b"", None
        self.sock.settimeout(0.3)
        while not self.closed:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    why = "the hub closed the connection"
                    break
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode(errors="replace").rstrip()
                    if text and text not in ("# hub: quiet on", "# hub: quiet off") and not text.startswith("--- connected as "):
                        self.outbox.add(self.label(text))
            except socket.timeout:
                pass
            except OSError as e:
                why = None if self.closed else f"the connection to the hub failed ({e})"
                break
            if self.idle_s and time.monotonic() - self.last_input > self.idle_s:
                why = f"nothing was typed for {self.idle_s / 60:.0f} minutes"
                break
        self.close(why)

    def close(self, why=None):
        first = not self.closed
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass
        if first:
            if self.own_outbox:
                self.outbox.close()
            self.on_close(self, why)
            if why and self.announce:
                self.post(f"Session with *{self.name}* closed: {why}. What the controller is doing keeps going.")


class Watch:
    """The traffic of several hubs in one stream: one hub-log connection each, one shared outbox, every line marked
    with its controller. Nothing is ever sent to a controller from a watch."""

    def __init__(self, controllers, who, post, idle_s, on_close=lambda w: None):
        self.post, self.on_close, self.closed = post, on_close, False
        self.outbox = Outbox(post)
        self.sessions, self.failed = {}, {}
        for name, (host, port) in controllers.items():
            try:
                self.sessions[name] = Session(name, host, port, who, post, idle_s, on_close=self.lost, outbox=self.outbox,
                                              prefix=f"[{name}] ", quiet=False, announce=False)
            except OSError as e:
                self.failed[name] = str(e)

    def lost(self, session, why):
        if self.closed:
            return
        self.sessions.pop(session.name, None)
        if why and why.startswith("nothing was typed"):
            self.close(why)                                    # idle: the whole watch ends, said once
        elif why:
            self.post(f"*{session.name}*: {why}. " + ("Still watching " + ", ".join(self.sessions) + "." if self.sessions else ""))
            if not self.sessions:
                self.close("no hub is left to watch")

    def close(self, why=None):
        if self.closed:
            return
        self.closed = True
        for s in list(self.sessions.values()):
            s.close()
        self.outbox.close()
        self.on_close(self)
        if why:
            self.post(f"Watching stopped: {why}.")


# ---- one request to a hub -----------------------------------------------------------------------------------------
def hub_request(host, port, who, line, done, timeout=8.0):
    """Connect as the quiet client `who`, send one line, collect reply lines until done(lines) is true. Returns the lines, or
    raises OSError/TimeoutError with a reason."""
    sock = socket.create_connection((host, port), timeout=min(timeout, 5.0))
    try:
        sock.sendall(f"@quiet {who}\n{line}\n".encode())
        buf, lines, end = b"", [], time.monotonic() + timeout
        while time.monotonic() < end:
            sock.settimeout(max(0.05, end - time.monotonic()))
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                if text:
                    lines.append(text)
            if done(lines):
                return lines
        if done(lines):
            return lines
        raise TimeoutError("no complete answer from the hub in time")
    finally:
        sock.close()


def reply_line(lines):
    return next((l for l in lines if l.startswith(("OK", "ERR"))), None)


class Bot:
    def __init__(self, cfg, request=hub_request, now=time.time, config_path=None, person=lambda user: user, upload=None):
        self.cfg, self.request, self.now = cfg, request, now
        self.upload = upload                                          # (channel, file name, bytes, title, comment) -> None or why not
        self.person = person                                          # Slack member ID -> the person's name, for .users
        self.config_path, self.config_mtime = config_path, self._mtime(config_path)
        self.recent = collections.defaultdict(collections.deque)      # user -> times of recent requests
        self.sessions = {}                                            # (channel, user) -> Session
        self.watches = {}                                             # (channel, user) -> Watch
        self.closing = None                                           # (the next nightly closing time, the setting it came from)
        self.marking = threading.local()                              # the mark function of the message this thread is handling
        self.configured = json.loads(json.dumps(cfg["controllers"]))  # as given in the settings, before discovery
        self.scanned = None
        self.rescan()

    # -- access
    @staticmethod
    def _mtime(path):
        try:
            return os.path.getmtime(path) if path else None
        except OSError:
            return None

    def refresh(self):
        """Take over who is allowed (and the limits) when the settings file was edited: no restart, open sessions stay.
        The tokens and the controllers are read only at start."""
        m = self._mtime(self.config_path)
        if m is None or m == self.config_mtime:
            return False
        self.config_mtime = m
        try:
            new = load_config(self.config_path)
        except (OSError, ValueError) as e:
            log_line(f"the settings file could not be read, keeping the old lists: {e}")
            return False
        for key in ("allow_users", "allow_channels", "danger_users", "rate_per_minute", "session_idle_min", "confirm_s",
                    "session_close_at"):
            self.cfg[key] = new[key]
        log_line(f"settings reloaded: {len(self.cfg['allow_users'])} users, {len(self.cfg['allow_channels'])} channels, "
                 f"{len(self.cfg['danger_users'])} on the danger list")
        return True

    def rescan(self, max_age=30.0):
        """Offer every hub this server is configured to run, beside the controllers named in the settings."""
        if not self.cfg.get("discover") or (self.scanned is not None and time.monotonic() - self.scanned < max_age):
            return
        self.scanned = time.monotonic()
        merged = merge_controllers(self.configured, discover_hubs(self.cfg["discover_dir"]))
        if set(merged) != set(self.cfg["controllers"]):
            log_line("controllers offered now: " + ", ".join(merged))
        self.cfg["controllers"] = merged

    def allowed(self, user, channel):
        self.refresh()
        self.rescan()
        return channel in self.cfg["allow_channels"] or user in self.cfg["allow_users"]

    def too_fast(self, user):
        q, t = self.recent[user], self.now()
        while q and t - q[0] > 60:
            q.popleft()
        if len(q) >= int(self.cfg["rate_per_minute"]):
            return True
        q.append(t)
        return False

    # -- hub access
    def who(self, user_name):
        return "slack-" + (re.sub(r"[^A-Za-z0-9_.-]", "", user_name) or "user")[:24]

    def ctl(self, name):
        c = self.cfg["controllers"][name]
        return c["host"], int(c["port"])

    def query(self, name, user_name, command):
        """The reply line to one query, or a sentence saying why there is none."""
        try:
            lines = self.request(*self.ctl(name), self.who(user_name), command, lambda ls: reply_line(ls) is not None)
            return reply_line(lines)
        except (OSError, TimeoutError) as e:
            return f"(no answer: {e})"

    def hub_status(self, name, user_name):
        try:
            lines = self.request(*self.ctl(name), self.who(user_name), "!hub status",
                                 lambda ls: any(l.startswith("# hub name=") for l in ls), 5.0)
        except (OSError, TimeoutError) as e:
            return None, str(e)
        line = next(l for l in lines if l.startswith("# hub name="))
        return dict(w.split("=", 1) for w in line.split()[2:] if "=" in w), None

    # -- the answers
    def summary(self, name, user_name):
        f, err = self.hub_status(name, user_name)
        if f is None:
            return f"*{name}*: the hub does not answer ({err})"
        board = f"{f.get('product', '?')} {f.get('serial', '?')}"
        if f.get("connected") != "1":
            return f"*{name}* ({board}): the hub cannot reach the controller (power or network cable?)"
        if f.get("match") == "0":
            return f"*{name}*: the board that answers ({board}) is not the one the hub expects ({f.get('expect')})"
        state = self.query(name, user_name, "STATE?") or "?"
        others = [w for w in f.get("who", "").split(",") if w and w != "-" and not w.startswith("slack-")]
        text = f"*{name}* ({board}, fw {f.get('fw', '?')}): `{state.replace('OK ', '', 1)}`"
        return text + (f"; connected: {', '.join(others)}" if others else "; nobody else connected")

    def log_rows(self, name, user_name, n, words):
        """(rows, None) or (None, why): the newest n matching rows of one hub's stored log."""
        try:
            lines = self.request(*self.ctl(name), self.who(user_name), " ".join(["!hub log", str(n)] + words),
                                 lambda ls: any(l.startswith(("# log-end", "# hub:")) for l in ls), 10.0)
        except (OSError, TimeoutError) as e:
            return None, f"no answer from the hub ({e})"
        bad = next((l for l in lines if l.startswith("# hub:")), None)
        if bad:
            return None, bad[2:].strip()
        rows = []
        for l in lines:
            if l.startswith("# log {"):
                try:
                    rows.append(json.loads(l[6:]))
                except ValueError:
                    continue
        return rows, None

    def log(self, name, user_name, args):
        """One hub's stored log, or with name None every hub's, merged in time order."""
        n = 10
        words = []
        for w in args:
            if w.isdigit():
                n = max(1, min(30, int(w)))
            elif re.fullmatch(r"(since|kind|client|grep)=[^\s]{1,60}", w):
                words.append(w)
            else:
                return f"I do not understand `{w}`. Use a number, `since=1h`, `kind=cmd`, `client=<name>` or `grep=<text>`."
        words.sort(key=lambda w: w.startswith("grep="))               # grep= takes the rest of the line: it goes last
        names = [name] if name else list(self.cfg["controllers"])
        rows, notes = [], []
        for c in names:
            got, why = self.log_rows(c, user_name, n, words)
            if got is None:
                notes.append(f"*{c}*: {why}")
            else:
                rows += [(r.get("t", 0), c, r) for r in got]
        if name and notes:
            return notes[0]
        rows = sorted(rows, key=lambda x: x[0])[-n:]
        width = max([len(c) for c in names] + [1])
        text = []
        for t, c, r in rows:
            stamp_ = datetime.datetime.fromtimestamp(t).strftime("%m-%d %H:%M:%S")
            where = f"{c:<{width}} " if not name else ""
            text.append(f"{stamp_} {where}{r.get('kind', '?'):<6} {(r.get('client') or '-'):<14} {r.get('line', '')}"[:170])
        head = f"*{name}* hub log" if name else f"Hub logs of {', '.join(names)}, merged"
        return (f"{head}, newest {len(rows)} matching rows:\n```\n" + ("\n".join(text) or "(nothing)") + "\n```"
                + ("\n" + "\n".join(notes) if notes else ""))

    # -- a day of a hub's log as a file
    def log_folder(self, name, user_name):
        """(folder of this hub's day files, None) or (None, why): the hub says where it writes its log."""
        f, err = self.hub_status(name, user_name)
        if f is None:
            return None, f"the hub does not answer ({err})"
        path = f.get("log", "-")
        if not path or path == "-":
            return None, "the hub has not written a log yet"
        return os.path.dirname(path), None

    def log_days(self, names, user_name):
        out = []
        for c in names:
            folder, why = self.log_folder(c, user_name)
            if folder is None:
                out.append(f"*{c}*: {why}")
                continue
            try:
                days = sorted(n for n in os.listdir(folder) if n.endswith(".jsonl"))
            except OSError as e:
                out.append(f"*{c}*: the log folder could not be read ({e})")
                continue
            rows = [f"{n[:-6]}  {os.path.getsize(os.path.join(folder, n)) / 1e6:6.1f} MB" for n in days[-40:]]
            out.append(f"*{c}*: {len(days)} day(s) of log kept" + (f", the newest {len(rows)}:" if len(days) > len(rows) else ":")
                       + "\n```\n" + ("\n".join(rows) or "(none)") + "\n```")
        return "\n".join(out)

    def log_as_file(self, names, user_name, channel, words):
        """One day of the hub log(s) as a file. Text by default: one readable line per row, without the hub's own heartbeat
        queries, several hubs merged in time order. raw: one hub's day file exactly as stored (JSON lines, everything)."""
        raw = "raw" in words
        dates = [w for w in words if w not in ("raw", "file", "download")]
        try:
            day = diary.a_date(dates[0]) if dates else datetime.date.today().isoformat()
        except diary.DiaryError as e:
            return str(e)
        if len(dates) > 1 or day is None:
            return "Which day? `log file [today|yesterday|YYYY-MM-DD] [raw]`"
        if raw and len(names) != 1:
            return "`raw` gives one hub's file as stored: name one controller, for example `log ech file raw`."
        if self.upload is None or not channel:
            return "Files can only be sent in Slack."
        rows, notes, data = [], [], b""
        quiet = {"internal": False, "since_t": None, "before_t": None, "kinds": None, "client": None, "grep": None}
        for c in names:
            folder, why = self.log_folder(c, user_name)
            path = os.path.join(folder, day + ".jsonl") if folder else None
            if folder is None or not os.path.exists(path):
                notes.append(f"*{c}*: " + (why or f"no log for {day}"))
                continue
            if os.path.getsize(path) > 200e6:
                notes.append(f"*{c}*: the log of {day} is {os.path.getsize(path) / 1e6:.0f} MB, too large to send")
                continue
            try:
                if raw:
                    with open(path, "rb") as f:
                        data = f.read()
                    continue
                with open(path, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        try:
                            r = json.loads(line)
                        except ValueError:
                            continue
                        if isinstance(r, dict) and wham_hub.log_row_matches(r, quiet):
                            rows.append((r.get("t", 0), c, r))
            except OSError as e:
                notes.append(f"*{c}*: the log could not be read ({e})")
        if not (data or rows):
            return f"Nothing to send for {day}.\n" + "\n".join(notes)
        label = names[0] if len(names) == 1 else "all"
        if raw:
            name, n, what = f"wham-hublog-{label}-{day}.jsonl", data.count(b"\n"), "as the hub stored it (JSON lines, every row)"
        else:
            rows.sort(key=lambda x: x[0])
            width = max(len(c) for c in names)
            text = [f"# WHAM hub log, {', '.join(names)}, {day}: {len(rows)} rows. Columns: time, "
                    + ("controller, " if len(names) > 1 else "") + "kind, client, line. The hubs' own heartbeat queries are left out (raw has them)."]
            for t, c, r in rows:
                stamp_ = datetime.datetime.fromtimestamp(t).strftime("%H:%M:%S.%f")[:12]
                text.append(f"{stamp_} " + (f"{c:<{width}} " if len(names) > 1 else "") + f"{r.get('kind', '?'):<9} {(r.get('client') or '-'):<16} "
                            + (f"[DANGEROUS {r['danger']}] " if r.get("danger") else "") + str(r.get("line", "")))
            data = ("\n".join(text) + "\n").encode()
            name, n, what = f"wham-hublog-{label}-{day}.txt", len(rows), "one line per row, without the hubs' own heartbeat queries"
        why = self.upload(channel, name, data, f"WHAM hub log {label} {day}",
                          f"Hub log of {', '.join(names)} for {day}: {n:,} rows, {len(data) / 1e6:.1f} MB, {what}."
                          + ("\n" + "\n".join(notes) if notes else ""))
        if why is None:
            return None
        if "missing_scope" in why or "not_allowed_token_type" in why:
            return "I am not allowed to send files: the Slack app needs the `files:write` permission."
        return f"The file could not be sent ({why})."

    def log_files(self, key, words, user_name, session):
        """log ... file / log ... days: which hub(s), then the file or the list of days."""
        rest = [w for w in words if w != "log"]
        if rest and rest[0] == "all":
            names, rest = list(self.cfg["controllers"]), rest[1:]
        elif rest and find_controller(self.cfg, rest[0]) and rest[0] not in ("file", "raw", "days", "dates", "today", "yesterday"):
            names, rest = [find_controller(self.cfg, rest[0])], rest[1:]
        elif session is not None:
            names = [session.name]
        else:
            return "Of which controller? `log <controller> file [date]`, `log all file [date]`, `log <controller> days`."
        if rest and rest[0] in ("days", "dates"):
            return self.log_days(names, user_name)
        return self.log_as_file(names, user_name, key[0], rest)

    def answer(self, text, user="U0", user_name="user"):
        """The reply to one message (the bot's mention already removed)."""
        words = text.strip().split()
        if not words or words[0].lower() in ("help", "?"):
            return HELP.format(names=", ".join(self.cfg["controllers"]))
        if words[0].lower() in ("status", "list", "all"):
            return "\n".join(self.summary(n, user_name) for n in self.cfg["controllers"])
        name = find_controller(self.cfg, words[0])
        if name is None:
            return (f"I do not know a controller called `{words[0][:30]}`. Controllers: "
                    f"{', '.join(self.cfg['controllers'])}. Try `help`.")
        rest = words[1:]
        verb = rest[0].lower() if rest else "state"
        if verb in ("state", "status"):
            head = self.summary(name, user_name)
            fault = self.query(name, user_name, "FAULT?") if "hub does not answer" not in head and "cannot reach" not in head else None
            return head + (f"\nFAULT? `{fault}`" if fault else "")
        if verb in ("who", "clients"):
            try:
                lines = self.request(*self.ctl(name), self.who(user_name), "!hub clients",
                                     lambda ls: bool(ls) and ls[0].startswith("# hub clients:") and
                                     len(ls) > int(ls[0].split(":")[1]), 5.0)
            except (OSError, TimeoutError) as e:
                return f"*{name}*: no answer from the hub ({e})"
            return f"*{name}*:\n```\n" + "\n".join(l[2:] for l in lines if l.startswith("#")) + "\n```"
        if verb in ("id", "idn"):
            return f"*{name}*: `{self.query(name, user_name, '*IDN?')}`"
        if verb == "log":
            return self.log(name, user_name, rest[1:])
        command = " ".join(rest)
        why = refusal(command)
        if why:
            return why
        return f"*{name}* `{command}`\n```\n{self.query(name, user_name, command)}\n```"

    def diary_as_file(self, channel, index=False):
        """diary file: the whole diary, exactly as it is stored, sent into the conversation as a text file.
        diary index file: one line per entry with its time, writer and place, without the texts."""
        path = diary_file(self.cfg)
        try:
            with open(path, "rb") as f:
                data = f.read()
        except FileNotFoundError:
            data = b""
        except OSError as e:
            return f"The diary file could not be read ({e})."
        all_ = diary.entries(path)
        n = len(all_)
        if not n:
            return "Diary: the diary is empty, so there is nothing to send."
        if index:
            data = ("\n".join(diary.index_line(e) for e in all_) + "\n").encode()
        if self.upload is None or not channel:
            return f"Files can only be sent in Slack. The diary is {path} on the server ({n} entries, {len(data)} bytes)."
        stamp_ = datetime.datetime.now().strftime('%Y-%m-%d_%H%M')
        if index:
            name, title = f"wham-diary-index-{stamp_}.txt", "WHAM operators' diary: index"
            comment = f"The diary's index as of now: {n} entries, without their texts. Each line: date time | who | where."
        else:
            name, title = f"wham-diary-{stamp_}.txt", "WHAM operators' diary"
            comment = f"The whole diary as of now: {n} entries, {len(data):,} bytes. Each line: date time | who | where | text."
        why = self.upload(channel, name, data, title, comment)
        if why is None:
            return None                                               # the file itself is the answer
        if "missing_scope" in why or "not_allowed_token_type" in why:
            return ("I am not allowed to send files yet: the Slack app needs the `files:write` permission (app settings, "
                    "*OAuth & Permissions*, Bot Token Scopes, then reinstall the app). `diary read <date>` and `diary search` work meanwhile.")
        return f"The file could not be sent ({why}). `diary read <date>` and `diary search` work meanwhile."

    def diary(self, text, user, where, channel=None):
        """diary ...: the operators' diary on the host (diary.py). Every line shown carries its date, time and writer."""
        words = text.strip().lower().split()
        if words and words[-1] in ("file", "download", "export", "send") and len(words) <= 2:
            if words[:-1] in ([], ["all"]):
                return self.diary_as_file(channel)
            if words[0] in ("index", "list", "entries", "names", "toc"):
                return self.diary_as_file(channel, index=True)
        try:
            req = diary.parse_request(text)
            out = diary.answer(diary_file(self.cfg), req, who=f"{self.person(user)} (Slack)", where=where, limit=40)
        except diary.DiaryError as e:
            return str(e)
        except OSError as e:
            return f"The diary file could not be used ({e})."
        if req["do"] in ("write", "correct"):
            return f"Written to the diary:\n```\n{out['lines'][0]}\n```"
        if not out["lines"]:
            return f"Diary: {out['note']}."
        more = ""
        if "are shown" in out["note"]:
            more = ("\n`diary index file` sends the whole index as a text file." if req["do"] == "index" else
                    "\n`diary file` sends the whole diary as a text file.")
        return f"Diary: {out['note']}.\n```\n" + "\n".join(out["lines"]).replace("```", "` ` `") + "\n```" + more

    def where(self, key):
        """where: what this conversation is connected to, and where each kind of line goes."""
        s, w = self.sessions.get(key), self.watches.get(key)
        out = ["*Where you are*"]
        if s is not None:
            out.append(f"\u2022 Connected to *{s.name}* as `{s.who}`" + (" with hub-log ON (you see everyone's traffic)." if s.hublog else "."))
            out.append(f"\u2022 A plain line goes to the *{s.name} controller* (reply shown as `[{s.name}] command \u2192 reply`).")
            out.append(f"\u2022 A `!hub ...` line goes to the *{s.name} hub* (shown as `[{s.name} hub] ...`): status, clients, log, shotnumber, read shotnumber.")
            out.append("\u2022 A line starting with a dot is for me: `.disconnect`, `.hublog on|off`, `.log`, `.who`, `.users`.")
        else:
            out.append("\u2022 Not connected to a controller: *everything you type is for me* (the bot). `connect <controller>` opens a session.")
            out.append("\u2022 One question without connecting: `<controller> state` or `<controller> <QUERY?>` (read only).")
        out.append("\u2022 Watching all traffic of " + ", ".join(w.sessions) + " (`watch off` stops it)." if w is not None
                   else "\u2022 Not watching (`watch all` shows every controller's traffic).")
        out.append("\u2022 Always mine, with or without a dot: `help`, `where`, `list`, `diary ...`, `repo`, `watch all|off`, `log all`.")
        if s is not None or w is not None:
            out.append("\u2022 Open sessions and watches: " + self.closing_rule() + ".")
        out.append("Reactions on your messages: :gear: controller \u00b7 :file_cabinet: hub \u00b7 :robot_face: me \u00b7 "
                   ":hourglass_flowing_sand: waiting for `.confirm` \u00b7 :no_entry_sign: not sent")
        return "\n".join(out)

    def users(self):
        """Who is authorized: names only (a Slack mention here would notify every one of them)."""
        allowed, danger = self.cfg["allow_users"], self.cfg["danger_users"]
        rows = []
        for u in allowed:
            rows.append(f"• {self.person(u)}" + ("  (may also send dangerous commands, each with `.confirm`)" if u in danger else ""))
        extra = [u for u in danger if u not in allowed]
        out = f"*Authorized to use this bot* ({len(allowed)}):\n" + ("\n".join(rows) or "nobody by name")
        if self.cfg["allow_channels"]:
            out += f"\nAlso anyone writing in {len(self.cfg['allow_channels'])} allowed channel(s): " + ", ".join(f"<#{c}>" for c in self.cfg["allow_channels"])
        if not any(u in allowed for u in danger):
            out += "\nNobody may send dangerous commands (ARM, FIRE, FAULT:CLEar, CHARGE, protections) from Slack."
        if extra:
            out += f"\n({len(extra)} entry on the danger list is not on the allowed list, so it has no effect.)"
        return out + "\nThe lists are kept by the bot's owner in its settings file on the lab server."

    # -- sessions
    def close_all(self, why):
        for s in list(self.sessions.values()):
            s.close(why)
        for w in list(self.watches.values()):
            w.close(why)

    def watch_all(self, key, user_name, post):
        old = self.watches.pop(key, None)
        if old is not None:
            old.close()

        def gone(w):
            if self.watches.get(key) is w:
                self.watches.pop(key, None)
        targets = {n: self.ctl(n) for n in self.cfg["controllers"]}
        w = Watch(targets, self.who(user_name) + "-watch", post, float(self.cfg["session_idle_min"]) * 60.0, on_close=gone)
        if not w.sessions:
            w.close()
            return "No hub could be reached: " + "; ".join(f"{n} ({e})" for n, e in w.failed.items())
        self.watches[key] = w
        return (f"Watching *{', '.join(w.sessions)}*: all their traffic is shown here, each line marked with its controller. "
                "This only watches: nothing you type is sent to them. `watch off` stops it; " + self.closing_rule() + "."
                + ("\nNot reachable: " + "; ".join(f"{n} ({e})" for n, e in w.failed.items()) if w.failed else ""))

    def closing_rule(self):
        """When an open session or watch ends by itself, in words."""
        at, idle = str(self.cfg.get("session_close_at") or ""), float(self.cfg.get("session_idle_min") or 0)
        parts = ([f"after {idle:g} minutes without a message from you"] if idle else []) + ([f"at {at} every night"] if at else [])
        return "it is closed " + " or ".join(parts) if parts else "it stays open until you close it"

    def next_close(self, now):
        """The next nightly closing time after `now` (a datetime), or None when there is none."""
        m = re.fullmatch(r"(\d{1,2}):(\d{2})", str(self.cfg.get("session_close_at") or "").strip())
        if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
            return None
        t = now.replace(hour=int(m.group(1)), minute=int(m.group(2)), second=0, microsecond=0)
        return t if t > now else t + datetime.timedelta(days=1)

    def tick(self, now=None):
        """Called every few seconds: at the nightly closing time, close every session and watch and say so in each
        conversation. Returns how many were closed."""
        now = now or datetime.datetime.now()
        if self.closing is None or str(self.cfg.get("session_close_at")) != self.closing[1]:
            self.closing = (self.next_close(now), str(self.cfg.get("session_close_at")))     # first call, or the time was changed
            return 0
        if self.closing[0] is None or now < self.closing[0]:
            return 0
        self.closing = (self.next_close(now), self.closing[1])
        return self.goodbye("it is the nightly closing time (sessions are closed once a night so the hubs can update)",
                            back="any time")

    def goodbye(self, why="I am being restarted (an update or maintenance)", back="in a few minutes"):
        """The bot is stopping: close every session and watch, and tell each conversation so, once."""
        told = 0
        for s in list(self.sessions.values()):
            s.announce = False
            s.close()
            s.post(f"Goodbye: {why}, so your session with *{s.name}* is closed. What the controller is doing keeps going. "
                   f"`connect {s.name}` again {back}.")
            told += 1
        for w in list(self.watches.values()):
            w.close()
            w.post(f"Goodbye: {why}, so the watch is stopped. `watch all` again {back}.")
            told += 1
        return told

    def connect(self, key, name, user_name, post, user=None):
        old = self.sessions.get(key)
        if old is not None:
            old.close()

        def gone(s, why=None):
            if self.sessions.get(key) is s:
                self.sessions.pop(key, None)
        try:
            s = Session(name, *self.ctl(name), self.who(user_name), post, float(self.cfg["session_idle_min"]) * 60.0, on_close=gone)
        except OSError as e:
            return f"Could not connect to the *{name}* hub ({e})."
        self.sessions[key] = s
        first = str(self.person(user) if user else user_name).split()[0]
        summary = self.summary(name, user_name)
        care = []
        if "`IDLE`" not in summary:
            care.append("The controller is not IDLE: look at its state above before you send anything.")
        if "; connected: " in summary:
            care.append("Someone else is connected to it: agree with them before you change anything.")
        return (f"Hello {first}! Connected to *{name}* as `{s.who}`.\n{summary}\n"
                + ("".join(f":warning: {c}\n" for c in care))
                + "Every line you type now goes to the controller. `.hublog on` shows all traffic, `.disconnect` closes, `help` lists the rest.")

    def bot_command(self, key, text, user, user_name, post):
        """A line for the bot itself (the dot already removed)."""
        words = text.split()
        verb = words[0].lower() if words else "help"
        s = self.sessions.get(key)
        if verb in ("help", "?"):
            return HELP.format(names=", ".join(self.cfg["controllers"]))
        if verb in ("users", "authorized", "allowed"):
            return self.users()
        if verb in ("repo", "repository", "github", "source", "about"):
            return REPO_TEXT
        if verb in ("where", "whereami", "mode"):
            return self.where(key)
        if verb == "diary":
            return self.diary(text.strip()[5:], user, s.name if s else "general", key[0])
        w = self.watches.get(key)
        if w is not None:
            for x in w.sessions.values():
                x.last_input = time.monotonic()                       # any message from the person keeps the watch alive
        if verb in ("list", "status", "controllers"):
            out = "\n".join(self.summary(n, user_name) for n in self.cfg["controllers"])
            out += (f"\nYou are connected to *{s.name}*" + (" (hub-log on)." if s.hublog else ".")) if s else ""
            return out + (f"\nYou are watching {', '.join(w.sessions)}." if w else "")
        if verb in ("watch", "hublog", "hub-log") and len(words) > 1 and words[1].lower() == "all":
            if len(words) > 2 and words[2].lower() in ("off", "0", "no"):
                verb, words = "watch", ["watch", "off"]
            else:
                return self.watch_all(key, user_name, post)
        if verb in ("watch", "hublog", "hub-log") and w is not None and len(words) > 1 and words[1].lower() in ("off", "0", "no"):
            w.close()
            return "Stopped watching." + (f" You are still connected to *{s.name}*." if s else "")
        low = [x.lower() for x in words]
        if "log" in low[:2] and any(x in low for x in ("file", "download", "days", "dates")):
            return self.log_files(key, low, user_name, s)
        if verb == "log" and len(words) > 1 and words[1].lower() == "all":
            return self.log(None, user_name, words[2:])
        if verb == "connect":
            name = find_controller(self.cfg, words[1]) if len(words) > 1 else None
            if name is None:
                return f"Connect to which controller? `connect <{'|'.join(self.cfg['controllers'])}>`"
            return self.connect(key, name, user_name, post, user)
        if s is None:
            if verb in ("hublog", "hub-log", "watch"):
                return "You are not connected. `watch all` shows every controller's traffic; `connect <controller>` then `.hublog on` shows one."
            if verb in ("disconnect", "confirm", "cancel", "log", "who"):
                return "You are not connected. `connect <controller>` first (`list` shows them); `log all` reads every hub's log."
            return self.answer(text, user, user_name)                  # a one-off, read-only question
        if verb in ("disconnect", "bye", "quit", "exit"):
            s.close()
            return f"Disconnected from *{s.name}*. What the controller is doing keeps going."
        if verb in ("hublog", "hub-log", "watch"):
            on = not (len(words) > 1 and words[1].lower() in ("off", "0", "no"))
            s.set_hublog(on)
            return (f"Hub-log ON for *{s.name}*: everything the hub sees is shown here (other clients as `[name] > command` and "
                    "`[name] < reply`). `.hublog off` stops it." if on else
                    "Hub-log off: only replies to your own lines and the controller's events.")
        if verb == "log":
            return self.log(s.name, user_name, words[1:])
        if verb in ("who", "clients"):
            s.send("!hub clients")
            return None
        if verb == "cancel":
            had, s.confirm = s.confirm, None
            return "Cancelled: not sent." if had else "Nothing was waiting."
        if verb == "confirm":
            had, s.confirm = s.confirm, None
            if not had:
                return "Nothing is waiting for confirmation."
            if self.now() - had[1] > float(self.cfg["confirm_s"]):
                return "Too late: not sent. Type the command again."
            if user not in self.cfg["danger_users"]:
                return "Not sent."
            s.send(had[0])
            self.mark("controller")
            return f"Sent `{had[0]}` to *{s.name}*."
        return f"I do not know `.{verb}`. `.help` lists what I understand."

    def mark(self, kind):
        """Note where the message being handled went (controller, hub, bot, confirm, refused): shown as a reaction."""
        m = getattr(self.marking, "fn", None)
        if m is not None:
            m(kind)

    def converse(self, user, channel, text, user_name, post):
        """One message in a conversation that may hold a session. Returns what to post now (or None); a session's own
        traffic arrives later through `post`."""
        key = (channel, user)
        if re.match(r"^\.?diary\b", text.strip(), re.I):              # the diary is the bot's, and an entry may have several lines
            self.mark("bot")
            return self.bot_command(key, text.strip().lstrip(".").strip(), user, user_name, post)
        lines = [l.strip() for l in text.splitlines() if l.strip()]
        if not lines:
            return None
        s = self.sessions.get(key)
        w = self.watches.get(key)
        if w is not None:
            for x in w.sessions.values():
                x.last_input = time.monotonic()
        first = lines[0].lower().split()
        own = len(lines) == 1 and (first[0] in ("help", "?", "repo", "github", "where") or first[:2] in (["watch", "all"], ["watch", "off"], ["log", "all"]))
        if s is None or lines[0].startswith(".") or own:               # the bot's own words are never controller commands
            self.mark("bot")
            return self.bot_command(key, lines[0].lstrip(".").strip(), user, user_name, post)   # help is never a controller command
        s.confirm = None                                               # anything else typed cancels a pending question
        if len(lines) > MAX_LINES:
            self.mark("refused")
            return f"That is {len(lines)} lines; at most {MAX_LINES} are sent from one message. Nothing was sent."
        for n, line in enumerate(lines):
            verdict, why = session_check(line, user in self.cfg["danger_users"])
            if verdict == "send":
                try:
                    s.send(line)
                except OSError as e:
                    self.mark("refused")
                    s.close(f"the connection to the hub failed ({e})")
                    return None
                self.mark("hub" if line.startswith("!") else "controller")
                continue
            rest = f" The {len(lines) - n - 1} line(s) after it were not sent either." if n < len(lines) - 1 else ""
            if verdict == "confirm":
                s.confirm = (line, self.now())
                self.mark("confirm")
                return (f":warning: {why} Controller: *{s.name}*. Reply `.confirm` within {self.cfg['confirm_s']} s to send it; "
                        f"anything else cancels.{rest}")
            self.mark("refused")
            return why + rest
        return None

    def handle(self, user, channel, text, user_name=None, post=None, mark=None):
        """What to post now for one Slack message, or None (nothing to say, or someone who is not allowed). With `post`
        (a function that posts into the conversation) sessions are available; without it, only one-off questions.
        `mark(kind)` is told where the message went: controller, hub, bot, confirm or refused."""
        if not self.allowed(user, channel):
            return None
        self.marking.fn = mark
        if self.too_fast(user):
            self.mark("refused")
            return f"Slow down: at most {self.cfg['rate_per_minute']} messages a minute."
        try:
            if post is not None:
                out = self.converse(user, channel, text, user_name or user, post)
                if out is None:
                    return None
            else:
                out = self.answer(text, user, user_name or user)
        except Exception as e:                               # a bug must not take the bot down, nor be silent
            out = f"Something went wrong in the bot ({type(e).__name__}: {e})."
        return out if len(out) <= MAX_TEXT else out[:MAX_TEXT] + "\n… (cut: too long for Slack)"


# ---- a small WebSocket client (RFC 6455), enough for Slack's Socket Mode ----------------------------------------------
class WebSocket:
    def __init__(self, url, timeout=20.0):
        u = urllib.parse.urlsplit(url)
        secure = u.scheme == "wss"
        port = u.port or (443 if secure else 80)
        raw = socket.create_connection((u.hostname, port), timeout=timeout)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=u.hostname) if secure else raw
        key = base64.b64encode(os.urandom(16)).decode()
        path = (u.path or "/") + ("?" + u.query if u.query else "")
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {u.hostname}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        head = self._until(b"\r\n\r\n").decode(errors="replace")
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        if " 101 " not in head.split("\r\n")[0] or want not in head:
            raise ConnectionError("the server did not accept the WebSocket: " + head.split("\r\n")[0])
        self.lock = threading.Lock()

    def _until(self, mark):
        while mark not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during the handshake")
            self.buf += chunk
        out, self.buf = self.buf.split(mark, 1)
        return out

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("the connection was closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, payload, opcode=1):
        data = payload.encode() if isinstance(payload, str) else payload
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x80 | opcode]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
                                         if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n))
        with self.lock:
            self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self, timeout=None):
        """The next text message; None on a timeout with nothing received. Pings are answered; a close raises."""
        self.sock.settimeout(timeout)
        parts = []
        while True:
            try:
                b0, b1 = self._read(2)
            except socket.timeout:
                if parts:
                    continue
                return None
            self.sock.settimeout(30.0)                         # the rest of a frame that has started must follow
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            op = b0 & 0x0F
            if op == 9:
                self.send(data, 10)
            elif op == 8:
                raise ConnectionError("the server closed the WebSocket")
            elif op in (0, 1, 2):
                parts.append(data)
                if b0 & 0x80:
                    return b"".join(parts).decode(errors="replace")
            self.sock.settimeout(timeout)

    def close(self):
        try:
            self.send(b"", 8)
        except OSError:
            pass
        self.sock.close()


# ---- Slack ----------------------------------------------------------------------------------------------------------
FORM_METHODS = ("users.info", "files.getUploadURLExternal")      # Slack's read methods take their arguments form-encoded, not as a JSON body


def slack_api(method, token, payload=None, timeout=15.0):
    if method in FORM_METHODS:
        data, ctype = urllib.parse.urlencode(payload or {}).encode(), "application/x-www-form-urlencoded"
    else:
        data, ctype = json.dumps(payload or {}).encode(), "application/json; charset=utf-8"
    req = urllib.request.Request("https://slack.com/api/" + method, data=data,
                                 headers={"Authorization": "Bearer " + token, "Content-Type": ctype})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


REACTIONS = {"controller": "gear", "hub": "file_cabinet", "bot": "robot_face", "confirm": "hourglass_flowing_sand",
             "refused": "no_entry_sign"}


def send_bytes(url, data, timeout=60.0):
    """POST raw bytes to the upload address Slack handed out."""
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status


class SlackLink:
    """Socket Mode: receive events, acknowledge each at once, answer it from a worker thread."""

    def __init__(self, bot, api=slack_api, connect=WebSocket, log=print, put=send_bytes):
        self.bot, self.api, self.connect, self.log, self.put = bot, api, connect, log, put
        bot.upload = self.upload
        self.bot_user = None
        self.seen = collections.deque(maxlen=500)             # event ids already handled (Slack retries)
        self.names, self.people = {}, {}
        self.react_off_until = 0.0
        self.stop = threading.Event()
        bot.person = lambda user: self.lookup(user)[1]

    def lookup(self, user):
        """(short name for the hub client name, the person's name for lists); the member ID when Slack will not say."""
        if user not in self.people:
            try:
                r = self.api("users.info", self.bot.cfg["bot_token"], {"user": user})
            except (OSError, ValueError):
                return user, user                              # not remembered: asked again next time
            p = r.get("user", {}) if r.get("ok") else {}
            self.people[user] = (p.get("name") or user, p.get("real_name") or p.get("name") or user)
            self.names[user] = self.people[user][0]
        return self.people[user]

    def user_name(self, user):
        return self.lookup(user)[0]

    def post(self, channel, text, thread=None):
        body = {"channel": channel, "text": text}
        if thread:
            body["thread_ts"] = thread
        try:
            r = self.api("chat.postMessage", self.bot.cfg["bot_token"], body)
            if not r.get("ok"):
                self.log(f"chat.postMessage refused: {r.get('error')}")
        except (OSError, ValueError) as e:
            self.log(f"chat.postMessage failed: {e}")

    def upload(self, channel, name, data, title, comment):
        """Send a file into a conversation (Slack's three steps: ask for an upload address, send the bytes, attach the
        file to the conversation). None when it worked, else Slack's reason."""
        token = self.bot.cfg["bot_token"]
        try:
            r = self.api("files.getUploadURLExternal", token, {"filename": name, "length": len(data)})
            if not r.get("ok"):
                return str(r.get("error"))
            self.put(r["upload_url"], data)
            r = self.api("files.completeUploadExternal", token, {"files": [{"id": r["file_id"], "title": title}],
                                                                 "channel_id": channel, "initial_comment": comment})
            return None if r.get("ok") else str(r.get("error"))
        except (OSError, ValueError, KeyError) as e:
            self.log(f"file upload failed: {e}")
            return str(e)

    def on_event(self, ev):
        if ev.get("bot_id") or ev.get("subtype") or not ev.get("user") or ev.get("user") == self.bot_user:
            return                                             # never answer a bot (itself included) or an edit
        kind, text = ev.get("type"), ev.get("text", "")
        if kind == "message" and ev.get("channel_type") != "im" and (ev.get("channel"), ev["user"]) not in self.bot.sessions:
            return                                             # in a channel: a mention, or a line of an open session
        if kind not in ("app_mention", "message"):
            return
        self.incoming(ev["user"], ev.get("channel", ""), re.sub(r"<@[A-Z0-9]+>", " ", text), ev.get("ts"))

    def react(self, channel, ts, kind, done):
        """Put the reaction for `kind` on the person's own message (once per kind). Never worth failing over: without the
        reactions:write permission it is skipped, and tried again ten minutes later."""
        name = REACTIONS.get(kind)
        if not name or not ts or kind in done or time.monotonic() < self.react_off_until:
            return
        done.add(kind)
        try:
            r = self.api("reactions.add", self.bot.cfg["bot_token"], {"channel": channel, "timestamp": ts, "name": name})
            if not r.get("ok") and r.get("error") in ("missing_scope", "not_allowed_token_type"):
                self.react_off_until = time.monotonic() + 600
                self.log("no reactions: the Slack app lacks the reactions:write permission")
        except (OSError, ValueError):
            pass

    def incoming(self, user, channel, text, ts=None):
        """One message for the bot, from an event or from /wham."""
        text = re.sub(r"<[^|>]+\|([^>]+)>", r"\1", text).replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
        text = re.sub(r"<(https?://[^>]+)>", r"\1", text).replace("`", " ").strip()
        if not self.bot.allowed(user, channel):
            self.log(f"ignored a message from {user} in {channel}: not in allow_channels / allow_users")
            return
        self.log(f"{self.user_name(user)} in {channel}: {text[:80]!r}")
        done = set()
        out = self.bot.handle(user, channel, text, self.user_name(user), post=lambda t: self.post(channel, t),
                              mark=lambda kind: self.react(channel, ts, kind, done))
        if out:
            self.post(channel, out)

    def on_envelope(self, ws, env):
        if env.get("envelope_id"):
            ws.send(json.dumps({"envelope_id": env["envelope_id"]}))      # within 3 s, before any work
        if env.get("type") == "slash_commands":                    # /wham <words>: the same as a message with those words
            p = env.get("payload", {})
            words = (p.get("text") or "").strip()
            name = (p.get("command") or "/wham").lstrip("/").lower()
            if name != "wham":                                  # /list, /connect ech, ... registered as commands of their own
                words = (name + " " + words).strip()
            threading.Thread(target=self.incoming, args=(p.get("user_id", ""), p.get("channel_id", ""), words or "help"),
                             daemon=True).start()
            return "slash_commands"
        if env.get("type") != "events_api":
            return env.get("type")
        payload = env.get("payload", {})
        eid = payload.get("event_id")
        if eid and eid in self.seen:
            return "events_api"
        self.seen.append(eid)
        threading.Thread(target=self.on_event, args=(payload.get("event", {}),), daemon=True).start()
        return "events_api"

    def session(self, should_stop=lambda: False):
        """One Socket Mode connection, until Slack asks for a new one, the link dies, or should_stop() says so."""
        r = self.api("apps.connections.open", self.bot.cfg["app_token"])
        if not r.get("ok"):
            raise ConnectionError(f"apps.connections.open: {r.get('error')}")
        ws = self.connect(r["url"])
        try:
            quiet = 0
            while not self.stop.is_set() and not should_stop():
                self.bot.tick()
                msg = ws.recv(timeout=20.0)
                if msg is None:
                    quiet += 1
                    if quiet >= 6:                              # two minutes of nothing: Slack pings far more often
                        raise ConnectionError("nothing from Slack for 2 minutes")
                    continue
                quiet = 0
                if self.on_envelope(ws, json.loads(msg)) == "disconnect":
                    return
        finally:
            ws.close()

    def run(self, should_stop=lambda: False):
        r = self.api("auth.test", self.bot.cfg["bot_token"])
        if not r.get("ok"):
            raise SystemExit(f"wham_slack: the bot token is not accepted (auth.test: {r.get('error')})")
        self.bot_user = r.get("user_id")
        self.log(f"connected to Slack as {r.get('user')} in {r.get('team')}")
        wait = 2.0
        while not self.stop.is_set() and not should_stop():
            try:
                self.session(should_stop)
                wait = 2.0
            except (OSError, ValueError, ConnectionError) as e:
                self.log(f"Slack link lost ({e}); again in {wait:.0f} s")
                self.stop.wait(wait)
                wait = min(60.0, wait * 2)


# ---- running it -----------------------------------------------------------------------------------------------------
def stop_on_signals():
    """SIGTERM and Ctrl-C end the bot through its normal exit, so open sessions get their goodbye. Returns the old handlers."""
    def stop(signum, frame):
        raise SystemExit(0)
    return {sig: signal.signal(sig, stop) for sig in (signal.SIGTERM, signal.SIGINT)}


def checkout_commit():
    r = subprocess.run(["git", "-C", HOST_DIR, "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def log_line(text):
    print(f"{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')} {text}", flush=True)


def cmd_check(cfg):
    ok = True
    for key, prefix in (("app_token", "xapp-"), ("bot_token", "xoxb-")):
        good = cfg[key].startswith(prefix)
        ok &= good
        print(f"{key}: {'present' if good else 'MISSING or not a ' + prefix + ' token'}")
    print(f"allow_channels: {cfg['allow_channels'] or 'none'}; allow_users: {cfg['allow_users'] or 'none'}"
          + ("" if cfg["allow_channels"] or cfg["allow_users"] else "  -> the bot would answer NOBODY"))
    if cfg["bot_token"].startswith("xoxb-"):
        try:
            r = slack_api("auth.test", cfg["bot_token"])
            print(f"Slack: {'bot ' + str(r.get('user')) + ' in ' + str(r.get('team')) if r.get('ok') else 'refused: ' + str(r.get('error'))}")
            ok &= bool(r.get("ok"))
        except (OSError, ValueError) as e:
            ok = False
            print(f"Slack: not reachable ({e})")
    bot = Bot(cfg)
    print("controllers offered (settings + the hubs configured in " + str(cfg["discover_dir"]) + "): " + ", ".join(
        n + (" [found]" if c.get("discovered") else "") for n, c in bot.cfg["controllers"].items()))
    for name in bot.cfg["controllers"]:
        print(re.sub(r"[*`]", "", bot.summary(name, "check")))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["run", "check", "ask", "install-cron", "uninstall-cron"])
    ap.add_argument("text", nargs="?", default="status", help='for "ask": the message, for example "ech state"')
    ap.add_argument("--config", default=LOCAL_CONFIG, help=f"settings and tokens (default {LOCAL_CONFIG}, git-ignored)")
    args = ap.parse_args(argv)
    cfg = load_config(args.config)
    if args.action == "ask":
        bot = Bot(cfg)                                          # the same path a Slack message takes, minus the allow lists
        out = bot.converse("terminal", "terminal", args.text, os.environ.get("USER", "terminal"), print)
        if out:
            print(out)
        if bot.sessions or bot.watches:
            time.sleep(3.0)                                    # show what arrives, then leave
            bot.close_all(None)
        return 0
    if args.action == "check":
        return cmd_check(cfg)
    if args.action in ("install-cron", "uninstall-cron"):
        os.makedirs(cfg["state_dir"], exist_ok=True)
        line = f"{sys.executable} {os.path.abspath(__file__)} run >> {os.path.join(cfg['state_dir'], 'bot.log')} 2>&1 {CRON_MARK}"
        cur = subprocess.run(["crontab", "-l"], capture_output=True, text=True)
        keep = [l for l in (cur.stdout.splitlines() if cur.returncode == 0 else []) if CRON_MARK not in l]
        new = keep + ([f"@reboot {line}", f"*/5 * * * * {line}"] if args.action == "install-cron" else [])
        subprocess.run(["crontab", "-"], input="\n".join(new) + "\n", text=True, check=True)
        print("\n".join(new))
        return 0
    if not (cfg["app_token"].startswith("xapp-") and cfg["bot_token"].startswith("xoxb-")):
        raise SystemExit("wham_slack: no Slack tokens (SLACK_APP_TOKEN / SLACK_BOT_TOKEN, or slack_bot.local.json). See docs/slack_bot.md.")
    os.makedirs(cfg["state_dir"], exist_ok=True)
    lock = open(os.path.join(cfg["state_dir"], "lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return 0                                               # another copy is running: cron's check, nothing to do
    started, checked = checkout_commit(), [time.monotonic()]

    bot = Bot(cfg, config_path=args.config)

    def moved():                                               # the checkout was updated: stop, cron starts the new code
        if time.monotonic() - checked[0] < 120 or bot.sessions or bot.watches:
            return False                                       # never under someone's open session
        checked[0] = time.monotonic()
        return started is not None and checkout_commit() != started
    if not (cfg["allow_channels"] or cfg["allow_users"]):
        log_line("WARNING: allow_channels and allow_users are both empty: the bot answers nobody")
    stop_on_signals()
    try:
        SlackLink(bot, log=log_line).run(should_stop=moved)
    finally:
        told = bot.goodbye()
        log_line(f"stopping (the checkout moved to another commit, or the bot was stopped); said goodbye to {told} "
                 "open session(s); cron starts it again")
    return 0


if __name__ == "__main__":
    sys.exit(main())

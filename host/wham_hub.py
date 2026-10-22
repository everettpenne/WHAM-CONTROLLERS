#!/usr/bin/env python3
"""wham_hub.py -- the per-controller host service (design: docs/device_identity_and_host_service.md, section 5).

ONE instance talks to ONE controller. It is the only client of that controller's serial-over-TCP bridge (the bridge
accepts exactly one), shares it with any number of local clients, learns WHICH board it is (SYS:ID?), and logs
everything with that identity. Run one instance per controller, as a service (host/deploy/wham-hub@.service).

    python3 host/wham_hub.py --config host/config/magnet2.yaml
    python3 host/wham_hub.py --host 192.168.130.138 --listen-port 5001 --name magnet2

What it does
  * connects to the bridge (host:port), reconnecting forever; on connect and after every !BOOT it asks the board
    *IDN? and SYS:ID? and binds the session to the board's serial (or, for a board with none, UNSET-<its whole MCU ID>);
  * notices a dead link: TCP keepalive on the bridge connection, and when nothing has arrived for heartbeat_s it asks
    *IDN?; no answer within heartbeat_timeout_s closes the connection and reconnects;
  * with `expect_serial` set, a different board on the other end is refused: client commands are NOT forwarded;
  * sends ONE command at a time to the controller, in arrival order, with a timeout (command_timeout_s; long_command_timeout_s
    for the dumps and flash erases in REPLY_TABLE) and gives each reply to the client that sent the command; the service's
    own queries (handshake, heartbeat) go through the same queue. Every command gets exactly one reply line: the
    controller's, or ERR hub ... (timeout, controller lost, restarted, refused);
  * clients speak the same raw protocol as python/net_broker.py (optional first line "@name <who>", then one command per
    line), so net_client.py, wham_console.py and fw_update.py work through it unchanged. An interactive client sees every
    controller line: its own replies bare, other clients' commands as "[name] > cmd" and their replies as "[name] < reply".
    A quiet client (first line "@quiet <who>", or "!hub quiet on") gets only replies to its own commands and the
    controller's unsolicited lines (!EVT, !BOOT, anything unattributable): no echoes, no notices, no greeting;
  * every line, in both directions, is appended to <log_dir>/<serial>/YYYY-MM-DD.jsonl with a host timestamp, the
    serial, the boot epoch (counts !BOOT lines) and the session (counts connections); log_keep_days prunes old days;
  * a client line starting "!hub" is answered by the service itself:  !hub status | !hub clients (who is connected, from
    where, how long, what they last sent) | !hub identify | !hub quiet [on|off] | !hub timestamps [on|off] | !hub confirm |
    !hub cancel;
  * !hub timestamps on makes EVERY line sent to that client start with the time, "[HH:MM:SS.mmm] " in the service's local
    time (the same clock as the log), for a person reading a terminal. It is per connection and OFF by default, because
    tools that parse replies (fw_update.py, wham_console.py, LabVIEW) must see them bare; net_client.py turns it on for
    a person typing at it;
  * several operators may be connected to one controller at once (no lease yet): everyone sees who joins and leaves, and a
    DANGEROUS command (dangerous_commands.py: starts output, arms, weakens a protection, forces/clears a fault, reflashes,
    resets, erases the identity) is announced to all the other clients and logged with its category. With confirm_dangerous
    on, such a command is held until its sender types !hub confirm (default off: scripts and fw_update would stall);
  * a client that stops reading is disconnected once client_buffer_max bytes are waiting for it.

Not here yet (later steps of the plan): write leases, a JSON client API, serial-port upstreams, a shared message bus.
There is NO authentication and the default listen address is all interfaces (as requested for now): anyone who can reach
the port can send ARM/FIRE. allow_from (IPs and CIDRs) refuses other source addresses, and the service warns at startup
when it listens beyond loopback without it; listen_host: 127.0.0.1 or the network restricts further.

Config: a flat YAML-style file (key: value, # comments), see host/config/example.yaml for every key. Command-line options
override it. PyYAML is used if installed, but is not required.
"""
import argparse
import asyncio
import collections
import csv
import datetime
import ipaddress
import json
import os
import re
import signal
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import provision_board as pb  # parse_id, load_registry (same directory)
import dangerous_commands as dc

DEFAULTS = {
    "name": None, "host": None, "port": 5000, "listen_host": "0.0.0.0", "listen_port": None,
    "expect_serial": None, "log_dir": os.path.join(HERE, "logs"), "registry": os.path.join(HERE, "devices.csv"),
    "reconnect_s": 3.0, "confirm_dangerous": False, "confirm_timeout_s": 15.0,
    "command_timeout_s": 5.0, "long_command_timeout_s": 30.0, "queue_max": 256,
    "heartbeat_s": 15.0, "heartbeat_timeout_s": 5.0,
    "keepalive_idle_s": 10, "keepalive_interval_s": 3, "keepalive_count": 3,
    "client_buffer_max": 1048576, "allow_from": None, "log_keep_days": 0,
}
FLOAT_KEYS = ("reconnect_s", "confirm_timeout_s", "command_timeout_s", "long_command_timeout_s", "heartbeat_s",
              "heartbeat_timeout_s")
INT_KEYS = ("port", "queue_max", "keepalive_idle_s", "keepalive_interval_s", "keepalive_count", "client_buffer_max",
            "log_keep_days")
BOOT_RE = re.compile(r"^!BOOT WHAM-")
IDN_REPLY = re.compile(r"OK WHAM-")           # the only reply that starts so: the service's own queries wait for theirs
ID_REPLY = re.compile(r"OK serial=|ERR ")     # SYS:ID?, or ERR from firmware without it
MAX_LOG_LINE = 400          # characters kept per logged line (firmware-update data lines are far longer)

# Replies longer than one quick line (patterns as in command_table.c), on long_command_timeout_s. The queue waits for the
# extra lines: "evlog" n !EVT lines after "OK <n>", "catalog" an ERR line after an OK line cut short by a read error.
PROFILE_SLOTS = 8                # PROFILE_SLOT_COUNT in both products' profile_store.h
REPLY_TABLE = [
    ("FWUPdate:BEGin", None),       # erases a flash bank before it answers
    ("FWUPdate:END", None),         # verifies the whole image
    ("LOG:DATA?", None),            # one line of up to ~40 KB, seconds at 115200 baud
    ("PFMIN:DATA?", None),
    ("PFMIN:RAW?", None),           # HBRIDGE: one line of up to ~4.4 KB
    ("PFM:GAPLOG?", None),
    ("SYS:EVLOG?", "evlog"),
    ("CONFig:PROFile:CATalog?", "catalog"),
]


def reply_spec(text):
    """(is_long, dump kind) for one command line, from REPLY_TABLE."""
    parts = text.split()
    if parts:
        for pattern, kind in REPLY_TABLE:
            if pattern.endswith("?") == parts[0].endswith("?") and dc.path_matches(pattern.rstrip("?"), parts[0].rstrip("?")):
                return True, kind
    return False, None


# ---- configuration ---------------------------------------------------------------------------------------------
def parse_flat_yaml(text):
    out = {}
    for raw in text.splitlines():
        line = raw.split(" #", 1)[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip() or ":" not in line:
            continue
        key, value = line.split(":", 1)
        value = value.strip().strip("'\"")
        out[key.strip()] = value if value != "" and value.lower() not in ("null", "~") else None
    return out


def load_config(path):
    text = open(path).read()
    try:
        import yaml
        data = yaml.safe_load(text) or {}
    except ImportError:
        data = parse_flat_yaml(text)
    return {k: v for k, v in data.items() if k in DEFAULTS}


def parse_allow_from(value):
    """A list of IPs/CIDRs (a list, or one comma/space separated string, brackets allowed) as ip_network objects."""
    if value is None:
        return []
    items = value.replace("[", " ").replace("]", " ").replace(",", " ").split() if isinstance(value, str) else value
    return [ipaddress.ip_network(str(i).strip(), strict=False) for i in items]


def is_loopback(host):
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host == "localhost"


def listen_warning(cfg):
    """The startup warning for a service reachable beyond this host with no allow_from, else None."""
    if is_loopback(cfg["listen_host"]) or parse_allow_from(cfg["allow_from"]):
        return None
    return (f"WARNING: listening on {cfg['listen_host']}:{cfg['listen_port']} with no allow_from and no authentication: "
            f"anyone who can reach this port can send ARM and FIRE. Set allow_from or listen_host: 127.0.0.1.")


def build_config(args):
    cfg = dict(DEFAULTS)
    if args.config:
        cfg.update(load_config(args.config))
    for key in DEFAULTS:
        v = getattr(args, key, None)
        if v is not None:
            cfg[key] = v
    for key in FLOAT_KEYS:
        cfg[key] = float(cfg[key])
    for key in INT_KEYS:
        cfg[key] = int(cfg[key])
    cfg["listen_port"] = int(cfg["listen_port"]) if cfg["listen_port"] is not None else None
    cfg["confirm_dangerous"] = str(cfg["confirm_dangerous"]).strip().lower() in ("1", "true", "yes", "on")
    try:
        parse_allow_from(cfg["allow_from"])
    except ValueError as exc:
        raise SystemExit(f"wham_hub: allow_from is not a list of IP addresses or CIDRs: {exc}")
    if not cfg["host"] or cfg["listen_port"] is None:
        raise SystemExit("wham_hub: the controller's host and a listen_port are required (--config or --host/--listen-port)")
    cfg["name"] = cfg["name"] or cfg["host"]
    return cfg


class ClientInfo:
    """Who is connected: what the service knows about one client."""

    def __init__(self, name, addr, quiet=False, stamp=False):
        self.name, self.addr, self.quiet = name, addr, quiet
        self.stamp = stamp           # every line sent to this client starts with the time (!hub timestamps)
        self.since = time.time()
        self.last_t = None
        self.last_cmd = ""
        self.cmds = 0
        self.danger = 0
        self.held = None             # (text, time, Danger) waiting for !hub confirm
        self.dropped = False         # not reading: being disconnected

    def who(self):
        return f"{self.name}@{self.addr}"


class Cmd:
    """One command on its way to the controller: a client's (owner is its writer) or the service's own (owner None)."""

    def __init__(self, owner, text, danger=None, timeout=None, future=None, expect=None, close_on_timeout=False, purpose=""):
        self.owner, self.text, self.danger, self.timeout, self.future = owner, text, danger, timeout, future
        self.purpose = purpose       # what a service query is for, named in its notes
        self.expect = expect         # a service query's reply pattern: other lines meanwhile belong to nobody
        self.close_on_timeout = close_on_timeout   # no answer means a dead link: close it, never free it for the next command
        self.kind = None             # REPLY_TABLE's dump kind, set when sent
        self.more = 0                # lines still to come after the reply line (see REPLY_TABLE)
        self.follow = ""             # what those lines start with


class LineReader:
    """A client's lines as the firmware ends them: at CR, LF or CR LF (one CR LF is one end, even split across reads)."""
    END = re.compile(rb"\r\n|\r|\n")
    LIMIT = 65536                # bytes without a line end before the client is disconnected

    def __init__(self, reader):
        self.reader, self.buf, self.after_cr = reader, b"", False
        self.cut = ""                # a line the disconnect cut off before its end

    async def line(self):
        """The next line without its end, or None when the client is gone."""
        while True:
            m = self.END.search(self.buf)
            if m:
                self.after_cr = m.group() == b"\r" and m.end() == len(self.buf)     # its LF may come in the next read
                line, self.buf = self.buf[:m.start()], self.buf[m.end():]
                return line.decode(errors="replace")
            if len(self.buf) > self.LIMIT:
                raise ConnectionError("line too long")
            data = await self.reader.read(4096)
            if not data:
                self.cut = self.buf.decode(errors="replace").strip()
                return None                       # a line cut off with no end is dropped, as the firmware would
            if self.after_cr and data.startswith(b"\n"):
                data = data[1:]
            self.after_cr = False
            self.buf += data


def stamped(text):
    """`text` (one or more lines joined by CR LF) with every line starting `[HH:MM:SS.mmm] ` in the service's local time."""
    now = datetime.datetime.now()
    prefix = now.strftime("[%H:%M:%S.") + f"{now.microsecond // 1000:03d}] "
    return "\r\n".join(prefix + line for line in text.split("\r\n"))


def hms(seconds):
    s = int(seconds)
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def set_keepalive(sock, idle, interval, count):
    """TCP keepalive with short probe timing, where the platform has the options (idle 0 turns it off)."""
    if not idle:
        return
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    idle_opt = "TCP_KEEPIDLE" if hasattr(socket, "TCP_KEEPIDLE") else "TCP_KEEPALIVE"      # macOS names it the latter
    for opt, value in ((idle_opt, idle), ("TCP_KEEPINTVL", interval), ("TCP_KEEPCNT", count)):
        if hasattr(socket, opt):
            sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), value)


# ---- the log ---------------------------------------------------------------------------------------------------
class DeviceLog:
    """Append-only JSON lines, one file per device per day."""

    def __init__(self, base, keep_days=0):
        self.base, self.keep_days = base, keep_days
        self.fh = None
        self.path = None
        self.failing = False
        self.closed = False

    def write(self, key, record):
        """A failed write (unwritable directory, full disk) is reported once and dropped: the service keeps running."""
        try:
            day = datetime.date.today().isoformat()
            path = os.path.join(self.base, key, day + ".jsonl")
            if path != self.path:
                if self.fh:
                    self.fh.close()
                self.fh, self.path = None, None
                os.makedirs(os.path.dirname(path), exist_ok=True)
                self.fh = open(path, "a", buffering=1)
                self.path = path
                self.prune()
            self.fh.write(json.dumps(record, separators=(",", ":")) + "\n")
            if self.closed:
                self.fh.close()
                self.fh, self.path = None, None
            self.failing = False
        except OSError as exc:
            if not self.failing:
                print(f"LOG WRITE FAILED under {self.base}: {exc!r}; records are dropped until it works",
                      file=sys.stderr, flush=True)
            self.failing = True
            self.path = None                      # try to open it again next time

    def prune(self):
        """Delete day files older than keep_days (0 keeps everything)."""
        if not self.keep_days:
            return
        cutoff = datetime.date.today() - datetime.timedelta(days=self.keep_days)
        try:
            devs = os.listdir(self.base)
        except OSError:
            return
        for dev in devs:
            try:
                names = os.listdir(os.path.join(self.base, dev))
            except OSError:
                continue                          # not a device folder (a plain file) or gone
            for name in names:
                try:
                    if name.endswith(".jsonl") and datetime.date.fromisoformat(name[:-6]) < cutoff:
                        os.remove(os.path.join(self.base, dev, name))
                except (ValueError, OSError):
                    continue

    def close(self):
        """Later records (clients leaving during shutdown) are still appended, opening the file each time."""
        if self.fh:
            self.fh.close()
        self.fh, self.path, self.closed = None, None, True


# ---- the service -----------------------------------------------------------------------------------------------
class Hub:
    def __init__(self, cfg):
        self.cfg = cfg
        self.allow = parse_allow_from(cfg["allow_from"])
        self.log = DeviceLog(cfg["log_dir"], cfg["log_keep_days"])
        self.identity = {}           # parsed SYS:ID? (plus idn), {} until the handshake succeeds
        self.epoch = 0               # !BOOT lines seen
        self.session = 0             # connections made
        self.up_writer = None
        self.connected = False
        self.clients = {}            # writer -> ClientInfo
        self.busy = False            # a handshake is in progress: client commands wait
        self.held = []               # (writer, text) waiting for the handshake
        self.hub_queue = collections.deque()     # the service's own queries: sent before any client command
        self.queue = collections.deque()         # client commands waiting their turn
        self.inflight = None         # the one command the controller has not answered yet
        self.timer = None
        self.last_rx = None          # monotonic time of the last byte from the controller
        self.handshake_task = None
        self.mismatch = None         # text when expect_serial is set and the board differs
        self.registered = None
        self.stop = asyncio.Event()
        self.fail_reported = False
        self.pending = []            # records written before the first handshake settled
        self.identity_settled = False
        self.ident_failed_at = None  # monotonic time the last handshake failed: retried every heartbeat_s

    # -- identity and logging
    def dev_key(self):
        sn = self.identity.get("serial")
        if sn and sn != "UNSET":
            return sn
        uid = self.identity.get("uid")
        return f"UNSET-{uid}" if uid else f"unidentified-{self.cfg['name']}"

    def serial(self):
        sn = self.identity.get("serial")
        return None if not sn or sn == "UNSET" else sn

    def record(self, kind, line, client=None, danger=None):
        now = time.time()
        rec = {"t": round(now, 3),
               "ts": datetime.datetime.fromtimestamp(now).astimezone().isoformat(timespec="milliseconds"),
               "dev": self.dev_key(), "serial": self.serial(), "name": self.cfg["name"],
               "epoch": self.epoch, "session": self.session, "kind": kind, "line": line}
        limit = 24 if line.upper().startswith("FWUPDATE:DATA") else MAX_LOG_LINE   # image bytes are not worth logging
        if len(line) > limit:
            rec["line"] = line[:limit]
            rec["truncated"] = len(line) - limit
        if client:
            rec["client"] = client
        if danger:
            rec["danger"] = danger
        if not self.identity and not self.identity_settled:
            self.pending.append(rec)       # who this is is not known yet: written once the first handshake settles
        else:
            self.log.write(self.dev_key(), rec)

    def settle(self):
        """The first handshake has finished (or failed): write what was held, under the identity found."""
        self.identity_settled = True
        for rec in self.pending:
            rec["dev"], rec["serial"] = self.dev_key(), self.serial()
            self.log.write(self.dev_key(), rec)
        self.pending = []

    def note(self, text, kind="hub"):
        """A service event: logged, printed, and shown to every interactive client as a --- line."""
        self.record(kind, text)
        print(f"[{self.cfg['name']}] {text}", flush=True)
        self.broadcast(f"--- hub: {text} ---", notice=True)

    # -- writing to clients
    def write_to(self, w, text):
        """Queue one line for a client; a client that has stopped reading is disconnected instead of buffered."""
        info = self.clients.get(w)
        if info is not None and info.dropped:
            return
        if info is not None and info.stamp:
            text = stamped(text)
        data = (text + "\r\n").encode()
        try:
            waiting = w.transport.get_write_buffer_size()
            if info is not None and waiting + len(data) > self.cfg["client_buffer_max"]:
                info.dropped = True
                self.note(f"{info.name} is not reading ({waiting} bytes waiting); disconnecting it")
                w.transport.abort()
                return
            w.write(data)
        except Exception:
            self.clients.pop(w, None)

    def broadcast(self, text, exclude=None, notice=False):
        """To every client but `exclude`; a notice (echo, join, hub note) skips the quiet ones."""
        for w, info in list(self.clients.items()):
            if w is not exclude and not (notice and info.quiet):
                self.write_to(w, text)

    def deny(self, w, text, err=False):
        """The answer to a command that is not sent: an ERR line for a quiet client (or err), else a --- line."""
        info = self.clients.get(w)
        self.write_to(w, f"ERR hub {text}" if err or (info and info.quiet) else f"--- hub: {text} ---")

    def status_line(self):
        i = self.identity
        age = "-" if self.last_rx is None else f"{time.monotonic() - self.last_rx:.1f}"
        return ("# hub name={n} target={h}:{p} connected={c} serial={s} identity={idn} verified={v} uid={u} flashuid={f} "
                "product={pr} fw={fw} epoch={e} session={se} clients={cl} who={who} confirm={cf} registered={r} expect={x} match={m} "
                "rx_age={age} inflight={inf} queued={qd} log={lg}").format(
            n=self.cfg["name"], h=self.cfg["host"], p=self.cfg["port"], c=int(self.connected),
            s=i.get("serial", "?"), idn=i.get("identity", "?"), v=i.get("verified", "?"), u=i.get("uid", "?"),
            f=i.get("flashuid", "?"), pr=i.get("product", "?"), fw=i.get("fw", "?"), e=self.epoch, se=self.session,
            cl=len(self.clients), who=",".join(c.who() for c in self.clients.values()) or "-",
            cf=int(self.cfg["confirm_dangerous"]), r={None: "?", True: 1, False: 0}[self.registered], x=self.cfg["expect_serial"] or "-",
            m=0 if self.mismatch else 1, age=age, inf=self.inflight.text[:40] if self.inflight else "-",
            qd=len(self.queue) + len(self.hub_queue), lg=self.log.path or "-")

    # -- talking to the controller: one command in flight
    async def query(self, cmd, expect, timeout=3.0, close_on_timeout=False, purpose=""):
        """Send one command of the service's own and return its reply line, the first matching expect (hidden from the clients)."""
        if not self.connected:
            raise ConnectionError("not connected")
        fut = asyncio.get_running_loop().create_future()
        self.hub_queue.append(Cmd(None, cmd, timeout=timeout, future=fut, expect=expect, close_on_timeout=close_on_timeout,
                                  purpose=purpose))
        self.pump()
        return await fut

    def submit(self, w, text, confirmed=False):
        """A client's command: refused with an answer, held for confirmation, or queued for the controller."""
        info = self.clients.get(w)
        if info is None or info.dropped:
            return
        if not self.connected:
            self.record("refused", text, info.name)
            self.deny(w, "the controller is not connected")
            return
        if self.mismatch:
            self.record("refused", text, info.name)
            self.deny(w, f"identity mismatch: {self.mismatch}", err=True)
            return
        danger = dc.classify(text)
        if danger and self.cfg["confirm_dangerous"] and not confirmed:
            info.held = (text, time.time(), danger)
            self.record("danger-held", text, info.name, danger.category)
            self.deny(w, f"DANGEROUS [{danger.category}] not sent yet: type !hub confirm within "
                         f"{self.cfg['confirm_timeout_s']:g} s to send it, or !hub cancel")
            self.broadcast(f"--- hub: DANGEROUS [{danger.category}] {info.who()} is about to send: {text[:80]} ---",
                           exclude=w, notice=True)
            return
        if len(self.queue) >= self.cfg["queue_max"]:
            self.record("refused", text, info.name)
            self.deny(w, "queue full: too many commands waiting for the controller", err=True)
            return
        self.queue.append(Cmd(w, text, danger))
        self.pump()

    def pump(self):
        """Send the next waiting command if the controller is free (the service's own first, clients' not mid-handshake)."""
        while self.inflight is None and self.connected:
            if self.hub_queue:
                cmd = self.hub_queue.popleft()
                if cmd.future.done():
                    continue                      # its caller is gone (a handshake replaced by a newer one)
            elif self.queue and not self.busy:
                cmd = self.queue.popleft()
                if cmd.owner not in self.clients or self.clients[cmd.owner].dropped:
                    continue
                if self.mismatch:
                    self.record("refused", cmd.text, self.clients[cmd.owner].name)
                    self.deny(cmd.owner, f"identity mismatch: {self.mismatch}", err=True)
                    continue
            else:
                return
            self.send(cmd)

    def send(self, cmd):
        long, cmd.kind = reply_spec(cmd.text)
        cmd.timeout = (self.cfg["long_command_timeout_s"] if long else cmd.timeout or self.cfg["command_timeout_s"])
        info = self.clients.get(cmd.owner)
        if info is None:
            self.record("hub-cmd", cmd.text)
        else:
            info.cmds += 1
            info.last_cmd, info.last_t = cmd.text, time.time()
            info.danger += 1 if cmd.danger else 0
            self.record("cmd", cmd.text, info.name, cmd.danger.category if cmd.danger else None)
            self.broadcast(f"[{info.name}] > {cmd.text}", exclude=cmd.owner, notice=True)
            if cmd.danger:
                self.broadcast(f"--- hub: DANGEROUS [{cmd.danger.category}] {info.who()}: {cmd.text[:80]} ---",
                               exclude=cmd.owner, notice=True)
        self.inflight = cmd
        self.timer = asyncio.get_running_loop().call_later(cmd.timeout, self.on_timeout, cmd)
        self.up_writer.write(cmd.text.encode() + b"\r\n")

    def finish(self):
        """The controller is free again."""
        if self.timer:
            self.timer.cancel()
        self.timer, self.inflight = None, None
        self.pump()

    def fail(self, cmd, text, exc):
        """Answer a command that got no reply from the controller: its client an ERR line, a service query an exception."""
        if cmd.future is not None:
            if not cmd.future.done():
                cmd.future.set_exception(exc)
        elif cmd.owner in self.clients:
            self.record("hub", f"{self.clients[cmd.owner].name}: {text}")
            self.write_to(cmd.owner, f"ERR hub {text}")

    def on_timeout(self, cmd):
        if self.inflight is not cmd:
            return
        if cmd.close_on_timeout:
            self.fail(cmd, f"timeout: no reply from the controller within {cmd.timeout:g} s to {cmd.text[:40]}",
                      asyncio.TimeoutError())
            self.note(f"{cmd.purpose}: no answer to {cmd.text[:40]} within {cmd.timeout:g} s; closing the connection to "
                      f"reconnect")
            self.connected = False                    # a reply already read must not let pump() send on it
            self.up_writer.transport.abort()          # stays in flight: the rest is answered when the connection ends
            return
        if cmd.more:
            self.note(f"the reply to {cmd.text[:40]} stopped {cmd.more} line(s) short after {cmd.timeout:g} s")
        else:
            self.fail(cmd, f"timeout: no reply from the controller within {cmd.timeout:g} s to {cmd.text[:40]}",
                      asyncio.TimeoutError())
        self.resync(cmd)
        self.finish()

    def resync(self, late):
        """The late reply can still come: the next command waits for a query whose reply cannot be mistaken for it."""
        idn = late.text.upper().split()[:1] == ["*IDN?"]
        text, expect = ("SYS:ID?", ID_REPLY) if idn else ("*IDN?", IDN_REPLY)
        fut = asyncio.get_running_loop().create_future()
        wait = max(self.cfg["heartbeat_timeout_s"], late.timeout)    # the rest of a long reply may still be on its way
        self.hub_queue.appendleft(Cmd(None, text, timeout=wait, future=fut, expect=expect, close_on_timeout=True,
                                      purpose=f"resync after {late.text[:40]}"))
        fut.add_done_callback(lambda f: f.cancelled() or f.exception())     # nobody awaits it: keep asyncio from warning

    def reply(self, cmd, text):
        """The controller's answer to the command in flight: to its sender, marked for the other interactive clients."""
        if cmd.future is not None:
            self.record("hub-reply", text)
            if not cmd.future.done():
                cmd.future.set_result(text)
            return
        info = self.clients.get(cmd.owner)
        self.record("reply", text, info.name if info else None)
        if info is not None:
            self.write_to(cmd.owner, text)
            self.broadcast(f"[{info.name}] < {text}", exclude=cmd.owner, notice=True)

    async def handshake(self):
        try:
            idn = await self.query("*IDN?", IDN_REPLY)
            ident = {"idn": idn}
            reply = await self.query("SYS:ID?", ID_REPLY)
            if reply.startswith("OK "):
                ident.update(pb.parse_id(reply))
            else:
                ident.update({"identity": "legacy", "serial": "UNSET"})   # firmware without SYS:ID?
            old = self.dev_key()
            self.identity = ident
            self.ident_failed_at = None
            self.settle()
            self.check_identity()
            self.note(f"identity {self.dev_key()}: " + " ".join(
                f"{k}={ident.get(k)}" for k in ("serial", "identity", "verified", "product", "fw")) + f" (was {old})")
        except (asyncio.TimeoutError, ConnectionError, OSError) as exc:
            self.identity = {}
            self.ident_failed_at = time.monotonic()
            self.settle()
            want = self.cfg["expect_serial"]          # an unknown board is not the expected one
            self.mismatch = f"expected serial {want}, the controller did not identify itself" if want else None
            self.note(f"handshake failed: {exc!r}; the controller did not answer")
        finally:
            if self.handshake_task is asyncio.current_task():     # not when a newer handshake has replaced this one
                self.busy = False
                held, self.held = self.held, []
                for w, text in held:
                    self.submit(w, text)
                self.pump()

    def check_identity(self):
        sn = self.serial()
        self.mismatch = None
        want = self.cfg["expect_serial"]
        if want and sn != want:
            self.mismatch = f"expected serial {want}, the controller says {sn or 'UNSET'}"
        try:
            rows = pb.load_registry(self.cfg["registry"])
        except (OSError, ValueError, csv.Error):
            rows = []
        rows = [r for r in rows if r.get("serial") and r.get("uid")]     # a malformed row is no evidence
        reg = [r for r in rows if sn and r["serial"] == sn]
        self.registered = (len(reg) > 0) if rows else None
        if reg and self.identity.get("uid") and reg[0]["uid"] != self.identity["uid"]:
            self.mismatch = (self.mismatch or "") + f" registry holds {sn} for different hardware ({reg[0]['uid']})"
        if self.mismatch:
            print(f"[{self.cfg['name']}] IDENTITY MISMATCH: {self.mismatch}", flush=True)

    def info_of(self, name):
        for c in self.clients.values():
            if c.name == name:
                return c
        return None

    def on_line(self, text):
        """One line from the controller."""
        if "!BOOT" in text:
            text = text[text.index("!BOOT"):]      # a reset can leave a stray byte in front of the banner
        if BOOT_RE.match(text):
            self.epoch += 1
            self.record("boot", text)
            self.broadcast(text)
            self.start_handshake(delay=0.6)       # let the rest of the banner finish
            waiting = [(q.owner, q.text) for q in self.queue] + self.held   # decided before the restart: not for the new board
            self.queue.clear()
            self.held = []
            for w, text in waiting:
                if w in self.clients:
                    self.record("refused", text, self.clients[w].name)
                    self.deny(w, "the controller restarted before it was sent: " + text[:40])
            cmd = self.inflight
            if cmd is not None and cmd.more:
                self.note(f"the reply to {cmd.text[:40]} was cut short by the restart, {cmd.more} line(s) missing")
                self.finish()
            elif cmd is not None:
                self.fail(cmd, "the controller restarted before it replied to " + cmd.text[:40], ConnectionError("controller restarted"))
                self.finish()
            return
        cmd = self.inflight
        if cmd is not None and cmd.more:
            if text.startswith("!") and cmd.follow != "!EVT":
                self.unsolicited(text)            # an event in the middle of a reply that is not a dump of events
                return
            if text.startswith(cmd.follow):
                cmd.more -= 1
                if cmd.follow == "!EVT":
                    self.unsolicited(text)
                else:
                    self.reply(cmd, text)
                if not cmd.more:
                    self.finish()
                return
            self.finish()                         # the reply ended early: this line belongs to nobody
        elif cmd is not None and text.strip() and not text.startswith("!"):
            if cmd.expect and not cmd.expect.match(text):
                self.unsolicited(text)            # a stray or late line, not the service query's reply
                return
            self.reply(cmd, text)                 # the next line that is not unsolicited, even a garbled one, is the reply
            parts, kind = text.split(), cmd.kind
            if kind == "evlog" and parts[0] == "OK" and len(parts) > 1 and parts[1].isdigit():
                cmd.more, cmd.follow = int(parts[1]), "!EVT"
            elif kind == "catalog" and parts[0] == "OK" and sum(1 for p in parts if re.match(r"\d+=", p)) < PROFILE_SLOTS:
                cmd.more, cmd.follow = 1, "ERR"
            if cmd.more:
                self.timer.cancel()
                self.timer = asyncio.get_running_loop().call_later(cmd.timeout, self.on_timeout, cmd)
            else:
                self.finish()
            return
        self.unsolicited(text)

    def unsolicited(self, text):
        """A controller line that answers no command: logged; a ! line to every client, any other to the interactive ones only."""
        kind = ("event" if text.startswith("!EVT") else "boot" if text.startswith("!BOOT") else
                "reply" if text.startswith(("OK", "ERR")) else "data")
        self.record(kind, text)
        self.broadcast(text, notice=not text.startswith("!"))     # a quiet client must never take it for its reply

    def start_handshake(self, delay=0.0):
        """Ask the board who it is; client commands wait until that is done."""
        if self.handshake_task and not self.handshake_task.done():
            self.handshake_task.cancel()
        self.busy = True

        async def run():
            if delay:
                await asyncio.sleep(delay)
            await self.handshake()
        self.handshake_task = asyncio.ensure_future(run())

    # -- connections
    async def heartbeat(self, writer):
        """While this connection lives: ask *IDN? whenever heartbeat_s passes with nothing received; no answer ends it.
        Also retries a failed handshake every heartbeat_s."""
        idle = self.cfg["heartbeat_s"]
        while self.up_writer is writer:
            now = time.monotonic()
            retry = None if self.ident_failed_at is None else self.ident_failed_at + idle - now
            if retry is not None and retry <= 0 and not self.busy:
                self.ident_failed_at = None
                self.start_handshake()            # still unidentified: ask again
                continue
            wait = self.last_rx + idle - now
            if wait > 0 or self.busy:
                await asyncio.sleep(max(wait if retry is None else min(wait, retry), 0.1))
                continue
            try:
                await self.query("*IDN?", IDN_REPLY, self.cfg["heartbeat_timeout_s"], close_on_timeout=True,
                                 purpose="heartbeat")
            except asyncio.TimeoutError:
                return                            # on_timeout has closed the connection
            except (ConnectionError, OSError):
                if not self.connected:
                    return                        # a reset during the query leaves the link up: keep beating

    async def upstream_loop(self):
        while not self.stop.is_set():
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self.cfg["host"], self.cfg["port"]), 5)
            except (OSError, asyncio.TimeoutError) as exc:
                if not self.identity_settled:
                    self.settle()                 # nobody to identify: write what was held, under the service name
                if not self.fail_reported:      # once per outage, not every retry
                    self.fail_reported = True
                    self.note(f"cannot connect to {self.cfg['host']}:{self.cfg['port']}: {exc!r}; retrying every {self.cfg['reconnect_s']:g} s")
                self.connected = False
                await self.sleep(self.cfg["reconnect_s"])
                continue
            self.keepalive(writer)
            self.up_writer, self.connected = writer, True
            self.last_rx = time.monotonic()
            self.fail_reported = False
            self.session += 1
            self.note(f"connected to the controller at {self.cfg['host']}:{self.cfg['port']} (session {self.session})")
            self.start_handshake()
            beat = asyncio.ensure_future(self.heartbeat(writer)) if self.cfg["heartbeat_s"] > 0 else None
            buf = b""
            try:
                while True:
                    data = await reader.read(4096)
                    if not data:
                        break
                    self.last_rx = time.monotonic()
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self.on_line(line.decode(errors="replace").strip("\r\n"))
            except (ConnectionError, OSError):
                pass
            self.connected = False
            if beat:
                beat.cancel()
            self.lose_controller()
            writer.close()
            self.note("connection to the controller closed; reconnecting")
            await self.sleep(self.cfg["reconnect_s"])

    def keepalive(self, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                set_keepalive(sock, self.cfg["keepalive_idle_s"], self.cfg["keepalive_interval_s"], self.cfg["keepalive_count"])
            except OSError:
                pass

    def lose_controller(self):
        """The bridge connection ended: every command not yet answered gets its answer."""
        if self.handshake_task and not self.handshake_task.done():
            self.handshake_task.cancel()
        self.busy = False
        held, self.held = self.held, []
        for w, text in held:
            self.submit(w, text)                  # refused with an answer: the controller is not connected
        if self.inflight is not None:
            cmd = self.inflight
            self.fail(cmd, "the connection to the controller was lost before it replied to " + cmd.text[:40],
                      ConnectionError("connection lost"))
            self.finish()
        for cmd in list(self.hub_queue):
            self.fail(cmd, "", ConnectionError("connection lost"))
        self.hub_queue.clear()
        while self.queue:
            cmd = self.queue.popleft()
            if cmd.owner in self.clients:
                self.record("refused", cmd.text, self.clients[cmd.owner].name)
                self.deny(cmd.owner, "the controller is not connected")

    async def sleep(self, seconds):
        try:
            await asyncio.wait_for(self.stop.wait(), seconds)
        except asyncio.TimeoutError:
            pass

    def allowed(self, addr):
        if not self.allow:
            return True
        try:
            ip = ipaddress.ip_address(addr.split("%")[0])
        except ValueError:
            return False
        ip = getattr(ip, "ipv4_mapped", None) or ip
        return any(ip in net for net in self.allow)

    async def handle_client(self, reader, writer):
        name = "client"
        peer = writer.get_extra_info("peername")
        addr = peer[0] if peer else "?"
        if not self.allowed(addr):
            self.record("hub", f"refused a connection from {addr} (not in allow_from)")
            print(f"[{self.cfg['name']}] refused a connection from {addr} (not in allow_from)", flush=True)
            writer.close()
            return
        try:
            self.keepalive(writer)
            lines = LineReader(reader)
            first = await lines.line()
            word = (first or "").split(" ", 1)[0]
            quiet = word == "@quiet"
            if word in ("@quiet", "@name"):          # the name is optional
                name = first[len(word):].strip() or name
                first = None
            while self.info_of(name):                  # names identify clients in the echo: keep them unique
                name += "'"
            self.clients[writer] = ClientInfo(name, addr, quiet)
            self.record("hub", f"{name} joined from {addr}" + (" (quiet)" if quiet else ""))
            self.broadcast(f"--- {name} ({addr}) joined ---", exclude=writer, notice=True)
            if not quiet:
                self.write_to(writer, f"--- connected as {name}; {len(self.clients)} client(s); controller {self.dev_key()} ---")
            text = first
            while True:
                if text is None:
                    text = await lines.line()
                    if text is None:
                        if lines.cut and not lines.cut.startswith(("!hub", "@")):     # only controller commands
                            self.record("refused", lines.cut, name)
                        await self.answered(writer)      # done sending (printf | nc) but maybe still reading
                        break
                if text.startswith("!hub"):
                    self.hub_command(name, writer, text)
                elif not text.strip():
                    if self.connected:                 # the firmware ignores a blank line and never answers it
                        self.up_writer.write(b"\r\n")
                elif self.busy:
                    self.held.append((writer, text))
                else:
                    self.submit(writer, text)
                text = None
        except (ConnectionError, OSError):
            pass
        finally:
            gone = self.clients.pop(writer, None)
            self.queue = collections.deque(c for c in self.queue if c.owner is not writer)
            self.held = [(w, t) for w, t in self.held if w is not writer]
            if gone:
                self.record("hub", f"{name} left ({gone.cmds} commands, {gone.danger} dangerous)")
                self.broadcast(f"--- {name} left ---", notice=True)
            writer.close()

    async def answered(self, writer):
        """Until every command this client sent has its answer; each is bounded by its timeout."""
        while self.clients.get(writer) is not None and not writer.is_closing() and (
                (self.inflight is not None and self.inflight.owner is writer)
                or any(c.owner is writer for c in self.queue) or any(w is writer for w, _ in self.held)):
            await asyncio.sleep(0.05)

    def hub_command(self, name, writer, text):
        cmd = text[4:].strip().lower()
        self.record("hub-cmd", text, name)
        info = self.info_of(name)
        if cmd == "status":
            self.write_to(writer, self.status_line())
        elif cmd in ("clients", "who"):
            now = time.time()
            lines = [f"# hub clients: {len(self.clients)}"]
            for c in self.clients.values():
                lines.append(f"#   {c.name:<14} {c.addr:<15} connected {hms(now - c.since)}  "
                             f"idle {hms(now - (c.last_t or c.since))}  cmds {c.cmds}  dangerous {c.danger}"
                             + ("  quiet" if c.quiet else "") + (f"  last: {c.last_cmd[:60]}" if c.last_cmd else ""))
            self.write_to(writer, "\r\n".join(lines))
        elif cmd in ("confirm", "cancel"):
            held, info.held = (info.held, None) if info else (None, None)
            if held is None:
                self.write_to(writer, "# hub: nothing is waiting for confirmation")
            elif cmd == "cancel":
                self.record("danger-cancelled", held[0], name, held[2].category)
                self.write_to(writer, "# hub: cancelled, not sent")
            elif time.time() - held[1] > self.cfg["confirm_timeout_s"]:
                self.record("danger-expired", held[0], name, held[2].category)
                self.write_to(writer, "# hub: too late, not sent; send the command again")
            else:
                self.submit(writer, held[0], confirmed=True)
        elif cmd == "identify":
            if self.connected:
                self.start_handshake()
                self.write_to(writer, "# hub: identifying the controller")
            else:
                self.write_to(writer, "# hub: the controller is not connected")
        elif cmd.split()[:1] == ["quiet"] and cmd.split()[1:] in ([], ["on"], ["off"]) and info:
            if cmd != "quiet":
                info.quiet = cmd.endswith("on")
            self.write_to(writer, f"# hub: quiet {'on' if info.quiet else 'off'}")
        elif cmd.split()[:1] == ["timestamps"] and cmd.split()[1:] in ([], ["on"], ["off"]) and info:
            if cmd != "timestamps":
                info.stamp = cmd.endswith("on")
            self.write_to(writer, f"# hub: timestamps {'on' if info.stamp else 'off'}")
        else:
            self.write_to(writer, "# hub: commands are  !hub status  |  !hub clients  |  !hub identify  |  !hub quiet [on|off]  |  "
                                  "!hub timestamps [on|off]  |  !hub confirm  |  !hub cancel")

    async def run(self):
        server = await asyncio.start_server(self.handle_client, self.cfg["listen_host"], self.cfg["listen_port"])
        allow = ",".join(str(n) for n in self.allow) or "any (no authentication)"
        print(f"[{self.cfg['name']}] controller {self.cfg['host']}:{self.cfg['port']}; clients on "
              f"{self.cfg['listen_host']}:{self.cfg['listen_port']}, allowed from {allow}; log {self.cfg['log_dir']}", flush=True)
        warning = listen_warning(self.cfg)
        if warning:
            print(f"[{self.cfg['name']}] {warning}", flush=True)
        self.log.prune()
        up = asyncio.ensure_future(self.upstream_loop())
        await self.stop.wait()
        server.close()
        up.cancel()
        try:
            await up
        except asyncio.CancelledError:
            pass
        for w in list(self.clients) + ([self.up_writer] if self.up_writer else []):
            w.close()
        self.log.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", help="flat YAML config file")
    ap.add_argument("--name"); ap.add_argument("--host", help="the controller's bridge address")
    ap.add_argument("--port", type=int, help="the bridge's TCP port (default 5000)")
    ap.add_argument("--listen-host", dest="listen_host"); ap.add_argument("--listen-port", dest="listen_port", type=int)
    ap.add_argument("--allow-from", dest="allow_from", help="comma separated IPs/CIDRs that may connect (default: any)")
    ap.add_argument("--expect-serial", dest="expect_serial"); ap.add_argument("--log-dir", dest="log_dir")
    ap.add_argument("--registry"); ap.add_argument("--reconnect-s", dest="reconnect_s", type=float)
    ap.add_argument("--heartbeat-s", dest="heartbeat_s", type=float, help="idle seconds before an *IDN? heartbeat (0 = off)")
    ap.add_argument("--confirm-dangerous", dest="confirm_dangerous", action="store_const", const=True, default=None,
                    help="hold dangerous commands until the sender types !hub confirm")
    cfg = build_config(ap.parse_args(argv))

    async def runner():
        hub = Hub(cfg)
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, hub.stop.set)
            except NotImplementedError:
                pass
        await hub.run()
    asyncio.run(runner())


if __name__ == "__main__":
    main()

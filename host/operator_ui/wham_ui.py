#!/usr/bin/env python3
"""wham_ui.py -- the WHAM operator console: a local web page for talking to any controller built on the WHAM controller board
(XREX magnet-supply controller, HBRIDGE Limiter/HVPS controller, ECH gyrotron controller), without a terminal.

    python3 host/operator_ui/wham_ui.py            # then use the page that opens in the browser (or double-click a launcher)
    python3 host/operator_ui/wham_ui.py --demo     # also offer a simulated controller for practice

HOW IT CONNECTS (the operator chooses on the page):
  * through the HOST (hub): a TCP connection to the controller's hub (host/wham_hub.py). The hub shares the controller with other
    people and programs, logs everything and says who is connected. This is the normal way.
  * DIRECTLY over the network: a TCP connection to the controller's serial-over-TCP bridge, for when the hub is not available.
    A bridge accepts ONE client: nobody else (including the hub) can be connected to it at the same time.
  * over a USB-SERIAL cable on this computer (needs pyserial).

WHAT THIS PROGRAM IS: a small web server that listens on THIS computer only (127.0.0.1) and holds the connections to the
controllers; the page in the browser is its screen. It needs nothing but Python 3 (pyserial only for a serial cable). Every
request from the page carries a random token printed nowhere but embedded in the page it served, and the Host header is checked,
so another web page in the same browser cannot send commands through it.

SAFETY (enforced here, not only on the page): a command that the project's dangerous-command list (host/dangerous_commands.py)
covers -- anything that arms, starts output, weakens a protection, clears a fault, reflashes or resets -- is refused by the server
unless the request says the operator confirmed it. A "query" request only runs commands that end in "?". A device that does not
identify itself as a WHAM controller gets no panels: only the raw console, with a warning.

Not here: a write lease between operators (the hub has none either: it announces who is connected and what they send), a firmware
update screen (use python/fw_update.py), authentication.
"""
import argparse
import collections
import getpass
import json
import os
import re
import secrets
import socket
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dangerous_commands as dc          # noqa: E402
import wham_hub as wh                    # noqa: E402  (only reply_spec(): which commands take long to answer)
sys.path.insert(0, HERE)
import recorder as rc                # noqa: E402
import shot_data as sd               # noqa: E402

STATIC = os.path.join(HERE, "static")
PRESETS_FILE = os.path.join(HERE, "controllers.json")
SHOTS_DIR = os.path.join(HERE, "shots")
DEFAULT_PRESETS = [
    {"name": "HBRIDGE bench board (via hub, ssh tunnel on 5102)", "mode": "hub", "host": "127.0.0.1", "port": 5102},
    {"name": "XREX controller (via hub, ssh tunnel on 5101)", "mode": "hub", "host": "127.0.0.1", "port": 5101},
]
REPLY_RE = re.compile(r"^(OK($| )|ERR( |$))")
# What the hub says instead of a reply to an interactive client when it does not forward a command (to a quiet client it is "ERR hub ...").
HUB_DENIAL_RE = re.compile(r"^--- hub: (the controller is not connected|DANGEROUS \[[^\]]*\] not sent yet.*|the controller restarted before it was sent.*) ---$")
SPLIT_RE = re.compile(rb"[\r\n]+")
HEARTBEAT_S = 2.0                   # how often the page's connections are asked STATE? (and !hub status on a hub)


# ---- one stream of lines ---------------------------------------------------------------------------------------------
class EventLog:
    """What the page shows in its console: every line with a sequence number, readable by long poll."""

    def __init__(self, cap=6000):
        self.cv = threading.Condition()
        self.items = collections.deque(maxlen=cap)
        self.seq = 0

    def add(self, direction, text, poll=False):
        with self.cv:
            self.seq += 1
            self.items.append({"seq": self.seq, "t": time.time(), "dir": direction, "text": text, "poll": poll})
            self.cv.notify_all()

    def since(self, seq, wait=0.0):
        with self.cv:
            if wait > 0 and self.seq <= seq:
                self.cv.wait(wait)
            return [i for i in self.items if i["seq"] > seq], self.seq


# ---- the three ways to reach a controller ----------------------------------------------------------------------------
class Link:
    """A byte pipe with a reader thread that hands every received line (CR or LF ended) to on_line()."""

    def __init__(self, on_line, on_close):
        self.on_line, self.on_close = on_line, on_close
        self.closed = False
        self.thread = None

    def start_reader(self):
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()

    def _read_loop(self):
        buf = b""
        while not self.closed:
            try:
                data = self.read()
            except Exception:
                break
            if data is None:
                break
            if not data:
                continue
            buf += data
            pieces = SPLIT_RE.split(buf)
            buf = pieces.pop()
            for piece in pieces:
                if piece:
                    self.on_line(piece.decode(errors="replace"))
        if not self.closed:
            self.closed = True
            self.on_close()


class TcpLink(Link):
    def __init__(self, host, port, on_line, on_close):
        super().__init__(on_line, on_close)
        self.sock = socket.create_connection((host, int(port)), timeout=5)
        self.sock.settimeout(0.5)
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        except OSError:
            pass

    def read(self):
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return b""
        return data if data else None

    def write(self, data):
        self.sock.sendall(data)

    def close(self):
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass


class SerialLink(Link):
    def __init__(self, device, on_line, on_close):
        super().__init__(on_line, on_close)
        import serial                          # pyserial: imported only when a cable is actually used
        self.ser = serial.Serial(device, 115200, timeout=0.3)

    def read(self):
        return self.ser.read(512)

    def write(self, data):
        self.ser.write(data)

    def close(self):
        self.closed = True
        try:
            self.ser.close()
        except Exception:
            pass


# ---- one controller ----------------------------------------------------------------------------------------------------
class CommandTimeout(Exception):
    pass


class NeedsConfirm(Exception):
    def __init__(self, danger):
        super().__init__(danger.category)
        self.danger = danger


def parse_idn(reply):
    """'OK WHAM-HBRIDGE-PFMG474 REVA v0.11 ab12cd34 SN=WB-0001-8 UID=...' -> dict, or None."""
    parts = reply.split()
    if len(parts) < 4 or parts[0] != "OK":
        return None
    info = {"board": parts[1], "hw": parts[2], "fw": parts[3], "commit": parts[4] if len(parts) > 4 and "=" not in parts[4] else "?"}
    for tok in parts[4:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            info[k.lower()] = v
    return info


def product_of(board):
    b = (board or "").upper()
    if not b.startswith("WHAM-"):
        return "unknown"
    return "hbridge" if "HBRIDGE" in b else "xrex" if "XREX" in b else "unknown"


def parse_kv(text):
    return dict(tok.split("=", 1) for tok in text.split() if "=" in tok)


class Connection:
    def __init__(self, cid, spec, user, shots_dir=SHOTS_DIR):
        self.id, self.spec, self.user = cid, spec, user
        self.mode = spec["mode"]                               # hub | direct | serial
        self.events = EventLog()
        self.lock = threading.RLock()                          # one command in flight
        self.replies = collections.deque()
        self.reply_cv = threading.Condition()
        self.hub_lines = []
        self.collect_hub = False
        self.link = None
        self.closed = False
        self.link_open = False
        self.ident = None                                      # parsed *IDN? (+ SYS:ID?), None until the board has answered
        self.state_text = None
        self.last_ok = None
        self.fails = 0
        self.hub = None                                        # parsed !hub status (hub mode)
        self.error = None
        self.poll_s = HEARTBEAT_S
        self.polling = True
        self.thread = None
        self.recorder = rc.Recorder(self, shots_dir)

    # -- naming
    def target(self):
        s = self.spec
        return s.get("device") if self.mode == "serial" else f"{s.get('host')}:{s.get('port')}"

    # -- start / stop
    def start(self):
        self._open()
        self.thread = threading.Thread(target=self._heartbeat, daemon=True)
        self.thread.start()

    def _open(self):
        s = self.spec
        if self.mode == "serial":
            self.link = SerialLink(s["device"], self._on_line, self._on_close)
        else:
            self.link = TcpLink(s["host"], s["port"], self._on_line, self._on_close)
        self.link_open = True
        self.error = None
        self.link.start_reader()
        if self.mode == "hub":
            self._raw(f"@name {self.user}")
        self.events.add("note", f"Connected to {self.target()} ({'through the host' if self.mode == 'hub' else 'directly'}).")
        if self.mode == "serial":
            time.sleep(0.4)                                    # a serial port may have just been opened by this call

    def close(self):
        self.closed = True
        self.link_open = False
        if self.link:
            self.link.close()
        self.events.add("note", "Disconnected.")

    def _raw(self, text):
        self.link.write(text.encode() + b"\r\n")

    # -- what arrives
    def _on_close(self):
        self.link_open = False
        self.events.add("note", "The connection was closed by the other end.")

    def _on_line(self, line):
        text = line.strip()
        if not text:
            return
        if text.startswith("# "):
            with self.reply_cv:
                if self.collect_hub:
                    self.hub_lines.append(text)
                    self.reply_cv.notify_all()
                    return
            self.events.add("hub", text[2:])
        elif REPLY_RE.match(text):
            with self.reply_cv:
                self.replies.append(text)
                self.reply_cv.notify_all()
            self.events.add("rx", text, poll=self._poll_in_flight)
        elif HUB_DENIAL_RE.match(text):
            denial = "ERR hub " + HUB_DENIAL_RE.match(text).group(1)
            with self.reply_cv:
                self.replies.append(denial)
                self.reply_cv.notify_all()
            self.events.add("rx", denial)
        elif re.match(r"^\[[^\]]+\] > ", text):
            self.events.add("other_tx", text)
        elif re.match(r"^\[[^\]]+\] < ", text):
            self.events.add("other_rx", text)
        elif text.startswith("---"):
            self.events.add("note", text.strip("- ").strip())
        else:
            self.events.add("async", text)

    _poll_in_flight = False

    # -- one command and its reply (the caller holds self.lock)
    def _exchange(self, text, timeout, poll=False):
        with self.reply_cv:
            self.replies.clear()
        self._poll_in_flight = poll
        self.events.add("tx", text, poll=poll)
        try:
            self._raw(text)
        except OSError as e:
            self.link_open = False
            raise CommandTimeout(f"not sent: {e}")
        deadline = time.time() + timeout
        with self.reply_cv:
            while not self.replies:
                left = deadline - time.time()
                if left <= 0:
                    self._poll_in_flight = False
                    raise CommandTimeout(f"no reply to {text} within {timeout:g} s")
                self.reply_cv.wait(left)
            reply = self.replies.popleft()
        self._poll_in_flight = False
        return reply

    def _timeout_for(self, text):
        long, _ = wh.reply_spec(text)
        if long:
            return 45.0
        return 9.0 if self.mode == "hub" else 5.0

    def hub_command(self, text):
        """A '!hub ...' line: answered by the hub itself with '#' lines (collected until they stop arriving)."""
        with self.lock:
            with self.reply_cv:
                self.hub_lines, self.collect_hub = [], True
            try:
                self.events.add("tx", text, poll=text == "!hub status")
                self._raw(text)
                end, last = time.time() + 3.0, None
                with self.reply_cv:
                    while time.time() < end:
                        n = len(self.hub_lines)
                        self.reply_cv.wait(0.2)
                        if self.hub_lines and len(self.hub_lines) == n:
                            break
                    lines = list(self.hub_lines)
            finally:
                with self.reply_cv:
                    self.collect_hub = False
        for ln in lines:
            self.events.add("hub", ln[2:], poll=text == "!hub status")
        if not lines:
            raise CommandTimeout(f"no answer from the hub to {text}")
        return lines

    def send(self, text, confirmed=False):
        """An operator's command: returns the controller's reply line. Dangerous commands need confirmed=True."""
        text = text.strip()
        if text.startswith("!"):
            if self.mode != "hub" or not text.startswith("!hub"):
                raise ValueError("only '!hub ...' lines are for the host, and only when connected through the host")
            lines = self.hub_command(text)
            return "\n".join(lines)
        danger = dc.classify(text)
        if danger and not confirmed:
            raise NeedsConfirm(danger)
        with self.lock:
            reply = self._exchange(text, self._timeout_for(text))
            if danger and confirmed and self.mode == "hub" and reply.startswith("ERR hub DANGEROUS"):
                # The host holds dangerous commands until "!hub confirm"; the operator has just confirmed on the page.
                reply = self._exchange("!hub confirm", self._timeout_for(text))
        return reply

    def query(self, commands):
        """Only queries (first word ends in '?'), in order, under one lock. Returns [(command, reply or None, error)]."""
        out = []
        with self.lock:
            for c in commands:
                c = c.strip()
                if not c or not c.split()[0].endswith("?"):
                    out.append((c, None, "not a query"))
                    continue
                try:
                    out.append((c, self._exchange(c, self._timeout_for(c), poll=True), None))
                except CommandTimeout as e:
                    out.append((c, None, str(e)))
        return out

    # -- identity, state, link health
    def identify(self):
        try:
            with self.lock:
                reply = self._exchange("*IDN?", 4.0)
                info = parse_idn(reply)
                if info is None:
                    self.error = f"the device answered *IDN? with: {reply}"
                    self.ident = {"board": "?", "reply": reply}
                    return
                try:
                    idr = self._exchange("SYS:ID?", 4.0)
                    if idr.startswith("OK "):
                        info.update({k: v for k, v in parse_kv(idr[3:]).items() if k in ("label", "serial", "uid", "verified", "identity")})
                except CommandTimeout:
                    pass
                self.ident = info
                self.error = None
        except CommandTimeout as e:
            self.error = str(e)

    def _heartbeat(self):
        last_try = 0.0
        while not self.closed:
            time.sleep(0.25)
            if self.closed:
                return
            if not self.link_open:
                if time.time() - last_try > 5.0 and self.mode != "serial":
                    last_try = time.time()
                    try:
                        self._open()
                        self.ident = None
                    except OSError:
                        pass
                continue
            fast = self.recorder.active()
            if time.time() - self.last_poll_t() < (0.35 if fast else self.poll_s):
                continue
            self._poll_t = time.time()
            if not self.lock.acquire(blocking=False):
                continue
            self.lock.release()
            try:
                if self.mode == "hub" and time.time() - self._hub_t >= self.poll_s:
                    self._hub_t = time.time()
                    try:
                        lines = self.hub_command("!hub status")
                        self.hub = parse_kv(lines[0][2:])
                    except CommandTimeout:
                        pass
                if self.ident is None:
                    self.identify()
                if self.ident is not None and self.polling:
                    res = self.query(["STATE?"])
                    c, reply, err = res[0]
                    if reply and reply.startswith("OK "):
                        self.state_text = reply[3:]
                        self.last_ok, self.fails = time.time(), 0
                        self.recorder.on_state(self.state_text.split()[0] if self.state_text else None)
                    else:
                        self.fails += 1
            except Exception as e:                   # never let the heartbeat thread die
                self.error = str(e)

    _poll_t = 0.0
    _hub_t = 0.0

    def last_poll_t(self):
        return self._poll_t

    def status(self):
        ident = self.ident or {}
        product = product_of(ident.get("board")) if self.ident else None
        hubinfo = None
        if self.hub:
            who = [w for w in (self.hub.get("who") or "").split(",") if w and w != "-"]
            hubinfo = {"connected": self.hub.get("connected") == "1", "clients": int(self.hub.get("clients") or 0),
                       "who": who, "confirm": self.hub.get("confirm") == "1", "name": self.hub.get("name"),
                       "serial": self.hub.get("serial"), "identity": self.hub.get("identity"), "rx_age": self.hub.get("rx_age")}
        state, fault = None, None
        if self.state_text:
            words = self.state_text.split()
            state = words[0] if words else None
            fault = " ".join(words[1:]) if state == "FAULT" and len(words) > 1 else None
        return {"id": self.id, "name": self.spec.get("name") or self.target(), "mode": self.mode, "target": self.target(),
                "link_open": self.link_open, "identified": self.ident is not None, "ident": self.ident, "product": product,
                "state": state, "fault": fault, "state_age": None if self.last_ok is None else round(time.time() - self.last_ok, 1),
                "link_ok": self.link_open and self.ident is not None and self.fails < 3, "error": self.error, "hub": hubinfo,
                "polling": self.polling, "seq": self.events.seq, "rec": self.recorder.status()}


# ---- the web server ------------------------------------------------------------------------------------------------------
class App:
    def __init__(self, presets_file=PRESETS_FILE, demo=False, shots_dir=SHOTS_DIR):
        self.token = secrets.token_urlsafe(24)
        self.connections = {}
        self.next_id = 1
        self.lock = threading.Lock()
        self.presets_file = presets_file
        self.shots_dir = shots_dir
        self.user = re.sub(r"[^A-Za-z0-9_.-]", "", getpass.getuser() or "operator") or "operator"
        self.demo = demo
        self.sims = {}
        self.allowed_hosts = set()

    # presets: the operators' list of controllers
    def presets(self):
        try:
            with open(self.presets_file) as f:
                data = json.load(f)
            if isinstance(data, list):
                return data
        except (OSError, ValueError):
            pass
        return list(DEFAULT_PRESETS)

    def save_presets(self, items):
        clean = []
        for it in items:
            if not isinstance(it, dict) or it.get("mode") not in ("hub", "direct", "serial"):
                raise ValueError("every saved controller needs a mode: hub, direct or serial")
            clean.append({k: it[k] for k in ("name", "mode", "host", "port", "device", "note") if k in it})
        tmp = self.presets_file + ".tmp"
        with open(tmp, "w") as f:
            json.dump(clean, f, indent=2)
        os.replace(tmp, self.presets_file)

    def serial_ports(self):
        try:
            from serial.tools import list_ports
        except ImportError:
            return {"available": False, "ports": []}
        return {"available": True, "ports": [{"device": p.device, "description": p.description} for p in list_ports.comports()
                                               if "Bluetooth" not in p.device and "debug-console" not in p.device]}

    def connect(self, spec):
        mode = spec.get("mode")
        if mode == "demo":
            import wham_sim
            product = spec.get("product", "hbridge")
            sim = self.sims.get(product)
            if sim is None:
                sim = wham_sim.SimController(product, 0, label=f"practice-{product}")
                sim.start()
                self.sims[product] = sim
            spec = {"mode": "direct", "host": "127.0.0.1", "port": sim.port, "name": f"Practice {product.upper()} (simulated)"}
        elif mode == "hub" or mode == "direct":
            if not spec.get("host") or not str(spec.get("port", "")).isdigit():
                raise ValueError("a host name or address and a port number are needed")
        elif mode == "serial":
            if not spec.get("device"):
                raise ValueError("choose the serial port")
        else:
            raise ValueError("unknown connection type")
        with self.lock:
            cid = f"c{self.next_id}"
            self.next_id += 1
        conn = Connection(cid, spec, self.user, self.shots_dir)
        try:
            conn.start()
        except OSError as e:
            raise ConnectionError(self.explain(spec, e))
        except ImportError:
            raise ConnectionError("Serial cables need the pyserial package (python3 -m pip install pyserial).")
        with self.lock:
            self.connections[cid] = conn
        return conn

    @staticmethod
    def explain(spec, e):
        where = spec.get("device") or f"{spec.get('host')}:{spec.get('port')}"
        if isinstance(e, ConnectionRefusedError):
            hint = ("nothing is listening there. For a connection through the host, is the ssh tunnel running (and the right port)? "
                    "Is the host service running?" if spec.get("mode") == "hub" else
                    "the controller's network bridge is not accepting a connection. It allows only one client at a time: is the host "
                    "service (hub), or someone else, already connected to it?")
            return f"Could not connect to {where}: {hint}"
        if isinstance(e, (socket.timeout, TimeoutError)):
            return f"Could not connect to {where}: no answer (wrong address, or the network is down)."
        return f"Could not connect to {where}: {e}"

    def get(self, cid):
        conn = self.connections.get(cid)
        if conn is None:
            raise KeyError("no such connection")
        return conn

    def disconnect(self, cid):
        with self.lock:
            conn = self.connections.pop(cid, None)
        if conn:
            conn.close()

    def shutdown(self):
        for cid in list(self.connections):
            self.disconnect(cid)
        for sim in self.sims.values():
            sim.stop()


class Handler(BaseHTTPRequestHandler):
    server_version = "WHAMUI/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    @property
    def app(self):
        return self.server.app

    # -- plumbing
    def _json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _bad(self, status, message, **extra):
        self._json(dict(error=message, **extra), status)

    def _host_ok(self):
        host = (self.headers.get("Host") or "").rsplit(":", 1)[0].strip("[]").lower()
        return host in self.app.allowed_hosts

    def _token_ok(self):
        return secrets.compare_digest(self.headers.get("X-WHAM-Token", ""), self.app.token)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 1_000_000:
            raise ValueError("request too large")
        return json.loads(self.rfile.read(n) or b"{}")

    # -- GET
    def do_GET(self):
        if not self._host_ok():
            return self._bad(403, "bad Host header")
        url = urlparse(self.path)
        if url.path == "/" or url.path == "/index.html":
            with open(os.path.join(STATIC, "index.html"), "rb") as f:
                page = f.read().replace(b"__WHAM_TOKEN__", self.app.token.encode()).replace(b"__WHAM_USER__", self.app.user.encode())
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(page)
            return
        if url.path.startswith("/static/"):
            name = os.path.normpath(url.path[len("/static/"):])
            path = os.path.join(STATIC, name)
            if name.startswith("..") or not os.path.isfile(path):
                return self._bad(404, "not found")
            ctype = {".js": "text/javascript", ".css": "text/css", ".svg": "image/svg+xml"}.get(os.path.splitext(path)[1], "application/octet-stream")
            with open(path, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", ctype + "; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if not url.path.startswith("/api/"):
            return self._bad(404, "not found")
        if not self._token_ok():
            return self._bad(403, "bad token")
        q = parse_qs(url.query)
        try:
            if url.path == "/api/state":
                return self._json({"connections": [c.status() for c in list(self.app.connections.values())], "presets": self.app.presets(),
                                   "serial": self.app.serial_ports(), "user": self.app.user, "demo": self.app.demo,
                                   "shots_dir": self.app.shots_dir})
            if url.path == "/api/shots":
                conn = self.app.get(q["id"][0])
                ident = conn.ident or {}
                ctrl = {"serial": ident.get("serial"), "name": conn.spec.get("name") or conn.target(), "board": ident.get("board")}
                return self._json({"shots": sd.list_recordings(self.app.shots_dir, ctrl)})
            if url.path == "/api/shot":
                sid = q["sid"][0]
                rec = sd.load(self.app.shots_dir, sid)
                return self._json(sd.describe(rec, sid, q.get("unit", ["A"])[0]))
            if url.path == "/api/shot/file":
                sid, name = q["sid"][0], q["name"][0]
                body = sd.read_file(self.app.shots_dir, sid, name)
                ctype = {"shot.json": "application/json", "shot.csv": "text/csv", "report.html": "text/html", "report.md": "text/markdown"}[name]
                fname = sid.replace("/", "_") + "_" + name
                self.send_response(200)
                self.send_header("Content-Type", ctype + "; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Content-Disposition", f'attachment; filename="{fname}"')
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return
            if url.path == "/api/events":
                conn = self.app.get(q["id"][0])
                since = int(q.get("since", ["0"])[0])
                wait = min(float(q.get("wait", ["0"])[0]), 25.0)
                events, seq = conn.events.since(since, wait)
                return self._json({"events": events, "seq": seq, "status": conn.status()})
        except KeyError as e:
            return self._bad(404, str(e))
        except (ValueError, IndexError) as e:
            return self._bad(400, str(e))
        return self._bad(404, "not found")

    # -- POST
    def do_POST(self):
        if not self._host_ok():
            return self._bad(403, "bad Host header")
        if not self._token_ok():
            return self._bad(403, "bad token")
        path = urlparse(self.path).path
        try:
            body = self._body()
            if path == "/api/connect":
                try:
                    conn = self.app.connect(body)
                except ConnectionError as e:
                    return self._bad(502, str(e))
                return self._json(conn.status())
            if path == "/api/disconnect":
                self.app.disconnect(body["id"])
                return self._json({"ok": True})
            if path == "/api/presets":
                self.app.save_presets(body["presets"])
                return self._json({"ok": True, "presets": self.app.presets()})
            if path == "/api/send":
                conn = self.app.get(body["id"])
                try:
                    reply = conn.send(str(body["command"]), confirmed=bool(body.get("confirmed")))
                except NeedsConfirm as e:
                    return self._bad(409, "confirmation needed", needs_confirm=True, category=e.danger.category, command=body["command"])
                except CommandTimeout as e:
                    return self._json({"ok": False, "reply": None, "error": str(e)})
                return self._json({"ok": reply.startswith("OK") or not reply.startswith("ERR"), "reply": reply, "error": None})
            if path == "/api/query":
                conn = self.app.get(body["id"])
                res = conn.query([str(c) for c in body["commands"]])
                return self._json({"results": [{"command": c, "reply": r, "error": e} for c, r, e in res]})
            if path in ("/api/record/arm", "/api/record/cancel", "/api/record/collect"):
                conn = self.app.get(body["id"])
                try:
                    if path.endswith("arm"):
                        st = conn.recorder.arm(body)
                    elif path.endswith("cancel"):
                        st = conn.recorder.cancel()
                    else:
                        st = conn.recorder.collect_now(body)
                except rc.RecordError as e:
                    return self._bad(409, str(e))
                except CommandTimeout as e:
                    return self._bad(504, f"the controller did not answer: {e}")
                return self._json({"rec": st})
            if path == "/api/polling":
                conn = self.app.get(body["id"])
                conn.polling = bool(body["on"])
                return self._json({"ok": True})
        except KeyError as e:
            return self._bad(404, f"missing or unknown: {e}")
        except (ValueError, TypeError) as e:
            return self._bad(400, str(e))
        return self._bad(404, "not found")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, app):
        super().__init__(addr, Handler)
        self.app = app


def make_server(app, host="127.0.0.1", port=8765):
    last = None
    for p in range(port, port + 20):
        try:
            srv = Server((host, p), app)
            app.allowed_hosts = {"127.0.0.1", "localhost", "::1"} | ({host} if host not in ("0.0.0.0", "") else set())
            return srv
        except OSError as e:
            last = e
    raise last


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765, help="the page's port on this computer (default 8765; the next free one if busy)")
    ap.add_argument("--listen", default="127.0.0.1", help="address to serve the page on (default 127.0.0.1 = this computer only)")
    ap.add_argument("--no-browser", action="store_true", help="do not open the page in the browser")
    ap.add_argument("--shots-dir", default=SHOTS_DIR, help="where recorded shots are kept (default: host/operator_ui/shots)")
    ap.add_argument("--demo", action="store_true", help="offer a simulated controller on the connection screen, for practice")
    args = ap.parse_args()
    app = App(demo=args.demo, shots_dir=args.shots_dir)
    srv = make_server(app, args.listen, args.port)
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    if args.listen not in ("127.0.0.1", "localhost", "::1"):
        print(f"WARNING: serving on {args.listen}: anyone who can reach this port can command the controllers you connect to.")
    print(f"WHAM operator console: {url}\nLeave this window open while you work; close it (Ctrl-C) to disconnect everything.")
    if not args.no_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.shutdown()


if __name__ == "__main__":
    main()

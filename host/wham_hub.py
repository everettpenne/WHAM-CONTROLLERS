#!/usr/bin/env python3
"""wham_hub.py -- the per-controller host service (design: docs/device_identity_and_host_service.md, section 5).

ONE instance talks to ONE controller. It is the only client of that controller's serial-over-TCP bridge (the bridge
accepts exactly one), shares it with any number of local clients, learns WHICH board it is (SYS:ID?), and logs
everything with that identity. Run one instance per controller, as a service (host/deploy/wham-hub@.service).

    python3 host/wham_hub.py --config host/config/magnet2.yaml
    python3 host/wham_hub.py --host 192.168.130.138 --listen-port 5001 --name magnet2

What it does
  * connects to the bridge (host:port), reconnecting forever; on connect and after every !BOOT it asks the board
    *IDN? and SYS:ID? and binds the session to the board's serial (or, for a board with none, UNSET-<last 8 of its MCU ID>);
  * with `expect_serial` set, a different board on the other end is refused: client commands are NOT forwarded;
  * clients speak the same raw protocol as python/net_broker.py (optional first line "@name <who>", then one command per
    line; everything the controller sends is broadcast; other clients' commands arrive as "[name] > cmd"), so
    net_client.py, wham_console.py and fw_update.py work through it unchanged;
  * every line, in both directions, is appended to <log_dir>/<serial>/YYYY-MM-DD.jsonl with a host timestamp, the
    serial, the boot epoch (counts !BOOT lines) and the session (counts connections);
  * a client line starting "!hub" is answered by the service itself:  !hub status | !hub identify.

Not here yet (later steps of the plan): write leases and the dangerous-command list, a JSON client API, serial-port
upstreams, a shared message bus. There is NO authentication and the default listen address is all interfaces (as
requested for now): anyone who can reach the port can send ARM/FIRE. Restrict it with listen_host (127.0.0.1) or the
network when that changes.

Config: a flat YAML-style file (key: value, # comments) -- keys name, host, port, listen_host, listen_port, expect_serial,
log_dir, registry, reconnect_s. Command-line options override it. PyYAML is used if installed, but is not required.
"""
import argparse
import asyncio
import datetime
import json
import os
import re
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import provision_board as pb  # parse_id, load_registry (same directory)

DEFAULTS = {
    "name": None, "host": None, "port": 5000, "listen_host": "0.0.0.0", "listen_port": None,
    "expect_serial": None, "log_dir": os.path.join(HERE, "logs"), "registry": os.path.join(HERE, "devices.csv"),
    "reconnect_s": 3.0,
}
BOOT_RE = re.compile(r"^!BOOT WHAM-")
MAX_LOG_LINE = 400          # characters kept per logged line (firmware-update data lines are far longer)


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


def build_config(args):
    cfg = dict(DEFAULTS)
    if args.config:
        cfg.update(load_config(args.config))
    for key in DEFAULTS:
        v = getattr(args, key, None)
        if v is not None:
            cfg[key] = v
    cfg["port"] = int(cfg["port"])
    cfg["listen_port"] = int(cfg["listen_port"]) if cfg["listen_port"] is not None else None
    cfg["reconnect_s"] = float(cfg["reconnect_s"])
    if not cfg["host"] or cfg["listen_port"] is None:
        raise SystemExit("wham_hub: the controller's host and a listen_port are required (--config or --host/--listen-port)")
    cfg["name"] = cfg["name"] or cfg["host"]
    return cfg


# ---- the log ---------------------------------------------------------------------------------------------------
class DeviceLog:
    """Append-only JSON lines, one file per device per day."""

    def __init__(self, base):
        self.base = base
        self.fh = None
        self.path = None

    def write(self, key, record):
        day = datetime.date.today().isoformat()
        path = os.path.join(self.base, key, day + ".jsonl")
        if path != self.path:
            if self.fh:
                self.fh.close()
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.fh = open(path, "a", buffering=1)
            self.path = path
        self.fh.write(json.dumps(record, separators=(",", ":")) + "\n")

    def close(self):
        if self.fh:
            self.fh.close()


# ---- the service -----------------------------------------------------------------------------------------------
class Hub:
    def __init__(self, cfg):
        self.cfg = cfg
        self.log = DeviceLog(cfg["log_dir"])
        self.identity = {}           # parsed SYS:ID? (plus idn), {} until the handshake succeeds
        self.epoch = 0               # !BOOT lines seen
        self.session = 0             # connections made
        self.up_writer = None
        self.connected = False
        self.clients = {}            # writer -> name
        self.busy = False            # a handshake is in progress: client commands wait
        self.held = []               # (client name, text) waiting for the handshake
        self.waiting = None          # future of the internal query in flight
        self.handshake_task = None
        self.mismatch = None         # text when expect_serial is set and the board differs
        self.registered = None
        self.stop = asyncio.Event()
        self.fail_reported = False
        self.pending = []            # records written before the first handshake settled
        self.identity_settled = False

    # -- identity and logging
    def dev_key(self):
        sn = self.identity.get("serial")
        if sn and sn != "UNSET":
            return sn
        uid = self.identity.get("uid")
        return f"UNSET-{uid[-8:]}" if uid else f"unidentified-{self.cfg['name']}"

    def serial(self):
        sn = self.identity.get("serial")
        return None if not sn or sn == "UNSET" else sn

    def record(self, kind, line, client=None):
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
        """A service event: logged, printed, and shown to every client as a --- line."""
        self.record(kind, text)
        print(f"[{self.cfg['name']}] {text}", flush=True)
        self.broadcast(f"--- hub: {text} ---")

    def broadcast(self, text, exclude=None):
        data = (text + "\r\n").encode()
        for w in list(self.clients):
            if w is not exclude:
                try:
                    w.write(data)
                except Exception:
                    self.clients.pop(w, None)

    def status_line(self):
        i = self.identity
        return ("# hub name={n} target={h}:{p} connected={c} serial={s} identity={idn} verified={v} uid={u} flashuid={f} "
                "product={pr} fw={fw} epoch={e} session={se} clients={cl} registered={r} expect={x} match={m} log={lg}").format(
            n=self.cfg["name"], h=self.cfg["host"], p=self.cfg["port"], c=int(self.connected),
            s=i.get("serial", "?"), idn=i.get("identity", "?"), v=i.get("verified", "?"), u=i.get("uid", "?"),
            f=i.get("flashuid", "?"), pr=i.get("product", "?"), fw=i.get("fw", "?"), e=self.epoch, se=self.session,
            cl=len(self.clients), r={None: "?", True: 1, False: 0}[self.registered], x=self.cfg["expect_serial"] or "-",
            m=0 if self.mismatch else 1, lg=self.log.path or "-")

    # -- talking to the controller
    async def query(self, cmd, timeout=3.0):
        """Send one command of the service's own and return its OK/ERR line (hidden from the clients)."""
        self.waiting = asyncio.get_running_loop().create_future()
        self.record("hub-cmd", cmd)
        self.up_writer.write(cmd.encode() + b"\r\n")
        await self.up_writer.drain()
        try:
            return await asyncio.wait_for(self.waiting, timeout)
        finally:
            self.waiting = None

    async def handshake(self):
        self.busy = True
        try:
            idn = await self.query("*IDN?")
            ident = {"idn": idn}
            reply = await self.query("SYS:ID?")
            if reply.startswith("OK "):
                ident.update(pb.parse_id(reply))
            else:
                ident.update({"identity": "legacy", "serial": "UNSET"})   # firmware without SYS:ID?
            old = self.dev_key()
            self.identity = ident
            self.settle()
            self.check_identity()
            self.note(f"identity {self.dev_key()}: " + " ".join(
                f"{k}={ident.get(k)}" for k in ("serial", "identity", "verified", "product", "fw")) + f" (was {old})")
        except (asyncio.TimeoutError, ConnectionError, OSError) as exc:
            self.identity = {}
            self.settle()
            self.note(f"handshake failed: {exc!r}; the controller did not answer")
        finally:
            self.busy = False
            held, self.held = self.held, []
            for who, text in held:
                await self.forward(who, text)

    def check_identity(self):
        sn = self.serial()
        self.mismatch = None
        want = self.cfg["expect_serial"]
        if want and sn != want:
            self.mismatch = f"expected serial {want}, the controller says {sn or 'UNSET'}"
        try:
            rows = pb.load_registry(self.cfg["registry"])
        except OSError:
            rows = []
        reg = [r for r in rows if sn and r["serial"] == sn]
        self.registered = (len(reg) > 0) if rows else None
        if reg and self.identity.get("uid") and reg[0]["uid"] != self.identity["uid"]:
            self.mismatch = (self.mismatch or "") + f" registry holds {sn} for different hardware ({reg[0]['uid']})"
        if self.mismatch:
            print(f"[{self.cfg['name']}] IDENTITY MISMATCH: {self.mismatch}", flush=True)

    async def forward(self, who, text):
        if not self.connected:
            self.record("refused", text, who)
            self.send_to(who, "--- hub: the controller is not connected ---")
        elif self.mismatch:
            self.record("refused", text, who)
            self.send_to(who, f"ERR hub identity mismatch: {self.mismatch}")
        else:
            self.record("cmd", text, who)
            self.broadcast(f"[{who}] > {text}", exclude=self.writer_of(who))
            self.up_writer.write(text.encode() + b"\r\n")
            await self.up_writer.drain()

    def writer_of(self, name):
        for w, n in self.clients.items():
            if n == name:
                return w
        return None

    def send_to(self, name, text):
        w = self.writer_of(name)
        if w:
            w.write((text + "\r\n").encode())

    def on_line(self, text):
        """One line from the controller."""
        if self.waiting is not None and not self.waiting.done() and (text.startswith("OK") or text.startswith("ERR")):
            self.record("hub-reply", text)
            self.waiting.set_result(text)
            return
        if "!BOOT" in text:
            text = text[text.index("!BOOT"):]      # a reset can leave a stray byte in front of the banner
        if BOOT_RE.match(text):
            self.epoch += 1
            self.record("boot", text)
            self.broadcast(text)
            self.start_handshake(delay=0.6)       # let the rest of the banner finish
            return
        kind = ("event" if text.startswith("!EVT") else "boot" if text.startswith("!BOOT") else
                "reply" if text.startswith(("OK", "ERR")) else "data")
        self.record(kind, text)
        self.broadcast(text)

    def start_handshake(self, delay=0.0):
        if self.handshake_task and not self.handshake_task.done():
            self.handshake_task.cancel()

        async def run():
            if delay:
                await asyncio.sleep(delay)
            await self.handshake()
        self.handshake_task = asyncio.ensure_future(run())

    # -- connections
    async def upstream_loop(self):
        while not self.stop.is_set():
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self.cfg["host"], self.cfg["port"]), 5)
            except (OSError, asyncio.TimeoutError) as exc:
                if not self.fail_reported:      # once per outage, not every retry
                    self.fail_reported = True
                    self.note(f"cannot connect to {self.cfg['host']}:{self.cfg['port']}: {exc!r}; retrying every {self.cfg['reconnect_s']:g} s")
                self.connected = False
                await self.sleep(self.cfg["reconnect_s"])
                continue
            self.up_writer, self.connected = writer, True
            self.fail_reported = False
            self.session += 1
            self.note(f"connected to the controller at {self.cfg['host']}:{self.cfg['port']} (session {self.session})")
            self.start_handshake()
            buf = b""
            try:
                while True:
                    data = await reader.read(4096)
                    if not data:
                        break
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self.on_line(line.decode(errors="replace").strip("\r\n"))
            except (ConnectionError, OSError):
                pass
            self.connected = False
            if self.waiting is not None and not self.waiting.done():
                self.waiting.cancel()
            writer.close()
            self.note("connection to the controller closed; reconnecting")
            await self.sleep(self.cfg["reconnect_s"])

    async def sleep(self, seconds):
        try:
            await asyncio.wait_for(self.stop.wait(), seconds)
        except asyncio.TimeoutError:
            pass

    async def handle_client(self, reader, writer):
        name = "client"
        try:
            first = await reader.readline()
            if first.startswith(b"@name "):
                name = first[6:].decode(errors="replace").strip() or name
                first = b""
            while name in self.clients.values():       # names identify clients in the echo: keep them unique
                name += "'"
            self.clients[writer] = name
            self.record("hub", f"{name} joined")
            self.broadcast(f"--- {name} joined ---", exclude=writer)
            writer.write(f"--- connected as {name}; {len(self.clients)} client(s); controller "
                         f"{self.dev_key()} ---\r\n".encode())
            pending = first
            while True:
                line = pending or await reader.readline()
                pending = b""
                if not line:
                    break
                text = line.decode(errors="replace").rstrip("\r\n")
                if text.startswith("!hub"):
                    await self.hub_command(name, writer, text)
                elif self.busy:
                    self.held.append((name, text))
                else:
                    await self.forward(name, text)
        except (ConnectionError, OSError):
            pass
        finally:
            self.clients.pop(writer, None)
            self.record("hub", f"{name} left")
            self.broadcast(f"--- {name} left ---")
            writer.close()

    async def hub_command(self, name, writer, text):
        cmd = text[4:].strip().lower()
        self.record("hub-cmd", text, name)
        if cmd == "status":
            writer.write((self.status_line() + "\r\n").encode())
        elif cmd == "identify":
            if self.connected:
                self.start_handshake()
                writer.write(b"# hub: identifying the controller\r\n")
            else:
                writer.write(b"# hub: the controller is not connected\r\n")
        else:
            writer.write(b"# hub: commands are  !hub status  |  !hub identify\r\n")

    async def run(self):
        server = await asyncio.start_server(self.handle_client, self.cfg["listen_host"], self.cfg["listen_port"])
        print(f"[{self.cfg['name']}] controller {self.cfg['host']}:{self.cfg['port']}; clients on "
              f"{self.cfg['listen_host']}:{self.cfg['listen_port']} (no authentication); log {self.cfg['log_dir']}", flush=True)
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
    ap.add_argument("--expect-serial", dest="expect_serial"); ap.add_argument("--log-dir", dest="log_dir")
    ap.add_argument("--registry"); ap.add_argument("--reconnect-s", dest="reconnect_s", type=float)
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

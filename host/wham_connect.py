#!/usr/bin/env python3
"""wham_connect.py -- one command from your terminal to one controller: opens the ssh tunnel to the lab host (only the port that
device needs), checks that the hub on the other end is the board you named, then gives you the interactive session.

    python3 host/wham_connect.py limiter            # tunnel + identity check + interactive session
    python3 host/wham_connect.py xrex
    python3 host/wham_connect.py ech
    python3 host/wham_connect.py WB-0002-6          # a serial number works too
    python3 host/wham_connect.py --list             # the devices this tool knows
    python3 host/wham_connect.py limiter --watch    # supervise: sends nothing, shows who joins/leaves and every command and reply
    python3 host/wham_connect.py limiter --watch --transcript limiter_watch.log     # ... and keep a copy in a file
    python3 host/wham_connect.py limiter --send "*IDN?" --send "STATE?"      # one-shot, for scripts
    python3 host/wham_connect.py --port 5104 --serial WB-0009-1               # a device that is not in the table

What it does, in order:
  1. Tunnel. `ssh -N -L <p>:127.0.0.1:<p> <host>` for that device's hub port (the same Tunnel class as the operator UI's "Open
     tunnel" button, host/operator_ui/wham_ui.py). If the port already answers on this computer (a tunnel you or the UI
     opened), it is used as it is and is NOT closed when you leave. A tunnel this tool started is closed when you leave
     (--keep-tunnel leaves it up). ssh runs with BatchMode: it needs a key, it never asks for a password.
     The lab host is `andrew`, or $WHAM_TUNNEL_HOST, or --tunnel-host (an ssh name or user@address).
  2. Identity. A separate quiet connection sends `*IDN?` and compares the SN= in the reply with the device's serial. A hub that
     answers for a different board, or a port held by something else, stops here (--force to go on anyway).
  3. Session. Interactive: everything on the shared hub session prints as it arrives with the time, other people's commands and
     replies show as "[name] > cmd" / "[name] < reply", and each line you type is sent. Ctrl-D (or Ctrl-C) quits.
     --watch is the same stream with no keyboard: it sends nothing to the controller, so it can sit in a window and supervise.
     Both modes start with the list of who is already connected, and highlight (when the terminal supports colour; NO_COLOR
     turns it off) joins and leaves in yellow and DANGEROUS announcements in red. --transcript FILE keeps everything shown.
     --hide-polls drops other clients' routine `STATE?` / `!hub status` polls and their replies (an operator console asks every
     2 s and otherwise fills the log); everything else, including every other command, still shows.

SAFETY: the hub has no login, and this tool sends whatever you type, including ARM and FIRE. The hub announces dangerous commands to the other
clients and logs them; it does not stop you. Nothing is sent that you did not type or pass as --send, except `*IDN?` in step 2 and
the `@name`, `!hub timestamps` and `!hub clients` set-up lines (answered by the hub, not the controller).

--no-tunnel skips step 1 (you are on the lab network and the hub's host is reachable: use --hub-host <host>).
Standard library only; Python 3.8+.
"""
import argparse
import getpass
import os
import re
import signal
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# name, hub port on andrew, serial, what it is. The ports match host/config/*.yaml (listen_port); the serials match host/devices.csv.
DEVICES = [
    {"key": "xrex", "label": "XREX-CTRL", "port": 5101, "serial": "WB-0003-4", "product": "WHAM-XREX-PFMG474",
     "aliases": ["xrex-ctrl", "magnet", "transrex"], "what": "XREX magnet-supply controller"},
    {"key": "limiter", "label": "LMTR-CTRL", "port": 5102, "serial": "WB-0002-6", "product": "WHAM-HBRIDGE-PFMG474",
     "aliases": ["lmtr", "lmtr-ctrl"], "what": "Limiter controller (HBRIDGE firmware)"},
    {"key": "ech", "label": "ECH-HVPS", "port": 5103, "serial": "WB-0001-8", "product": "WHAM-HBRIDGE-PFMG474",
     "aliases": ["ech-hvps", "gyrotron"], "what": "ECH gyrotron HVPS (HBRIDGE firmware)"},
]


def find_device(word):
    """The table row for a name, alias, label, serial number or hub port, or None. Case does not matter."""
    w = word.strip().lower()
    for d in DEVICES:
        names = {d["key"], d["label"].lower(), d["serial"].lower(), str(d["port"])} | set(d["aliases"])
        if w in names:
            return d
    return None


def quiet_query(host, port, command, timeout=4.0):
    """Ask the hub one question on a quiet connection and return the first reply line that is not a `!` or `#` line.
    Raises OSError if the hub cannot be reached, TimeoutError if nothing answers."""
    s = socket.create_connection((host, port), timeout=5)
    try:
        s.settimeout(0.5)
        s.sendall(b"@quiet wham_connect-idcheck\r\n" + command.encode() + b"\r\n")
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line and line[0] not in "!#":
                    return line
        raise TimeoutError(f"no reply to {command} within {timeout:g} s")
    finally:
        s.close()


def serial_of(idn_reply):
    m = re.search(r"\bSN=(\S+)", idn_reply)
    return m.group(1) if m else None


def check_identity(host, port, want_serial, want_product=None):
    """(ok, message). ok is False only when the board answered and is not the one wanted; a controller the hub cannot reach
    is reported with ok False too, because every command would fail."""
    try:
        reply = quiet_query(host, port, "*IDN?")
    except (OSError, TimeoutError) as e:
        return False, f"the hub at {host}:{port} did not answer *IDN?: {e}"
    if reply.startswith("ERR"):
        return False, f"the hub answered, but not with an identity: {reply}  (the controller may be off or its bridge unreachable)"
    sn = serial_of(reply)
    if want_serial and sn and sn != want_serial:
        return False, f"wrong board: this port answers as {sn}, not {want_serial}. Reply: {reply}"
    if want_serial and not sn:
        return False, f"the reply has no SN= field, so the board cannot be confirmed as {want_serial}: {reply}"
    if want_product and want_product not in reply:
        return False, f"unexpected product: wanted {want_product}. Reply: {reply}"
    return True, reply


def open_tunnel(host, port, tunnel_host, ssh=None):
    """Make 127.0.0.1:port reach the hub. Returns (tunnel, state, message): tunnel is the Tunnel object if this call started
    ssh (so the caller closes it), else None."""
    sys.path.insert(0, os.path.join(HERE, "operator_ui"))
    import wham_ui                                   # imported only here: --list and --no-tunnel never need it
    t = wham_ui.Tunnel(host=tunnel_host, ports_fn=lambda: [port], ssh=ssh)
    before = t.status()["state"]
    st = t.start()
    if st["state"] == "open-elsewhere" and before == "open-elsewhere":
        return None, "open-elsewhere", f"tunnel to {tunnel_host}: port {port} already open (not started by this tool; left alone on exit)"
    if st["state"] == "open":
        return t, "open", f"tunnel to {tunnel_host}: port {port} opened"
    return None, "closed", st.get("error") or "the tunnel did not open"


RED, YELLOW, BOLD, RESET = "\033[1;31m", "\033[33m", "\033[1m", "\033[0m"


def style_line(line, color):
    """Highlight what a supervisor looks for: DANGEROUS announcements, joins and leaves, and other people's commands."""
    if not color:
        return line
    if "DANGEROUS" in line:
        return RED + line + RESET
    if re.search(r"--- .*\b(joined|left)\b", line):
        return YELLOW + line + RESET
    if re.search(r"\[[^\]]+\] >", line):
        return BOLD + line + RESET
    return line


POLLS = ("STATE?", "!HUB STATUS")
_LINE = re.compile(r"^(?:\[\d\d:\d\d:\d\d\.\d+\] )?\[([^\]]+)\] ([<>]) ?(.*)$")


class PollFilter:
    """Hides other clients' routine polls (STATE?, !hub status) and the reply to each, so the log shows what they actually do.
    Only an exact poll command is hidden; the reply is the next `[name] <` line from the same client."""

    def __init__(self, enabled):
        self.enabled, self.pending, self.hidden = enabled, {}, 0

    def hide(self, line):
        if not self.enabled:
            return False
        m = _LINE.match(line)
        if not m:
            return False
        who, kind, text = m.groups()
        if kind == ">" and text.strip().upper() in POLLS:
            self.pending[who] = self.pending.get(who, 0) + 1
            self.hidden += 1
            return True
        if kind == "<" and self.pending.get(who, 0) > 0:
            self.pending[who] -= 1
            return True
        return False


def stream_reader(sock, done, transcript=None, color=False, polls=None):
    """Print what the hub sends, a line at a time (highlighted, and copied to the transcript file). When the hub closes the
    connection, say so and wake the main thread."""
    buf = b""
    polls = polls or PollFilter(False)
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace").rstrip("\r")
            if polls.hide(line):
                continue
            print(style_line(line, color), flush=True)
            if transcript:
                transcript.write(line + "\n")
                transcript.flush()
    if not done.is_set():
        print("\n--- the hub closed the connection ---", flush=True)
        done.set()
        import _thread
        _thread.interrupt_main()                     # wakes the main thread out of input() / the watch loop


def open_session(host, port, name):
    """A normal (non-quiet) hub connection with timestamps on, who is connected listed up front."""
    sock = socket.create_connection((host, port), timeout=5)
    sock.settimeout(None)
    sock.sendall(f"@name {name}\r\n".encode() + b"!hub timestamps on\r\n!hub clients\r\n")
    return sock


def run_session(host, port, name, transcript=None, color=False, watch=False, hide_polls=False):
    """The interactive terminal (or, with watch, a read-only one): print what arrives, and send each typed line unless watching.
    Returns on Ctrl-D, Ctrl-C or when the hub closes."""
    sock = open_session(host, port, name)
    done = threading.Event()
    polls = PollFilter(hide_polls)
    threading.Thread(target=stream_reader, args=(sock, done, transcript, color, polls), daemon=True).start()
    try:
        if watch:
            while not done.is_set():
                time.sleep(0.5)                      # nothing is read from the keyboard and nothing is sent
        else:
            try:
                import readline                      # noqa: F401  line editing and history for input()
            except ImportError:
                pass
            while not done.is_set():
                try:
                    line = input()
                except EOFError:
                    break
                sock.sendall(line.rstrip("\r\n").encode() + b"\r\n")
    except KeyboardInterrupt:
        pass
    finally:
        done.set()
        try:
            sock.close()
        except OSError:
            pass
        if polls.hidden:
            print(f"({polls.hidden} routine polls from other clients were hidden)")


def run_oneshot(host, port, name, commands, wait):
    sock = socket.create_connection((host, port), timeout=5)
    sock.settimeout(None)
    sock.sendall(f"@name {name}\r\n".encode())

    def reader():
        while True:
            try:
                data = sock.recv(4096)
            except OSError:
                return
            if not data:
                return
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    time.sleep(0.3)
    for c in commands:
        sock.sendall(c.encode() + b"\r\n")
        time.sleep(0.4)
    time.sleep(wait)
    sock.close()


def print_devices():
    print(f"{'name':9} {'port':5} {'serial':10} {'product':22} what")
    for d in DEVICES:
        print(f"{d['key']:9} {d['port']:<5} {d['serial']:10} {d['product']:22} {d['what']}  (also: {', '.join(d['aliases'])})")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Open the tunnel and a hub session for one WHAM controller.")
    ap.add_argument("device", nargs="?", help="limiter | xrex | ech | a serial number (WB-0002-6) | a hub port; see --list")
    ap.add_argument("--list", action="store_true", help="show the known devices and exit")
    ap.add_argument("--port", type=int, help="hub port, for a device that is not in the table")
    ap.add_argument("--serial", help="the serial number to require, with --port (default: none, no identity check against a serial)")
    ap.add_argument("--tunnel-host", default=os.environ.get("WHAM_TUNNEL_HOST", "andrew"),
                    help="the lab host that runs the hubs (an ssh name or user@address; default andrew, or $WHAM_TUNNEL_HOST)")
    ap.add_argument("--no-tunnel", action="store_true", help="do not open a tunnel (on the lab network, or the tunnel is managed elsewhere)")
    ap.add_argument("--hub-host", default="127.0.0.1", help="where the hub is reached from this computer (default 127.0.0.1, the tunnel's end)")
    ap.add_argument("--keep-tunnel", action="store_true", help="leave a tunnel this tool started open when it exits")
    ap.add_argument("--force", action="store_true", help="go on even if the identity check fails")
    ap.add_argument("--name", default=None, help="your name as the other clients see it (default: your login name, with -watch added for --watch)")
    ap.add_argument("--watch", action="store_true",
                    help="supervise: read-only, sends nothing; shows who joins and leaves and every command and reply on the hub")
    ap.add_argument("--hide-polls", action="store_true",
                    help="interactive/--watch: hide other clients' routine STATE? and !hub status polls and their replies")
    ap.add_argument("--transcript", metavar="FILE", help="interactive/--watch: also append everything shown to FILE")
    ap.add_argument("--send", action="append", help="one-shot: command to send (repeatable); prints replies, then exits")
    ap.add_argument("--wait", type=float, default=1.0, help="one-shot: seconds to listen after the last command (default 1)")
    args = ap.parse_args(argv)

    if args.list:
        print_devices()
        return 0

    dev = None
    if args.port:
        dev = {"key": f"port {args.port}", "label": f"port {args.port}", "port": args.port, "serial": args.serial,
               "product": None, "what": "device given by --port"}
    elif args.device:
        dev = find_device(args.device)
        if dev is None:
            print(f"unknown device '{args.device}'. Known:", file=sys.stderr)
            print_devices()
            return 2
    else:
        ap.print_usage(sys.stderr)
        print("name a device (limiter, xrex, ech), or --list", file=sys.stderr)
        return 2

    if args.watch and args.send:
        print("--watch sends nothing; --send sends commands. Use one.", file=sys.stderr)
        return 2
    name = args.name or (getpass.getuser() + ("-watch" if args.watch else ""))
    port, tunnel = dev["port"], None

    def finish(code):
        if tunnel is not None and not args.keep_tunnel:
            tunnel.stop()
            print(f"tunnel to {args.tunnel_host} closed")
        return code

    def stop_on_signal(signum, frame):
        raise SystemExit(130)
    signal.signal(signal.SIGTERM, stop_on_signal)
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, stop_on_signal)

    try:
        print(f"{dev['label']}: {dev['what']}" + (f", serial {dev['serial']}" if dev["serial"] else ""))
        if args.no_tunnel:
            print(f"tunnel: skipped, going to {args.hub_host}:{port}")
        else:
            tunnel, state, msg = open_tunnel(args.hub_host, port, args.tunnel_host)
            print(("tunnel: " if state != "closed" else "tunnel FAILED: ") + msg)
            if state == "closed" and not args.force:
                print("Open it yourself or use --no-tunnel if the hub is reachable another way. --force tries anyway.", file=sys.stderr)
                return finish(1)
        ok, msg = check_identity(args.hub_host, port, dev["serial"], dev["product"])
        print(("identity: " if ok else "identity FAILED: ") + msg)
        if not ok and not args.force:
            return finish(1)
        if args.send:
            run_oneshot(args.hub_host, port, name, args.send, args.wait)
        else:
            print("Watching (read-only, nothing is sent). Ctrl-C quits.\n" if args.watch
                  else "Connected. Type SCPI commands; Ctrl-D quits.\n")
            color = sys.stdout.isatty() and "NO_COLOR" not in os.environ
            transcript = open(args.transcript, "a", encoding="utf-8") if args.transcript else None
            try:
                run_session(args.hub_host, port, name, transcript=transcript, color=color, watch=args.watch, hide_polls=args.hide_polls)
            finally:
                if transcript:
                    transcript.close()
        return finish(0)
    except SystemExit:
        finish(130)
        raise
    except KeyboardInterrupt:
        return finish(130)
    except OSError as e:
        print(f"cannot connect to {args.hub_host}:{port}: {e}", file=sys.stderr)
        return finish(1)


if __name__ == "__main__":
    sys.exit(main())

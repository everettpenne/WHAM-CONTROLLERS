#!/usr/bin/env python3
"""Client for net_broker.py -- interactive terminal, or one-shot send.

    python3 python/net_client.py                       # interactive (default name "you")
    python3 python/net_client.py --name claude --send "*IDN?" --send "STATE?" --wait 1.0

Against wham_hub.py an interactive session asks the service to start every line with the time, [HH:MM:SS.mmm]
(--no-timestamps to turn that off, and for net_broker.py, which does not know it). One-shot output is never stamped.

Interactive: everything on the shared session prints as it arrives (controller
output plus "[name] > cmd" for other people's commands); each line you type is
sent. Ctrl-D quits. One-shot: sends each --send in order (0.4 s apart), then
prints whatever arrives for --wait seconds and exits.
"""
import argparse
import socket
import sys
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("--name", default="you")
ap.add_argument("--port", type=int, default=5001)
ap.add_argument("--send", action="append", help="one-shot: command to send (repeatable)")
ap.add_argument("--wait", type=float, default=1.0, help="one-shot: seconds to listen after sending")
ap.add_argument("--no-timestamps", action="store_true",
                help="interactive: do not ask the host service to put the time at the start of every line")
args = ap.parse_args()

sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
sock.settimeout(None)
sock.sendall(f"@name {args.name}\r\n".encode())
if not args.send and not args.no_timestamps:
    # wham_hub.py: every line sent to this connection starts with [HH:MM:SS.mmm]. (net_broker.py does not know the
    # command and answers ERR 1; use --no-timestamps there.) One-shot mode stays bare so scripts can parse it.
    sock.sendall(b"!hub timestamps on\r\n")


def reader():
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            return
        if not data:
            print("\n--- broker closed ---")
            break
        sys.stdout.write(data.decode(errors="replace"))
        sys.stdout.flush()


threading.Thread(target=reader, daemon=True).start()

if args.send:
    time.sleep(0.3)
    for cmd in args.send:
        sock.sendall(cmd.encode() + b"\r\n")
        time.sleep(0.4)
    time.sleep(args.wait)
else:
    try:
        for line in sys.stdin:
            sock.sendall(line.rstrip("\r\n").encode() + b"\r\n")
    except (KeyboardInterrupt, BrokenPipeError):
        pass
sock.close()

#!/usr/bin/env python3
"""provision_board.py -- give a WHAM controller board its serial number, and keep the fleet registry.

Design: docs/device_identity_and_host_service.md (section 3). A board's serial is written ONCE into the identity
region of its external flash (SYS:SERial) and bound to its MCU and flash-chip unique IDs. This tool does the
whole procedure the same way every time:

    1. read *IDN? and SYS:ID? (what board is this? is it already provisioned?)
    2. pick the serial: the one the registry already holds for these hardware IDs (a board that was cleared or
       whose record was damaged), otherwise the next free WB-NNNN-C (C = a check digit that catches typos)
    3. show the plan and ask (unless --yes)
    4. SYS:SERial, optionally SYS:LABel, then read SYS:ID? back and require verified=1
    5. append the board to the registry, devices.csv (serial, uid, flashuid, product, hw, firmware, date, label)

    python3 host/provision_board.py --url socket://localhost:5002          # a board behind the bench broker / tunnel
    python3 host/provision_board.py --url /dev/cu.usbserial-1130 --label magnet.2
    python3 host/provision_board.py --url socket://localhost:5002 --check  # read only: compare board and registry
    python3 host/provision_board.py --list                                # show the registry

The board refuses a second SERial (ERR 19) until SYS:ID:CLEar CONFIRM; this tool never clears one. Run only
against a board you have identified; it changes the board's flash.
"""
import argparse
import csv
import datetime
import os
import re
import socket
import sys
import time

REGISTRY_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "devices.csv")
REGISTRY_FIELDS = ["serial", "uid", "flashuid", "product", "hw", "fw", "commit", "label", "provisioned_utc"]
SERIAL_RE = re.compile(r"^[A-Z0-9-]{3,16}$")


# ---- serial numbers -------------------------------------------------------------------------------------------
def luhn_digit(digits: str) -> str:
    """Luhn check digit for a string of decimal digits."""
    total = 0
    for i, ch in enumerate(reversed(digits)):
        d = int(ch)
        if i % 2 == 0:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return str((10 - total % 10) % 10)


def make_serial(number: int) -> str:
    body = f"{number:04d}"
    return f"WB-{body}-{luhn_digit(body)}"


def serial_ok(serial: str) -> bool:
    """A well-formed serial the firmware accepts; the WB-NNNN-C form also has to carry the right check digit."""
    if not SERIAL_RE.match(serial) or serial == "UNSET":
        return False
    m = re.match(r"^WB-(\d{4})-(\d)$", serial)
    return m is None or luhn_digit(m.group(1)) == m.group(2)


def serial_number(serial: str):
    m = re.match(r"^WB-(\d{4})-\d$", serial)
    return int(m.group(1)) if m else None


# ---- registry -------------------------------------------------------------------------------------------------
def load_registry(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def append_registry(path, row):
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=REGISTRY_FIELDS)
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in REGISTRY_FIELDS})


def next_serial(rows):
    used = [serial_number(r["serial"]) for r in rows if serial_number(r["serial"]) is not None]
    return make_serial(max(used, default=0) + 1)


def find_by_hardware(rows, uid, flashuid):
    return [r for r in rows if r["uid"] == uid and r["flashuid"] == flashuid]


# ---- the link -------------------------------------------------------------------------------------------------
class Link:
    """Line link to one board: socket://host:port (a TCP bridge or the bench broker) or a serial device."""

    def __init__(self, url):
        self.sock = None
        self.ser = None
        self.buf = b""
        if url.startswith("socket://"):
            host, port = url[len("socket://"):].rsplit(":", 1)
            self.sock = socket.create_connection((host, int(port)), timeout=5)
            self.sock.settimeout(0.2)
        else:
            import serial  # pyserial, only needed for a direct serial port
            self.ser = serial.Serial(url, 115200, timeout=0.2)
            time.sleep(1.5)

    def _read(self):
        if self.sock:
            try:
                return self.sock.recv(4096)
            except socket.timeout:
                return b""
        return self.ser.read(4096)

    def _write(self, data):
        (self.sock.sendall if self.sock else self.ser.write)(data)

    def drain(self, quiet=0.15):
        end = time.time() + quiet
        while time.time() < end:
            if self._read():
                end = time.time() + quiet
        self.buf = b""

    def query(self, cmd, timeout=4.0):
        """Send one command, return its OK/ERR line (other lines -- !EVT/!BOOT pushes, broker notes -- are skipped)."""
        self.drain()
        self._write((cmd + "\n").encode())
        end = time.time() + timeout
        while time.time() < end:
            self.buf += self._read()
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if text.startswith("OK") or text.startswith("ERR"):
                    return text
        raise TimeoutError(f"no reply to {cmd!r}")


def parse_id(reply):
    """'OK key=value key=value ...' -> dict."""
    if not reply.startswith("OK "):
        raise ValueError(f"unexpected reply: {reply!r}")
    return dict(tok.split("=", 1) for tok in reply[3:].split() if "=" in tok)


# ---- main -----------------------------------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", help="socket://host:port or a serial device")
    ap.add_argument("--registry", default=REGISTRY_DEFAULT)
    ap.add_argument("--serial", help="use this serial instead of the registry's / the next free one")
    ap.add_argument("--label", help="optional label (A-Z a-z 0-9 . _ -, up to 32)")
    ap.add_argument("--check", action="store_true", help="read only: compare the board with the registry")
    ap.add_argument("--list", action="store_true", help="print the registry and exit")
    ap.add_argument("--yes", action="store_true", help="do not ask before writing")
    ap.add_argument("--no-registry", action="store_true", help="do not record the board in the registry (a bench test)")
    args = ap.parse_args(argv)

    rows = load_registry(args.registry)
    if args.list:
        for r in rows:
            print(f"{r['serial']:<12} {r['product']:<24} {r['uid']}  {r['label'] or '-':<16} {r['provisioned_utc']}")
        print(f"{len(rows)} board(s) in {args.registry}")
        return 0
    if not args.url:
        ap.error("--url is required")

    link = Link(args.url)
    idn = link.query("*IDN?")
    print("board:    ", idn)
    if not idn.startswith("OK WHAM-"):
        print("not a WHAM controller; nothing was changed")
        return 2
    info = parse_id(link.query("SYS:ID?"))
    uid, fuid = info["uid"], info["flashuid"]
    print(f"hardware:  uid={uid} flashuid={fuid}")
    print(f"identity:  {info['identity']}  serial={info['serial']}  verified={info['verified']}")

    known = find_by_hardware(rows, uid, fuid)
    if info["identity"] == "ok":
        reg = [r for r in rows if r["serial"] == info["serial"]]
        print("registry:  " + ("this serial is registered" if reg else "this serial is NOT in the registry"))
        if reg and (reg[0]["uid"], reg[0]["flashuid"]) != (uid, fuid):
            print("WARNING: the registry has this serial for DIFFERENT hardware IDs")
            return 3
        if info["verified"] != "1":
            print("WARNING: the stored record was written for different silicon (verified=0)")
            return 3
        return 0
    if args.check:
        print("registry:  " + (f"these hardware IDs are registered as {known[0]['serial']}" if known else "unknown board"))
        return 0
    if info["identity"] == "nochip":
        print("the flash chip did not answer; nothing was changed")
        return 2

    if args.serial:
        serial = args.serial
    elif known:
        serial = known[0]["serial"]
    else:
        serial = next_serial(rows)
    if not serial_ok(serial):
        print(f"{serial!r} is not an acceptable serial (3-16 of A-Z 0-9 -; WB-NNNN-C needs its check digit)")
        return 2
    clash = [r for r in rows if r["serial"] == serial and (r["uid"], r["flashuid"]) != (uid, fuid)]
    if clash:
        print(f"{serial} is already registered to a different board ({clash[0]['uid']}); nothing was changed")
        return 3

    print(f"plan:      write serial {serial}" + (f" and label {args.label}" if args.label else "")
          + ("  (re-using the registry's serial for this board)" if known and not args.serial else ""))
    if not args.yes and input("write it to the board? [y/N] ").strip().lower() != "y":
        print("cancelled; nothing was changed")
        return 1

    reply = link.query(f"SYS:SERial {serial}")
    if reply != "OK":
        print("the board refused:", reply)
        return 2
    if args.label:
        reply = link.query(f"SYS:LABel {args.label}")
        if reply != "OK":
            print("the serial is written, but the label was refused:", reply)
    after = parse_id(link.query("SYS:ID?"))
    if after["serial"] != serial or after["verified"] != "1" or (after["uid"], after["flashuid"]) != (uid, fuid):
        print("READ-BACK MISMATCH:", after)
        return 2
    print(f"written:   serial={after['serial']} label={after['label']} verified={after['verified']}")

    if known and not args.serial and known[0]["serial"] == serial:
        print("registry:  already recorded (not added again)")
    elif not args.no_registry:
        append_registry(args.registry, {
            "serial": serial, "uid": uid, "flashuid": fuid, "product": after["product"], "hw": after["hw"],
            "fw": after["fw"], "commit": after["commit"], "label": "" if after["label"] == "-" else after["label"],
            "provisioned_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")})
        print(f"registry:  recorded in {args.registry}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""pull_and_plot.py -- read a finished shot's data back from an HBRIDGE (Limiter) controller through the host hub, save it as CSV
and plot it. READ-ONLY: it sends only queries (commands whose first word ends in "?"); it never arms, fires or changes a setting,
and refuses to send anything else. Run it AFTER the shot has ended (STATE? = OK IDLE). The data lives in the controller's RAM and is
lost at reset or when the next run starts, so pull it before arming again.

    python3 pull_and_plot.py log   --port 5102                 # closed-loop log (LOG:ARM ... LOG:DATA?): target, measured input, output
    python3 pull_and_plot.py pfmin --port 5102                 # PFM_Input period capture (PFMIN:CAPTURE ... PFMIN:DATA?): carrier frequency
    python3 pull_and_plot.py pfmin --port 5102 --inputs 1,2,3 --out my_shot
    python3 pull_and_plot.py duty  --port 5102 --inputs 1,2,3          # DUTY CYCLE of each switching cycle (inputs in both-edge mode)
    python3 pull_and_plot.py duty  --port 5102 --inputs 1 --programmed "1=0:20,5000:50,10000:80"   # overlay what was programmed

--port is the hub's TCP port for this controller (the ssh tunnel's local port; 5102 for the Limiter). Output goes to --out
(default: a new folder "pulled_<time>" here): <what>_<n>.csv, one plot <what>.png (log.png, pfmin.png, duty.png), and transcript.txt (every line sent and received).
Needs numpy and matplotlib. Every long reply carries a check sum (SUM=): a page whose numbers do not add up is read again, because the
serial bridge can lose characters.
"""
import argparse
import csv
import datetime
import os
import re
import socket
import sys
import time

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CLK_HZ = 170_000_000          # the capture timer's clock: PFMIN periods are in ticks of this
LOG_PAGE = 40                 # LOG:DATA? samples per reply (the firmware's own cap)
PFMIN_PAGE = 100              # PFMIN:DATA? values per reply (the bridge loses characters in longer ones)
TRIES = 25
LINE_END = re.compile(rb"\r\n|\r|\n")


class Hub:
    """A minimal, careful hub client (see docs/labview_hub_guide.md): "@quiet <name>" first, one command per line, one reply each."""

    def __init__(self, port, host="127.0.0.1", name="pull-and-plot", hub=True):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.2)
        self.buf = b""
        self.transcript = []
        if hub:
            self._send(f"@quiet {name}")                   # the hub's first-line client name; a bare bridge/simulator has no such line

    def _send(self, text):
        self.transcript.append((time.time(), ">", text))
        self.sock.sendall(text.encode() + b"\r\n")

    def _line(self, wait):
        end = time.time() + wait
        while time.time() < end:
            m = LINE_END.search(self.buf)
            if m and not (m.group() == b"\r" and m.end() == len(self.buf)):
                line, self.buf = self.buf[:m.start()].decode(errors="replace"), self.buf[m.end():]
                if line:
                    return line
                continue
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                raise ConnectionError("the hub closed the connection")
            self.buf += data
        return None

    def query(self, text, timeout=15.0):
        """One QUERY and its one reply line. Anything that is not a query is refused here, so this tool can never change anything."""
        if not text.split()[0].endswith("?"):
            raise ValueError(f"refusing to send {text!r}: this tool only sends queries")
        self._send(text)
        end = time.time() + timeout
        while time.time() < end:
            line = self._line(0.5)
            if line is None:
                continue
            self.transcript.append((time.time(), "<", line if len(line) < 400 else line[:400] + f" ...({len(line)} chars)"))
            if line.startswith("!"):
                continue                                   # an unsolicited event or boot banner is never a reply
            return line
        raise TimeoutError(f"no reply to {text!r} in {timeout:g} s")

    def save_transcript(self, path):
        with open(path, "w") as f:
            f.write("# > sent, < received (local time)\n")
            for t, d, text in self.transcript:
                f.write(f"{datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3]}  {d} {text}\n")


def checked(hub, command, parse):
    """Send `command`, parse the reply with parse(reply) -> result (raise ValueError if damaged), retry until it parses."""
    last = None
    for _ in range(TRIES):
        reply = hub.query(command)
        if reply.startswith("ERR"):
            raise RuntimeError(f"{command} -> {reply}")
        try:
            return parse(reply)
        except (ValueError, IndexError) as e:
            last = e
    raise RuntimeError(f"{command}: no undamaged reply in {TRIES} tries ({last})")


# ---------------------------------------------------------------------------------------------------------------- closed-loop log
def pull_log(hub, loop):
    """All samples of one loop: (rate_hz, target_mV, measured_mV, output_milli_percent) or None if that loop holds no data."""
    t, m, o, start, count, rate = [], [], [], 0, None, None
    while count is None or start < count:
        def parse(reply):
            p = reply.split()
            if p[0] != "OK" or not p[3].startswith("SUM="):
                raise ValueError("not a log page")
            n, r, want = int(p[1]), float(p[2]), int(p[3][4:])
            nums = [int(x) for x in p[4:]]
            expect = 3 * max(0, min(LOG_PAGE, n - start))
            if len(nums) != expect or sum(x & 0xFFFFFFFF for x in nums) & 0xFFFFFFFF != want:
                raise ValueError("check sum or length mismatch")
            return n, r, nums
        try:
            count, rate, nums = checked(hub, f"LOG:DATA? {loop} {start} {LOG_PAGE}", parse)
        except RuntimeError as e:
            if str(e).find("ERR") >= 0 and start == 0:
                return None                                # "that loop was not logged"
            raise
        if count == 0:
            return None
        t += nums[0::3]; m += nums[1::3]; o += nums[2::3]
        start += LOG_PAGE
    return rate, np.array(t), np.array(m), np.array(o)


def do_log(hub, args, out):
    fig, axes = plt.subplots(3, 1, figsize=(9, 8), sharex=True)
    got = 0
    for loop in args.loops:
        r = pull_log(hub, loop)
        if r is None:
            print(f"loop {loop} ({'UVW'[loop - 1]}): nothing logged")
            continue
        rate, tgt, mea, outp = r
        tt = np.arange(len(tgt)) / rate
        tgt_v, mea_v, out_pct = tgt / 1000.0, mea / 1000.0, outp / 1000.0       # mV -> V; milli-percent -> percent (Limiter)
        path = os.path.join(out, f"log_loop{loop}.csv")
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "target_V", "measured_V", "output_duty_pct"])
            w.writerows(zip(tt, tgt_v, mea_v, out_pct))
        err = mea_v - tgt_v
        print(f"loop {loop} ({'UVW'[loop - 1]}): {len(tt)} samples at {rate:g}/s ({tt[-1] * 1000:.1f} ms), error rms {np.sqrt(np.mean(err ** 2)):.3f} V, "
              f"max |error| {np.max(np.abs(err)):.3f} V  -> {path}")
        name = f"loop {loop} ({'UVW'[loop - 1]})"
        axes[0].plot(tt * 1000, tgt_v, "--", label=f"{name} target"); axes[0].plot(tt * 1000, mea_v, label=f"{name} measured")
        axes[1].plot(tt * 1000, err, label=name)
        axes[2].plot(tt * 1000, out_pct, label=name)
        got += 1
    if not got:
        print("No loop has logged data. Was LOG:ARM sent before ARM, for a closed-loop run, and has anything been run since?")
        return False
    axes[0].set_ylabel("process volts"); axes[1].set_ylabel("measured - target (V)"); axes[2].set_ylabel("output duty (%)")
    axes[2].set_xlabel("time since the run started (ms)")
    for a in axes:
        a.grid(alpha=0.3); a.legend(fontsize=8, ncol=2)
    axes[0].set_title("Closed-loop log: target, measured feedback input and output")
    fig.tight_layout(); fig.savefig(os.path.join(out, "log.png"), dpi=110)
    return True


# ---------------------------------------------------------------------------------------------------------------- PFM_Input capture
def pull_pfmin(hub, ch):
    """(periods_in_ticks array, overcap) of one input, or None when it captured nothing. Needs the input in RISING mode (the default)."""
    vals, total, overcap, start = [], None, 0, 0
    while total is None or start < total:
        def parse(reply):
            p = reply.split()
            n, oc, want = int(p[1]), int(p[2].split("=")[1]), int(p[3].split("=")[1])
            nums = [int(x) for x in p[4:]]
            if len(nums) != min(PFMIN_PAGE, n - start) or sum(nums) & 0xFFFFFFFF != want:
                raise ValueError("check sum or length mismatch")
            return n, oc, nums
        total, overcap, nums = checked(hub, f"PFMIN:DATA? {ch} {start} {PFMIN_PAGE}", parse)
        if total == 0:
            return None
        vals += nums
        start += PFMIN_PAGE
    return np.array(vals, dtype=np.int64), overcap


def do_pfmin(hub, args, out):
    counts = [int(x) for x in hub.query("PFMIN:STATus?").split()[1:]]
    print("PFMIN:STATus? (periods captured per input 1..6):", counts)
    fig, ax = plt.subplots(figsize=(9, 5))
    got = 0
    for ch in args.inputs:
        if ch > len(counts) or counts[ch - 1] == 0:
            print(f"input {ch}: nothing captured (not wired, not armed with PFMIN:CAPTURE before ARM, or the shot never ran)")
            continue
        try:
            r = pull_pfmin(hub, ch)
        except RuntimeError as e:
            print(f"input {ch}: {e}" + ("   (a both-edge input: use the 'duty' mode of this tool)" if "BOTH" in str(e) else ""))
            continue
        if r is None:
            continue
        ticks, overcap = r
        t_s = np.cumsum(ticks) / CLK_HZ
        freq = CLK_HZ / ticks
        path = os.path.join(out, f"pfmin_input{ch}.csv")
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["period_index", "t_end_s", "period_ticks", "frequency_Hz"])
            w.writerows(zip(range(len(ticks)), t_s, ticks, freq))
        print(f"input {ch}: {len(ticks)} periods over {t_s[-1] * 1000:.2f} ms, carrier {freq.mean() / 1000:.3f} kHz "
              f"(min {freq.min() / 1000:.3f}, max {freq.max() / 1000:.3f}), OVERCAP={overcap}  -> {path}")
        ax.plot(t_s * 1000, freq / 1000, label=f"input {ch}" + (f" (OVERCAP {overcap})" if overcap else ""))
        got += 1
    if not got:
        return False
    ax.set_xlabel("time since the first captured period (ms)"); ax.set_ylabel("carrier frequency (kHz)")
    ax.set_title("PFM_Input capture: measured carrier frequency, one point per switching period"); ax.grid(alpha=0.3); ax.legend()
    fig.tight_layout(); fig.savefig(os.path.join(out, "pfmin.png"), dpi=110)
    return True


# ---------------------------------------------------------------------------------------------------------------- duty from both edges
def pull_raw(hub, ch):
    """The both-edge capture of one input: (edge_count, overcap, level_before, [t0, d1, d2, ...]) or None when it holds nothing.
    PFMIN:RAW? <ch> <start> <count> -> OK <n> OVERCAP=<k> LEVEL=<0|1> SUM=<s> <t0> <d1> ...: t0 is the first edge's raw timer value and
    each later number the tick difference from the previous edge; LEVEL is the pin level just before the capture, so the first edge
    is the opposite transition and edges alternate."""
    values, n, overcap, level = [], None, 0, 0
    while n is None or len(values) < n:
        start = len(values)

        def parse(reply):
            p = reply.split()
            n_, oc, lv, want = int(p[1]), int(p[2].split("=")[1]), int(p[3].split("=")[1]), int(p[4].split("=")[1])
            nums = [int(x) for x in p[5:]]
            if len(nums) != min(PFMIN_PAGE, n_ - start) or sum(nums) & 0xFFFFFFFF != want:
                raise ValueError("check sum or length mismatch")
            return n_, oc, lv, nums
        n, overcap, level, nums = checked(hub, f"PFMIN:RAW? {ch} {start} {PFMIN_PAGE}", parse)
        if n == 0:
            return None
        values += nums
    return n, overcap, level, values


def decode_cycles(level, values):
    """Every complete switching cycle (rising edge, falling edge, next rising edge) as (rising time s, period s, high time s)."""
    t = np.concatenate([[0], np.cumsum(np.array(values[1:], dtype=np.int64))]) / CLK_HZ
    first_is_rising = (level == 0)
    rising = np.array([(i % 2 == 0) == first_is_rising for i in range(len(t))])
    r = np.where(rising)[0]
    cycles = []
    for a, b in zip(r[:-1], r[1:]):
        fall = [i for i in range(a + 1, b) if not rising[i]]
        if len(fall) == 1:                                   # exactly one falling edge between two rising edges
            cycles.append((t[a], t[b] - t[a], t[fall[0]] - t[a]))
    return np.array(cycles)


def parse_programmed(items):
    """--programmed "1=0:20,5000:50,10000:80" (input = time_us:duty_pct, ...) -> {1: (times_s, duties)}"""
    out = {}
    for item in items or []:
        ch, _, pts = item.partition("=")
        pairs = [p.split(":") for p in pts.split(",") if p]
        out[int(ch)] = (np.array([float(a) for a, _ in pairs]) * 1e-6, np.array([float(b) for _, b in pairs]))
    return out


def do_duty(hub, args, out):
    modes = hub.query("PFMIN:EDGes?").split()[1:]
    print("PFMIN:EDGes? (R = rising only, B = both edges; inputs 1..6):", " ".join(modes))
    programmed = parse_programmed(args.programmed)
    fig, axes = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    got = 0
    for ch in args.inputs:
        if ch > len(modes) or modes[ch - 1] != "B":
            print(f"input {ch}: not in both-edge mode, so it cannot give a duty. Before ARM send:  PFMIN:EDGes {ch} BOTH   then PFMIN:CAPTURE <M>")
            continue
        try:
            r = pull_raw(hub, ch)
        except RuntimeError as e:
            print(f"input {ch}: {e}")
            continue
        if r is None:
            print(f"input {ch}: nothing captured (not wired, PFMIN:CAPTURE not sent before ARM, or the shot never ran)")
            continue
        n, overcap, level, values = r
        c = decode_cycles(level, values)
        if len(c) < 3:
            print(f"input {ch}: only {len(c)} complete cycles in {n} edges")
            continue
        t0 = c[0, 0]
        t_s, period, high = c[:, 0] - t0, c[:, 1], c[:, 2]
        duty, freq = 100.0 * high / period, 1.0 / period
        med = float(np.median(period))
        flag = np.array([""] * len(c), dtype=object)
        flag[np.abs(period - med) > 0.25 * med] = "odd period"      # a merged or split cycle: an edge missing from the record
        flag[0] = "startup"                                         # the capture starts just before the output does
        good = flag == ""
        path = os.path.join(out, f"duty_input{ch}.csv")
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["cycle", "t_s", "period_us", "high_us", "duty_pct", "frequency_Hz", "flag"])
            for i in range(len(c)):
                w.writerow([i, f"{t_s[i]:.9f}", f"{period[i] * 1e6:.4f}", f"{high[i] * 1e6:.4f}", f"{duty[i]:.3f}", f"{freq[i]:.1f}", flag[i]])
        line = (f"input {ch}: {len(c)} cycles ({int((~good).sum())} flagged) over {t_s[-1] * 1e3:.2f} ms, duty {duty[good].min():.2f} to {duty[good].max():.2f} %, "
                f"mean carrier {freq[good].mean() / 1e3:.3f} kHz, OVERCAP={overcap}")
        label = f"input {ch}"
        if ch in programmed:
            want = np.interp(t_s, *programmed[ch])
            err = (duty - want)[good]
            line += f"; measured - programmed: mean {err.mean():+.2f}, std {err.std():.2f} percentage points"
            axes[0].plot(t_s * 1e3, want, "-", lw=2, alpha=0.4, label=f"{label} programmed")
        print(line + f"  -> {path}")
        axes[0].plot(t_s[good] * 1e3, duty[good], ".", ms=3, label=f"{label} measured")
        axes[0].plot(t_s[~good] * 1e3, duty[~good], "x", ms=6, color="k")
        axes[1].plot(t_s[good] * 1e3, freq[good] / 1e3, ".", ms=3, label=label)
        got += 1
    if not got:
        return False
    axes[0].set_ylabel("duty (%)"); axes[0].set_ylim(0, 100); axes[1].set_ylabel("carrier (kHz)"); axes[1].set_xlabel("time since the first cycle (ms)")
    axes[0].set_title("PFM_Input both-edge capture: duty of every switching cycle (x = flagged: start-up or a missing edge)")
    for a in axes:
        a.grid(alpha=0.3); a.legend(fontsize=8, ncol=2)
    fig.tight_layout(); fig.savefig(os.path.join(out, "duty.png"), dpi=110)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["log", "pfmin", "duty"],
                    help="log = the closed-loop log (LOG:DATA?); pfmin = the PFM_Input period capture, rising edges only (PFMIN:DATA?); "
                         "duty = the duty of every cycle from a both-edge capture (PFMIN:RAW?)")
    ap.add_argument("--port", type=int, required=True, help="the hub's TCP port for this controller (through the ssh tunnel: 5102 for the Limiter)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--no-hub", action="store_true", help="the port is a controller's bridge or a simulator, not a hub: do not send the '@quiet' line")
    ap.add_argument("--loops", default="1,2,3", help="log: which loops (phases U,V,W = 1,2,3) to read (default all three)")
    ap.add_argument("--inputs", default="1,2,3", help="pfmin: which PFM_Input channels to read (default 1,2,3: phases U,V,W on the bench)")
    ap.add_argument("--programmed", action="append", metavar="IN=T:D,T:D",
                    help='duty: overlay the programmed duty of an input, "1=0:20,5000:50,10000:80" (time_us:duty_pct setpoints, straight lines between); repeatable')
    ap.add_argument("--out", default=None, help="folder for the files (default: pulled_<time> next to this script)")
    args = ap.parse_args()
    args.loops = [int(x) for x in args.loops.split(",") if x]
    args.inputs = [int(x) for x in args.inputs.split(",") if x]
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "pulled_" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    hub = Hub(args.port, args.host, hub=not args.no_hub)
    try:
        ident = hub.query("*IDN?")
        print(ident)
        if "HBRIDGE" not in ident:
            sys.exit("This is not an HBRIDGE controller: stopping.")
        state = hub.query("STATE?")
        print(state)
        if state.strip() != "OK IDLE":
            print("Note: the controller is not IDLE; the data of a run still in progress is not complete.")
        ok = {"log": do_log, "pfmin": do_pfmin, "duty": do_duty}[args.what](hub, args, out)
    finally:
        hub.save_transcript(os.path.join(out, "transcript.txt"))
        hub.sock.close()
    print(f"files in {out}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

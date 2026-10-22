#!/usr/bin/env python3
"""run_example.py -- the reference example for operators: program and fire ONE open-loop shot on the HBRIDGE controller in
LIMITER mode, with a DIFFERENT duty-cycle profile on each of the three phases, through the host hub; measure what the
outputs really did; plot it; and keep a transcript of every line sent to and received from the controller.

It follows docs/operators_guide.md sections 5.3 and 6 step by step, and is written the way a LabVIEW program should talk to the
hub (docs/labview_hub_guide.md): connect over TCP, send "@quiet <name>" first, one command per line ending CR LF, one reply per
command in order, "!" lines are unsolicited and are never a reply.

    python3 run_example.py --port 5102                 # the hub's TCP port for this controller (here, via an ssh tunnel)
    python3 run_example.py --port 5102 --out some/dir  # write the files somewhere else
    python3 run_example.py --reanalyse                 # decode raw_edges.json again and redraw the plot; the controller is not touched

Everything it writes goes to --out (default: the folder this script is in):
    transcript.txt        every line to and from the hub, with a time stamp
    transcript.jsonl      the same, one JSON record per line
    shot_setpoints.csv    the shot as programmed
    raw_edges.json        the raw edge capture of the three phase outputs, as read back
    measured_duty.csv     the duty and period of every switching cycle of every phase, decoded from the raw edges
    summary.json          the numbers quoted in README.md
    shot_plot.png         the plot

WHAT IT NEEDS: an idle HBRIDGE controller whose phase outputs U, V, W are fibred to PFM_Input_1, _2, _3 (the bench wiring),
and the hub reachable on --port. Requires numpy and matplotlib. It sends ARM and FIRE: run it only on a controller you are
allowed to fire (the bench simulator board, here).
"""
import argparse
import csv
import datetime
import json
import os
import re
import socket
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "..", "python")))
import pfm_shot_test as T            # the exact Python port of the firmware's shot generator (held to the C code by tests)

CLK_HZ = 170_000_000                 # the capture timer's clock: edge times are in ticks of this
PHASES = ("U", "V", "W")
LINE_END = re.compile(rb"\r\n|\r|\n")
CAPTURE_INPUT = {"U": 1, "V": 2, "W": 3}          # PFM_Input_1..3 are fibred to phases U, V, W on this bench

# --------------------------------------------------------------------------------------------------------------------
# THE SHOT.  Limiter mode: carrier 75-125 kHz, duty 5-95 % per phase.  Three setpoints, 10 ms long, 100 kHz throughout.
# One duty per phase at each setpoint:   [U, V, W]  in percent.
#   U ramps up   20 -> 50 -> 80 %
#   V ramps down 80 -> 50 -> 20 %
#   W rises then falls  50 -> 90 -> 50 %
# --------------------------------------------------------------------------------------------------------------------
CARRIER_HZ = 100_000
SETPOINTS = [            # (time in microseconds, carrier in hertz, [duty U, V, W in percent])
    (0,      CARRIER_HZ, [20.0, 80.0, 50.0]),
    (5_000,  CARRIER_HZ, [50.0, 50.0, 90.0]),
    (10_000, CARRIER_HZ, [80.0, 20.0, 50.0]),
]
CLIENT_NAME = "operator-example"


# --------------------------------------------------------------------------------------------------------------------
# A small, careful hub client: the pattern a LabVIEW program should follow
# --------------------------------------------------------------------------------------------------------------------
class Hub:
    def __init__(self, port, name, host="127.0.0.1"):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.2)
        self.buf = b""
        self.records = []                       # (epoch seconds, "TX"|"RX"|"EVT"|"HUB", text)
        self.send_raw(f"@quiet {name}")         # FIRST LINE: quiet mode. No reply is sent to it.

    def _log(self, kind, text):
        self.records.append((time.time(), kind, text))

    def send_raw(self, text):
        self._log("TX", text)
        self.sock.sendall(text.encode() + b"\r\n")

    def _lines(self, wait):
        """The complete lines that are available within `wait` seconds (a line ends at CR LF, CR or LF); [] if none."""
        end = time.time() + wait
        while True:
            lines = []
            while True:
                m = LINE_END.search(self.buf)
                if m is None or (m.group() == b"\r" and m.end() == len(self.buf)):   # a lone CR at the very end may be half a CR LF
                    break
                lines.append(self.buf[:m.start()].decode(errors="replace"))
                self.buf = self.buf[m.end():]
            if lines:
                return lines
            if time.time() >= end:
                return []
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                raise ConnectionError("the hub closed the connection")
            self.buf += data

    def cmd(self, text, timeout=10.0):
        """Send one command and return its ONE reply line. Unsolicited '!' lines (events, boot banners) are recorded and
        skipped; they are never a reply. '#' lines belong to '!hub' commands."""
        self.send_raw(text)
        end = time.time() + timeout
        while time.time() < end:
            for line in self._lines(0.5):
                if not line:
                    continue
                if line.startswith("!"):
                    self._log("EVT", line)
                    continue
                self._log("RX", line)
                return line
        raise TimeoutError(f"no reply to {text!r} in {timeout} s")

    def hub_cmd(self, text, quiet_for=0.6):
        """A '!hub ...' command: the hub answers with one or more '#' lines."""
        self.send_raw(text)
        out = []
        end = time.time() + 5.0
        while time.time() < end:
            lines = self._lines(quiet_for)
            if not lines and out:
                break
            for line in lines:
                self._log("HUB" if line.startswith("#") else "EVT", line)
                if line.startswith("#"):
                    out.append(line)
        return out

    def must(self, text, expect="OK", timeout=10.0):
        r = self.cmd(text, timeout)
        if not r.startswith(expect):
            raise RuntimeError(f"{text!r} -> {r!r} (expected {expect!r}...)")
        return r

    def close(self):
        self.sock.close()


def stamp(t):
    return datetime.datetime.fromtimestamp(t).strftime("%H:%M:%S.%f")[:-3]


def write_transcript(hub, out_dir):
    arrows = {"TX": ">", "RX": "<", "EVT": "<!", "HUB": "<#"}
    with open(os.path.join(out_dir, "transcript.txt"), "w") as f:
        f.write("# Every line sent to (>) and received from (<) the controller through the hub, with the local time.\n")
        f.write("# '<!' = an unsolicited line from the controller; '<#' = the hub answering a '!hub' command.\n")
        f.write(f"# Date {datetime.date.fromtimestamp(hub.records[0][0]).isoformat()}\n")
        for t, kind, text in hub.records:
            f.write(f"{stamp(t)}  {arrows[kind]:<2} {text if len(text) < 400 else text[:400] + ' ...(' + str(len(text)) + ' chars)'}\n")
    with open(os.path.join(out_dir, "transcript.jsonl"), "w") as f:
        for t, kind, text in hub.records:
            f.write(json.dumps({"t": round(t, 3), "time": stamp(t), "dir": kind, "line": text}) + "\n")


# --------------------------------------------------------------------------------------------------------------------
# Reading the raw edge capture back (PFMIN:RAW? pages, each checked against the SUM the controller sends)
# --------------------------------------------------------------------------------------------------------------------
def read_raw(hub, ch, page=100):
    """Return (n_edges, overcapture, level, [t0, d1, d2, ...]). A page is used only when its numbers add up to the SUM in
    its reply: the serial bridge can lose characters in long replies, so a damaged page is simply read again."""
    values, n, ov, lvl = [], None, None, None
    while n is None or len(values) < n:
        start = len(values)
        for _ in range(25):
            r = hub.cmd(f"PFMIN:RAW? {ch} {start} {page}", timeout=40.0)
            try:
                t = r.split()
                n_, ov_, lvl_ = int(t[1]), t[2], int(t[3].split("=")[1])
                want = int(t[4].split("=")[1])
                vals = [int(x) for x in t[5:]]
            except (IndexError, ValueError):
                continue
            if len(vals) == min(page, n_ - start) and sum(vals) % 2 ** 32 == want:
                n, ov, lvl = n_, ov_, lvl_
                values += vals
                break
        else:
            raise RuntimeError(f"PFMIN:RAW? {ch} {start}: no reply with a matching SUM in 25 reads")
    return n, ov, lvl, values


def decode_edges(n, level, values):
    """Edge times (seconds, relative to the first edge) and the rising/falling flags of one phase's capture.
    values = [t0, d1, d2, ...]: the first edge's raw timer value, then the tick difference to each next edge. LEVEL is the
    pin level before the capture, so the first edge is the opposite transition and edges alternate."""
    ticks = np.cumsum(np.array(values[1:], dtype=np.int64))           # edge i (i >= 1) is d1+...+di after edge 0
    t = np.concatenate([[0], ticks]) / CLK_HZ
    first_is_rising = (level == 0)
    rising = np.array([(i % 2 == 0) == first_is_rising for i in range(len(t))])
    return t, rising


def duty_per_cycle(t, rising):
    """For every complete cycle (rising edge, falling edge, next rising edge): (rising time, period, high time)."""
    r = np.where(rising)[0]
    cycles = []
    for a, b in zip(r[:-1], r[1:]):
        fall = [i for i in range(a + 1, b) if not rising[i]]
        if len(fall) == 1:
            cycles.append((t[a], t[b] - t[a], t[fall[0]] - t[a]))
    return np.array(cycles)


def programmed_duty_at(setpoints, ph, t_s):
    """The shot's nominal duty of phase `ph` (index 0-2) at times t_s (seconds): straight lines between setpoints."""
    ts = np.array([s[0] for s in setpoints]) * 1e-6
    ds = np.array([s[2][ph] for s in setpoints])
    return np.interp(t_s, ts, ds)


def analyse_and_plot(raw, out_dir, duration_us):
    """Decode the raw edge captures, write measured_duty.csv and summary.json, and draw shot_plot.png."""
    cycles = {}
    for ph in PHASES:
        t, rising = decode_edges(raw[ph]["edges"], raw[ph]["level_before"], raw[ph]["values"])
        cycles[ph] = duty_per_cycle(t, rising)
    t_zero = cycles["U"][0][0]                                         # the shot's time zero: phase U's first rising edge
    period0 = float(np.median(cycles["U"][:, 1]))
    rows, stats, data = [], {}, {}
    for k, ph in enumerate(PHASES):
        c = cycles[ph]
        t_shot = c[:, 0] - t_zero - k * period0 / 3.0                  # phase k is locked k/3 of a period after phase U
        duty = 100.0 * c[:, 2] / c[:, 1]
        freq = 1.0 / c[:, 1]
        want = programmed_duty_at(SETPOINTS, k, t_shot)
        # Flag the cycles that are not a normal switching cycle: the first one (the start-up period, which is not the
        # programmed period) and any whose period is far from the median (a missing or extra edge in the capture).
        med = float(np.median(c[:, 1]))
        flag = np.array(["" for _ in range(len(c))], dtype=object)
        flag[0] = "startup"
        flag[np.abs(c[:, 1] - med) > 0.25 * med] = "odd period"
        flag[0] = "startup"
        good = flag == ""
        err = (duty - want)[good]
        stats[ph] = {"cycles": int(len(c)), "cycles_flagged": int((~good).sum()),
                     "flagged_cycles": [{"cycle": int(i), "time_us": round(float(t_shot[i] * 1e6), 1),
                                         "period_us": round(float(c[i, 1] * 1e6), 3), "why": str(flag[i])}
                                        for i in np.where(~good)[0]],
                     "mean_error_pct_points": round(float(err.mean()), 2),
                     "std_error_pct_points": round(float(err.std()), 2),
                     "max_abs_deviation_from_mean_pct_points": round(float(np.abs(err - err.mean()).max()), 2),
                     "mean_carrier_hz": round(float(freq[good].mean()), 1)}
        data[ph] = (t_shot, duty, want, flag)
        for i in range(len(c)):
            rows.append([ph, i, round(t_shot[i] * 1e6, 3), round(c[i, 1] * 1e6, 4), round(c[i, 2] * 1e6, 4), round(duty[i], 3),
                         round(freq[i], 1), round(float(want[i]), 3), flag[i]])
    with open(os.path.join(out_dir, "measured_duty.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["phase", "cycle", "time_us", "period_us", "high_us", "measured_duty_pct", "measured_carrier_hz",
                    "programmed_duty_pct", "flag"])
        w.writerows(rows)
    summary = {"shot": {"mode": "LIMITER", "carrier_hz": CARRIER_HZ, "duration_us": duration_us, "setpoints": len(SETPOINTS),
                        "duties_pct_per_phase": {ph: [s[2][k] for s in SETPOINTS] for k, ph in enumerate(PHASES)}},
               "measurement": stats}
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(stats, indent=2))

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    colors = {"U": "#1f77b4", "V": "#d62728", "W": "#2ca02c"}
    fig, axes = plt.subplots(4, 1, figsize=(10, 12), sharex=True, gridspec_kw={"height_ratios": [2.2, 1, 1, 1]})
    ax = axes[0]
    tt = np.linspace(0, duration_us * 1e-6, 400)
    for k, ph in enumerate(PHASES):
        t_shot, duty, want, flag = data[ph]
        good = flag == ""
        ax.plot(tt * 1e3, programmed_duty_at(SETPOINTS, k, tt), "-", color=colors[ph], lw=2, alpha=0.45)
        ax.plot(t_shot[good] * 1e3, duty[good], ".", ms=2.5, color=colors[ph], label=f"phase {ph}")
        ax.plot(t_shot[~good] * 1e3, duty[~good], "x", ms=6, color="k")
    ax.plot([], [], "-", color="gray", lw=2, alpha=0.45, label="programmed (straight lines between setpoints)")
    ax.plot([], [], ".", color="gray", ms=5, label="measured, one point per switching cycle")
    ax.plot([], [], "x", color="k", ms=6, label="flagged cycle (start-up, or a missing edge)")
    ax.set_ylabel("duty (%)")
    ax.set_ylim(0, 100)
    ax.set_title("HBRIDGE, Limiter mode: one shot, a different duty profile on each phase  (100 kHz carrier, 10 ms)")
    ax.grid(alpha=0.3)
    ax.legend(loc="lower center", ncol=2, fontsize=8)
    for k, ph in enumerate(PHASES):
        ax = axes[1 + k]
        t_shot, duty, want, flag = data[ph]
        good = flag == ""
        ax.plot(t_shot[good] * 1e3, (duty - want)[good], ".", ms=2.5, color=colors[ph])
        ax.axhline(stats[ph]["mean_error_pct_points"], color="k", lw=0.8, ls="--")
        ax.set_ylabel(f"{ph}: measured\n- programmed\n(% points)")
        ax.set_ylim(-6, 3)
        ax.grid(alpha=0.3)
        ax.text(0.99, 0.08, f"mean {stats[ph]['mean_error_pct_points']:+.2f}   spread (std) {stats[ph]['std_error_pct_points']:.2f}   "
                f"carrier {stats[ph]['mean_carrier_hz'] / 1e3:.2f} kHz   flagged cycles {stats[ph]['cycles_flagged']}",
                transform=ax.transAxes, ha="right", fontsize=8)
    axes[-1].set_xlabel("time since the start of the shot (ms)")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "shot_plot.png"), dpi=130)
    print("wrote", os.path.join(out_dir, "shot_plot.png"))


def save_raw(raw, setpoints, out_dir):
    with open(os.path.join(out_dir, "raw_edges.json"), "w") as f:
        json.dump({"clock_hz": CLK_HZ, "note": "values = [t0, d1, d2, ...]: first edge's raw timer value, then ticks to each next edge",
                   "phases": raw}, f)
    with open(os.path.join(out_dir, "shot_setpoints.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["time_us", "carrier_hz", "duty_U_pct", "duty_V_pct", "duty_W_pct"])
        for t_us, f_hz, d in setpoints:
            w.writerow([t_us, f_hz, *d])


# --------------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, help="the hub's TCP port for this controller")
    ap.add_argument("--reanalyse", action="store_true",
                    help="do not touch the controller: decode raw_edges.json in --out again and redraw the plot")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--out", default=HERE)
    ap.add_argument("--name", default=CLIENT_NAME)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.reanalyse:
        raw = json.load(open(os.path.join(a.out, "raw_edges.json")))["phases"]
        analyse_and_plot(raw, a.out, SETPOINTS[-1][0])
        return
    if a.port is None:
        ap.error("--port is required (unless --reanalyse)")

    hub = Hub(a.port, a.name, a.host)
    failed = None
    summary = {}
    raw = {}
    try:
        # ---- 1. connect and check we have the right controller, in the right state (operators_guide 5.3, 6.1) ----------
        print("== connect and check")
        idn = hub.must("*IDN?")
        print(idn)
        assert idn.split()[1] == "WHAM-HBRIDGE-PFMG474", f"wrong controller: {idn}"
        print(hub.must("SYS:ID?"))
        for line in hub.hub_cmd("!hub status"):
            print(line[:200])
        for line in hub.hub_cmd("!hub clients"):
            print(line)
        state = hub.must("STATE?")
        assert state == "OK IDLE", f"controller is not idle: {state}"
        assert hub.must("FAULT?") == "OK 0", "a fault is latched"

        # ---- 2. supply mode and phases (6.2) ---------------------------------------------------------------------
        print("== supply mode and phases")
        hub.must("CONFig:MODE LIMiter")
        print(hub.must("CONFig:MODE?"), "|", hub.must("CONFig:LIMits?"))
        hub.must("PHASe:SELect UVW")
        print(hub.must("PHASe:ENAble?"))

        # ---- 3. enter the shot, one setpoint at a time, checking every reply (6.3) -------------------------------
        print("== enter the shot")
        hub.must("SHOT:BEGin")
        for t_us, f_hz, duties in SETPOINTS:
            hub.must(f"SHOT:STEP {t_us} {f_hz} {duties[0]:.2f} {duties[1]:.2f} {duties[2]:.2f}")
        end = hub.must("SHOT:END")
        print(end)
        n_setpoints, duration_us = int(end.split()[1]), int(end.split()[2])
        print(hub.must("SHOT?"))
        for i in range(len(SETPOINTS)):            # read every setpoint back: the controller's own copy is the proof
            print(f"setpoint {i}:", hub.must(f"SHOT:POINt? {i}"))

        # ---- 4. arm the measurement: capture both edges of the three phase outputs ---------------------------------
        print("== arm the edge capture")
        for ph in PHASES:
            hub.must(f"PFMIN:EDGes {CAPTURE_INPUT[ph]} BOTH")
        print(hub.must("PFMIN:EDGes?"))
        periods = int(duration_us * 1e-6 * CARRIER_HZ) + 5
        hub.must(f"PFMIN:CAPTURE {periods}")

        # ---- 5. arm and fire (6.5) ---------------------------------------------------------------------------------
        print("== ARM and FIRE")
        hub.must("ARM")
        assert hub.must("STATE?") == "OK ARMED"
        hub.must("FIRE")
        t_fire = time.time()
        for _ in range(100):                        # the shot is 10 ms long: poll STATE? until it is IDLE again
            time.sleep(0.05)
            state = hub.must("STATE?")
            if state != "OK FIRING":
                break
        print(f"state after the shot: {state}  ({time.time() - t_fire:.2f} s after FIRE)")
        if state != "OK IDLE":
            failed = f"the controller did not return to IDLE: {state}"
            print("FAULT:SOURces? ->", hub.must("FAULT:SOURces?"))
            print("GDS:LAST?      ->", hub.must("GDS:LAST?"))
            raise RuntimeError(failed)
        print("FAULT? ->", hub.must("FAULT?"), "| FAULT:SOURces? ->", hub.must("FAULT:SOURces?"))
        print("PFM:DIAG? ->", hub.must("PFM:DIAG?"))
        print("PFMIN:STATus? ->", hub.must("PFMIN:STATus?"))

        # ---- 6. read the capture back (SUM-checked pages) -------------------------------------------------------------
        print("== read back the edge capture")
        for ph in PHASES:
            n, ov, lvl, vals = read_raw(hub, CAPTURE_INPUT[ph])
            raw[ph] = {"input": CAPTURE_INPUT[ph], "edges": n, "overcapture": ov, "level_before": lvl, "values": vals}
            print(f"phase {ph}: {n} edges, {ov}, level before capture {lvl}")

        # ---- 7. put the capture back as we found it ------------------------------------------------------------------
        for ph in PHASES:
            hub.must(f"PFMIN:EDGes {CAPTURE_INPUT[ph]} RISING")
        print("PFMIN:EDGes? ->", hub.must("PFMIN:EDGes?"))
        print("STATE? ->", hub.must("STATE?"), "| FAULT? ->", hub.must("FAULT?"))
    except Exception as exc:                        # keep what was gathered: the transcript is the evidence
        failed = failed or f"{type(exc).__name__}: {exc}"
        print("FAILED:", failed)
    finally:
        write_transcript(hub, a.out)
        hub.close()

    if failed:
        print("transcript written; no plot because the run failed:", failed)
        sys.exit(1)

    # ---- 8. save the raw data, decode, plot ------------------------------------------------------------------------------
    print("== decode and plot")
    save_raw(raw, SETPOINTS, a.out)
    analyse_and_plot(raw, a.out, duration_us)


if __name__ == "__main__":
    main()

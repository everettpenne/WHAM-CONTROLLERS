#!/usr/bin/env python3
"""pfm_profile_demo.py -- fire a 50 ms demonstration shot and plot what the three
phases (U, V, W on PFM_Input_1..3) actually did, from the PFM_Input capture.

  hvps     HVPS mode, duty fixed at 50 %, frequency a TRIANGLE WAVE between 10 and
           30 kHz: up, down, up, down ... (4 cycles in 50 ms)
  limiter  LIMITER mode, frequency fixed at 75 kHz, duty a TRIANGLE WAVE between
           5 and 95 %: up, down, up, down ... (4 cycles in 50 ms)

The shot is two or more SHOT:STEP setpoints per ramp (linear between them, the
firmware generates every switching period); the capture is taken on BOTH edges of
inputs 1-3 so the real waveform is reconstructed, and PFMIN:RAW? is read back a
page at a time with its checksum (the ethernet serial bridge loses characters).

    python3 python/pfm_profile_demo.py hvps    --url socket://localhost:5002 --out hvps.png --json hvps.json
    python3 python/pfm_profile_demo.py limiter --url socket://localhost:5002 --out limiter.png
    python3 python/pfm_profile_demo.py hvps --from-json hvps.json --out hvps.png   # replot, no hardware

FIRE drives the real outputs; the board must be IDLE.

Timebases: U and V (PFM_Input_1, 2) are captured by one timer and share a time
axis; W (PFM_Input_3) is on another timer, so its phase relative to U and V is
NOT measured -- it is drawn from its own start.
"""
import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pfm_shot_test as T          # Link, generate_profile (exact port of the firmware generator)

CLK = T.CLK_HZ
SHOT_MS = 50.0
CYCLES = 4
PHASES = [("U", 1, "#1f77b4"), ("V", 2, "#2ca02c"), ("W", 3, "#d62728")]
PAGE = 100


def make_setpoints(kind):
    """[(t_us, freq_mHz, duty_hundredths)] for the demonstration profile."""
    half_us = int(SHOT_MS * 1000 / (2 * CYCLES))
    pts = []
    for i in range(2 * CYCLES + 1):
        t = i * half_us
        hi = (i % 2 == 1)
        if kind == "hvps":
            pts.append((t, 30_000_000 if hi else 10_000_000, 5000))
        else:
            pts.append((t, 75_000_000, 9500 if hi else 500))
    return pts


# --------------------------------------------------------------------------
# hardware
# --------------------------------------------------------------------------
def read_pages(link, ch):
    """The whole PFMIN:RAW? stream of input `ch`, a page at a time, each checked
    against its own SUM. Returns (count, level, deltas)."""
    vals, count, level = [], None, 0
    while count is None or len(vals) < count:
        start = len(vals)
        for _ in range(40):
            r = link.cmd(f"PFMIN:RAW? {ch} {start} {PAGE}", timeout=6.0)
            try:
                t = r.split()
                cnt = int(t[1]); level = int(t[3].split("=")[1]); want = int(t[4].split("=")[1])
                page = [int(x) for x in t[5:]]
            except (AttributeError, IndexError, ValueError):
                continue
            if len(page) == min(PAGE, cnt - start) and sum(page) % 2**32 == want:
                count = cnt
                vals += page
                break
        else:
            sys.exit(f"[fail] input {ch}: no clean page at offset {start}")
    return count, level, vals


def run(kind, url, name):
    mode = "HVPS" if kind == "hvps" else "LIMITER"
    sp = make_setpoints(kind)
    per, cmp_ = T.generate_profile(sp, mode)
    if len(per) > T.MAX_PERIODS:
        sys.exit(f"[fail] {len(per)} periods do not fit the capture ({T.MAX_PERIODS})")
    link = T.Link(url, name)
    print("board: ", link.must("*IDN?"))
    if link.must("STATE?") != "OK IDLE":
        sys.exit("[fail] board is not IDLE")
    link.must(f"CONFig:MODE {mode}")
    for _, ch, _ in PHASES:
        link.must(f"PFMIN:EDGes {ch} BOTH")
    link.must("SHOT:BEGin")
    for t, f, d in sp:
        r = link.cmd(f"SHOT:STEP {t} {f // 1000}.{f % 1000:03d} {d // 100}.{d % 100:02d}")
        if r != "OK":
            sys.exit(f"[fail] SHOT:STEP {t} -> {r}")
    print("shot:  ", link.must("SHOT:END"), f"-> {len(per)} periods; capturing {len(per)}")
    link.must(f"PFMIN:CAPTURE {len(per)}")
    link.must("ARM")
    link.must("FIRE")
    for _ in range(300):
        time.sleep(0.1)
        st = link.cmd("STATE?")
        if st in ("OK IDLE", "OK FAULT GENERAL"):
            break
    if st != "OK IDLE":
        sys.exit(f"[fail] the shot ended in {st} ({link.cmd('FAULT:SOURces?')})")
    diag = link.must("PFM:DIAG?").split()
    print(f"played: {diag[1]} periods (generated {len(per)}); fewest table entries ready "
          f"{diag[6] if int(diag[6]) < 2**31 else 'n/a'}, {diag[7]} refills")
    out = {"kind": kind, "mode": mode, "setpoints": sp, "per": per.tolist(), "cmp": cmp_.tolist(), "streams": {}}
    for nm, ch, _ in PHASES:
        t0 = time.time()
        count, level, deltas = read_pages(link, ch)
        out["streams"][nm] = {"count": count, "level": level, "deltas": deltas}
        print(f"read:   input {ch} ({nm}): {count} edges in {time.time() - t0:.0f} s")
    for _, ch, _ in PHASES:                 # leave the board as found
        link.must(f"PFMIN:EDGes {ch} RISING")
    return out


# --------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------
def edges(stream):
    """(times in clocks, level after each edge). The first delta is the raw 16-bit
    stamp of the first edge, the rest are differences; the first edge goes to the
    opposite of the level seen before the capture."""
    d = np.array(stream["deltas"], dtype=np.int64)
    t = np.cumsum(d)
    lvl = (1 - stream["level"]) ^ (np.arange(len(d)) & 1)
    return t, lvl


def cycles(stream):
    """Per rising edge: (time of the rising edge, period to the next rising edge,
    high time), all in clocks. The last cycle is dropped (cut off where the outputs
    are forced off); the first is the shot's start-up cycle and is kept so the
    index lines up with the programmed periods."""
    t, lvl = edges(stream)
    rise = np.flatnonzero(lvl == 1)
    fall = np.flatnonzero(lvl == 0)
    rows = []
    for a, b in zip(rise[:-1], rise[1:]):
        f = fall[(fall > a) & (fall < b)]
        if len(f) == 1:
            rows.append((t[a], t[b] - t[a], t[f[0]] - t[a]))
    rows = np.array(rows[:-1], dtype=np.int64)
    return rows[:, 0], rows[:, 1], rows[:, 2]


def align(meas, prog):
    """The measured cycle j belongs to programmed period j - lag (the board applies a
    step a period or two after it is generated, and the capture starts before the
    outputs). Returns the lag, 0..4, that fits best, ignoring the start-up cycle."""
    best = None
    for lag in range(0, 5):
        j = np.arange(max(lag, 2), min(len(meas), len(prog) + lag))
        sd = float(np.std(meas[j] - prog[j - lag]))
        if best is None or sd < best[0] - 1e-12:
            best = (sd, lag)
    return best[1]


def waveform(stream, t_from, t_to, t_ref):
    """Step coordinates (us) of the reconstructed digital output between two times
    (clocks, same axis as t_ref's)."""
    t, lvl = edges(stream)
    m = (t >= t_from) & (t <= t_to)
    xs, ys = [], []
    prev = None
    for ti, li in zip(t[m], lvl[m]):
        if prev is not None:
            xs += [(ti - t_ref) / 170.0] * 2
            ys += [prev, li]
        prev = li
    return xs, ys


# --------------------------------------------------------------------------
# plot
# --------------------------------------------------------------------------
def plot(data, out_png):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    kind = data["kind"]
    per = np.array(data["per"]); cmp_ = np.array(data["cmp"])
    t_prog = np.concatenate([[0], np.cumsum(per + 1)])[:-1] / CLK * 1e3          # ms, start of each programmed period
    f_prog = CLK / (per + 1)
    d_prog = 100.0 * cmp_ / per
    prog = f_prog / 1e3 if kind == "hvps" else d_prog                              # kHz or %
    cyc = {nm: cycles(data["streams"][nm]) for nm, _, _ in PHASES}

    fig = plt.figure(figsize=(15, 12.5))
    gs = fig.add_gridspec(4, 2, height_ratios=[2.2, 1.2, 1.5, 1.5], hspace=0.5, wspace=0.18)
    ax_main = fig.add_subplot(gs[0, :]); ax_err = fig.add_subplot(gs[1, :], sharex=ax_main)
    ax_main.plot(t_prog, prog, color="k", lw=1.2, zorder=5,
                 label="programmed" if kind == "hvps" else "programmed (compare / period)")
    summary = []
    for nm, _, col in PHASES:
        tr, pr, hi = cyc[nm]
        meas = CLK / pr / 1e3 if kind == "hvps" else 100.0 * hi / pr
        lag = align(meas, prog)
        j = np.arange(max(lag, 2), min(len(meas), len(prog) + lag))
        tt = t_prog[j - lag]
        err = meas[j] - prog[j - lag]
        ax_main.plot(tt, meas[j], ".", ms=2.2, color=col, label=f"{nm} measured", alpha=0.85)
        ax_err.plot(tt, err * (1000 if kind == "hvps" else 1), ".", ms=2.2, color=col, alpha=0.85)
        summary.append((nm, lag, len(j), float(np.mean(err)), float(np.max(np.abs(err)))))
    ax_main.set_ylabel("frequency (kHz)" if kind == "hvps" else "duty (%)")
    ttl = ("HVPS: frequency triangle wave 10 - 30 - 10 kHz, 4 cycles in 50 ms, duty 50 %" if kind == "hvps"
           else "LIMITER: duty triangle wave 5 - 95 - 5 %, 4 cycles in 50 ms, 75 kHz")
    ax_main.set_title(ttl + "   (cycle by cycle, from PFM_Input 1-3)", fontsize=11)
    ax_main.legend(loc="upper right", ncol=4, fontsize=8); ax_main.grid(alpha=0.3)
    ax_err.axhline(0, color="k", lw=0.6)
    ax_err.set_ylabel("measured - programmed\n(Hz)" if kind == "hvps" else "measured - programmed\n(% points)")
    ax_err.set_xlabel("programmed time (ms)"); ax_err.grid(alpha=0.3)
    if kind == "limiter":
        ax_err.set_ylim(-2.0, 0.5)
        ax_err.text(0.5, 0.92, "constant offset: the high time is the programmed compare minus a fixed 32-34 clocks "
                    "(output SET on CMP3 + dead time), 1.4 points at 75 kHz", transform=ax_err.transAxes,
                    fontsize=8, va="top", ha="center", bbox=dict(facecolor="white", edgecolor="none", alpha=0.85))

    # zoomed waveforms at the two extremes of the profile
    half = SHOT_MS / (2 * CYCLES)
    zooms = [("minimum", 2 * half), ("maximum", 1 * half)]
    tr_u, pr_u, hi_u = cyc["U"]
    sv = data["streams"]["V"]["deltas"][0] - data["streams"]["U"]["deltas"][0]
    shift_v = int(((sv + 32768) % 65536) - 32768) - sv      # unwrap V's first stamp next to U's: one timer
    for k, (label, t_ms) in enumerate(zooms):
        axw = fig.add_subplot(gs[2:, k])
        i0 = int(np.argmin(np.abs((tr_u - tr_u[0]) / CLK * 1e3 - t_ms)))
        win = int(3 * pr_u[i0])
        notes = []
        for lane, (nm, ch, col) in enumerate(PHASES):
            tr, pr, hi = cyc[nm]
            if nm == "W":                       # its own timer: start from the same elapsed time
                j = int(np.argmin(np.abs((tr - tr[0]) / CLK * 1e3 - t_ms)))
                ref, sh = tr[j], 0
            else:
                j, ref, sh = i0, tr_u[i0], (shift_v if nm == "V" else 0)
                if nm == "V":
                    j = int(np.argmin(np.abs(tr + sh - ref)))
            t_all, l_all = edges(data["streams"][nm])
            t_all = t_all + sh
            m = (t_all >= ref - 100) & (t_all <= ref + win)
            xs, ys, prev = [], [], None
            for ti, li in zip(t_all[m], l_all[m]):
                if prev is not None:
                    xs += [(ti - ref) / 170.0] * 2
                    ys += [prev, li]
                prev = li
            axw.plot(xs, [y * 0.8 + 2 * (2 - lane) for y in ys], color=col, lw=1.4)
            axw.text(-0.01, 2 * (2 - lane) + 0.4, nm, transform=axw.get_yaxis_transform(), ha="right", va="center", color=col)
            notes.append(f"{nm} {CLK / pr[j] / 1e3:.2f} kHz, {100.0 * hi[j] / pr[j]:.1f} %")
        axw.set_yticks([]); axw.set_xlabel("time (us); W is on another timer, its phase to U, V is not measured")
        axw.set_title(f"at the {label}, t = {t_ms:g} ms:   " + "  |  ".join(notes), fontsize=8.5)
        axw.grid(alpha=0.3, axis="x")
    fig.savefig(out_png, dpi=110, bbox_inches="tight")
    print("plot:  ", out_png)
    for nm, lag, n, mean, worst in summary:
        u = "Hz" if kind == "hvps" else "points"
        f = 1000 if kind == "hvps" else 1
        print(f"{nm}: {n} cycles compared (measured cycle j = programmed j-{lag}); error mean {mean * f:+.2f} {u}, worst {worst * f:.2f} {u}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=["hvps", "limiter"])
    ap.add_argument("--url", default="socket://localhost:5000")
    ap.add_argument("--name", default="demo")
    ap.add_argument("--out", default=None)
    ap.add_argument("--json", help="save the raw capture here")
    ap.add_argument("--from-json", help="replot a saved capture, no hardware")
    a = ap.parse_args()
    out_png = a.out or f"pfm_{a.kind}_demo.png"
    if a.from_json:
        data = json.load(open(a.from_json))
    else:
        data = run(a.kind, a.url, a.name)
        if a.json:
            json.dump(data, open(a.json, "w"))
            print("saved: ", a.json)
    plot(data, out_png)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
pfm_gap_test.py -- bench test of the output-off gap (a 0 Hz SHOT:STEP, firmware 2026-11-16) on a controller whose
PFM outputs are wired to its own PFM inputs (the same wiring pfm_shot_test.py uses).

Uploads   20 kHz for T1, OFF from T1 to T2, back on at 25 kHz until T3   (HVPS defaults; --mode LIMITER scales it),
fires it, and reads back the period of every rising edge on each wired input (PFMIN:DATA?). A gap shows up as ONE
long measured period -- the time from the last pulse before the gap to the first pulse after it -- and nothing in
between. The test checks, per input:
   * the pulses before the gap are periods of the first frequency, the ones after of the second,
   * exactly one long period, and its length is the generator's gap (plus the pulse-to-pulse lead-in) within a
     tolerance of --tol periods,
   * the number of pulses before/after matches what the generator planned (within the pipeline lag),
   * the controller returns to IDLE with no fault.
It does NOT see pulse widths or the complement output: look at those with the logic analyzer (DSLogic) separately.

FIRE drives the real outputs. The board must be IDLE (clear any FAULT first).

  python3 python/pfm_gap_test.py --url socket://localhost:5000 --mode HVPS
  python3 python/pfm_gap_test.py --selftest          # the checks on synthetic data, no hardware
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np                                                    # noqa: E402
import pfm_shot_test as P                                             # noqa: E402


def gap_shot(mode, t1_us, t2_us, t3_us):
    f1, f2 = (20_000_000, 25_000_000) if mode == "HVPS" else (80_000_000, 100_000_000)
    duty = 5000
    return [(0, f1, duty), (t1_us, 0, 0), (t2_us, f2, duty), (t3_us, f2, duty)]


def expected(setpoints, mode):
    """From the generator: periods before the gap, the gap length in clocks (first off entry start to the end of
    the last off entry), the periods after it, and the two on-periods."""
    per, cmp_ = P.generate_profile(setpoints, mode)
    off = np.nonzero(cmp_ == 0)[0]
    first, last = int(off[0]), int(off[-1])
    return {"before": first, "after": len(per) - last - 1, "gap_clocks": int((per[first:last + 1] + 1).sum()),
            "per1": int(per[0] + 1), "per2": int(per[-1] + 1), "n": len(per)}


WRAP = 65536     # the capture keeps the low 16 bits of each stamp: periods are reported modulo this (385 us)


def check_input(vals, exp, tol_periods):
    """vals = measured periods (clocks, modulo 65536) of one input. Returns [(ok, text)]. The gap (up to
    milliseconds) wraps around the 16-bit stamp, so it is compared modulo 65536; its absolute length is
    checked against the firmware's own interrupt spacing separately (gap_from_gaplog())."""
    v = np.array(vals, dtype=float)
    out = []
    near = lambda x, ref: abs(x - ref) < 0.03 * ref                      # noqa: E731
    odd = [k for k in range(1, len(v)) if not near(v[k], exp["per1"]) and not near(v[k], exp["per2"])]
    # the first pulse after the gap sits one period-change off the nominal period: allow 10 %
    odd = [k for k in odd if not (abs(v[k] - exp["per2"]) < 0.1 * exp["per2"] and k > 0 and k - 1 not in odd)] or odd
    out.append((len(odd) == 1, f"exactly one gap in the pulse train (found {len(odd)})"))
    if len(odd) != 1:
        return out
    k = odd[0]
    before, after = v[:k], v[k + 1:]
    out.append((abs(np.median(before[1:]) - exp["per1"]) < 0.01 * exp["per1"], f"before the gap: {np.median(before[1:]):.0f} clocks/period (want {exp['per1']})"))
    out.append((abs(np.median(after) - exp["per2"]) < 0.01 * exp["per2"], f"after the gap: {np.median(after):.0f} clocks/period (want {exp['per2']})"))
    want = exp["gap_clocks"] + exp["per1"]
    err = ((v[k] - want + WRAP / 2) % WRAP - WRAP / 2) / exp["per1"]     # modulo 65536, signed
    out.append((abs(err) <= tol_periods, f"gap measured {v[k]:.0f} clocks (mod 65536); generator {want % WRAP} (gap {exp['gap_clocks'] / 170:.0f} us + 1 period); off by {err:+.2f} periods"))
    out.append((abs(len(before) - exp["before"]) <= 3, f"{len(before)} pulses before the gap (generator {exp['before']})"))
    out.append((abs(len(after) + 1 - exp["after"]) <= 3, f"{len(after) + 1} pulses after the gap (generator {exp['after']})"))
    return out


def gap_from_gaplog(gaplog, exp):
    """The absolute gap length from the firmware's interrupt spacing: the spacings between the last on-period
    interrupt and the first on interrupt after the off entries (clocks)."""
    g = np.array(gaplog[1:], dtype=float)
    big = np.nonzero(g > 3 * exp["per1"])[0]
    return float(g[big[0]:big[-1] + 1].sum()) if len(big) else 0.0


def read_gaplog(link, n):
    """The firmware's own interrupt spacing (PFM:GAPLOG?, CPU cycles = HRTIM clocks) for the last shot."""
    r = link.cmd(f"PFM:GAPLOG? 0 {n}", timeout=5.0)
    try:
        t = r.split()
        return [int(x) for x in t[3:]]
    except (AttributeError, IndexError, ValueError):
        return None


def plot(exp, per, cmp_, data, labels, gaplog, out, mode):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(3, 1, figsize=(11, 10))
    # 1. what the generator planned: period of every entry, off entries marked
    t = np.concatenate([[0], np.cumsum(per + 1)])[:-1] / 170.0
    on = cmp_ != 0
    ax[0].step(t, (per + 1) / 170.0, where="post", color="#555", lw=.8)
    ax[0].plot(t[on], (per + 1)[on] / 170.0, ".", ms=4, color="#1f77b4", label="on entries")
    ax[0].plot(t[~on], (per + 1)[~on] / 170.0, "s", ms=5, color="#d62728", label="off entries (outputs disconnected)")
    ax[0].set_ylabel("entry length (us)"); ax[0].set_xlabel("planned time (us)"); ax[0].legend()
    ax[0].set_title(f"Generator's table ({mode}): {exp['before']} on, {int((~on).sum())} off, {exp['after']} on")
    # 2. what the firmware did: interrupt-to-interrupt spacing
    if gaplog:
        g = np.array(gaplog[1:], dtype=float) / 170.0
        ax[1].plot(np.arange(1, len(g) + 1), g, "o-", ms=3, color="#2ca02c")
        ax[1].set_yscale("log")
    ax[1].set_ylabel("ISR spacing (us)"); ax[1].set_xlabel("period interrupt #")
    ax[1].set_title("Measured by the firmware: time between period interrupts (the wide spacing = gap; the doubled one after = reconnect)")
    # 3. what the pins did: period of each captured rising edge
    for ch, d in data.items():
        v = np.array(d["ticks"], dtype=float) / 170.0
        ax[2].plot(np.arange(len(v)), v, "o-", ms=3, label=f"PFM_Input_{ch} ({labels[ch - 1]})")
    ax[2].set_yscale("log"); ax[2].set_ylabel("measured period (us)"); ax[2].set_xlabel("captured pulse #")
    ax[2].set_title("Captured on the PFM inputs: the gap is ONE long period"); ax[2].legend()
    fig.tight_layout(); fig.savefig(out, dpi=110)


def selftest():
    sp = gap_shot("HVPS", 1000, 3000, 4000)
    e = expected(sp, "HVPS")
    ok = e["before"] == 20 and e["gap_clocks"] == 340_000 and e["per1"] == 8500 and e["per2"] == 6800
    print(f"  {'ok ' if ok else 'BAD'} expected(): {e}")
    synth = [8400] + [8500] * 19 + [(e["gap_clocks"] + 8500) % WRAP] + [6800] * (e["after"] - 1)
    res = check_input(synth, e, 2.5)
    for good, text in res:
        print(f"  {'ok ' if good else 'BAD'} {text}")
        ok = ok and good
    bad = list(synth)
    bad[5] = 40000                                       # a second odd period must be caught
    ok = ok and not all(g for g, _ in check_input(bad, e, 2.5))
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="socket://localhost:5000")
    ap.add_argument("--name")
    ap.add_argument("--mode", choices=["LIMITER", "HVPS"])
    ap.add_argument("--t1-ms", type=float, default=1.0, help="gap starts (default 1 ms)")
    ap.add_argument("--t2-ms", type=float, default=3.0, help="gap ends (default 3 ms)")
    ap.add_argument("--t3-ms", type=float, default=4.0, help="shot ends (default 4 ms)")
    ap.add_argument("--map", default=",".join(P.DEFAULT_MAP), help="what PFM_Input_1.. are wired to (default U,V,W,UN)")
    ap.add_argument("--tol", type=float, default=2.5, help="allowed error of the measured gap, in periods")
    ap.add_argument("--out", default="pfm_gap.png", help="plot file")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())

    labels = [s.strip().upper() for s in a.map.split(",")]
    link = P.Link(a.url, a.name)
    mode = a.mode or link.must("CONFig:MODE?").split()[1]
    if mode not in P.MODE_LIMITS:
        sys.exit("[fail] no supply mode on the board -- pass --mode LIMITER or HVPS")
    sp = gap_shot(mode, int(a.t1_ms * 1000), int(a.t2_ms * 1000), int(a.t3_ms * 1000))
    exp = expected(sp, mode)
    print(f"expect: {exp}")
    per, cmp_ = P.generate_profile(sp, mode)
    data = P.run_shot(link, sp, per, cmp_, a.mode, list(range(1, len(labels) + 1)))
    print(f"state:  {link.cmd('STATE?')}   fault: {link.cmd('FAULT?')}")
    gaplog = read_gaplog(link, len(per))
    allok_gap = True
    if gaplog:
        absgap = gap_from_gaplog(gaplog, exp)
        okgap = abs(absgap - exp["gap_clocks"]) < 3 * exp["per1"]
        print(f"firmware: interrupt spacing across the gap = {absgap:.0f} clocks = {absgap / 170:.1f} us (generator {exp['gap_clocks'] / 170:.1f} us)  {'ok' if okgap else 'BAD'}")
        allok_gap = okgap
    plot(exp, per, cmp_, data, labels, gaplog, a.out, mode)
    print(f"plot:   {a.out}")
    allok = (link.cmd("STATE?") == "OK IDLE") and allok_gap
    for ch, d in data.items():
        print(f"\ninput {ch} ({labels[ch - 1]}): {d['count']} periods captured, {d['overcap']}")
        for good, text in check_input(d["ticks"], exp, a.tol):
            print(f"  {'ok ' if good else 'BAD'} {text}")
            allok = allok and good
    print("\nRESULT", "PASS" if allok else "FAIL")
    sys.exit(0 if allok else 1)


if __name__ == "__main__":
    main()

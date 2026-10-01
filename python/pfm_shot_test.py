#!/usr/bin/env python3
"""pfm_shot_test.py -- fire a programmed shot and compare it with what the
PFM_Input capture measured, then plot the result.

WHAT IT DOES
  1. Selects the supply mode (CONFig:MODE) if asked and uploads a two-setpoint
     time-series shot (SHOT:BEGin/STEP/END): at t = 0 --start-hz and
     --duty-start, at --duration-ms --end-hz and --duty-end, ramping linearly
     between them. The firmware turns that into one table entry per switching
     period; generate_profile() below is an exact Python port of that
     generator (held to the C code by the selftest), so the tool knows the
     per-period values the board played.
  2. Arms the PFM_Input capture (PFMIN:CAPTURE) for the first
     min(periods in the shot, 200) periods, ARMs and FIREs, waits for the shot
     to finish (STATE? back to IDLE; a FAULT here is a table underrun).
  3. Reads each capture channel back with PFMIN:DATA?, and re-reads any
     reply that was damaged in transit (see "Readback" below).
  4. Analyses and plots: programmed vs measured frequency, the raw error,
     the residual after removing the expected phase-lock shift, and -- when
     a phase and its complement are both wired (U and UN) -- the duty
     information that pair carries (see "Duty" below).

WIRING ASSUMED (edit with --map): PFM_Input_1..4 = U, V, W, UN over fiber.
Inputs 5 and 6 are ignored. The capture only measures PERIOD (rising edge to
rising edge, 170 MHz ticks); that is all the firmware records.

WHAT THE MEASUREMENT SHOULD LOOK LIKE (learned from real shots)
  - Phase U's rising edge is the start of the period, so its measured period
    is the programmed period (per + 1 ticks) minus a constant 1 tick.
  - V and W are phase-locked at 1/3 and 2/3 of the period, and the
    complement UN rises one dead time (17 ticks) after U falls. When the
    period or duty CHANGES between steps, those edges move, so the raw
    measured period of V, W and UN differs from the programmed one by a
    predictable amount. The tool models that and reports the residual --
    about -1 tick with well under 1 tick of spread on a healthy setup.
  - A channel's capture can run a whole period behind the others (phase W
    did); the tool finds the alignment (0-3 periods) that fits best and
    prints it. The capture starts just BEFORE the outputs, so the first measured
    period is the shot's start-up period (a little off the programmed one) and
    the normal alignment is 2.

Duty
  The capture has no high-time, so absolute duty cannot be read. But UN's
  edge minus U's edge is duty + dead time, so (UN period - U period) of a
  step equals the change of the compare value between consecutive steps.
  Summed over the shot that gives compare(i) - compare(0) in ticks with no
  assumption about timers or anchoring, and is plotted against the table.
  It shows whether the duty MOVED as programmed; it does not give the level
  (that needs the first edge's timestamp, which the firmware does not
  report -- see docs/changelog.txt). Experimental.

Readback
  The Ethernet serial bridge occasionally drops characters, and one dropped
  digit turns a normal 10473-tick period into a bogus "1473". Every
  PFMIN:DATA? reply is therefore validated (right count, every value close
  to the programmed period, no jumps) and re-read until it is clean.

USAGE
  python3 python/pfm_shot_test.py --url socket://localhost:5002 --name shot \\
      --mode HVPS --start-hz 20000 --end-hz 15000 --duration-ms 10 --out ramp.png
  python3 python/pfm_shot_test.py --url /dev/cu.usbserial-XXXX --mode HVPS \\
      --start-hz 20000 --end-hz 20000 --duty-start 40 --duty-end 60
  python3 python/pfm_shot_test.py --selftest       # no hardware needed

  --url takes anything pyserial understands (a serial device, or
  socket://host:port for the ethernet bridge / python/net_broker.py; with the
  broker pass --name so other clients can see who is sending).

  FIRE drives the real outputs. Make sure whatever they feed is safe first.
  The board must be IDLE (clear any FAULT first) and have a supply mode
  (--mode selects one; it discards any table already uploaded).
"""
import argparse
import json
import sys
import time

import numpy as np

CLK_HZ = 170_000_000        # HRTIM / capture-timer clock
DEAD_TICKS = 17             # HRTIM_DEADTIME_COUNTS (100 ns)
MAX_PERIODS = 200           # PFM_INPUT_MAX_PERIODS
DEFAULT_MAP = ["U", "V", "W", "UN"]      # PFM_Input_1..4
PHASE_INDEX = {"U": 0, "V": 1, "W": 2}   # channel index -> Master-compare phase
COLORS = ["#1f77b4", "#2ca02c", "#d62728", "#9467bd", "#ff7f0e", "#8c564b"]


# --------------------------------------------------------------------------
# table
# --------------------------------------------------------------------------
MODE_LIMITS = {                      # (f min Hz, f max Hz, duty min %, duty max %) -- ctrlr_config.h
    "HVPS": (10_000, 30_000, 40, 60),
    "LIMITER": (75_000, 125_000, 5, 95),
}
SHOT_MAX_DURATION_US = 1_000_000     # ctrlr_config.h
SHOT_SETPOINT_MAX = 500


def _tdiv(a, b):
    """Integer division truncated toward zero, like C's."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def make_setpoints(start_hz, end_hz, duty_start, duty_end, duration_ms):
    """The two-setpoint shot this tool uploads: [(t_us, freq_mHz, duty_hundredths), ...]."""
    t_us = int(round(duration_ms * 1000))
    return [(0, int(round(start_hz * 1000)), int(round(duty_start * 100))),
            (t_us, int(round(end_hz * 1000)), int(round(duty_end * 100)))]


def generate_profile(setpoints, mode, periods_per_entry=1):
    """Exact port of the firmware's generator (src/app/control/shot_profile.c,
    ShotGen_Next): the {per, cmp} of every switching period of a time-series
    shot, linear in time between setpoints, all in integer arithmetic. Returns
    (per, cmp) as int arrays. setpoints = [(t_us, freq_mHz, duty_hundredths)]."""
    fmin, fmax, dmin, dmax = MODE_LIMITS[mode]
    per1_min = -(-CLK_HZ // fmax)
    per1_max = CLK_HZ // fmin
    tpu = CLK_HZ // 1_000_000
    end = setpoints[-1][0] * tpu
    per, cmp_ = [], []
    tick, seg = 0, -1
    seg_start = seg_end = f0 = d0 = slope_f = slope_d = 0
    while tick < end:
        while seg < 0 or tick >= seg_end:
            seg += 1
            (ta, fa, da), (tb, fb, db) = setpoints[seg], setpoints[seg + 1]
            seg_start, seg_end, f0, d0 = ta * tpu, tb * tpu, fa, da
            dt = seg_end - seg_start
            slope_f = _tdiv((fb - fa) * (1 << 32), dt)
            slope_d = _tdiv((db - da) * (1 << 32), dt)
        elapsed = tick - seg_start
        f = f0 + ((slope_f * elapsed) >> 32)            # arithmetic shift = floor, as in C
        d = d0 + ((slope_d * elapsed) >> 32)
        per1 = (CLK_HZ * 1000 + f // 2) // f
        per1 = min(max(per1, per1_min), per1_max)
        p = per1 - 1
        c = (p * d + 5000) // 10000
        c = min(max(c, (dmin * p + 99) // 100), (dmax * p) // 100)
        per.append(p)
        cmp_.append(c)
        tick += per1 * periods_per_entry
    return np.array(per, dtype=int), np.array(cmp_, dtype=int)


# --------------------------------------------------------------------------
# analysis (pure functions -- exercised by --selftest)
# --------------------------------------------------------------------------
def parse_label(label):
    """'U' -> ('U', False); 'UN' -> ('U', True) (complement); 'W'/'WN' likewise."""
    base = label.upper()
    comp = base.endswith("N") and len(base) == 2
    if comp:
        base = base[0]
    if base not in PHASE_INDEX:
        raise ValueError(f"unknown channel label {label!r} (use U, V, W, UN, VN, WN)")
    return base, comp


def edge_offset(label, per, cmp_, i, n_phases=3):
    """Where inside step i (HRTIM ticks after the period start) this signal's
    rising edge falls: the phase-lock offset of its phase (k/N of the period,
    hrtim.c / pfm.c PFM_PhaseForChannel), plus duty and dead time for a
    complement."""
    base, comp = parse_label(label)
    k = PHASE_INDEX[base]
    off = (k * (int(per[i]) + 1)) // n_phases
    if comp:
        off += int(cmp_[i]) + DEAD_TICKS
    return off


def model_period(label, per, cmp_, i, n_phases=3):
    """Expected measured period between this signal's edge in step i and in
    step i+1."""
    return (int(per[i]) + 1) + edge_offset(label, per, cmp_, i + 1, n_phases) - \
        edge_offset(label, per, cmp_, i, n_phases)


def best_alignment(label, measured, per, cmp_, max_lag=3, preferred_lag=2):
    """The capture's j-th period can belong to programmed step j-lag. Returns
    (lag, js, residual array in ticks) for the lag that fits the model best
    (smallest spread of the residual). When the shot gives the model nothing
    to tell lags apart (constant period and duty) several lags fit equally;
    `preferred_lag` (2: since 2026-09-30 the capture starts BEFORE the outputs, so it
    also holds the start-up period and one more before the table; it was 1) is
    then chosen, so a flat shot is not given an arbitrary alignment."""
    fits = {}
    for lag in range(0, max_lag + 1):
        js = np.arange(lag, len(measured))
        js = js[(js - lag) < len(per) - 1]
        if len(js) < 5:
            continue
        model = np.array([model_period(label, per, cmp_, j - lag) for j in js])
        resid = measured[js] - model
        fits[lag] = (float(np.std(resid)), js, resid)
    if not fits:
        return None
    best = min(v[0] for v in fits.values())
    tol = best * 1.15 + 0.08
    candidates = [lag for lag, v in fits.items() if v[0] <= tol]
    lag = preferred_lag if preferred_lag in candidates else min(candidates, key=lambda l: (fits[l][0], l))
    _, js, resid = fits[lag]
    return lag, js, resid


def validate_read(vals, count, per):
    """A PFMIN:DATA? reply is trustworthy if it has the announced number of
    values, all within 12 % of the programmed period range, with no jump
    larger than the programmed steps could explain."""
    a = np.array(vals, dtype=float)
    if len(a) != count or count < 2:
        return False
    # The first period is the shot's start-up period (the outputs begin from a
    # forced state, so it is genuinely a little off, e.g. 8398 for 8499): not
    # held to the ramp's range or step limit.
    a = a[1:]
    lo, hi = per.min() + 1, per.max() + 1
    if a.min() < 0.88 * lo or a.max() > 1.12 * hi:
        return False
    max_step = np.max(np.abs(np.diff(per))) * 2 + 60
    return bool(np.max(np.abs(np.diff(a))) <= max_step)


def duty_from_pair(base_label, comp_label, m_base, m_comp, lag_base, lag_comp, per, cmp_):
    """compare(i) - compare(0) in ticks from the pair (UN period - U period),
    with the programmed values for the same steps. Returns (steps, measured,
    programmed) or None if the two labels are not a phase and its
    complement."""
    b, bc = parse_label(base_label)
    c, cc = parse_label(comp_label)
    if b != c or bc or not cc:
        return None
    n = min(len(m_base) - lag_base, len(m_comp) - lag_comp, len(per) - 1)
    if n < 3:
        return None
    step = np.arange(n)
    delta = m_comp[lag_comp:lag_comp + n] - m_base[lag_base:lag_base + n]  # cmp[i+1]-cmp[i]
    rel = np.concatenate([[0.0], np.cumsum(delta)])                       # cmp[i]-cmp[0], i=0..n
    prog = (cmp_[:n + 1] - cmp_[0]).astype(float)
    return np.arange(n + 1), rel, prog


# --------------------------------------------------------------------------
# board link
# --------------------------------------------------------------------------
class Link:
    def __init__(self, url, name=None):
        import serial
        self.s = serial.serial_for_url(url, baudrate=115200, timeout=0.05)
        self.buf = b""
        if name:
            self.s.write(f"@name {name}\r\n".encode())
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def cmd(self, text, timeout=3.0):
        """Send one line, return the reply line (banner, telemetry and broker
        chatter skipped), or None on timeout."""
        self.buf = b""
        self.s.write(text.encode() + b"\r\n")
        end = time.time() + timeout
        while time.time() < end:
            self.buf += self.s.read(max(1, self.s.in_waiting))
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip().decode(errors="replace")
                if line.startswith(("OK", "ERR")):
                    return line
        return None

    def must(self, text, expect="OK", timeout=3.0):
        r = self.cmd(text, timeout)
        if r is None or not r.startswith(expect):
            sys.exit(f"[fail] {text!r} -> {r!r}")
        return r


def run_shot(link, setpoints, per, cmp_, mode, channels):
    """Upload the shot, fire and read back the first periods. `per`/`cmp_` are the
    per-period values the board generates (generate_profile()). Returns
    {channel: {'count','overcap','ticks'}}."""
    idn = link.must("*IDN?")
    print(f"board:  {idn}")
    state = link.must("STATE?")
    if state != "OK IDLE":
        sys.exit(f"[fail] board is {state!r}, not IDLE (FAULT:CLEAR / DISARM first)")
    if mode:
        link.must(f"CONFig:MODE {mode}")
    cur = link.must("CONFig:MODE?").split()[1]
    if cur == "NONE":
        sys.exit("[fail] no supply mode selected on the board -- pass --mode LIMITER or HVPS")
    print(f"mode:   {cur}   {link.must('CONFig:LIMits?')}")

    n_shot = len(per)
    n = min(n_shot, MAX_PERIODS)          # the capture holds at most 200 periods
    print(f"upload: {len(setpoints)} setpoints, {setpoints[-1][0] / 1000:g} ms -> {n_shot} periods "
          f"(per {per[0]} -> {per[-1]}) ...", end="", flush=True)
    t0 = time.time()
    link.must("SHOT:BEGin")
    for t_us, f_mhz, d_h in setpoints:
        r = link.cmd(f"SHOT:STEP {t_us} {f_mhz // 1000}.{f_mhz % 1000:03d} {d_h // 100}.{d_h % 100:02d}")
        if r != "OK":
            sys.exit(f"\n[fail] SHOT:STEP {t_us} ... -> {r!r}")
    r = link.must("SHOT:END")
    print(f" done ({time.time() - t0:.1f} s) {r}")

    link.must(f"PFMIN:CAPTURE {n}")
    link.must("ARM")
    link.must("FIRE")
    t_end = time.time() + 10 + setpoints[-1][0] / 1e6 * 1.5
    while time.time() < t_end:
        time.sleep(0.1)
        state = link.cmd("STATE?")
        if state == "OK IDLE":
            break
        if state and state.startswith("OK FAULT"):
            sys.exit(f"[fail] the shot faulted ({state}) -- a table underrun (PFM:DIAG? ring fields) or a real fault")
    else:
        sys.exit("[fail] shot did not finish (STATE? never returned to IDLE)")
    diag = link.cmd("PFM:DIAG?").split()
    if len(diag) >= 9:
        calls, ready, refills, produced = int(diag[1]), int(diag[6]), int(diag[7]), int(diag[8])
        print(f"shot:   done; {calls} periods played (generated {n_shot}), fewest table entries ready {ready}, "
              f"{refills} refills; counts {link.must('PFMIN:STATus?')}")
    else:
        print(f"shot:   done; counts {link.must('PFMIN:STATus?')}")
    per, cmp_ = per[:n + 4], cmp_[:n + 4]

    data = {}
    for ch in channels:
        for attempt in range(1, 16):
            r = link.cmd(f"PFMIN:DATA? {ch}", timeout=5.0)
            toks = r.split() if r else []
            try:
                count = int(toks[1])
                ov = [t for t in toks if t.startswith("OVERCAP=")][0]
                vals = [int(t) for t in toks[2:] if not t.startswith("OVERCAP=")]
            except (IndexError, ValueError):
                continue
            if count == 0:
                data[ch] = {"count": 0, "overcap": ov, "ticks": []}
                break
            if validate_read(vals, count, per):
                data[ch] = {"count": count, "overcap": ov, "ticks": vals}
                if attempt > 1:
                    print(f"        input {ch}: reply damaged in transit, clean on read {attempt}")
                break
        else:
            print(f"[warn] input {ch}: no clean readback after 15 tries -- left out")
    return data


# --------------------------------------------------------------------------
# report + plot
# --------------------------------------------------------------------------
def analyse(per, cmp_, data, labels):
    """labels: list of names for PFM_Input_1..len. Returns per-channel results."""
    out = {}
    for idx, label in enumerate(labels, start=1):
        d = data.get(idx)
        if not d or d["count"] < 5:
            continue
        m = np.array(d["ticks"], dtype=float)
        fit = best_alignment(label, m, per, cmp_)
        if fit is None:
            continue
        lag, js, resid = fit
        i = js - lag
        prog = per[i] + 1.0
        out[idx] = dict(label=label, lag=lag, js=js, i=i, m=m, resid=resid,
                        f_meas=CLK_HZ / m[js], f_prog=CLK_HZ / prog, overcap=d["overcap"],
                        count=d["count"])
    return out


def report(res, per):
    print("\ninput  signal  periods  align  raw error (Hz) mean / max|.|   residual vs model (ticks) mean / std / max|.|   overcapture")
    for idx, r in res.items():
        err = r["f_meas"] - r["f_prog"]
        print(f"  {idx}     {r['label']:3s}     {r['count']:4d}     {r['lag']}     "
              f"{err.mean():8.1f} / {np.abs(err).max():6.1f}            "
              f"{r['resid'].mean():6.2f} / {r['resid'].std():4.2f} / {np.abs(r['resid']).max():4.1f}"
              f"                    {r['overcap']}")
    print("\n(healthy: residual mean about -1 tick, spread under 1 tick; 'align' 2 is normal -- the capture "
          "starts before the outputs and includes the start-up period -- a channel at 3 ran a further period behind)")


def plot(per, cmp_, res, labels, data, out_png, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    prog_ticks = per + 1
    t_end = np.cumsum(prog_ticks) / CLK_HZ * 1e3
    t_start = t_end - prog_ticks / CLK_HZ * 1e3
    f_prog = CLK_HZ / prog_ticks

    duty = None
    idx_of = {r["label"]: k for k, r in res.items()}
    for base, comp in (("U", "UN"), ("V", "VN"), ("W", "WN")):
        if base in idx_of and comp in idx_of:
            rb, rc = res[idx_of[base]], res[idx_of[comp]]
            duty = duty_from_pair(base, comp, rb["m"], rc["m"], rb["lag"], rc["lag"], per, cmp_)
            duty_names = (base, comp)
            break

    rows = 4 if duty else 3
    fig, ax = plt.subplots(rows, 1, figsize=(11, 3.6 + 2.6 * rows),
                           gridspec_kw={"height_ratios": [3, 1.7, 1.7] + ([1.9] if duty else [])})
    fmin, fmax = f_prog.min(), f_prog.max()
    pad = max((fmax - fmin) * 0.12, 0.02 * fmax)
    ax[0].step(t_start, f_prog, where="post", color="k", lw=2.4, label="programmed (table)", zorder=5)
    for n, (k, r) in enumerate(res.items()):
        c = COLORS[n % len(COLORS)]
        x = t_start[r["i"]]
        ax[0].plot(x, r["f_meas"], "o", ms=3, color=c, alpha=.85, label=f"measured  {r['label']} -> PFM_Input_{k}")
        ax[1].plot(x, r["f_meas"] - r["f_prog"], ".-", lw=.7, ms=3, color=c, label=r["label"])
        ax[2].plot(x, r["resid"], ".-", lw=.7, ms=3, color=c)
    ax[0].set_ylim(fmin - pad, fmax + pad)
    ax[0].set_ylabel("carrier frequency (kHz)")
    ax[0].yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v / 1e3:.2f}"))
    ax[0].set_title(title)
    ax[0].legend(loc="best", fontsize=8.5)
    ax[0].grid(alpha=.3)
    ax[1].set_ylabel("measured - programmed\n(Hz)")
    ax[1].axhline(0, color="k", lw=.6)
    ax[1].grid(alpha=.3)
    ax[1].legend(ncol=6, fontsize=8, loc="best")
    ax[1].set_title("raw difference (V, W and the complement carry the phase-lock shift a changing period causes)", fontsize=9)
    ax[2].set_ylabel("residual vs model\n(ticks @170 MHz)")
    ax[2].axhline(0, color="k", lw=.6)
    ax[2].grid(alpha=.3)
    lim = max(4.0, min(12.0, 1.5 * max(np.abs(r["resid"]).max() for r in res.values())))
    ax[2].set_ylim(-lim, lim)
    ax[2].set_title("after removing the expected edge-position shift; healthy = flat near -1 tick", fontsize=9)
    if duty:
        steps, rel, prog = duty
        ax[3].plot(t_start[steps[:-1]], prog[:-1], "k-", lw=2.2, label="programmed compare(i) - compare(0)")
        ax[3].plot(t_start[steps[:-1]], rel[:-1], "o", ms=3, color="#ff7f0e",
                   label=f"measured from ({duty_names[1]} period - {duty_names[0]} period), cumulative")
        ax[3].set_ylabel("compare change since\nstep 0 (ticks)")
        ax[3].grid(alpha=.3)
        ax[3].legend(fontsize=8.5, loc="best")
        err = rel - prog
        ax[3].set_title(f"EXPERIMENTAL duty movement (level not measurable): end-of-shot difference "
                        f"{err[-1]:+.1f} ticks, worst {np.abs(err).max():.1f} ticks", fontsize=9)
    ax[-1].set_xlabel("time since FIRE (ms), from the programmed table")
    fig.tight_layout()
    fig.savefig(out_png, dpi=130)
    print(f"plot:   {out_png}")
    if duty:
        steps, rel, prog = duty
        print(f"duty movement: compare change over the shot programmed {prog[-1]:+.0f} ticks, "
              f"measured {rel[-1]:+.0f} ticks (worst step error {np.abs(rel - prog).max():.1f} ticks = "
              f"{np.abs(rel - prog).max() / CLK_HZ * 1e9:.0f} ns)")


# --------------------------------------------------------------------------
# self test: a synthetic board, no hardware
# --------------------------------------------------------------------------
def synthetic_capture(per, cmp_, labels, lags, rng, drop_first=0):
    """What a healthy capture would report: edge times from the same model the
    analysis uses, plus 0-1 tick of quantisation and a constant -1 offset,
    with `lags[k]` extra start-up periods in front (channel k's j-th measured
    period belongs to programmed step j - lag)."""
    data = {}
    for idx, label in enumerate(labels, start=1):
        lag = lags[idx - 1]
        edges = []
        t = 0
        for i in range(len(per)):
            edges.append(t + edge_offset(label, per, cmp_, i) + rng.integers(0, 2))
            t += per[i] + 1
        periods = np.diff(np.array(edges)) - 1
        # the capture starts `lag` periods before the table's first edge
        periods = np.concatenate([np.full(lag, per[0]), periods])
        data[idx] = {"count": len(periods), "overcap": "OVERCAP=0", "ticks": periods.tolist()}
    return data


def selftest():
    rng = np.random.default_rng(1)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(f"  {'ok ' if cond else 'BAD'} {what}")
        ok = ok and bool(cond)

    print("generator, held to the C code (tests/test_shot_profile.c GOLDEN lines):")
    p, c = generate_profile([(0, 20_000_000, 5000), (1000, 20_000_000, 5000)], "HVPS")
    check(len(p) == 20 and p[0] == 8499 and c[0] == 4250, "flat 20 kHz / 50 %, 1 ms: 20 entries, per 8499, cmp 4250")
    ramp = [(0, 20_000_000, 4000), (10_000, 15_000_000, 6000)]
    per, cmp_ = generate_profile(ramp, "HVPS")
    check(len(per) == 176 and per[0] == 8499 and cmp_[0] == 3400 and per[-1] == 11329, "ramp 20 -> 15 kHz, 40 -> 60 %, 10 ms: 176 entries")
    check(int((per + 1).sum()) == 1_709_700, "ramp: the periods add up to 1709700 clocks")
    check(per[80] == 9501 and cmp_[80] == 4601, "ramp: entry 80 is per 9501, cmp 4601")
    p, c = generate_profile([(0, 75_000_000, 500), (1000, 75_000_000, 500)], "LIMITER")
    check(p[0] == 2265 and c[0] == 114, "LIMITER 75 kHz / 5 %: period and compare clamped onto the limits (2265, 114)")
    p, c = generate_profile([(0, 125_000_000, 500), (1000, 125_000_000, 500)], "LIMITER")
    check(p[0] == 1359 and c[0] == 68, "LIMITER 125 kHz / 5 %: per 1359, cmp 68")
    p, c = generate_profile([(0, 20_000_000, 5000), (1_000_000, 20_000_000, 5000)], "HVPS")
    check(len(p) == 20000, "the longest shot, 1 s at 20 kHz: 20,000 entries")
    per2, cmp2 = generate_profile([(0, 20_000_000, 4000), (5000, 20_000_000, 6000)], "HVPS")
    check(len(per2) == 100 and abs(cmp2[0] / per2[0] - .4) < 2e-4 and abs(cmp2[-1] / per2[-1] - .6) < 3e-3, "duty ramp 40 -> 60 % at constant frequency")
    check(make_setpoints(20000, 15000, 40, 60, 10) == ramp, "the CLI's two setpoints")
    print("alignment and residual on a synthetic capture (U, V, W, UN; W one period further behind):")
    labels = ["U", "V", "W", "UN"]
    data = synthetic_capture(per, cmp_, labels, [2, 2, 3, 2], rng)
    res = analyse(per, cmp_, data, labels)
    check(res[1]["lag"] == 2 and res[2]["lag"] == 2 and res[4]["lag"] == 2, "U, V, UN found at lag 2")
    check(res[3]["lag"] == 3, "W found at lag 3")
    for k in res:
        check(res[k]["resid"].std() < 0.9 and abs(res[k]["resid"].mean() + 1.0) < 0.3, f"input {k} residual flat (~-1 tick)")
    print("duty movement (40 -> 60 % at constant frequency):")
    data = synthetic_capture(per2, cmp2, labels, [2, 2, 3, 2], rng)
    res = analyse(per2, cmp2, data, labels)
    d = duty_from_pair("U", "UN", res[1]["m"], res[4]["m"], res[1]["lag"], res[4]["lag"], per2, cmp2)
    _, rel, prog = d
    check(d is not None and np.abs(rel - prog).max() < 3, "compare change reconstructed within 3 ticks")
    check(duty_from_pair("U", "V", res[1]["m"], res[2]["m"], 2, 2, per2, cmp2) is None, "U/V is not a phase + complement")
    print("readback validation:")
    good = data[1]["ticks"]
    bad = list(good)
    bad[50] = int(str(bad[50])[1:])           # a dropped leading digit
    check(validate_read(good, len(good), per2), "clean reply accepted")
    check(not validate_read(bad, len(bad), per2), "a dropped digit is rejected")
    check(not validate_read(good[:-1], len(good), per2), "a short reply is rejected")
    startup = list(good)
    startup[0] = int(startup[0] * 0.988)      # the real start-up period is ~100 ticks short
    check(validate_read(startup, len(startup), per2), "an off start-up period is accepted")
    print("\nSELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="socket://localhost:5000", help="serial device or socket://host:port")
    ap.add_argument("--name", help="name to show other clients when going through net_broker.py")
    ap.add_argument("--mode", choices=["LIMITER", "HVPS"], help="select the supply mode first (discards any table)")
    ap.add_argument("--start-hz", type=float, default=20000.0)
    ap.add_argument("--end-hz", type=float, default=15000.0)
    ap.add_argument("--duty-start", type=float, default=50.0, help="percent")
    ap.add_argument("--duty-end", type=float, default=None, help="percent (default: same as --duty-start)")
    ap.add_argument("--duration-ms", type=float, default=10.0,
                    help="shot length; the capture holds the first 200 periods of it (default 10)")
    ap.add_argument("--map", default=",".join(DEFAULT_MAP),
                    help="what PFM_Input_1.. are wired to, e.g. U,V,W,UN (default) or U,UN")
    ap.add_argument("--out", default="pfm_shot.png", help="plot file")
    ap.add_argument("--json", help="also save the programmed table and raw readback here")
    ap.add_argument("--from-json", help="skip the hardware: analyse and plot a saved --json file")
    ap.add_argument("--selftest", action="store_true", help="check the analysis on synthetic data and exit")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())

    labels = [s.strip().upper() for s in args.map.split(",")]
    for lab in labels:
        parse_label(lab)

    if args.from_json:
        saved = json.load(open(args.from_json))
        per, cmp_ = np.array(saved["per"]), np.array(saved["cmp"])
        data = {int(k): v for k, v in saved["data"].items()}
        labels = saved.get("labels", labels)
        info = saved.get("info", "saved shot")
    else:
        duty_end = args.duty_start if args.duty_end is None else args.duty_end
        mode = args.mode
        link = Link(args.url, args.name)
        if mode is None:
            mode = link.must("CONFig:MODE?").split()[1]
            if mode not in MODE_LIMITS:
                sys.exit("[fail] no supply mode on the board -- pass --mode LIMITER or HVPS")
        setpoints = make_setpoints(args.start_hz, args.end_hz, args.duty_start, duty_end, args.duration_ms)
        per_all, cmp_all = generate_profile(setpoints, mode)
        data = run_shot(link, setpoints, per_all, cmp_all, args.mode, list(range(1, len(labels) + 1)))
        per, cmp_ = per_all[:min(len(per_all), MAX_PERIODS) + 4], cmp_all[:min(len(per_all), MAX_PERIODS) + 4]
        info = (f"{args.duration_ms:g} ms shot ({len(per_all)} periods): {args.start_hz / 1e3:g} -> {args.end_hz / 1e3:g} kHz, "
                f"duty {args.duty_start:g} -> {duty_end:g} %")
        if args.json:
            json.dump({"per": per.tolist(), "cmp": cmp_.tolist(), "labels": labels, "info": info,
                       "data": {str(k): v for k, v in data.items()}}, open(args.json, "w"))
            print(f"saved:  {args.json}")

    res = analyse(per, cmp_, data, labels)
    if not res:
        sys.exit("[fail] no channel captured enough periods to analyse")
    report(res, per)
    plot(per, cmp_, res, labels, data, args.out,
         f"{info}\nprogrammed table vs. period measured by the PFM_Input capture")


if __name__ == "__main__":
    main()

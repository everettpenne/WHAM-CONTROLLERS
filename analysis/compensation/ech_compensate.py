#!/usr/bin/env python3
"""
ech_compensate.py -- shot-to-shot frequency-profile correction for the ECH-HVPS (HBRIDGE, HVPS mode).

WHY
    The ECH gyrotron's high-voltage supply is driven by a PFM frequency profile (SHOT:STEP <t_us> <hz> <duty>). Its output
    rises as the frequency is lowered towards resonance (~18.8 kHz), falls as the capacitor bank droops during a pulse,
    and the gyrotron's forward power is a steep, drifting function of that voltage. Hand-tuning the profile takes many
    shots. This script closes the loop between shots: after each shot it compares what was measured (MDSplus) with what
    the operator asked for, and writes a corrected frequency profile for the next shot.

TWO WAYS TO SAY WHAT YOU WANT (the CSV decides)
    power target       time_us,demand_kw          the forward power you want (from power_waveform_picker.html). The
                                                  script chooses the frequencies.
    frequency profile  time_us,demand_hz          a frequency profile written as if the bank never drooped. The script
                                                  keeps the gyrotron voltage it would have given and corrects the droop.
    'off' (power) or 0 (frequency) starts an output-off gap that lasts until the next row, exactly as in SHOT:STEP.

HOW TO RUN (one file, one CSV)
    python ech_compensate.py my_shot.csv --target dummy-load --bank-volts 500
        * the CSV has no frequencies yet (fresh from the picker): writes my_shot_iter0.csv, a FIRST-SHOT profile computed
          from a model of the supply (2026-11-17 dummy-load data). Check it before firing.
        * the CSV has frequencies (an _iterN file): reads the latest ECH shot from MDS (or --shot N), writes
          my_shot_iter<N+1>.csv and a .png, and prints one RESULT line.
    Upload the carrier_hz,duty_pct columns of the new file (each row = SHOT:STEP <time_us> <carrier_hz> <duty_pct>; 0 Hz
    = off) for the next shot, then run the script on that file. --target and --bank-volts may also come from '# target='
    and '# bank_volts=' lines in the CSV (the command line wins). --next-bank-volts when the next shot uses a different
    bank charge.

SAFETY LIMITS (absolute; the script refuses to write a profile outside them)
    18.7-21.0 kHz everywhere, at most --max-step-hz (100) change from the last shot at any time, predicted gyrotron
    voltage at most --max-kv (70), shot at most 15 ms, the 2026 HBRIDGE HVPS rules (duty 40-60 %, <= 500 setpoints, ...).
    The first --settle-ms (1.5) of every pulse is never corrected on its own: the rise is slow (~1.1 ms to 90 %) and
    lowering the frequency at a pulse's start to hurry it would mean starting closer to resonance. The start of a pulse
    only follows the correction the pulse's flat top needs.

WHAT IT TALKS TO
    MDSplus, read only, and the terminal (or LabVIEW System Exec) that runs it -- nothing else: no controller, no hub,
    no other network service, and it starts no other program. Everything else it needs (the profile that fired, the
    bank charge, the target) comes from its CSV and its command line, so it runs on any machine that can read MDS, or
    offline with --data. Uploading and firing stay with the operator. tests/test_isolation.py enforces this.
    Needs numpy; matplotlib for the plot (skipped if missing); MDSplus only to read shots.

METHOD, IN SHORT (analysis/compensation/README.md has the details and the data behind the numbers)
    measured y = gyrotron voltage (frequency profile) or forward power (power target), target y*. On the flat top of
    every pulse:  new f = last f - gain * (y* - y)(t + lead) / sensitivity,  sensitivity = how much y rises per Hz lower:
    kappa = 4 %/100 Hz of the gyrotron voltage (measured 2026-11-17, +/-0.2 % over 8 bank charges), times dP/dV from the
    measured power-voltage curve in power-target mode. Each pulse is corrected from its own error, so the ~6.5 % deficit
    measured at the start of pulse 2 is learned without needing its cause.

    python ech_compensate.py --selftest      runs the loop against a simulated supply
"""
import argparse
import math
import os
import re
import sys

import numpy as np

# --- controller rules (firmware/hbridge: ctrlr_config.h SUPPLY HVPS limits, SHOT_SETPOINT_MAX; shot_profile.c) -------
HVPS_FREQ_MIN_HZ, HVPS_FREQ_MAX_HZ = 10_000.0, 30_000.0
HVPS_DUTY_MIN_PCT, HVPS_DUTY_MAX_PCT = 40.0, 60.0
SHOT_SETPOINT_MAX = 500
# --- operating limits for this supply (the operator's, 2026-11-17) --------------------------------------------------
F_FLOOR_HZ = 18_700.0          # never below: resonance is near 18.8 kHz
F_CEIL_HZ = 21_000.0          # upper limit (raised from 20 kHz on 2026-11-17 to let a pulse start lower and cut the early overshoot)
F_REF_HZ = 20_000.0           # the model's reference point: KAPPA and V20_* below are measured relative to 20 kHz
MAX_SHOT_US = 15_000           # 15 ms
DEFAULT_MAX_STEP_HZ = 100.0
DEFAULT_MAX_KV = 70.0
DEFAULT_DUTY = 50.0
# --- the supply, measured 2026-11-17 (shots 261117004-028, dummy load, filament 41.0 A, gun coil 2.1) ----------------
KAPPA = 4.0e-4                 # relative output rise per Hz lower (4 % per 100 Hz near 19.5-20 kHz)
V20_SLOPE_KV_PER_V = 0.164     # settled gyrotron kV at 20 kHz per bank V above V20_OFFSET
V20_OFFSET_V = 151.0
DROOP_PER_MS = 0.010           # output fraction lost per ms of on-time at constant frequency, at REF_KV
REF_KV = 57.7
GAP_DEFICIT = 0.05             # extra loss at the start of each later pulse (measured 6-7 %; 5 % = under-correct)
HOLD_POWER_V_RISE_PER_MS = 0.008   # voltage rise per ms that held the forward power flat (morning ramp)
SETTLE_MS = 2.0
LEAD_MS = 0.65                 # the output's settling lag
# forward power (kW) against settled gyrotron voltage (kV): V at 2 ms vs P at 3 ms of the single-pulse sweep
PV_TABLE_KV = np.array([40.0, 45.0, 49.0, 50.5, 52.1, 53.7, 55.5, 57.1, 58.9, 60.9, 63.0, 64.9, 66.0, 72.0])
PV_TABLE_KW = np.array([0.0, 1.0, 5.5, 10.5, 24.6, 44.3, 70.6, 100.2, 132.0, 167.0, 217.3, 265.8, 279.3, 360.0])
DPDV_MIN = 5.0                 # kW per kV: below the gyrotron's threshold the curve is flat; never divide by ~0

NODE_V = "ECH_PROC.GYROTRON_V:FILTERED"
NODE_P = {"vessel": "ECH_PROC.WG_MONITOR_F:FILTERED", "dummy-load": "ECH_PROC.DL_MONITOR_F:FILTERED"}
NODE_BANK = "ECH_PARAMS:HVPS_V"
NODE_TARGET = "ECH_PARAMS:ECH_TARGET"
MDS_TARGET_NAMES = {"vessel": ("vessel", "plasma", "waveguide", "wg", "machine"), "dummy-load": ("dummy_load", "dummy-load", "dummy", "dl")}

GRID_US = 10.0


class CompError(Exception):
    """A problem the operator can act on."""


# =====================================================================================================================
# Profile
# =====================================================================================================================
class Profile:
    """Rows: t_us, demand (kW or Hz; NaN = off), carrier_hz (None = not chosen yet), duty_pct. mode 'power' | 'frequency'."""

    def __init__(self, t_us, demand, carrier, duty, mode, meta=None):
        self.t_us = np.asarray(t_us, dtype=float)
        self.demand = np.asarray(demand, dtype=float)
        self.carrier = None if carrier is None else np.asarray(carrier, dtype=float)
        self.duty = None if duty is None else np.asarray(duty, dtype=float)
        self.mode = mode
        self.meta = dict(meta or {})

    @property
    def on(self):
        return np.isfinite(self.demand)

    @property
    def duration_us(self):
        return float(self.t_us[-1])


def _num(cell):
    c = cell.strip().lower()
    if c in ("off", "", "nan", "-"):
        return float("nan")
    return float(c)


def read_profile(path):
    meta, rows, header = {}, [], None
    with open(path, newline="") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                m = re.match(r"#\s*([A-Za-z0-9_]+)\s*=\s*(.*)$", line)
                if m:
                    meta[m.group(1)] = m.group(2).strip()
                continue
            cells = [c.strip() for c in line.split(",")]
            if header is None:
                header = [c.lower() for c in cells]
                continue
            rows.append(cells)
    if header is None or not rows:
        raise CompError(f"{path}: no header or no rows")
    col = {n: i for i, n in enumerate(header)}
    if "time_us" not in col:
        raise CompError(f"{path}: missing column time_us")
    if "demand_kw" in col:
        mode, dname = "power", "demand_kw"
    elif "demand_hz" in col:
        mode, dname = "frequency", "demand_hz"
    else:
        raise CompError(f"{path}: need a demand_kw (power target) or demand_hz (frequency profile) column")
    try:
        t = [float(r[col["time_us"]]) for r in rows]
        d = [_num(r[col[dname]]) for r in rows]
        carrier = [float(r[col["carrier_hz"]]) for r in rows] if "carrier_hz" in col else None
        duty = [float(r[col["duty_pct"]]) for r in rows] if "duty_pct" in col else None
    except (ValueError, IndexError) as e:
        raise CompError(f"{path}: a row is not numeric ({e})")
    d = np.array(d)
    if mode == "frequency":
        d[d == 0] = np.nan                       # 0 Hz = off, as in SHOT:STEP
    if carrier is not None and duty is None:
        duty = [DEFAULT_DUTY if c > 0 else 0.0 for c in carrier]
    return Profile(t, d, carrier, duty, mode, meta)


def check_profile(p, check_commanded=True):
    """The controller's rules plus this supply's limits. Raises CompError listing every problem."""
    errs = []
    t, n = p.t_us, len(p.t_us)
    if n < 2:
        errs.append("at least 2 setpoints are needed")
    if n > SHOT_SETPOINT_MAX:
        errs.append(f"{n} setpoints; the controller holds at most {SHOT_SETPOINT_MAX}")
    if n and t[0] != 0:
        errs.append("the first setpoint must be at time_us = 0")
    if np.any(np.diff(t) <= 0):
        errs.append("times must strictly increase")
    if np.any(t != np.round(t)):
        errs.append("times must be whole microseconds")
    if n and t[-1] > MAX_SHOT_US:
        errs.append(f"the shot is {t[-1] / 1000:g} ms; the longest allowed is {MAX_SHOT_US / 1000:g} ms")
    on = p.on
    if n and not on[0]:
        errs.append("the first setpoint cannot be 'off'")
    if n >= 2 and not (on[-1] and on[-2]):
        errs.append("the last two setpoints must be on (the last pulse needs at least two points)")
    if p.mode == "power":
        if np.any(p.demand[on] < 0):
            errs.append("negative power demand")
    else:
        bad = on & ((p.demand < F_FLOOR_HZ) | (p.demand > F_CEIL_HZ))
        for i in np.nonzero(bad)[0][:5]:
            errs.append(f"row {i + 1}: demand {p.demand[i]:g} Hz is outside {F_FLOOR_HZ:g}-{F_CEIL_HZ:g} Hz")
    if check_commanded and p.carrier is not None:
        c, du = p.carrier, p.duty
        con = c > 0
        if np.any(con != on):
            i = int(np.nonzero(con != on)[0][0])
            errs.append(f"row {i + 1}: the commanded profile's gaps must be the demand's gaps (carrier {c[i]:g}, demand "
                        f"{'off' if not on[i] else p.demand[i]})")
        bad = con & ((c < F_FLOOR_HZ - 1e-6) | (c > F_CEIL_HZ + 1e-6))
        for i in np.nonzero(bad)[0][:5]:
            errs.append(f"row {i + 1}: carrier {c[i]:g} Hz is outside {F_FLOOR_HZ:g}-{F_CEIL_HZ:g} Hz")
        bad = con & ((du < HVPS_DUTY_MIN_PCT) | (du > HVPS_DUTY_MAX_PCT))
        for i in np.nonzero(bad)[0][:5]:
            errs.append(f"row {i + 1}: duty {du[i]:g} % is outside {HVPS_DUTY_MIN_PCT:g}-{HVPS_DUTY_MAX_PCT:g} %")
    if errs:
        raise CompError("profile does not follow the controller's or this supply's rules:\n  " + "\n  ".join(errs))


def eval_rows(t_us, values, on, tau_us):
    """The firmware's interpolation: linear between two on rows, an on row followed by an off row holds its value until
    the gap, off from an off row to the next row, the shot over at the last row's time. Returns (values, on_mask)."""
    tau = np.asarray(tau_us, dtype=float)
    n = len(t_us)
    k = np.clip(np.searchsorted(t_us, tau, side="right") - 1, 0, n - 2)
    f = np.clip((tau - t_us[k]) / (t_us[k + 1] - t_us[k]), 0.0, 1.0)
    f = np.where(on[k + 1], f, 0.0)
    v0 = np.where(np.isfinite(values[k]), values[k], 0.0)
    v1 = np.where(np.isfinite(values[k + 1]), values[k + 1], 0.0)
    out = v0 + (v1 - v0) * f
    ok = on[k] & (tau >= 0) & (tau < t_us[-1])
    return np.where(ok, out, np.nan), ok


def values_at(t_us, values, on, times):
    """Row values at given times, the last row's time included (eval_rows stops just before it)."""
    out = np.empty(len(times))
    for i, ti in enumerate(times):
        k = int(np.clip(np.searchsorted(t_us, ti, side="right") - 1, 0, len(t_us) - 1))
        if t_us[k] == ti or k == len(t_us) - 1:
            out[i] = values[k] if on[k] else np.nan
        elif not on[k]:
            out[i] = np.nan
        elif not on[k + 1]:
            out[i] = values[k]
        else:
            out[i] = values[k] + (values[k + 1] - values[k]) * (ti - t_us[k]) / (t_us[k + 1] - t_us[k])
    return out


def pulses_of(t_us, on):
    """[(start_us, end_us)] of the output's on intervals."""
    out, start = [], None
    for i in range(len(t_us)):
        if on[i] and start is None:
            start = t_us[i]
        if not on[i] and start is not None:
            out.append((start, t_us[i]))
            start = None
    if start is not None:
        out.append((start, t_us[-1]))
    return out


def demand_breakpoints(p):
    """Rows where the demand changes course (first, last, gap edges, a change of slope); rows an earlier iteration added on
    straight stretches of the demand are dropped, so the row count does not grow from one iteration to the next."""
    n = len(p.t_us)
    keep = np.zeros(n, dtype=bool)
    keep[0] = keep[-1] = True
    on = p.on
    tol = 0.006 if p.mode == "power" else 0.5
    for i in range(1, n - 1):
        if not (on[i - 1] and on[i] and on[i + 1]):
            keep[i] = True
            continue
        f = (p.t_us[i] - p.t_us[i - 1]) / (p.t_us[i + 1] - p.t_us[i - 1])
        if abs(p.demand[i - 1] + (p.demand[i + 1] - p.demand[i - 1]) * f - p.demand[i]) > tol:
            keep[i] = True
    return np.nonzero(keep)[0]


def write_profile(path, p, meta):
    lines = [f"# {k}={v}" for k, v in meta.items()]
    dname = "demand_kw" if p.mode == "power" else "demand_hz"
    lines.append(f"time_us,{dname},carrier_hz,duty_pct")
    for i in range(len(p.t_us)):
        if np.isfinite(p.demand[i]):
            d = f"{p.demand[i]:.2f}" if p.mode == "power" else f"{p.demand[i]:.1f}"
        else:
            d = "off" if p.mode == "power" else "0"
        c = p.carrier[i]
        lines.append(f"{int(round(p.t_us[i]))},{d},{c:.1f},{p.duty[i]:.2f}" if c > 0 else f"{int(round(p.t_us[i]))},{d},0,0")
    with open(path, "w", newline="") as f:
        f.write("\n".join(lines) + "\n")


# =====================================================================================================================
# The supply model (starting numbers; the iteration corrects what it gets wrong)
# =====================================================================================================================
def p_of_v(v_kv):
    return np.interp(v_kv, PV_TABLE_KV, PV_TABLE_KW)


def v_of_p(p_kw):
    return np.interp(p_kw, PV_TABLE_KW, PV_TABLE_KV)


def static_dpdv(v_kv):
    h = 0.25
    return np.maximum((p_of_v(v_kv + h) - p_of_v(v_kv - h)) / (2 * h), DPDV_MIN)


# Frequency sensitivity before anything is learned: 4 %/100 Hz measured at 19.5-20 kHz (8 bank charges), 1.7 %/100 Hz
# measured near 19.0 kHz (shots 261117039/040, same charge, only the tail moved). Linear in between, flat outside.
PRIOR_KAPPA_F = [18700.0, 19000.0, 19400.0, 21500.0]
PRIOR_KAPPA = [1.5e-4, 1.7e-4, 4.0e-4, 4.0e-4]
KAPPA_MIN, KAPPA_MAX = 0.2e-4, 8.0e-4       # what a learned sensitivity is clipped to before it is used


class Plant:
    """What the tool knows about the supply, updated after every analysed shot and kept in a JSON file next to the
    CSVs (ech_plant.json): the relative output sensitivity kappa (fraction per Hz lower) on a 100 Hz grid, and the
    gyrotron's dP/dV (kW per kV) on a 1 kV grid. Each grid point is a weighted average of what has been measured there
    (weights capped, so it keeps following a gyrotron that changes) and falls back to the prior where nothing was."""
    F = np.arange(18500.0, 21501.0, 100.0)
    V = np.arange(40.0, 76.0, 1.0)
    T = np.arange(0.0, 20.01, 0.5)          # ms into the shot, for the bank-charge offset
    W_CAP = 20.0

    def __init__(self):
        self.reset()

    def reset(self):
        self.kappa = np.interp(self.F, PRIOR_KAPPA_F, PRIOR_KAPPA)
        self.kw = np.zeros(len(self.F))
        self.dpdv_t = static_dpdv(self.V)
        self.dw = np.zeros(len(self.V))
        self.boff = np.full(len(self.T), V20_OFFSET_V)   # output ~ (bank - boff(t)) at a given profile
        self.bw = np.zeros(len(self.T))
        self.history = []
        self._build()

    def _build(self):
        self._fg = np.arange(18000.0, 22001.0, 1.0)
        k = np.interp(self._fg, self.F, self.kappa)
        c = np.concatenate([[0.0], np.cumsum(0.5 * (k[1:] + k[:-1]))])     # integral of kappa from 18 kHz up
        self._lng = float(np.interp(F_REF_HZ, self._fg, c)) - c             # ln(gain) relative to 20 kHz

    def kappa_at(self, f):
        return np.interp(f, self.F, self.kappa)

    def lngain(self, f):
        return np.interp(f, self._fg, self._lng)

    def freq_for_lngain(self, lg):
        return np.interp(-np.asarray(lg, dtype=float), -self._lng, self._fg)

    def dpdv_at(self, v):
        return np.maximum(np.interp(v, self.V, self.dpdv_t), DPDV_MIN)

    def bank_offset_at(self, t_ms):
        return np.interp(t_ms, self.T, self.boff)

    def learn_bank(self, t_ms, off, w):
        t_ms, off, w = (np.atleast_1d(np.asarray(x, dtype=float)) for x in (t_ms, off, w))
        ok = np.isfinite(t_ms) & np.isfinite(off) & (w > 0)
        if ok.any():
            self._blend(self.T, self.boff, self.bw, t_ms[ok], np.clip(off[ok], 0.0, 450.0), w[ok], self.W_CAP)

    @staticmethod
    def _blend(grid, table, wtab, x, val, w, cap):
        idx = np.clip(np.round((x - grid[0]) / (grid[1] - grid[0])).astype(int), 0, len(grid) - 1)
        for i in np.unique(idx):
            m = idx == i
            ws = float(np.sum(w[m]))
            if ws <= 0:
                continue
            mean = float(np.sum(w[m] * val[m]) / ws)
            ws = min(ws, cap / 2)
            table[i] = (wtab[i] * table[i] + ws * mean) / (wtab[i] + ws)
            wtab[i] = min(wtab[i] + ws, cap)

    def learn_kappa(self, f, k, w):
        ok = np.isfinite(f) & np.isfinite(k) & (w > 0)
        if ok.any():
            self._blend(self.F, self.kappa, self.kw, f[ok], np.clip(k[ok], KAPPA_MIN, KAPPA_MAX), w[ok], self.W_CAP)
            self._build()

    def learn_dpdv(self, v, d, w):
        ok = np.isfinite(v) & np.isfinite(d) & (w > 0)
        if ok.any():
            self._blend(self.V, self.dpdv_t, self.dw, v[ok], np.clip(d[ok], DPDV_MIN, 80.0), w[ok], self.W_CAP)

    def save(self, path):
        import json
        with open(path, "w") as f:
            json.dump({"note": "ech_compensate.py supply model: kappa = relative output rise per Hz lower; dpdv in kW/kV",
                       "f_hz": self.F.tolist(), "kappa": [float(x) for x in self.kappa], "kappa_weight": self.kw.round(3).tolist(),
                       "v_kv": self.V.tolist(), "dpdv": self.dpdv_t.round(3).tolist(), "dpdv_weight": self.dw.round(3).tolist(),
                       "t_ms": self.T.tolist(), "bank_offset_v": self.boff.round(2).tolist(), "bank_offset_weight": self.bw.round(3).tolist(),
                       "history": self.history[-200:]}, f, indent=1)

    def load(self, path):
        import json
        with open(path) as f:
            d = json.load(f)
        self.reset()
        if np.allclose(d["f_hz"], self.F) and np.allclose(d["v_kv"], self.V):
            self.kappa, self.kw = np.array(d["kappa"], float), np.array(d["kappa_weight"], float)
            self.dpdv_t, self.dw = np.array(d["dpdv"], float), np.array(d["dpdv_weight"], float)
        if "bank_offset_v" in d and np.allclose(d["t_ms"], self.T):
            self.boff, self.bw = np.array(d["bank_offset_v"], float), np.array(d["bank_offset_weight"], float)
        self.history = d.get("history", [])
        self._build()


PLANT = Plant()


def dpdv(v_kv):
    return PLANT.dpdv_at(v_kv)


def gain(f_hz):
    """Output relative to 20 kHz, from the frequency-dependent sensitivity (exp of its integral)."""
    return np.exp(PLANT.lngain(f_hz))


def freq_for_gain(g):
    return PLANT.freq_for_lngain(np.log(np.maximum(g, 1e-6)))


def v20(bank_v):
    """Settled gyrotron kV at 20 kHz, start of the first pulse."""
    return V20_SLOPE_KV_PER_V * max(bank_v - V20_OFFSET_V, 0.0)


# =====================================================================================================================
# Grid helpers
# =====================================================================================================================
class Grid:
    """The shot on a 10 us grid: tau (us), on mask, pulse index (-1 off), time since the pulse's start, controllable mask
    (past the settle window, before the pulse's last 0.1 ms) and accumulated on-time (ms)."""

    def __init__(self, p, settle_ms):
        self.tau = np.arange(0.0, p.duration_us, GRID_US)
        _, self.on = eval_rows(p.t_us, np.where(p.on, 1.0, np.nan), p.on, self.tau)
        self.pulses = pulses_of(p.t_us, p.on)
        self.pidx = np.full(len(self.tau), -1)
        self.since = np.zeros(len(self.tau))
        for k, (a, b) in enumerate(self.pulses):
            m = (self.tau >= a) & (self.tau < b)
            self.pidx[m] = k
            self.since[m] = (self.tau[m] - a) / 1000.0
        self.ctrl = self.on & (self.since >= settle_ms)
        for k, (a, b) in enumerate(self.pulses):
            self.ctrl &= ~((self.pidx == k) & (self.tau >= b - 100.0))
        self.on_ms = np.cumsum(self.on) * GRID_US / 1000.0


def flatten_demand(p, t_ms, y, *, settle_ms=SETTLE_MS, t0_ms=0.0):
    """--flatten: rescale each pulse's demand (in place) so that its mean over the controllable window equals the mean
    measured there, i.e. aim at the level the shot already reached and correct only the shape around it. Returns the
    new per-pulse mean levels. Power mode only (a frequency demand already means 'this shape, whatever the level')."""
    if p.mode != "power":
        raise CompError("--flatten needs a power demand (demand_kw)")
    g = Grid(p, settle_ms)
    tau_ms = g.tau / 1000.0
    if t_ms[0] > t0_ms or t_ms[-1] < t0_ms + tau_ms[-1]:
        raise CompError("the measurement does not cover the shot")
    ym = resample(t_ms, y, tau_ms + t0_ms)
    y_star = eval_rows(p.t_us, p.demand, p.on, g.tau)[0]
    levels = []
    demand = p.demand.copy()
    for k, (a, b) in enumerate(g.pulses):
        c = g.ctrl & (g.pidx == k) & np.isfinite(y_star)
        if not np.any(c):
            raise CompError(f"pulse {k + 1} has no flat top past the {settle_ms:g} ms rise: nothing to flatten")
        meas, dem = float(np.mean(ym[c])), float(np.mean(y_star[c]))
        if meas <= 0 or dem <= 0:
            raise CompError(f"pulse {k + 1}: no output measured, nothing to flatten to")
        rows = p.on & (p.t_us >= a) & (p.t_us <= b) & np.isfinite(p.demand)
        demand[rows] = p.demand[rows] * meas / dem
        levels.append(meas)
    p.demand = demand
    return levels


def extend_into_pulse(values, grid):
    """Fill each pulse's samples outside its controllable window with the nearest controllable value of the same pulse
    (backwards over the rise, forwards over the end); a pulse with no controllable samples gets NaN."""
    out = np.full(len(values), np.nan)
    for k in range(len(grid.pulses)):
        m = np.nonzero(grid.pidx == k)[0]
        c = m[grid.ctrl[m]]
        if len(c) == 0:
            continue
        v = values[c]
        out[m] = np.interp(grid.tau[m], grid.tau[c], v)          # np.interp holds the end values flat
    return out


def smooth(x, n):
    if n <= 1:
        return x
    k = np.ones(n) / n
    return np.convolve(x, k, mode="same")


def smooth_edge(x, n):
    """smooth() without the fall-off at the ends (the ends are held, not padded with zeros)."""
    if n <= 1:
        return x
    return smooth(np.pad(x, n, mode="edge"), n)[n:-n]


# =====================================================================================================================
# First-shot profile (no measurement yet)
# =====================================================================================================================
def initial_profile(p, bank_v, *, max_kv=DEFAULT_MAX_KV, settle_ms=SETTLE_MS, tol_hz=5.0, duty=DEFAULT_DUTY,
                    max_rows=SHOT_SETPOINT_MAX, fixed_times_us=None):
    """A first frequency profile from the model. Power target: the voltage that gives the power (+ the rise that held
    the power flat on 2026-11-17); frequency profile: the demand itself."""
    check_profile(p, check_commanded=False)
    g = Grid(p, settle_ms)
    notes = []
    if p.mode == "frequency":
        f = eval_rows(p.t_us, p.demand, p.on, g.tau)[0]
        q = make_profile(p, g, f, duty, tol_hz, max_rows, fixed_times_us, np.where(g.on, F_FLOOR_HZ, np.nan))
        return q, {"kind": "initial", "notes": notes, "grid": g, "f_new": f, "v_pred": None}
    if bank_v is None:
        raise CompError("--bank-volts is needed for the first-shot profile")
    avail0 = v20(bank_v)
    if avail0 <= 0:
        raise CompError(f"a bank of {bank_v:g} V gives no output in the model (offset {V20_OFFSET_V:g} V)")
    p_star = eval_rows(p.t_us, p.demand, p.on, g.tau)[0]
    v_star = v_of_p(np.nan_to_num(p_star)) * (1 + HOLD_POWER_V_RISE_PER_MS * np.maximum(g.since - settle_ms, 0))
    over = g.on & (v_star > max_kv)
    if over.any():
        notes.append(f"power demand needs more than {max_kv:g} kV: capped at {max_kv:g} kV")
    v_star = np.minimum(v_star, max_kv)
    droop = DROOP_PER_MS * (v_star / REF_KV) ** 2 * g.on_ms
    later = np.where(g.pidx > 0, (1 - GAP_DEFICIT) ** np.maximum(g.pidx, 0), 1.0)
    avail = avail0 * np.maximum(1 - droop, 0.2) * later
    f = freq_for_gain(v_star / avail)
    f = np.where(g.ctrl, f, np.nan)
    f = extend_into_pulse(f, g)
    lo, hi = g.on & (f < F_FLOOR_HZ), g.on & (f > F_CEIL_HZ)
    if lo.any():
        notes.append(f"demand not reachable at {bank_v:g} V: {F_FLOOR_HZ / 1000:g} kHz floor reached at "
                     f"{fmt_iv(intervals(g.tau / 1000, lo))} ms")
    if hi.any():
        notes.append(f"bank {bank_v:g} V gives more than the demand even at {F_CEIL_HZ / 1000:g} kHz at "
                     f"{fmt_iv(intervals(g.tau / 1000, hi))} ms: lower the bank charge")
    f = np.clip(f, F_FLOOR_HZ, F_CEIL_HZ)
    v_pred = avail * gain(f)
    q = make_profile(p, g, f, duty, tol_hz, max_rows, fixed_times_us, np.where(g.on, F_FLOOR_HZ, np.nan))
    return q, {"kind": "initial", "notes": notes, "grid": g, "f_new": f, "v_pred": v_pred, "v_star": v_star}


# =====================================================================================================================
# Measurement
# =====================================================================================================================
def read_mds(shot, target, tree_name="ech"):
    """(shot, t_ms, V_kV, P_kW, bank_param, target_param) for one ECH shot; shot None/0 = the latest WHAM shot."""
    try:
        import MDSplus as mds
    except ImportError:
        raise CompError("the MDSplus Python package is not installed (or use --data FILE for an offline run)")
    if not shot:
        try:
            shot = int(mds.Tree.getCurrent("wham"))                     # the shot number only; no tree is opened
        except Exception as e:                                          # noqa: BLE001
            raise CompError(f"could not get the current WHAM shot ({e}); pass --shot N")
    try:
        t = mds.Tree(tree_name, shot, "readonly")
    except Exception as e:                                              # noqa: BLE001
        raise CompError(f"could not open the {tree_name} tree for shot {shot}: {e}")

    def sig(path):
        n = t.getNode(path)
        return np.asarray(n.dim_of().data(), dtype=float) * 1e3, np.asarray(n.data(), dtype=float)

    try:
        tv, v = sig(NODE_V)
        tp, pw = sig(NODE_P[target])
    except Exception as e:                                              # noqa: BLE001
        raise CompError(f"shot {shot}: could not read the gyrotron voltage / forward power ({e})")
    bank = tgt = None
    try:
        bank = float(t.getNode(NODE_BANK).data())
    except Exception:                                                   # noqa: BLE001
        pass
    try:
        tgt = str(t.getNode(NODE_TARGET).data()).strip()
    except Exception:                                                   # noqa: BLE001
        pass
    pw = np.interp(tv, tp, pw)
    return shot, tv, v / 1000.0, pw, bank, tgt


def read_data_csv(path):
    """Offline measurement: time_ms,gyrotron_kv,power_kw (a header line is allowed)."""
    rows = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            try:
                a = [float(x) for x in s.split(",")[:3]]
                if len(a) == 3:
                    rows.append(a)
            except ValueError:
                continue
    if len(rows) < 10:
        raise CompError(f"{path}: need columns time_ms,gyrotron_kv,power_kw")
    a = np.array(rows)
    a = a[np.argsort(a[:, 0])]
    return a[:, 0], a[:, 1], a[:, 2]


def resample(t_ms, x, grid_ms):
    """Cell averages on grid_ms (the measurement is much finer than 10 us)."""
    dt = grid_ms[1] - grid_ms[0]
    cum = np.concatenate([[0.0], np.cumsum(0.5 * (x[1:] + x[:-1]) * np.diff(t_ms))])
    hi = np.interp(grid_ms + dt / 2, t_ms, cum)
    lo = np.interp(grid_ms - dt / 2, t_ms, cum)
    return (hi - lo) / dt


def measured_pulses(tau_ms, v, frac=0.3):
    """[(start_ms, end_ms)] where the voltage is above frac of its peak."""
    on = v > frac * np.nanmax(v)
    return intervals(tau_ms, on, min_len=10)


POWER_SCALE_RANGE = (0.5, 2.0)   # a meter calibration factor outside this is a typo, not a calibration


def check_power_scale(value):
    """The power scale K (experimental-meter kW per dummy-load-meter kW) from the command line or the CSV; 1 if unset."""
    if value is None or str(value).strip() == "":
        return 1.0
    try:
        k = float(value)
    except ValueError:
        raise CompError(f"power scale {value!r} is not a number")
    if not POWER_SCALE_RANGE[0] <= k <= POWER_SCALE_RANGE[1]:
        raise CompError(f"power scale {k:g} is outside {POWER_SCALE_RANGE[0]:g}-{POWER_SCALE_RANGE[1]:g}: "
                        f"it is the ratio of two meters' readings for the same power, normally close to 1")
    return k


def target_names_match(target, mds_target):
    if not mds_target:
        return None
    s = mds_target.strip().lower()
    return any(s == x or s.startswith(x) for x in MDS_TARGET_NAMES[target])


# =====================================================================================================================
# The correction
# =====================================================================================================================
class Result:
    pass


def compensate(p, t_ms, v_kv, p_kw, bank_v, *, next_bank_v=None, gain_ilc=0.7, max_step_hz=DEFAULT_MAX_STEP_HZ,
               max_kv=DEFAULT_MAX_KV, settle_ms=SETTLE_MS, lead_ms=LEAD_MS, t0_ms=0.0, tol_hz=5.0, kappa=KAPPA,
               max_rows=SHOT_SETPOINT_MAX, v_ref=None, prev=None, gain_measured=0.9, guard_kappa=2.0e-4,
               learn=True, shape_rise=True, rise_max_hz=300.0, fixed_times_us=None):
    """v_ref (frequency profile only): the voltage target's anchor from the first analysed shot. Re-anchoring on every
    shot would let each correction move its own target.
    prev: the shot before this one, {"profile": Profile, "t_ms", "v_kv", "p_kw", "bank": V}. Where the frequency changed
    between the two shots, the voltage change measures the real sensitivity there: it is used for this correction
    (with gain_measured instead of gain_ilc), folded into PLANT (learn=True), and where it is below guard_kappa (we are
    near the gain peak) the frequency is not lowered any more.
    shape_rise: inside each pulse's rise window the frequency is the flat top's starting frequency PLUS a raise r(t) >= 0,
    learned from how far the output ran above its target there (looked up a lag ahead). r only ever adds frequency on top
    of the flat top's start, so the rise can never be pushed closer to resonance than the flat top itself; it can shrink
    back to 0 when it overdid it. Capped at rise_max_hz."""
    if p.carrier is None:
        raise CompError("this CSV has no commanded frequencies yet: run without a shot to get the first-shot profile")
    check_profile(p)
    if not 0 < gain_ilc <= 1:
        raise CompError("--gain must be in (0, 1]")
    g = Grid(p, settle_ms)
    tau_ms = g.tau / 1000.0
    f_cmd = eval_rows(p.t_us, np.where(p.carrier > 0, p.carrier, np.nan), p.carrier > 0, g.tau)[0]
    duty = eval_rows(p.t_us, np.where(p.carrier > 0, p.duty, np.nan), p.carrier > 0, g.tau)[0]

    # measurement on the shot's grid
    if t_ms[0] > t0_ms or t_ms[-1] < t0_ms + tau_ms[-1]:
        raise CompError(f"the measurement ({t_ms[0]:.2f}..{t_ms[-1]:.2f} ms) does not cover the shot "
                        f"({t0_ms:.2f}..{t0_ms + tau_ms[-1]:.2f} ms)")
    v = resample(t_ms, v_kv, tau_ms + t0_ms)
    pw = resample(t_ms, p_kw, tau_ms + t0_ms)
    mp = measured_pulses(tau_ms, v)
    notes = []
    if len(mp) != len(g.pulses):
        raise CompError(f"the shot has {len(mp)} voltage pulse(s), the profile {len(g.pulses)}: wrong shot or wrong CSV?")
    delays = [m[0] - a / 1000.0 for m, (a, b) in zip(mp, g.pulses)]
    if max(abs(d) for d in delays) > 0.5:
        notes.append(f"pulses start {', '.join(f'{d:.2f}' for d in delays)} ms after their commanded times (expected "
                     f"~0.1 ms): check --t0-ms")

    # what to reach
    if p.mode == "power":
        y = pw
        y_star = eval_rows(p.t_us, p.demand, p.on, g.tau)[0]
        if np.nanmax(pw[g.on]) < 5.0:
            notes.append("no forward power measured: wrong --target, or the gyrotron did not oscillate")
    else:
        y = v
        f_dem = eval_rows(p.t_us, p.demand, p.on, g.tau)[0]
        ref = np.nonzero(g.ctrl & (g.pidx == 0))[0]
        if len(ref) == 0:
            raise CompError(f"the first pulse is shorter than the {settle_ms:g} ms rise: nothing to anchor the target to")
        k_ref = ref[:max(1, int(200 / GRID_US))]
        if v_ref is None:
            v_ref = float(np.mean(v[k_ref]))
        f_ref = float(np.mean(f_dem[k_ref]))
        y_star = v_ref * gain(f_dem) / gain(f_ref)
        for k in range(len(g.pulses)):
            gk = gain(f_dem[g.pidx == k])
            if len(gk) and np.nanmax(gk) / np.nanmin(gk) > 1.10:
                notes.append(f"the demand frequencies of pulse {k + 1} alone imply a {100 * (np.nanmax(gk) / np.nanmin(gk) - 1):.0f} % "
                             f"voltage change: a frequency demand means 'this, as if the bank never drooped', so a profile "
                             f"already hand-corrected for droop asks for far more than intended -- use a power target instead")

    # the error where it is meaningful, smoothed (ripple), looked up a lag ahead
    err = np.where(g.ctrl, y_star - y, np.nan)
    err_s = np.full(len(err), np.nan)
    v_s = np.full(len(err), np.nan)
    for k in range(len(g.pulses)):
        c = np.nonzero(g.ctrl & (g.pidx == k))[0]
        if len(c):
            n = max(1, int(200 / GRID_US))
            err_s[c] = smooth(np.pad(err[c], n, mode="edge"), n)[n:-n]
            v_s[c] = smooth(np.pad(v[c], n, mode="edge"), n)[n:-n]
    lead = int(round(lead_ms * 1000 / GRID_US))
    ahead = np.full(len(err), np.nan)
    v_ahead = np.full(len(err), np.nan)
    for k in range(len(g.pulses)):
        c = np.nonzero(g.ctrl & (g.pidx == k))[0]
        if len(c):
            j = np.minimum(c + lead, c[-1])
            ahead[c] = err_s[j]
            v_ahead[c] = v_s[j]
    # sensitivity: measured from the previous shot where the frequency moved, the learned model elsewhere
    k_meas = np.full(len(err), np.nan)
    w_meas = np.zeros(len(err))
    d_meas = np.full(len(err), np.nan)
    learned = {"pairs": 0}
    if prev is not None:
        if abs(float(prev["bank"]) - float(bank_v)) > 0.5:
            notes.append(f"the previous shot ran at {prev['bank']:g} V, this one at {bank_v:g} V: nothing learned from the pair")
        else:
            pp = prev["profile"]
            f_prev = eval_rows(pp.t_us, np.where(pp.carrier > 0, pp.carrier, np.nan), pp.carrier > 0, g.tau)[0]
            pt = prev["t_ms"]
            if pt[0] <= t0_ms and pt[-1] >= t0_ms + tau_ms[-1]:
                vp = resample(pt, prev["v_kv"], tau_ms + t0_ms)
                pwp = resample(pt, prev["p_kw"], tau_ms + t0_ms)
                n1 = max(1, int(1000 / GRID_US))                                  # 1 ms smoothing of the differences
                for k in range(len(g.pulses)):
                    c = np.nonzero(g.ctrl & (g.pidx == k) & np.isfinite(f_prev))[0]
                    if len(c) < 10:
                        continue
                    sm = lambda x: smooth(np.pad(x[c], n1, mode="edge"), n1)[n1:-n1]   # noqa: E731
                    v1, v0, p1, p0 = sm(v), sm(vp), sm(pw), sm(pwp)
                    j = np.minimum(np.arange(len(c)) + lead, len(c) - 1)          # the response a lag later
                    dlnv = np.log(np.maximum(v1[j], 1.0)) - np.log(np.maximum(v0[j], 1.0))
                    dv, dp = v1[j] - v0[j], p1[j] - p0[j]
                    dfk = f_cmd[c] - f_prev[c]
                    big = np.abs(dfk) >= 12.0
                    k_meas[c[big]] = -dlnv[big] / dfk[big]
                    w_meas[c] = np.clip((np.abs(dfk) - 8.0) / 25.0, 0.0, 1.0)   # 20 Hz -> 0.48, 33 Hz -> 1
                    okv = np.abs(dv) >= 0.4
                    d_meas[c[okv]] = dp[okv] / dv[okv]
                learned["pairs"] = 1
                use = w_meas >= 0.4
                learned["kappa_samples"] = int(use.sum())
                if learn:
                    PLANT.learn_kappa(0.5 * (f_cmd + f_prev), k_meas, np.where(use, w_meas, 0.0) / 100.0)
                    wd = np.where(np.isfinite(d_meas) & g.ctrl, 1.0, 0.0) / 100.0
                    PLANT.learn_dpdv(v_s, d_meas, wd)
            else:
                notes.append("the previous shot's measurement does not cover this shot: nothing learned from the pair")
    k_model = PLANT.kappa_at(f_cmd)
    k_use = np.where(np.isfinite(k_meas), w_meas * np.clip(np.nan_to_num(k_meas), KAPPA_MIN, KAPPA_MAX) + (1 - w_meas) * k_model,
                     k_model)
    guard = g.ctrl & (w_meas >= 0.4) & np.isfinite(k_meas) & (k_meas < guard_kappa)
    sens_v = k_use * np.maximum(v_ahead, 1.0)                             # kV per Hz lower
    sens = sens_v * dpdv(v_ahead) if p.mode == "power" else sens_v        # kW or kV per Hz lower
    gain_arr = np.where(w_meas >= 0.4, gain_measured, gain_ilc)
    df = -gain_arr * ahead / sens
    df = extend_into_pulse(df, g)

    # rise shaping: a raise r >= 0 over the flat top's start, learned from the overshoot
    r_new = np.zeros(len(err))
    rise_floor = np.full(len(err), np.nan)                                 # no rise below its flat top's (new) start
    if shape_rise:
        n2 = max(1, int(200 / GRID_US))
        for k in range(len(g.pulses)):
            idx = np.nonzero(g.pidx == k)[0]
            c = np.nonzero(g.ctrl & (g.pidx == k))[0]
            if len(c) == 0:
                continue
            rise = idx[g.since[idx] < settle_ms]
            if len(rise) == 0:
                continue
            base = f_cmd[c[0]]                                    # the flat top's commanded start
            r_old = np.maximum(f_cmd[rise] - base, 0.0)
            j = np.minimum(rise + lead, idx[-1])
            e_all = np.where(g.pidx == k, y_star - y, 0.0)
            e = smooth(np.pad(e_all[idx], n2, mode="edge"), n2)[n2:-n2][j - idx[0]]
            up = y[j] > 0.5 * y_star[j]                           # the output is near its target: the overshoot region
            vv = np.maximum(v[j], 1.0)
            sr = PLANT.kappa_at(f_cmd[rise]) * vv * (dpdv(vv) if p.mode == "power" else 1.0)
            dr = np.where(up, -gain_ilc * e / sr, -r_old)         # below the target: let any raise fall back to 0
            dr = np.clip(dr, -max_step_hz, max_step_hz)
            r_new[rise] = np.clip(r_old + dr, 0.0, rise_max_hz)
            # the rise follows the flat top's (corrected) start, plus the raise
            f_rise = (base + df[c[0]]) + r_new[rise]                # the flat top's new start, plus the raise
            rise_floor[rise] = base + df[c[0]]
            df[rise] = np.maximum(f_rise, f_cmd[rise] - max_step_hz) - f_cmd[rise]   # still <= max_step towards resonance
    clipped_step = g.on & (np.abs(df) > max_step_hz)
    df = np.clip(df, -max_step_hz, max_step_hz)

    # bank charge change for the next shot
    bank_change = next_bank_v is not None and next_bank_v != bank_v
    vb_ratio = np.ones(len(err))
    if bank_change:
        off = PLANT.bank_offset_at(tau_ms)                          # learned: output ~ (bank - off(t))
        if next_bank_v <= float(np.max(off)) + 10 or bank_v <= float(np.max(off)) + 10:
            raise CompError(f"bank charges {bank_v:g}/{next_bank_v:g} V are too close to the model's offset "
                            f"({float(np.max(off)):.0f} V) to rescale")
        vb_ratio = (next_bank_v - off) / (bank_v - off)
        notes.append(f"bank {bank_v:g} -> {next_bank_v:g} V: frequencies rescaled to keep the same output (output "
                     f"{100 * (float(np.nanmin(vb_ratio[g.on])) - 1):+.1f}..{100 * (float(np.nanmax(vb_ratio[g.on])) - 1):+.1f} % "
                     f"from the charge; less certain than a correction at the same charge)")
    f_new = f_cmd + df
    if bank_change:
        f_new = freq_for_gain(gain(f_new) / vb_ratio)

    # near the gain peak: where lowering the frequency bought less than guard_kappa, lower nothing that is at or below
    # the highest such frequency (and nothing else below it)
    guarded = np.zeros(len(err), dtype=bool)
    if guard.any():
        f_trip = float(np.nanmax(f_cmd[guard]))
        floor_g = np.minimum(f_cmd, f_trip)
        guarded = g.on & (f_new < floor_g - 0.05)
        f_new = np.where(g.on, np.maximum(f_new, floor_g), f_new)
        kk = k_meas[guard]
        notes.append(f"near the gain peak: lowering the frequency bought only {100 * 100 * np.nanmedian(kk):.1f} %/100 Hz at "
                     f"{fmt_iv(intervals(tau_ms, guard))} ms (guard {100 * 100 * guard_kappa:.0f} %/100 Hz) -- nothing at or "
                     f"below {f_trip:.0f} Hz is lowered; more output there needs more bank charge")

    # voltage ceiling: predicted V = measured V * bank ratio * gain ratio
    v_base = extend_into_pulse(np.where(g.ctrl, v_s, np.nan), g)
    with np.errstate(divide="ignore", invalid="ignore"):
        g_max = gain(f_cmd) * max_kv / (np.maximum(v_base, 1e-3) * vb_ratio)
    # + the setpoint fitting's tolerance, so the setpoints actually written still respect the ceiling
    f_vlim = freq_for_gain(g_max) + (FIXED_DIP_TOL_HZ if fixed_times_us is not None else tol_hz)
    vcap = g.on & (f_new < f_vlim)
    f_new = np.where(vcap, f_vlim, f_new)

    floor = g.on & (f_new < F_FLOOR_HZ)
    ceil = g.on & (f_new > F_CEIL_HZ)
    f_new = np.clip(f_new, F_FLOOR_HZ, F_CEIL_HZ)
    f_new[~g.on] = np.nan
    v_pred = np.where(g.ctrl, v_base * vb_ratio * gain(f_new) / gain(f_cmd), np.nan)   # after the rise only

    base = f_cmd if not bank_change else np.clip(freq_for_gain(gain(f_cmd) / vb_ratio), F_FLOOR_HZ, F_CEIL_HZ)
    f_lower = np.maximum(F_FLOOR_HZ, base - max_step_hz)                   # what no setpoint line may dip below
    f_lower = np.maximum(f_lower, np.where(np.isfinite(f_vlim), f_vlim, F_FLOOR_HZ))
    if guard.any():
        f_lower = np.maximum(f_lower, floor_g)
    if bank_change:
        rise_floor = freq_for_gain(gain(rise_floor) / vb_ratio)
    f_lower = np.where(np.isfinite(rise_floor), np.maximum(f_lower, rise_floor), f_lower)
    f_lower = np.where(g.on, np.minimum(f_lower, F_CEIL_HZ), np.nan)
    q = make_profile(p, g, f_new, duty, tol_hz, max_rows, fixed_times_us, f_lower)
    check_profile(q)
    f_q = eval_rows(q.t_us, np.where(q.carrier > 0, q.carrier, np.nan), q.carrier > 0, g.tau)[0]
    # predictions from the setpoints actually written (not the ideal curve they approximate)
    v_pred = np.where(g.ctrl, v_base * vb_ratio * gain(f_q) / gain(f_cmd), np.nan)
    moved = (f_q - f_cmd)[g.on]
    step = float(np.nanmax(np.abs(moved))) if g.on.any() else 0.0
    # The per-shot limit guards the correction's steps TOWARDS resonance. Measured from the profile rescaled to the next
    # shot's bank charge (that rescale keeps the output the same; the floor and the voltage ceiling still apply to it),
    # and only downwards (a move up -- the voltage ceiling, a higher bank -- lowers the output).
    down = float(-np.nanmin((f_q - base)[g.on])) if g.on.any() else 0.0
    if down > max_step_hz + tol_hz + 1:
        raise CompError(f"internal: the correction moves {down:.0f} Hz towards resonance, more than --max-step-hz")

    r = Result()
    r.profile, r.grid, r.notes = q, g, notes
    r.v_ref = v_ref if p.mode == "frequency" else None
    r.y, r.y_star, r.v, r.pw, r.f_cmd, r.f_new, r.v_pred = y, y_star, v, pw, f_cmd, f_new, v_pred
    r.f_profile = f_q
    r.delays = delays
    r.rms_pct = []
    for k in range(len(g.pulses)):
        c = g.ctrl & (g.pidx == k)
        if c.any():
            scale = max(float(np.nanmean(np.abs(y_star[c]))), 1e-6)
            r.rms_pct.append(100.0 * float(np.sqrt(np.nanmean((y[c] - y_star[c]) ** 2))) / scale)
        else:
            r.rms_pct.append(float("nan"))
    r.n_step, r.n_vcap, r.n_floor, r.n_ceil = (int(x.sum()) for x in (clipped_step, vcap, floor, ceil))
    r.n_guard = int(guarded.sum())
    r.k_meas, r.w_meas, r.learned = k_meas, w_meas, learned
    # predicted next shot, for the plot: this shot's measurement changed by what the new frequencies (and bank charge)
    # should do -- relative voltage change from the sensitivity model, applied one output lag later; power through dP/dV
    dl = np.where(g.on, np.log(np.maximum(gain(f_q), 1e-9)) - np.log(np.maximum(gain(f_cmd), 1e-9)) + np.log(vb_ratio), 0.0)
    dl_lag = np.zeros(len(dl))
    for k in range(len(g.pulses)):
        idx = np.nonzero(g.pidx == k)[0]
        if len(idx):
            src = np.maximum(np.arange(len(idx)) - lead, 0)
            dl_lag[idx] = dl[idx][src]
    v_sm = smooth_edge(np.nan_to_num(v), max(1, int(100 / GRID_US)))
    r.v_pred_plot = np.where(g.on, v_sm * np.exp(dl_lag), np.nan)
    r.p_pred_plot = (np.where(g.on, pw + dpdv(np.maximum(v_sm, 1.0)) * v_sm * (np.exp(dl_lag) - 1.0), np.nan)
                     if p.mode == "power" else None)
    r.rise_raise = r_new
    r.max_v_pred = float(np.nanmax(v_pred)) if np.isfinite(v_pred).any() else float("nan")
    r.max_step = step
    r.overshoot_pct = []
    for k in range(len(g.pulses)):
        m = (g.pidx == k) & (g.since < settle_ms + 1.0) & np.isfinite(y_star) & (y_star > 0)
        r.overshoot_pct.append(float(np.nanmax(100.0 * (smooth(y, 20)[m] - y_star[m]) / y_star[m])) if m.any() else float("nan"))
    r.limits = {k: intervals(tau_ms, m) for k, m in (("step", clipped_step), ("vmax", vcap), ("floor", floor), ("ceil", ceil),
                                                      ("guard", guarded))}
    return r


def train(items, *, reset=True, settle_ms=SETTLE_MS, top_range_v=60.0, log=print):
    """Learn PLANT from recorded shots, in firing order: items = [{"shot", "profile" (the Profile that fired, with
    carrier_hz), "t_ms", "v_kv", "p_kw", "bank"}].
      1. frequency sensitivity (and dP/dV) from every two consecutive shots at the same bank charge;
      2. from every profile fired at 3+ bank charges: the bank offset (output ~ bank - offset, per 0.5 ms of the shot)
         and dP/dV between neighbouring charges, both fitted on the charges within top_range_v of the highest one.
    Returns a summary dict."""
    if reset:
        PLANT.reset()
    summary = {"pairs": 0, "pair_samples": 0, "groups": 0, "bank_bins": 0, "dpdv_points": 0, "skipped": []}
    prev = None
    for it in items:
        if prev is not None and abs(prev["bank"] - it["bank"]) < 0.5:
            try:
                r = compensate(it["profile"], it["t_ms"], it["v_kv"], it["p_kw"], it["bank"], prev=prev, learn=True,
                               settle_ms=settle_ms, shape_rise=False)
                if r.learned.get("pairs"):
                    summary["pairs"] += 1
                    summary["pair_samples"] += r.learned.get("kappa_samples", 0)
                    PLANT.history.append({"shot": str(it["shot"]), "prev_shot": str(prev["shot"]), "bank_volts": it["bank"],
                                          "kappa_samples": r.learned.get("kappa_samples", 0), "note": "trained"})
            except CompError as e:
                summary["skipped"].append(f"{prev['shot']}->{it['shot']}: {str(e).splitlines()[0]}")
        prev = it
    groups = {}
    for it in items:
        pr = it["profile"]
        key = tuple((int(t), round(float(c), 1)) for t, c in zip(pr.t_us, pr.carrier))
        groups.setdefault(key, []).append(it)
    for grp in groups.values():
        banks = sorted({round(it["bank"], 1) for it in grp})
        if len(banks) < 3:
            continue
        top = max(banks)
        use = sorted([it for it in grp if it["bank"] >= top - top_range_v], key=lambda it: it["bank"])
        if len({round(it["bank"], 1) for it in use}) < 3:
            continue
        summary["groups"] += 1
        g = Grid(use[0]["profile"], settle_ms)
        tau_ms = g.tau / 1000.0
        V = np.array([resample(it["t_ms"], it["v_kv"], tau_ms) for it in use])
        P = np.array([resample(it["t_ms"], it["p_kw"], tau_ms) for it in use])
        B = np.array([it["bank"] for it in use])
        for tb in PLANT.T:
            m = g.on & (g.since >= 0.8) & (tau_ms >= tb - 0.25) & (tau_ms < tb + 0.25)
            if m.sum() < 10:
                continue
            vm, pm = V[:, m].mean(axis=1), P[:, m].mean(axis=1)
            a, c = np.polyfit(B, vm, 1)
            if a <= 0:
                continue
            resid = vm - (a * B + c)
            r2 = 1 - np.sum(resid ** 2) / max(np.sum((vm - vm.mean()) ** 2), 1e-9)
            if r2 < 0.95:
                continue
            PLANT.learn_bank(tb, -c / a, len(use) / 4.0)
            summary["bank_bins"] += 1
            for i in range(len(B) - 1):                               # dP/dV between neighbouring charges
                dv, dp = vm[i + 1] - vm[i], pm[i + 1] - pm[i]
                if dv > 0.3 and pm[i] > 10:
                    PLANT.learn_dpdv(np.array([0.5 * (vm[i] + vm[i + 1])]), np.array([dp / dv]), np.array([0.25]))
                    summary["dpdv_points"] += 1
    PLANT._build()
    return summary


def read_manifest(path):
    """shot,profile_csv,bank_volts[,data_csv] -- paths relative to the manifest; data from MDS when data_csv is empty."""
    base = os.path.dirname(os.path.abspath(path))
    rows = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#") or s.lower().startswith("shot"):
                continue
            c = [x.strip() for x in s.split(",")]
            rows.append({"shot": c[0], "profile_csv": os.path.join(base, c[1]), "bank": float(c[2]),
                         "data_csv": os.path.join(base, c[3]) if len(c) > 3 and c[3] else None})
    return rows


FIXED_DIP_TOL_HZ = 15.0   # with fixed setpoint times, how far a straight line may dip below the lower bound between points


def parse_fixed_times(text):
    """'0:15:1' (start:stop:step, ms, stop included) or '0,1,2.5,...' (ms) -> setpoint times in us."""
    if text is None or str(text).strip() == "":
        return None
    t = str(text).strip()
    if ":" in t:
        a, b, c = (float(x) for x in t.split(":"))
        ms = np.arange(a, b + c / 2, c)
    else:
        ms = np.array([float(x) for x in t.split(",")])
    return np.unique(np.round(ms * 1000.0))


def _fixed_profile(p, g, f_grid, duty_grid, times_us, f_lower):
    """The best frequencies at fixed setpoint times: least squares of the firmware's straight lines between them against
    f_grid, then each point raised to the highest lower bound on either side of it, so no straight line dips below
    f_lower anywhere (the floor, the voltage ceiling, the guard and the per-shot limit live in f_lower)."""
    T = np.asarray(times_us, dtype=float)
    if T[0] != 0 or abs(T[-1] - p.duration_us) > 0.5:
        raise CompError(f"fixed setpoint times must start at 0 and end at the shot's end ({p.duration_us / 1000:g} ms); "
                        f"they run {T[0] / 1000:g}-{T[-1] / 1000:g} ms")
    bp = demand_breakpoints(p)
    missing = [t for t in p.t_us[bp] if not np.any(np.abs(T - t) < 0.5)]
    if missing:
        raise CompError("the demand changes course at " + ", ".join(f"{t / 1000:g}" for t in missing[:6]) +
                        " ms, which are not among the fixed setpoint times")
    demand = values_at(p.t_us, p.demand, p.on, T)
    on_t = np.isfinite(demand)
    K = len(T)
    use = g.on & np.isfinite(f_grid)
    tau, f = g.tau[use], f_grid[use]
    k = np.clip(np.searchsorted(T, tau, side="right") - 1, 0, K - 2)
    a = (tau - T[k]) / (T[k + 1] - T[k])
    a = np.where(on_t[k + 1], a, 0.0)                     # on -> off: the firmware holds the value
    A = np.zeros((len(tau), K))
    A[np.arange(len(tau)), k] = 1 - a
    A[np.arange(len(tau)), k + 1] += a
    cols = np.nonzero(on_t)[0]
    A = A[:, cols]
    D2 = np.diff(np.eye(len(cols)), 2, axis=0)
    lam = 1e-6 * max(float(np.mean(np.sum(A * A, axis=0))), 1e-9)
    x = np.linalg.solve(A.T @ A + lam * (D2.T @ D2) + 1e-12 * np.eye(len(cols)), A.T @ f)
    vals = np.zeros(K)
    vals[cols] = x
    dip = 0.0
    if f_lower is not None:
        lbk = values_at(g.tau, np.where(np.isfinite(f_lower), f_lower, -np.inf), np.ones(len(g.tau), bool), T)
        vals = np.where(on_t, np.maximum(vals, np.nan_to_num(lbk, nan=-np.inf)), vals)   # every point >= its bound
        lb = f_lower[use]
        okb = np.isfinite(lb)
        for _ in range(200):                                       # then no line dips more than FIXED_DIP_TOL_HZ below
            line = vals[k] * (1 - a) + vals[k + 1] * a
            d = np.where(okb, lb - line, -np.inf)
            j = int(np.argmax(d))
            if d[j] <= FIXED_DIP_TOL_HZ:
                break
            vals[k[j]] += d[j] - FIXED_DIP_TOL_HZ
            if a[j] > 0:
                vals[k[j] + 1] += (d[j] - FIXED_DIP_TOL_HZ) * a[j]
        line = vals[k] * (1 - a) + vals[k + 1] * a
        dip = float(np.max(np.where(okb, lb - line, -np.inf))) if okb.any() else 0.0
    carrier = np.where(on_t, np.round(np.clip(vals, F_FLOOR_HZ, F_CEIL_HZ), 1), 0.0)
    di = np.interp(T, g.tau, duty_grid)
    duty = np.where(on_t, np.round(np.clip(di, HVPS_DUTY_MIN_PCT, HVPS_DUTY_MAX_PCT), 2), 0.0)
    q = Profile(T, demand, carrier, duty, p.mode, p.meta)
    q.max_dip_hz = max(dip, 0.0)
    return q


def make_profile(p, g, f_grid, duty_grid, tol_hz, max_rows=SHOT_SETPOINT_MAX, fixed_times_us=None, f_lower=None):
    """Setpoints from a frequency on the grid: the demand's breakpoints and gaps kept, Douglas-Peucker within tol_hz
    (widened until the profile fits max_rows, at most SHOT_SETPOINT_MAX). Frequencies rounded to 0.1 Hz.
    fixed_times_us: use exactly these setpoint times instead (see _fixed_profile)."""
    if fixed_times_us is not None:
        duty_grid = np.where(np.isfinite(duty_grid), duty_grid, DEFAULT_DUTY) if np.ndim(duty_grid) else np.full(len(g.tau), duty_grid)
        return _fixed_profile(p, g, f_grid, duty_grid, fixed_times_us, f_lower)
    max_rows = min(int(max_rows), SHOT_SETPOINT_MAX)
    bp = demand_breakpoints(p)
    duty_grid = np.where(np.isfinite(duty_grid), duty_grid, DEFAULT_DUTY) if np.ndim(duty_grid) else np.full(len(g.tau), duty_grid)
    segs = []
    for a, b in zip(bp[:-1], bp[1:]):
        if not p.on[a]:
            continue
        ta, tb = p.t_us[a], p.t_us[b]
        end = tb if p.on[b] else tb - GRID_US
        sel = (g.tau >= ta) & (g.tau <= end) & np.isfinite(f_grid)
        tt, ff = g.tau[sel], f_grid[sel]
        if end > ta and len(tt) and tt[-1] < end:
            tt, ff = np.append(tt, end), np.append(ff, ff[-1])
        if len(tt) >= 2:
            segs.append((tt, ff))
    tol = tol_hz
    for _ in range(40):
        times = set(p.t_us[bp].tolist())
        for tt, ff in segs:
            times.update(float(round(tt[k])) for k in simplify(tt, ff[:, None], tol))
        t_new = np.array(sorted(times))
        if len(t_new) <= max_rows:
            break
        tol *= 1.5
    else:
        raise CompError(f"could not fit the profile into {max_rows} setpoints (the demand alone has {len(bp)} corners and gaps)")
    demand = values_at(p.t_us, p.demand, p.on, t_new)
    on_new = np.isfinite(demand)
    tg = g.tau
    fi = np.interp(t_new, tg[np.isfinite(f_grid)], f_grid[np.isfinite(f_grid)])
    di = np.interp(t_new, tg, duty_grid)
    carrier = np.where(on_new, np.round(np.clip(fi, F_FLOOR_HZ, F_CEIL_HZ), 1), 0.0)
    duty = np.where(on_new, np.round(np.clip(di, HVPS_DUTY_MIN_PCT, HVPS_DUTY_MAX_PCT), 2), 0.0)
    return Profile(t_new, demand, carrier, duty, p.mode, p.meta)


def simplify(t, Y, tol):
    n = len(t)
    if n <= 2:
        return np.arange(n)
    keep = np.zeros(n, dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        f = (t[i + 1:j] - t[i]) / (t[j] - t[i])
        err = np.max(np.abs(Y[i + 1:j] - (Y[i] + (Y[j] - Y[i]) * f[:, None])), axis=1)
        k = int(np.argmax(err))
        if err[k] > tol:
            k += i + 1
            keep[k] = True
            stack += [(i, k), (k, j)]
    return np.nonzero(keep)[0]


def intervals(t, mask, min_len=1):
    out = []
    padded = np.concatenate([[False], mask, [False]])
    e = np.nonzero(padded[1:] != padded[:-1])[0]
    for a, b in zip(e[::2], e[1::2]):
        if b - a >= min_len:
            out.append((float(t[a]), float(t[b - 1])))
    return out


def fmt_iv(iv, limit=4):
    s = ", ".join(f"{a:.2f}-{b:.2f}" for a, b in iv[:limit])
    return s + (" ..." if len(iv) > limit else "")


# =====================================================================================================================
# Plot
# =====================================================================================================================
def plot(path, title, p, g, *, f_cmd=None, f_new=None, y=None, y_star=None, v=None, v_pred=None, max_kv=DEFAULT_MAX_KV,
         y_pred=None, y_label=None):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    t = g.tau / 1000.0
    fig, ax = plt.subplots(3, 1, figsize=(11, 10), sharex=True)
    unit = "kW" if p.mode == "power" else "kV"
    name = "forward power" if p.mode == "power" else "gyrotron voltage"
    if y is not None:
        ax[0].plot(t, np.where(g.on, y, np.nan), color="#1f77b4", lw=1, label=f"measured {name}")
    if y_star is not None:
        ax[0].plot(t, np.where(g.on, y_star, np.nan), color="#000", lw=1.4, ls="--", label="target")
    if y_pred is not None:
        ax[0].plot(t, np.where(g.on, smooth_edge(np.nan_to_num(y_pred), 10), np.nan), color="#2ca02c", lw=1.2,
                   label=f"predicted {name}, next shot")
    ax[0].set_ylabel(y_label or f"{name} ({unit})")
    if v is not None:
        ax[1].plot(t, np.where(g.on, v, np.nan), color="#9467bd", lw=1, label="measured gyrotron voltage")
    if v_pred is not None:
        ax[1].plot(t, np.where(g.on, v_pred, np.nan), color="#2ca02c", lw=1, label="predicted with the new profile")
    ax[1].axhline(max_kv, color="#d62728", lw=0.8, ls=":", label=f"ceiling {max_kv:g} kV")
    ax[1].set_ylabel("gyrotron voltage (kV)")
    if p.mode == "frequency":
        fd = eval_rows(p.t_us, p.demand, p.on, g.tau)[0]
        ax[2].plot(t, fd / 1000, color="#000", lw=1, ls="--", label="demand")
    if f_cmd is not None:
        ax[2].plot(t, f_cmd / 1000, color="#ff7f0e", lw=1, label="this shot")
    if f_new is not None:
        ax[2].plot(t, f_new / 1000, color="#2ca02c", lw=1.4, label="next shot")
    ax[2].axhline(F_FLOOR_HZ / 1000, color="#d62728", lw=0.8, ls=":", label=f"floor {F_FLOOR_HZ / 1000:g} kHz")
    ax[2].set_ylabel("frequency (kHz)")
    ax[2].set_xlabel("shot time (ms)")
    for a in ax:
        for k, (s, e) in enumerate(g.pulses):
            a.axvspan(s / 1000, min(s / 1000 + SETTLE_MS, e / 1000), color="#888", alpha=0.12, lw=0,
                      label="rise (not corrected)" if (k == 0 and a is ax[0]) else None)
        a.grid(alpha=0.3)
        a.legend(loc="best", fontsize=8)
    ax[0].set_title(title, fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True


def next_name(path, iteration):
    base, ext = os.path.splitext(path)
    base = re.sub(r"_iter\d+$", "", base)
    return f"{base}_iter{iteration}{ext or '.csv'}"


# =====================================================================================================================
# Simulated supply (self-test, unit tests)
# =====================================================================================================================
def simulate(p, bank_v, *, kappa=4.4e-4, c_eff=0.69, gap_deficit=0.065, lag_ms=0.65, t_delay_ms=0.08, noise=0.003,
             p_decay=0.35, p_decay_ms=2.5, seed=1, dt_us=2.0, pre_ms=5.0, post_ms=5.0, gain_fn=None):
    """A plausible ECH supply: settled output 0.164 kV/V*(bank-151 V)*(1+kappa*(20 kHz-f)), the bank drained by V*I
    through c_eff, an extra loss at every later pulse's start, a first-order lag, and a gyrotron whose power falls by
    p_decay over ~p_decay_ms of each pulse at constant voltage. Returns (t_ms, V_kV, P_kW)."""
    rng = np.random.default_rng(seed)
    tau = np.arange(0.0, p.duration_us, dt_us)
    c = np.where(p.carrier > 0, p.carrier, np.nan)
    f, on = eval_rows(p.t_us, c, p.carrier > 0, tau)
    pulses = pulses_of(p.t_us, p.carrier > 0)
    vb = float(bank_v)
    vout, vs = 0.0, np.zeros(len(tau))
    pw = np.zeros(len(tau))
    k_pulse, since = -1, 0.0
    a = dt_us / 1000.0 / lag_ms
    for i, ti in enumerate(tau):
        if on[i]:
            k = next(j for j, (s, e) in enumerate(pulses) if s <= ti < e)
            if k != k_pulse:
                if k > 0:
                    vb -= gap_deficit * (vb - V20_OFFSET_V)
                k_pulse, since = k, 0.0
            since += dt_us / 1000.0
            gf = gain_fn(f[i]) if gain_fn is not None else (1 + kappa * (F_REF_HZ - f[i]))
            target = V20_SLOPE_KV_PER_V * max(vb - V20_OFFSET_V, 0) * gf
            vout += a * (target - vout) if since > 0.15 else a * 3 * (target - vout)
            cur = 21.0 * (max(vout, 0) / REF_KV) ** 0.6
            vb -= vout * 1e3 * cur / (c_eff * vb) * dt_us * 1e-6
            pw[i] = p_of_v(vout) * (1 - p_decay * (1 - math.exp(-since / p_decay_ms)))
        else:
            vout *= math.exp(-dt_us / 1000.0 / 0.3)
            pw[i] = 0.0
        vs[i] = vout
    shift = int(round(t_delay_ms * 1000 / dt_us))
    vs = np.concatenate([np.zeros(shift), vs])[:len(tau)]
    pw = np.concatenate([np.zeros(shift), pw])[:len(tau)]
    pre = np.arange(-pre_ms * 1000, 0, dt_us)
    post = p.duration_us + np.arange(0, post_ms * 1000, dt_us)
    t_us = np.concatenate([pre, tau, post])
    V = np.concatenate([np.zeros(len(pre)), vs, vs[-1] * np.exp(-(post - p.duration_us) / 300.0)])
    P = np.concatenate([np.zeros(len(pre)), pw, np.zeros(len(post))])
    V = V * (1 + noise * rng.standard_normal(len(V)))
    P = P + 2.0 * rng.standard_normal(len(P))
    return t_us / 1000.0, V, P


def picker_like(points, gaps=()):
    """A power demand Profile from [(t_ms, kW), ...] with gaps after the listed point indices (as the picker writes)."""
    t, d = [], []
    for i, (tm, kw) in enumerate(points):
        t.append(round(tm * 1000))
        d.append(kw)
        if i in gaps:
            t.append(round(tm * 1000) + GRID_US)
            d.append(float("nan"))
    return Profile(t, d, None, None, "power")


def selftest():
    print("power target, two pulses (200 kW for 4 ms, 5 ms gap, 200 kW for 4 ms), bank 530 V:")
    dem = picker_like([(0, 200), (4, 200), (9, 200), (13, 200)], gaps=(1,))
    p, info = initial_profile(dem, 530)
    for n in info["notes"]:
        print("   note:", n)
    errs = []
    for it in range(6):
        t, V, P = simulate(p, 530, seed=it)
        r = compensate(p, t, V, P, 530)
        errs.append(max(r.rms_pct))
        print(f"   shot {it}: rms error per pulse {', '.join(f'{x:.1f}' for x in r.rms_pct)} %   max V predicted "
              f"{r.max_v_pred:.1f} kV   largest move {r.max_step:.0f} Hz   {len(r.profile.t_us)} setpoints")
        p = r.profile
    ok = errs[-1] < 5.0 and errs[-1] < errs[0]
    print("frequency profile (today's morning ramp), bank 500 V:")
    t_us = [0, 2000, 10000]
    fp = Profile(t_us, [20000, 20000, 20000], None, None, "frequency")
    fp, _ = initial_profile(fp, 500)
    errs = []
    for it in range(5):
        t, V, P = simulate(fp, 500, seed=10 + it)
        r = compensate(fp, t, V, P, 500, v_ref=(r.v_ref if it else None))
        errs.append(r.rms_pct[0])
        print(f"   shot {it}: voltage rms error {r.rms_pct[0]:.2f} %   largest move {r.max_step:.0f} Hz")
        fp = r.profile
    ok = ok and errs[-1] < 1.0 and errs[-1] < errs[0]
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


# =====================================================================================================================
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?", help="the demand (from the picker) or the last _iterN file")
    ap.add_argument("--target", choices=["vessel", "dummy-load"], help="where the gyrotron fires (picks the power signal)")
    ap.add_argument("--bank-volts", type=float, help="HVPS bank charge of the analysed shot (LabVIEW setting)")
    ap.add_argument("--next-bank-volts", type=float, help="bank charge planned for the next shot (default: the same)")
    ap.add_argument("--shot", type=int, help="MDS shot (default: the latest)")
    ap.add_argument("--data", help="offline: a time_ms,gyrotron_kv,power_kw CSV instead of MDS")
    ap.add_argument("--out", help="output CSV (default <csv>_iter<N>.csv)")
    ap.add_argument("--gain", type=float, default=0.7, help="fraction of the computed correction to apply (default 0.7)")
    ap.add_argument("--max-step-hz", type=float, default=DEFAULT_MAX_STEP_HZ)
    ap.add_argument("--max-kv", type=float, default=DEFAULT_MAX_KV, help="gyrotron voltage ceiling")
    ap.add_argument("--settle-ms", type=float, default=SETTLE_MS)
    ap.add_argument("--t0-ms", type=float, default=0.0, help="MDS time of the shot's t=0 (default 0 = the trigger)")
    ap.add_argument("--tol-hz", type=float, default=5.0, help="setpoint fitting tolerance")
    ap.add_argument("--max-setpoints", type=int, default=SHOT_SETPOINT_MAX,
                    help="at most this many SHOT:STEP rows (the fitting tolerance widens until the profile fits)")
    ap.add_argument("--ignore-mds-target", action="store_true", help="do not refuse when MDS records the other target")
    ap.add_argument("--plant", help="the supply model file (default: ech_plant.json next to the CSV); learned from every shot")
    ap.add_argument("--no-learn", action="store_true", help="use the supply model but do not update it")
    ap.add_argument("--prev-data", help="offline: the previous shot's time_ms,gyrotron_kv,power_kw CSV (else read from MDS)")
    ap.add_argument("--gain-measured", type=float, default=0.9,
                    help="fraction of the correction where the sensitivity was measured from the previous shot (default 0.9)")
    ap.add_argument("--flatten", action="store_true",
                    help="aim at this shot's own flat-top mean instead of the demand's level (shape only); the level is "
                         "written into the output CSV's demand, so later iterations keep it without the option")
    ap.add_argument("--power-scale", type=float,
                    help="experimental-meter kW per dummy-load-meter kW for the same gyrotron output (the factor "
                         "ech_meter_calibration.py measures). With it, demand_kw is in experimental-meter kW on either "
                         "target; kept in the CSV for the next iterations")
    ap.add_argument("--no-rise-shaping",action="store_true", help="leave each pulse's rise at the flat top's start frequency")
    ap.add_argument("--fixed-times-ms", help="put the setpoints at exactly these times: '0:15:1' (start:stop:step, ms) or "
                    "'0,1,2.5,...'; kept in the CSV for the next iterations")
    ap.add_argument("--rise-max-hz", type=float, default=300.0, help="the most the rise may be raised above the flat top's start")
    ap.add_argument("--guard-pct", type=float, default=2.0,
                    help="no further lowering where 100 Hz lower bought less than this many %% (default 2)")
    ap.add_argument("--no-plot", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--train", metavar="MANIFEST", help="retrain the supply model from recorded shots: a CSV of "
                    "shot,profile_csv,bank_volts[,data_csv] in firing order (MDS when data_csv is empty)")
    ap.add_argument("--train-append", action="store_true", help="add to the existing model instead of starting afresh")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.train:
        try:
            plant_path = a.plant or os.path.join(os.path.dirname(os.path.abspath(a.train)), "ech_plant.json")
            if a.train_append and os.path.exists(plant_path):
                PLANT.load(plant_path)
            items = []
            k_train = check_power_scale(a.power_scale)
            for row in read_manifest(a.train):
                pr = read_profile(row["profile_csv"])
                if pr.carrier is None:
                    raise CompError(f"{row['profile_csv']}: no carrier_hz column (the profile that fired is needed)")
                tgt = a.target or pr.meta.get("target") or "dummy-load"
                if row["data_csv"]:
                    t_ms, v, pw = read_data_csv(row["data_csv"])
                else:
                    _, t_ms, v, pw, _, _ = read_mds(int(row["shot"]), tgt)
                if tgt == "vessel":
                    pw = pw / k_train                                   # the model is kept in dummy-load-meter kW
                items.append({"shot": row["shot"], "profile": pr, "t_ms": t_ms, "v_kv": v, "p_kw": pw, "bank": row["bank"]})
            sm = train(items, reset=not a.train_append, settle_ms=a.settle_ms)
            PLANT.save(plant_path)
        except (CompError, OSError, ValueError, KeyError, IndexError) as e:
            print(f"ERROR {e}", file=sys.stderr)
            print(f"RESULT status=error message=\"{str(e).splitlines()[0]}\"")
            return 1
        print(f"RESULT status=ok kind=train shots={len(items)} pairs={sm['pairs']} pair_samples={sm['pair_samples']} "
              f"bank_groups={sm['groups']} bank_bins={sm['bank_bins']} dpdv_points={sm['dpdv_points']} plant={plant_path}")
        for sk in sm["skipped"]:
            print(f"WARNING skipped {sk}", file=sys.stderr)
        return 0
    if not a.csv:
        ap.error("give the CSV")
    try:
        p = read_profile(a.csv)
        plant_path = a.plant or os.path.join(os.path.dirname(os.path.abspath(a.csv)), "ech_plant.json")
        if os.path.exists(plant_path):
            PLANT.load(plant_path)
        target = a.target or p.meta.get("target")
        if target not in ("vessel", "dummy-load"):
            raise CompError("say where the gyrotron fires: --target vessel or --target dummy-load")
        bank = a.bank_volts if a.bank_volts is not None else (float(p.meta["bank_volts"]) if p.meta.get("bank_volts") else None)
        if a.max_setpoints == SHOT_SETPOINT_MAX and p.meta.get("max_setpoints"):
            a.max_setpoints = int(p.meta["max_setpoints"])            # carried from the previous iteration's CSV
        iteration = int(p.meta.get("iteration", "-1") or -1) + 1
        out = a.out or next_name(a.csv, iteration)
        png = os.path.splitext(out)[0] + ".png"
        meta = dict(p.meta)
        meta.update({"tool": "ech_compensate.py", "mode": p.mode, "target": target, "iteration": iteration,
                     "from_csv": os.path.basename(a.csv)})
        if a.max_setpoints < SHOT_SETPOINT_MAX:
            meta["max_setpoints"] = a.max_setpoints
        fixed_txt = a.fixed_times_ms or p.meta.get("fixed_times_ms")
        fixed = parse_fixed_times(fixed_txt)
        if fixed is not None:
            meta["fixed_times_ms"] = fixed_txt
        # power scale: the CSV's demand is in experimental-meter kW; everything inside (the model, the correction) works
        # in dummy-load-meter kW, so the demand is divided by K here and multiplied back when written
        K = check_power_scale(a.power_scale if a.power_scale is not None else p.meta.get("power_scale"))
        if K != 1.0:
            if p.mode != "power":
                raise CompError("--power-scale applies to a power target (demand_kw) only")
            meta["power_scale"] = f"{K:.4f}"
            p.demand = p.demand / K
        y_label = "forward power, experimental meter (kW)" if K != 1.0 else None
        scaled = (lambda x: None if x is None else np.asarray(x, dtype=float) * K)

        if p.carrier is None:                                       # first shot: from the model
            q, info = initial_profile(p, a.next_bank_volts or bank, max_kv=a.max_kv, settle_ms=a.settle_ms, tol_hz=a.tol_hz,
                                       max_rows=a.max_setpoints, fixed_times_us=fixed)
            check_profile(q)
            meta.update({"bank_volts": a.next_bank_volts or bank, "kind": "first-shot model guess"})
            q.demand = q.demand * K
            write_profile(out, q, meta)
            g = info["grid"]
            plotted = (not a.no_plot) and plot(png, f"First-shot profile from the model ({p.mode}), bank "
                                               f"{a.next_bank_volts or bank} V -- check before firing", q, g,
                                               f_new=info["f_new"], y_star=(scaled(eval_rows(p.t_us, p.demand, p.on, g.tau)[0])
                                                                            if p.mode == "power" else None),
                                               v_pred=info["v_pred"], max_kv=a.max_kv, y_label=y_label)
            fmin = float(np.min(q.carrier[q.carrier > 0]))
            print(f"RESULT status=ok kind=initial mode={p.mode} out={out} setpoints={len(q.t_us)} "
                  f"duration_ms={q.duration_us / 1000:g} min_freq_hz={fmin:.0f}"
                  + (f" power_scale={K:.4f}" if K != 1.0 else ""))
            for n in info["notes"]:
                print(f"WARNING {n}", file=sys.stderr)
            print("NOTE this first profile comes from a model of the supply (2026-11-17 data); check it before firing",
                  file=sys.stderr)
            if plotted:
                print(f"plot: {png}")
            return 0

        if bank is None:
            raise CompError("give the analysed shot's bank charge: --bank-volts")
        if a.data:
            shot, mds_bank, mds_target = a.shot or "offline", None, None
            t_ms, v, pw = read_data_csv(a.data)
        else:
            shot, t_ms, v, pw, mds_bank, mds_target = read_mds(a.shot, target)
        if target == "vessel":
            pw = pw / K                                             # experimental meter -> dummy-load-meter kW
        match = target_names_match(target, mds_target)
        if match is False and not a.ignore_mds_target:
            raise CompError(f"MDS records ECH_TARGET={mds_target} for shot {shot}, but --target is {target}: "
                            f"refusing to correct against the wrong power signal (--ignore-mds-target to override)")
        warn = []
        if mds_bank is not None and abs(mds_bank - bank) > 0.5:
            warn.append(f"MDS records HVPS_V={mds_bank:g} V for shot {shot}, --bank-volts is {bank:g} V")
        prev = None                                                 # the shot before: the CSV this one was made from
        prev_csv = os.path.join(os.path.dirname(os.path.abspath(a.csv)), p.meta.get("from_csv", "")) if p.meta.get("from_csv") else None
        if prev_csv and os.path.exists(prev_csv) and p.meta.get("from_shot"):
            pp = read_profile(prev_csv)
            if pp.carrier is not None and pp.meta.get("bank_volts"):
                try:
                    if a.prev_data:
                        pt, pv, ppw = read_data_csv(a.prev_data)
                    elif not a.data:
                        _, pt, pv, ppw, _, _ = read_mds(int(p.meta["from_shot"]), target)
                    else:
                        pt = None
                        warn.append("offline run without --prev-data: nothing learned from the previous shot")
                    if pt is not None:
                        if target == "vessel":
                            ppw = ppw / K
                        # the bank the previous shot actually ran at is recorded in THIS CSV (analysed_bank_volts,
                        # the bank of from_shot); the previous CSV's bank_volts is only the bank it was planned for
                        prev_bank = float(p.meta.get("analysed_bank_volts") or pp.meta["bank_volts"])
                        prev = {"profile": pp, "t_ms": pt, "v_kv": pv, "p_kw": ppw, "bank": prev_bank,
                                "shot": p.meta["from_shot"]}
                except CompError as e:
                    warn.append(f"could not read the previous shot ({e}): nothing learned from it")
        if a.flatten:
            levels = flatten_demand(p, t_ms, pw if p.mode == "power" else v, settle_ms=a.settle_ms, t0_ms=a.t0_ms)
            meta["flatten_from_shot"] = shot
            levels = [x * K for x in levels]                       # report in the CSV's units
            meta["flatten_level"] = "/".join(f"{x:.1f}" for x in levels)
            print(f"flatten: demand rescaled to the measured flat-top mean of shot {shot}: "
                  f"{', '.join(f'{x:.1f}' for x in levels)} {'kW' if p.mode == 'power' else 'kV'}", file=sys.stderr)
        r = compensate(p, t_ms, v, pw, bank, next_bank_v=a.next_bank_volts, gain_ilc=a.gain, max_step_hz=a.max_step_hz,
                       max_kv=a.max_kv, settle_ms=a.settle_ms, t0_ms=a.t0_ms, tol_hz=a.tol_hz,
                       max_rows=a.max_setpoints,
                       v_ref=float(p.meta["target_v_ref_kv"]) if p.meta.get("target_v_ref_kv") else None,
                       prev=prev, gain_measured=a.gain_measured, guard_kappa=a.guard_pct / 100.0 / 100.0,
                       learn=not a.no_learn, shape_rise=not a.no_rise_shaping, rise_max_hz=a.rise_max_hz,
                       fixed_times_us=fixed)
        if prev is not None and r.learned.get("pairs") and not a.no_learn:
            PLANT.history.append({"shot": str(shot), "prev_shot": str(prev["shot"]), "bank_volts": bank,
                                  "kappa_samples": r.learned.get("kappa_samples", 0)})
            PLANT.save(plant_path)
    except CompError as e:
        print(f"ERROR {e}", file=sys.stderr)
        print(f"RESULT status=error message=\"{str(e).splitlines()[0]}\"")
        return 1
    except (OSError, ValueError, KeyError) as e:
        print(f"ERROR {e}", file=sys.stderr)
        print(f"RESULT status=error message=\"{e}\"")
        return 2

    meta.update({"from_shot": shot, "bank_volts": a.next_bank_volts or bank, "analysed_bank_volts": bank,
                 "rms_error_pct": "/".join(f"{x:.2f}" for x in r.rms_pct), "gain": a.gain,
                 "max_v_predicted_kv": f"{r.max_v_pred:.1f}"})
    meta.pop("kind", None)
    if r.v_ref is not None:
        meta["target_v_ref_kv"] = f"{r.v_ref:.3f}"                  # fixed from the first analysed shot on
    r.profile.demand = r.profile.demand * K                         # back to the CSV's units (K = 1 without a scale)
    write_profile(out, r.profile, meta)
    title = (f"ECH shot {shot} ({p.mode}, {target}, bank {bank:g} V, iteration {iteration})   error per pulse "
             f"{', '.join(f'{x:.1f}' for x in r.rms_pct)} %   max V next {r.max_v_pred:.1f} kV"
             + (f"   power scale {K:.3f}" if K != 1.0 else ""))
    if p.mode == "power":
        y_plot, ystar_plot, ypred_plot = scaled(r.y), scaled(r.y_star), scaled(r.p_pred_plot)
    else:
        y_plot, ystar_plot, ypred_plot = r.y, r.y_star, r.v_pred_plot
    plotted = (not a.no_plot) and plot(png, title, r.profile, r.grid, f_cmd=r.f_cmd, f_new=r.f_profile, y=y_plot,
                                       y_star=ystar_plot, v=r.v, v_pred=r.v_pred_plot, max_kv=a.max_kv,
                                       y_pred=ypred_plot, y_label=y_label)
    print(f"RESULT status=ok kind=correction mode={p.mode} shot={shot} iteration={iteration} out={out} "
          f"rms_pct={'/'.join(f'{x:.2f}' for x in r.rms_pct)} max_v_pred_kv={r.max_v_pred:.1f} "
          f"largest_move_hz={r.max_step:.0f} setpoints={len(r.profile.t_us)} "
          f"limited_by_step={r.n_step} limited_by_vmax={r.n_vcap} at_floor={r.n_floor} at_ceiling={r.n_ceil} "
          f"held_by_guard={r.n_guard} learned_from_previous={'yes' if r.learned.get('pairs') else 'no'} "
          f"overshoot_pct={'/'.join(f'{x:.1f}' for x in r.overshoot_pct)} rise_raise_max_hz={float(np.max(r.rise_raise)):.0f}"
          + (f" fixed_times_max_dip_hz={getattr(r.profile, 'max_dip_hz', 0.0):.1f}" if fixed is not None else "")
          + (f" power_scale={K:.4f}" if K != 1.0 else ""))
    for n in warn + r.notes:
        print(f"WARNING {n}", file=sys.stderr)
    for key, label in (("step", f"the {a.max_step_hz:g} Hz per-shot limit"), ("vmax", f"the {a.max_kv:g} kV ceiling"),
                       ("floor", f"the {F_FLOOR_HZ / 1000:g} kHz floor"), ("ceil", f"the {F_CEIL_HZ / 1000:g} kHz ceiling"),
                       ("guard", "the gain-peak guard")):
        if r.limits[key]:
            print(f"WARNING held by {label} at {fmt_iv(r.limits[key])} ms", file=sys.stderr)
    if plotted:
        print(f"plot: {png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

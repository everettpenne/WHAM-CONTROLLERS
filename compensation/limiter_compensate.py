#!/usr/bin/env python3
"""
limiter_compensate.py -- shot-to-shot droop compensation for the Limiter supply (HBRIDGE, LIMITER mode).

WHY
    In LIMITER mode the output is roughly   Vout = s * Vcap(t) * (2D - 1)   (s = +1 or -1, found from the data).
    The operator programs a duty profile D(t) as if the capacitor bank held its voltage, but the bank droops during
    the shot -- by an amount that depends on the shot itself -- so the output falls short. Until closed-loop control
    can run in the lab, this script closes the loop between shots instead: after a shot it compares the measured
    output (V_LIM, from MDSplus) with what the operator's duty profile would have produced on an undrooped bank, and
    writes a corrected duty profile for the next shot. Repeating shot -> script -> upload converges in a few shots.

HOW TO RUN (the operator runs only this file, pointed at one CSV)
    python limiter_compensate.py my_shot.csv                 analyse the latest WHAM shot
    python limiter_compensate.py my_shot.csv --shot 250117042   analyse a given shot
  It writes my_shot_iter1.csv (then _iter2, _iter3, ... when run on the previous output) and a .png beside it, and
  prints one RESULT line. Upload the new file's COMMANDED columns for the next shot, then run the script on that file.

THE CSV (the whole state of the iteration; see compensation/README.md)
    time_us,carrier_hz,demand_duty_pct,commanded_duty_pct
    time_us,carrier_hz,demand_duty_U_pct,demand_duty_V_pct,demand_duty_W_pct,commanded_duty_U_pct,...  (per phase)
  demand    = the operator's idealized profile; never changed by this script.
  commanded = what was uploaded for the shot being analysed (missing on the first run: taken equal to demand).
  A first CSV may also use plain duty_pct / duty_U_pct.. columns (taken as the demand).
  carrier_hz = 0 (duties 0) is an output-off gap, exactly as in SHOT:STEP.  '# key=value' lines at the top are
  metadata (shot, iteration, ...) and are carried forward.  Each row maps to one
      SHOT:STEP <time_us> <carrier_hz> <commanded duty or the three per-phase duties>

METHOD (details in compensation/README.md)
  1. validate the profile against the firmware's LIMITER rules; evaluate duty as the firmware does (linear between
     setpoints; an on setpoint followed by an off one holds its value until the gap)
  2. read V_LIM (or --data), resample to a 10 us grid
  3. find the shot's start t0 in the measurement time base and the sign s by cross-correlating the measurement with
     (2D-1) (zero outside the shot and in gaps)
  4. fit a smooth effective bank voltage Vc(t) to   V = s*Vc(t)*(2(D+delta)-1)   by least squares, using only the
     samples where |2D-1| is large enough to carry information
  5. V0 = Vc at the start of the shot; the target is  Vt = s*V0*(2(Ddemand+delta)-1)
  6. ideal duty  D* = (1 + (2(Ddemand+delta)-1)*V0/Vc)/2 - delta,  new = commanded + gain*(D* - commanded),
     clamped to the mode's 5-95 %
  7. re-express as setpoints (Douglas-Peucker within --tol-pct, the demand's own breakpoints and gaps kept, <= 500)
  With per-phase duties the measured output is compared with the MEAN of the phase duties, and the same Vc(t) scales
  every phase (the droop is the bank's, common to all three).

This script never talks to a controller: it only reads MDSplus and writes files. Uploading and firing stay with the
operator. Needs numpy; matplotlib for the plot (skipped if missing); MDSplus only when reading a shot from MDS.
Self-contained on purpose so it can be copied to the LabVIEW PC alone.  Test: python limiter_compensate.py --selftest
"""
import argparse
import math
import os
import re
import sys

import numpy as np

# --- firmware rules, mirrored from firmware/hbridge (keep in step) -------------------------------------------------
#   src/config/ctrlr_config.h: SUPPLY LIMITER limits, SHOT_SETPOINT_MAX, SHOT_MAX_DURATION_US
#   src/app/control/shot_profile.c: ShotProfile_Append / _Finish rules
LIM_FREQ_MIN_HZ = 75_000.0
LIM_FREQ_MAX_HZ = 125_000.0
LIM_DUTY_MIN_PCT = 5.0
LIM_DUTY_MAX_PCT = 95.0
SHOT_SETPOINT_MAX = 500
SHOT_MAX_DURATION_US = 1_000_000

PHASES = ("U", "V", "W")
DEFAULT_NODE = "bias.bias_supply.V_LIM.filtered"
DEFAULT_TREE = "wham"


class CompError(Exception):
    """A problem the operator can act on (bad CSV, no data, analysis not trustworthy)."""


# =====================================================================================================================
# Profile: reading, checking, evaluating, writing
# =====================================================================================================================
class Profile:
    """Setpoints: t_us (n,), carrier_hz (n,), demand (n, P) and commanded (n, P) duties in percent, P = 1 or 3."""

    def __init__(self, t_us, carrier_hz, demand, commanded, meta=None):
        self.t_us = np.asarray(t_us, dtype=float)
        self.carrier_hz = np.asarray(carrier_hz, dtype=float)
        self.demand = np.atleast_2d(np.asarray(demand, dtype=float).T).T
        self.commanded = np.atleast_2d(np.asarray(commanded, dtype=float).T).T
        self.meta = dict(meta or {})

    @property
    def nphase(self):
        return self.demand.shape[1]

    @property
    def duration_us(self):
        return float(self.t_us[-1])

    @property
    def on(self):
        return self.carrier_hz > 0


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
            try:
                rows.append([float(c) for c in cells])
            except ValueError:
                raise CompError(f"{path}: row {len(rows) + 1} is not numeric: {line}")
    if header is None or not rows:
        raise CompError(f"{path}: no header or no rows")
    if any(len(r) != len(header) for r in rows):
        raise CompError(f"{path}: every row must have {len(header)} values")
    a = np.array(rows)
    col = {name: i for i, name in enumerate(header)}
    for need in ("time_us", "carrier_hz"):
        if need not in col:
            raise CompError(f"{path}: missing column {need} (have: {', '.join(header)})")

    def pick(prefix):
        if f"{prefix}duty_pct" in col:
            return a[:, [col[f"{prefix}duty_pct"]]]
        names = [f"{prefix}duty_{p.lower()}_pct" for p in PHASES]
        if all(n in col for n in names):
            return a[:, [col[n] for n in names]]
        return None

    demand = pick("demand_")
    if demand is None:
        demand = pick("")
    if demand is None:
        raise CompError(f"{path}: need demand_duty_pct (or demand_duty_U/V/W_pct, or duty_pct) columns")
    commanded = pick("commanded_")
    if commanded is None:
        commanded = demand.copy()
    if commanded.shape[1] != demand.shape[1]:
        if commanded.shape[1] == 1:
            commanded = np.repeat(commanded, demand.shape[1], axis=1)
        elif demand.shape[1] == 1:
            demand = np.repeat(demand, commanded.shape[1], axis=1)
    return Profile(a[:, col["time_us"]], a[:, col["carrier_hz"]], demand, commanded, meta)


def check_profile(p, which=("demand", "commanded")):
    """The firmware's own upload rules for a LIMITER shot. Raises CompError listing every problem."""
    errs = []
    t, c = p.t_us, p.carrier_hz
    n = len(t)
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
    if n and t[-1] > SHOT_MAX_DURATION_US:
        errs.append(f"the shot is {t[-1]:.0f} us; the longest is {SHOT_MAX_DURATION_US} us")
    if np.any(c < 0):
        errs.append("negative carrier frequency")
    if n and c[0] == 0:
        errs.append("the first setpoint cannot be an output-off (carrier 0) setpoint")
    if n >= 2 and (c[-1] == 0 or c[-2] == 0):
        errs.append("the last two setpoints must be on (carrier > 0)")
    on = c > 0
    bad_f = on & ((c < LIM_FREQ_MIN_HZ) | (c > LIM_FREQ_MAX_HZ))
    for i in np.nonzero(bad_f)[0][:5]:
        errs.append(f"row {i + 1}: carrier {c[i]:g} Hz is outside {LIM_FREQ_MIN_HZ:g}-{LIM_FREQ_MAX_HZ:g} Hz")
    for name in which:
        d = getattr(p, name)
        bad = on[:, None] & ((d < LIM_DUTY_MIN_PCT - 1e-9) | (d > LIM_DUTY_MAX_PCT + 1e-9))
        for i in np.nonzero(bad.any(axis=1))[0][:5]:
            errs.append(f"row {i + 1}: {name} duty {', '.join(f'{v:g}' for v in d[i])} % is outside "
                        f"{LIM_DUTY_MIN_PCT:g}-{LIM_DUTY_MAX_PCT:g} %")
    if errs:
        raise CompError("profile does not follow the controller's LIMITER rules:\n  " + "\n  ".join(errs))


def eval_profile(t_us, carrier, duty, tau_us):
    """The firmware's interpolation at times tau_us (array). duty is (n, P) percent.
    Returns (duty (m, P) percent, on (m,) bool). Between two on setpoints values ramp linearly; an on setpoint followed
    by an off one holds its values until the gap; from an off setpoint to the next one the output is off; the shot
    covers [0, t_last)."""
    tau = np.asarray(tau_us, dtype=float)
    n = len(t_us)
    k = np.clip(np.searchsorted(t_us, tau, side="right") - 1, 0, n - 2)
    on_k, on_k1 = carrier[k] > 0, carrier[k + 1] > 0
    f = np.clip((tau - t_us[k]) / (t_us[k + 1] - t_us[k]), 0.0, 1.0)
    f = np.where(on_k1, f, 0.0)                                      # on -> off: hold
    d = duty[k] + (duty[k + 1] - duty[k]) * f[:, None]
    on = on_k & (tau >= 0) & (tau < t_us[-1])
    d[~on] = 0.0
    return d, on


def write_profile(path, p, meta):
    lines = []
    for key, val in meta.items():
        lines.append(f"# {key}={val}")
    if p.nphase == 1:
        lines.append("time_us,carrier_hz,demand_duty_pct,commanded_duty_pct")
    else:
        lines.append("time_us,carrier_hz," + ",".join(f"demand_duty_{x}_pct" for x in PHASES) + ","
                     + ",".join(f"commanded_duty_{x}_pct" for x in PHASES))
    for i in range(len(p.t_us)):
        vals = [f"{int(round(p.t_us[i]))}", f"{p.carrier_hz[i]:.3f}".rstrip("0").rstrip(".")]
        vals += [f"{v:.2f}" for v in p.demand[i]] + [f"{v:.2f}" for v in p.commanded[i]]
        lines.append(",".join(vals))
    with open(path, "w", newline="") as f:
        f.write("\n".join(lines) + "\n")


def demand_breakpoints(p):
    """Rows where the demand profile actually changes course (first, last, gap edges, a change of slope in carrier or
    any demand duty). Rows added by an earlier iteration lie on straight lines of the demand and are dropped, so the
    setpoint count does not grow from one iteration to the next."""
    n = len(p.t_us)
    keep = np.zeros(n, dtype=bool)
    keep[0] = keep[-1] = True
    on = p.on
    y = np.column_stack([p.carrier_hz / 1000.0, p.demand])
    for i in range(1, n - 1):
        if not on[i] or not on[i - 1] or not on[i + 1]:
            keep[i] = True
            continue
        f = (p.t_us[i] - p.t_us[i - 1]) / (p.t_us[i + 1] - p.t_us[i - 1])
        mid = y[i - 1] + (y[i + 1] - y[i - 1]) * f
        if np.max(np.abs(mid - y[i])) > 0.006:            # half the 0.01 % / 1 mHz resolution, with margin
            keep[i] = True
    return np.nonzero(keep)[0]


# =====================================================================================================================
# Measurement
# =====================================================================================================================
def read_mds(shot, tree_name=DEFAULT_TREE, node=DEFAULT_NODE):
    """(shot, t_ms, volts) from MDSplus. shot None or 0 = the tree's current (latest) shot."""
    try:
        import MDSplus as mds
    except ImportError:
        raise CompError("the MDSplus Python package is not installed (or use --data FILE for an offline run)")
    if not shot:
        try:
            shot = int(mds.Tree.getCurrent(tree_name))
        except Exception as e:                                          # noqa: BLE001 -- report whatever MDS says
            raise CompError(f"could not get the current {tree_name} shot ({e}); pass --shot N")
    tree = None
    try:
        tree = mds.Tree(tree_name, shot, "readonly")                    # read only: the tools never write to MDS
        n = tree.getNode(node)
        v = np.asarray(n.data(), dtype=float)
        try:
            t = np.asarray(n.dim_of().data(), dtype=float)
        except Exception:                                               # noqa: BLE001
            alt = re.sub(r"\.filtered$", ".signal", node, flags=re.I)
            t = np.asarray(tree.getNode(alt).dim_of().data(), dtype=float)
    except CompError:
        raise
    except Exception as e:                                              # noqa: BLE001
        raise CompError(f"could not read {node} for shot {shot}: {e}")
    finally:
        if tree is not None:
            try:
                tree.close()
            except Exception:                                           # noqa: BLE001
                pass
    if v.shape != t.shape or v.size < 10:
        raise CompError(f"{node} for shot {shot}: {v.size} samples, time base {t.size}")
    return shot, t * 1e3, v


def read_data_csv(path):
    """Offline measurement: two columns, time in ms and volts (a header line is allowed)."""
    rows = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            try:
                a, b = s.split(",")[:2]
                rows.append((float(a), float(b)))
            except ValueError:
                continue                                                 # header
    if len(rows) < 10:
        raise CompError(f"{path}: need columns time_ms,volts with at least 10 rows")
    a = np.array(rows)
    o = np.argsort(a[:, 0])
    return a[o, 0], a[o, 1]


def resample(t_ms, v, dt_ms):
    """Uniform grid. When the raw samples are much finer than the grid, average over each grid cell rather than
    picking points (no aliasing of switching ripple)."""
    t_ms = np.asarray(t_ms, dtype=float)
    v = np.asarray(v, dtype=float)
    grid = np.arange(t_ms[0], t_ms[-1], dt_ms)
    raw_dt = np.median(np.diff(t_ms))
    if raw_dt < dt_ms / 2:
        cum = np.concatenate([[0.0], np.cumsum(0.5 * (v[1:] + v[:-1]) * np.diff(t_ms))])
        hi = np.interp(grid + dt_ms / 2, t_ms, cum)
        lo = np.interp(grid - dt_ms / 2, t_ms, cum)
        span = np.clip(grid + dt_ms / 2, t_ms[0], t_ms[-1]) - np.clip(grid - dt_ms / 2, t_ms[0], t_ms[-1])
        return grid, np.where(span > 0, (hi - lo) / np.maximum(span, 1e-12), np.interp(grid, t_ms, v))
    return grid, np.interp(grid, t_ms, v)


# =====================================================================================================================
# Analysis
# =====================================================================================================================
def _correlate_valid(m, e):
    """c[L] = sum_j m[L+j] e[j], L = 0 .. len(m)-len(e) (FFT)."""
    n = len(m) + len(e)
    size = 1 << (n - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(m, size) * np.conj(np.fft.rfft(e, size)), size)
    return c[:len(m) - len(e) + 1]


def align(grid_ms, m, x_shot, dt_ms, search=None):
    """t0 (ms, measurement time of the shot's t=0), sign and correlation quality. x_shot = (2D-1) on the shot grid
    (zero in gaps); outside the shot the expected output is zero."""
    if len(m) <= len(x_shot):
        raise CompError("the measurement record is shorter than the shot")
    c = _correlate_valid(m, x_shot)
    lags = grid_ms[0] + np.arange(len(c)) * dt_ms
    ok = np.ones(len(c), dtype=bool)
    if search is not None:
        ok = (lags >= search[0]) & (lags <= search[1])
        if not ok.any():
            raise CompError(f"--t0-search {search[0]} {search[1]} ms is outside the measurement record")
    L = int(np.argmax(np.where(ok, np.abs(c), -1.0)))
    # sub-sample peak
    frac = 0.0
    if 0 < L < len(c) - 1 and ok[L - 1] and ok[L + 1]:
        y0, y1, y2 = abs(c[L - 1]), abs(c[L]), abs(c[L + 1])
        den = y0 - 2 * y1 + y2
        if den < 0:
            frac = float(np.clip(0.5 * (y0 - y2) / den, -0.5, 0.5))
    seg = m[L:L + len(x_shot)]
    quality = float(c[L] / (np.linalg.norm(seg) * np.linalg.norm(x_shot) + 1e-30))
    return lags[L] + frac * dt_ms, (1 if c[L] >= 0 else -1), abs(quality)


def edge_mask(on, n_edge):
    """True for samples within n_edge samples of an on/off transition (including the shot's start and end)."""
    padded = np.concatenate([[False], on, [False]])
    trans = np.nonzero(padded[1:] != padded[:-1])[0]
    near = np.zeros(len(on), dtype=bool)
    for k in trans:
        near[max(0, k - n_edge):min(len(on), k + n_edge)] = True
    return near


def fit_vc(tau_ms, y, b, T_ms, knot_ms, lam):
    """Least-squares fit of a piecewise-linear Vc(t) (knots every ~knot_ms) to  y = b * Vc(tau), b = s*(2D-1).
    A first-difference penalty keeps it smooth and holds it flat where there is no data. Returns (knots, values)."""
    nk = int(min(400, max(2, math.ceil(T_ms / knot_ms) + 1)))
    knots = np.linspace(0.0, T_ms, nk)
    h = knots[1] - knots[0]
    k = np.clip((tau_ms / h).astype(int), 0, nk - 2)
    f = np.clip(tau_ms / h - k, 0.0, 1.0)
    A = np.zeros((nk, nk))
    r = np.zeros(nk)
    w0, w1 = b * (1 - f), b * f
    np.add.at(A, (k, k), w0 * w0)
    np.add.at(A, (k, k + 1), w0 * w1)
    np.add.at(A, (k + 1, k), w0 * w1)
    np.add.at(A, (k + 1, k + 1), w1 * w1)
    np.add.at(r, k, w0 * y)
    np.add.at(r, k + 1, w1 * y)
    scale = max(np.mean(np.diag(A)), 1e-12)
    D1 = np.diff(np.eye(nk), axis=0)
    A = A + lam * scale * (D1.T @ D1) + 1e-9 * scale * np.eye(nk)
    return knots, np.linalg.solve(A, r)


def simplify(t, Y, tol):
    """Douglas-Peucker on a curve t (n,), Y (n, P): indices to keep so that straight lines between them stay within
    tol of every column."""
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
        line = Y[i] + (Y[j] - Y[i]) * f[:, None]
        err = np.max(np.abs(Y[i + 1:j] - line), axis=1)
        k = int(np.argmax(err))
        if err[k] > tol:
            k += i + 1
            keep[k] = True
            stack.append((i, k))
            stack.append((k, j))
    return np.nonzero(keep)[0]


def intervals(tau_ms, mask, min_len=1):
    """[(start_ms, end_ms)] of the runs where mask is True."""
    out = []
    padded = np.concatenate([[False], mask, [False]])
    edges = np.nonzero(padded[1:] != padded[:-1])[0]
    for a, b in zip(edges[::2], edges[1::2]):
        if b - a >= min_len:
            out.append((float(tau_ms[a]), float(tau_ms[b - 1])))
    return out


class Result:
    pass


def compensate(p, t_ms, volts, *, gain=1.0, sign="auto", t0_ms=None, t0_search=None, duty_offset_pct=0.0,
               tol_pct=0.1, grid_us=10.0, knot_ms=0.5, lam=1e-3, xmin=0.1, edge_us=30.0, v0_window_ms=0.2,
               min_quality=0.5):
    """The whole analysis. Returns a Result with the new Profile and everything needed to report and plot."""
    check_profile(p)
    if not 0 < gain <= 1.0:
        raise CompError("--gain must be in (0, 1]")
    dt_ms = grid_us / 1000.0
    delta = duty_offset_pct / 100.0
    T_us = p.duration_us
    tau_us = np.arange(0.0, T_us, grid_us)
    tau_ms = tau_us / 1000.0

    d_cmd, on = eval_profile(p.t_us, p.carrier_hz, p.commanded, tau_us)
    d_dem, _ = eval_profile(p.t_us, p.carrier_hz, p.demand, tau_us)
    x_cmd = np.where(on, 2 * (d_cmd.mean(axis=1) / 100.0 + delta) - 1, 0.0)
    x_dem_ph = np.where(on[:, None], 2 * (d_dem / 100.0 + delta) - 1, 0.0)
    x_dem = x_dem_ph.mean(axis=1)

    grid, m = resample(t_ms, volts, dt_ms)
    if np.allclose(x_cmd, 0):
        if t0_ms is None or sign == "auto":
            raise CompError("the commanded duty is 50 % throughout: no signal to align on; pass --t0-ms and --sign")

    # alignment: once on the raw record, then again with the pre-shot baseline removed
    baseline = 0.0
    quality = None
    for _ in range(2):
        if t0_ms is None:
            t0, s_est, quality = align(grid, m - baseline, x_cmd, dt_ms, t0_search)
        else:
            t0, s_est = float(t0_ms), None
            if sign == "auto":
                seg = np.interp(tau_ms + t0, grid, m - baseline)
                s_est = 1 if np.dot(seg, x_cmd) >= 0 else -1
        pre = (grid < t0 - 0.2)
        new_base = float(np.median(m[pre])) if pre.sum() >= 50 else 0.0
        if new_base == baseline:
            break
        baseline = new_base
    s = s_est if sign == "auto" else int(sign)
    if quality is not None and quality < min_quality:
        raise CompError(f"the measurement does not look like this profile (correlation {quality:.2f} < "
                        f"{min_quality}): wrong shot, wrong CSV, or no output? Override with --t0-ms and --sign")
    if tau_ms[-1] + t0 > grid[-1] or t0 < grid[0]:
        raise CompError(f"the shot (t0 = {t0:.3f} ms, {T_us / 1000:.3f} ms long) is not inside the measurement record "
                        f"({grid[0]:.3f} .. {grid[-1]:.3f} ms)")
    y = np.interp(tau_ms + t0, grid, m) - baseline

    # fit the effective bank voltage
    use = on & (np.abs(x_cmd) >= xmin) & ~edge_mask(on, int(round(edge_us / grid_us)))
    if use.sum() < 20:
        raise CompError(f"too few samples with |2D-1| >= {xmin} to estimate the bank voltage "
                        f"(duty too close to 50 %?)")
    knots, kv = fit_vc(tau_ms[use], y[use], s * x_cmd[use], T_us / 1000.0, knot_ms, lam)
    vc = np.interp(tau_ms, knots, kv)
    early = tau_ms <= v0_window_ms
    v0 = float(np.mean(vc[early])) if early.any() else float(vc[0])
    if v0 <= 0:
        raise CompError(f"the fitted bank voltage is not positive (V0 = {v0:.1f} V): check --sign")
    floor = 0.05 * v0
    weak = vc < floor
    vc = np.maximum(vc, floor)

    # target and correction (fractions)
    target = s * v0 * x_dem
    d_star = 0.5 * (1 + x_dem_ph * (v0 / vc)[:, None]) - delta
    d_new = d_cmd / 100.0 + gain * (d_star - d_cmd / 100.0)
    lo, hi = LIM_DUTY_MIN_PCT / 100.0, LIM_DUTY_MAX_PCT / 100.0
    clipped = on[:, None] & ((d_new < lo) | (d_new > hi))
    d_new = np.clip(d_new, lo, hi) * 100.0
    d_new[~on] = 0.0

    # back to setpoints
    new_p, tol_used = to_setpoints(p, tau_us, d_new, on, tol_pct, grid_us)
    check_profile(new_p)

    r = Result()
    r.profile = new_p
    r.tol_used = tol_used
    r.t0_ms, r.sign, r.quality, r.baseline = t0, s, quality, baseline
    r.v0 = v0
    ev = on & ~edge_mask(on, int(round(edge_us / grid_us)))      # error over the shot, edges excluded
    r.vc_end = float(vc[np.nonzero(on)[0][-1]])
    r.droop_pct = 100.0 * (v0 - r.vc_end) / v0
    r.rms_pct = 100.0 * float(np.sqrt(np.mean((y[ev] - target[ev]) ** 2))) / v0
    r.unreachable = intervals(tau_ms, clipped.any(axis=1))
    r.weak_vc = intervals(tau_ms, weak & on)
    r.tau_ms, r.y, r.target, r.vc, r.on = tau_ms, y, target, vc, on
    r.d_dem, r.d_cmd, r.d_new = d_dem, d_cmd, d_new
    r.use = use
    with np.errstate(divide="ignore", invalid="ignore"):
        r.vc_point = np.where(use, y / (s * x_cmd), np.nan)
    return r


def to_setpoints(p, tau_us, d_new, on, tol_pct, grid_us):
    """New Profile on: the demand's breakpoints + Douglas-Peucker points of the corrected duty, <= SHOT_SETPOINT_MAX."""
    bp = demand_breakpoints(p)
    bt = p.t_us[bp]
    tol = tol_pct
    segments = []
    for a, b in zip(bp[:-1], bp[1:]):
        if p.carrier_hz[a] <= 0:
            continue                                          # an off setpoint: nothing inside the gap
        ta, tb = p.t_us[a], p.t_us[b]
        end = tb if p.carrier_hz[b] > 0 else tb - grid_us     # on -> off: the last value is held into the gap
        sel = (tau_us >= ta) & (tau_us <= end)
        tt, yy = tau_us[sel], d_new[sel]
        if end > ta and (len(tt) == 0 or tt[-1] < end):       # the grid stops short of the segment's end
            tt = np.append(tt, end)
            yy = np.vstack([yy, _corrected_at(end, tau_us, d_new, on)])
        if len(tt) >= 2:
            segments.append((tt, yy))
    for _ in range(30):
        times = set(bt.tolist())
        for tt, yy in segments:
            for k in simplify(tt, yy, tol):
                times.add(float(round(tt[k])))
        t_new = np.array(sorted(times))
        if len(t_new) <= SHOT_SETPOINT_MAX:
            break
        tol *= 1.5
    else:
        raise CompError("could not fit the corrected profile into 500 setpoints")

    carrier = _values_at(p, p.carrier_hz[:, None], t_new)[:, 0]
    demand = _values_at(p, p.demand, t_new)
    cmd = np.empty_like(demand)
    for i, t in enumerate(t_new):
        cmd[i] = 0.0 if carrier[i] == 0 else _corrected_at(t, tau_us, d_new, on)
    cmd = np.round(cmd, 2)
    demand = np.round(demand, 2)
    return Profile(t_new, carrier, demand, cmd, p.meta), tol


def _values_at(p, values, t):
    """Setpoint values at times t: exact at a setpoint, linear between two on setpoints, held from an on setpoint to
    an off one. (Unlike eval_profile, t = the last time returns the last setpoint.)"""
    out = np.empty((len(t), values.shape[1]))
    for i, ti in enumerate(t):
        k = int(np.searchsorted(p.t_us, ti, side="right") - 1)
        k = min(max(k, 0), len(p.t_us) - 1)
        if p.t_us[k] == ti or k == len(p.t_us) - 1:
            out[i] = values[k]
        elif p.carrier_hz[k + 1] <= 0 or p.carrier_hz[k] <= 0:
            out[i] = values[k]
        else:
            f = (ti - p.t_us[k]) / (p.t_us[k + 1] - p.t_us[k])
            out[i] = values[k] + (values[k + 1] - values[k]) * f
    return out


def _corrected_at(t, tau_us, d_new, on):
    """The corrected duty at time t (the grid value at or just before t, the last on sample past the shot end)."""
    i = int(np.clip(np.searchsorted(tau_us, t, side="right") - 1, 0, len(tau_us) - 1))
    while i > 0 and not on[i]:
        i -= 1
    j = min(i + 1, len(tau_us) - 1)
    if on[i] and on[j] and tau_us[j] > tau_us[i] and tau_us[i] <= t <= tau_us[j]:
        f = (t - tau_us[i]) / (tau_us[j] - tau_us[i])
        return d_new[i] + (d_new[j] - d_new[i]) * f
    return d_new[i]


# =====================================================================================================================
# Reporting
# =====================================================================================================================
def plot(r, path, title):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    fig, ax = plt.subplots(3, 1, figsize=(11, 10), sharex=True)
    t = r.tau_ms
    on = r.on
    ax[0].plot(t, np.where(on | (np.abs(r.y) > 0), r.y, np.nan), color="#1f77b4", lw=1, label="measured")
    ax[0].plot(t, np.where(on, r.target, np.nan), color="#000", lw=1.4, ls="--", label="target (demand, undrooped bank)")
    for i, (a, b) in enumerate(r.unreachable):
        for x in (ax[0], ax[2]):
            x.axvspan(a, b, color="#d62728", alpha=0.12, lw=0,
                      label="not reachable: duty clamped" if (i == 0 and x is ax[0]) else None)
    ax[0].set_ylabel("output (V)")
    ax[0].legend(loc="best", fontsize=8)
    ax[0].set_title(title, fontsize=10)
    ax[1].plot(t, r.vc_point, ".", ms=1.5, color="#bbb", label="V / (s(2D-1)) pointwise")
    ax[1].plot(t, np.where(on, r.vc, np.nan), color="#d62728", lw=1.5, label="fitted bank voltage Vc(t)")
    ax[1].axhline(r.v0, color="#555", lw=0.8, ls=":", label=f"V0 = {r.v0:.0f} V")
    ax[1].set_ylabel("effective bank (V)")
    ax[1].legend(loc="best", fontsize=8)
    lo, hi = np.nanmin(r.vc[on]), np.nanmax(r.vc[on])
    pad = max(0.1 * (hi - lo), 0.02 * abs(hi))
    ax[1].set_ylim(lo - pad, hi + pad)
    for k in range(r.d_dem.shape[1]):
        tag = "" if r.d_dem.shape[1] == 1 else f" {PHASES[k]}"
        ax[2].plot(t, np.where(on, r.d_dem[:, k], np.nan), color="#000", lw=1, ls="--", label=f"demand{tag}")
        ax[2].plot(t, np.where(on, r.d_cmd[:, k], np.nan), color="#ff7f0e", lw=1, label=f"commanded (this shot){tag}")
        ax[2].plot(t, np.where(on, r.d_new[:, k], np.nan), color="#2ca02c", lw=1.3, label=f"new (next shot){tag}")
    ax[2].set_ylabel("duty (%)")
    ax[2].set_xlabel("shot time (ms)")
    ax[2].legend(loc="best", fontsize=8, ncol=r.d_dem.shape[1])
    for a in ax:
        a.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True


def next_name(path, iteration):
    base, ext = os.path.splitext(path)
    base = re.sub(r"_iter\d+$", "", base)
    return f"{base}_iter{iteration}{ext or '.csv'}"


def fmt_intervals(iv, limit=4):
    s = ", ".join(f"{a:.2f}-{b:.2f}" for a, b in iv[:limit])
    return s + (" ..." if len(iv) > limit else "")


# =====================================================================================================================
# Synthetic plant (self-test and unit tests)
# =====================================================================================================================
def simulate(p, *, v0=900.0, rc=0.02, sign=1, t0_ms=3.217, pre_ms=2.0, post_ms=5.0, noise=0.01, offset=0.0,
             dt_us=2.0, filt_us=10.0, seed=1, duty_offset_pct=0.0):
    """Measured output of a bank of capacitance C feeding a resistive load R (rc = R*C seconds): output
    s*Vc*(2(D+delta)-1), bank energy drained by the output power, C*Vc*dVc/dt = -Vout^2/R. Returns (t_ms, volts)."""
    rng = np.random.default_rng(seed)
    tau = np.arange(0.0, p.duration_us, dt_us)
    d, on = eval_profile(p.t_us, p.carrier_hz, p.commanded, tau)
    x = np.where(on, 2 * (d.mean(axis=1) / 100.0 + duty_offset_pct / 100.0) - 1, 0.0)
    vc = v0
    out = np.empty(len(tau))
    dt = dt_us * 1e-6
    for i, xi in enumerate(x):
        out[i] = sign * vc * xi
        vc -= out[i] ** 2 / (rc * vc) * dt               # dVc/dt = -Vout^2 / (R C Vc)
    t_ms = np.concatenate([np.arange(-pre_ms * 1000, 0, dt_us), tau, p.duration_us + np.arange(0, post_ms * 1000, dt_us)])
    v = np.concatenate([np.zeros(int(round(pre_ms * 1000 / dt_us))), out, np.zeros(int(round(post_ms * 1000 / dt_us)))])
    v = v[:len(t_ms)]
    a = dt_us / (filt_us + dt_us)                         # first-order filter, like the .filtered node
    for i in range(1, len(v)):
        v[i] = v[i - 1] + a * (v[i] - v[i - 1])
    v = v + offset + noise * v0 * rng.standard_normal(len(v))
    return t_ms / 1000.0 + t0_ms, v


def example_profile():
    """A Limiter demand: 80 % for 4 ms, ramp to 30 % by 6 ms, hold to 10 ms (single duty, 100 kHz)."""
    t = [0, 4000, 6000, 10000]
    c = [100000] * 4
    d = [[80.0], [80.0], [30.0], [30.0]]
    return Profile(t, c, d, d)


def selftest():
    p = example_profile()
    ok = True
    errs = []
    for it in range(4):
        t_ms, v = simulate(p, seed=it)
        r = compensate(p, t_ms, v)
        errs.append(r.rms_pct)
        print(f"  shot {it}: t0 {r.t0_ms:.3f} ms (true 3.217)  sign {r.sign:+d}  V0 {r.v0:.0f} V  droop "
              f"{r.droop_pct:.1f} %  rms error {r.rms_pct:.2f} % of V0  -> {len(r.profile.t_us)} setpoints")
        p = r.profile
    ok = errs[-1] < 2.0 and all(b <= a + 0.1 for a, b in zip(errs, errs[1:]))
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


# =====================================================================================================================
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?", help="the shot's CSV (demand, and commanded once it has been through here)")
    ap.add_argument("--shot", type=int, help="MDS shot number (default: the latest shot)")
    ap.add_argument("--out", help="output CSV (default: <csv>_iter<N>.csv)")
    ap.add_argument("--data", help="offline: a time_ms,volts CSV instead of MDS")
    ap.add_argument("--tree", default=DEFAULT_TREE)
    ap.add_argument("--node", default=DEFAULT_NODE)
    ap.add_argument("--t0-ms", type=float, help="measurement time of the shot's t=0 (skips the alignment)")
    ap.add_argument("--t0-search", type=float, nargs=2, metavar=("LO_MS", "HI_MS"), help="limit the t0 search")
    ap.add_argument("--sign", default="auto", choices=["auto", "+1", "-1", "1"])
    ap.add_argument("--gain", type=float, default=1.0, help="0 < gain <= 1: fraction of the correction to apply")
    ap.add_argument("--duty-offset", type=float, default=0.0, help="dead-time duty bias in %% (e.g. -1.9)")
    ap.add_argument("--tol-pct", type=float, default=0.1, help="setpoint fitting tolerance, duty %%")
    ap.add_argument("--knot-ms", type=float, default=0.5, help="bank-voltage fit resolution")
    ap.add_argument("--no-plot", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="run on a simulated bank, no MDS, no files")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.csv:
        ap.error("give the shot's CSV")
    try:
        p = read_profile(a.csv)
        check_profile(p)
        if a.data:
            shot = a.shot or p.meta.get("next_shot") or "offline"
            t_ms, v = read_data_csv(a.data)
        else:
            shot, t_ms, v = read_mds(a.shot, a.tree, a.node)
        r = compensate(p, t_ms, v, gain=a.gain, sign=a.sign if a.sign != "1" else "+1",
                       t0_ms=a.t0_ms, t0_search=a.t0_search, duty_offset_pct=a.duty_offset, tol_pct=a.tol_pct,
                       knot_ms=a.knot_ms)
    except CompError as e:
        print(f"ERROR {e}", file=sys.stderr)
        print(f"RESULT status=error message=\"{str(e).splitlines()[0]}\"")
        return 1
    except (OSError, ValueError) as e:
        print(f"ERROR {e}", file=sys.stderr)
        print(f"RESULT status=error message=\"{e}\"")
        return 2

    iteration = int(p.meta.get("iteration", "0") or 0) + 1
    out = a.out or next_name(a.csv, iteration)
    meta = {k: v for k, v in p.meta.items()}
    meta.update({
        "tool": "limiter_compensate.py",
        "iteration": iteration,
        "from_shot": shot,
        "from_csv": os.path.basename(a.csv),
        "t0_ms": f"{r.t0_ms:.4f}",
        "sign": f"{r.sign:+d}",
        "v0_volts": f"{r.v0:.1f}",
        "droop_pct": f"{r.droop_pct:.2f}",
        "rms_error_pct": f"{r.rms_pct:.2f}",
        "gain": a.gain,
        "duty_offset_pct": a.duty_offset,
    })
    write_profile(out, r.profile, meta)
    png = os.path.splitext(out)[0] + ".png"
    title = (f"Limiter shot {shot}  (iteration {iteration})   t0 {r.t0_ms:.3f} ms   sign {r.sign:+d}   "
             f"V0 {r.v0:.0f} V   droop {r.droop_pct:.1f} %   output error {r.rms_pct:.2f} % of V0 (rms)")
    plotted = (not a.no_plot) and plot(r, png, title)

    q = "" if r.quality is None else f" match={r.quality:.2f}"
    print(f"RESULT status=ok shot={shot} iteration={iteration} out={out} t0_ms={r.t0_ms:.3f} sign={r.sign:+d} "
          f"v0={r.v0:.1f} droop_pct={r.droop_pct:.2f} rms_pct={r.rms_pct:.2f} "
          f"setpoints={len(r.profile.t_us)}{q}")
    if r.unreachable:
        print(f"WARNING demand not reachable with this bank (duty clamped to {LIM_DUTY_MIN_PCT:g}-{LIM_DUTY_MAX_PCT:g} %) "
              f"at {fmt_intervals(r.unreachable)} ms", file=sys.stderr)
    if r.weak_vc:
        print(f"WARNING fitted bank voltage below 5 % of V0 at {fmt_intervals(r.weak_vc)} ms", file=sys.stderr)
    if r.quality is not None and r.quality < 0.8:
        print(f"WARNING weak match between measurement and profile ({r.quality:.2f}); check the shot number",
              file=sys.stderr)
    if r.tol_used > a.tol_pct:
        print(f"WARNING tolerance widened to {r.tol_used:.3f} % to fit {SHOT_SETPOINT_MAX} setpoints", file=sys.stderr)
    if plotted:
        print(f"plot: {png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

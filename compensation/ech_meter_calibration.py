#!/usr/bin/env python3
"""
ech_meter_calibration.py -- the ratio between the ECH's two forward-power meters, for ech_compensate.py --power-scale.

WHY
    Shots into the dummy load are measured by the dummy-load meter (ECH_PROC.DL_MONITOR_F:FILTERED); experimental shots
    into the vessel by the waveguide forward-power meter (ECH_PROC.WG_MONITOR_F:FILTERED). The two never see the same
    pulse: a shot goes to one target or the other. So a profile tuned on the dummy load lands at a different "experimental"
    power when it is fired into the vessel, by the ratio of the two meters (and of the two waveguide paths' losses).

    The power scale K is   (experimental-meter reading) / (dummy-load-meter reading)   for the same gyrotron output.
    It is measured from back-to-back shots that fire the SAME profile, at the SAME bank charge and gyrotron settings, into
    each target in turn. The supply does not know where the power goes, so the gyrotron voltage and current should be the
    same on both; this script checks that before it trusts the ratio.

HOW TO RUN
    Take the calibration shots first: one profile (a staircase gives several power levels in one shot), fixed bank
    charge, alternating targets -- dummy load, vessel, dummy load, vessel -- at least two of each. Then:

        python ech_meter_calibration.py 261118030 261118031 261118032 261118033

    Each shot's target comes from MDS (ECH_PARAMS:ECH_TARGET). Offline, give CSV files instead of shot numbers: each with
    columns time_ms, forward power (kW), gyrotron voltage (kV), gyrotron current (A), and a '# target=dummy-load' or
    '# target=vessel' line (the files figures/*/plot_shot.py --fetch writes, or --save-data DIR below, work as they are).

    It prints one RESULT line, writes ech_meter_calibration.json and .png (--out PREFIX to change), and says which
    --power-scale to give ech_compensate.py.

        RESULT status=ok power_scale=1.034 uncertainty=0.006 per_shot_spread=0.004 fit_slope=1.041 fit_offset_kw=-1.3
               v_mismatch_pct=0.21 i_mismatch_pct=0.35 lag_ms=0.010 dummy_shots=2 vessel_shots=2 samples=2318

METHOD
    1. Each shot: baseline (mean before t = -0.2 ms) removed from the power; everything resampled to a 10 us grid.
    2. The shots of each target are averaged. The two targets' gyrotron voltage and current are compared over the flat
       tops (rms difference, % of the mean); more than --max-mismatch-pct (1 %) is refused, because then the gyrotron
       itself ran differently and the ratio would mix physics into the calibration (--force to accept it anyway).
    3. A small timing difference between the meters (filters, cabling) is found by cross-correlating the two power traces
       within +/-0.3 ms, and removed.
    4. Flat-top samples only: power above --min-frac (0.5) of the peak, more than --settle-ms (1.0) after the start of
       each pulse and more than 0.2 ms before its end, away from fast edges.
    5. K = sum(P_vessel) / sum(P_dummy) over those samples (a power-weighted ratio). Its uncertainty combines the spread
       between individual vessel/dummy shot pairs with the sample scatter. A straight-line fit P_vessel = a P_dummy + b
       over the same samples shows whether the ratio depends on power (a meter nonlinearity or an offset); when b is
       large the README explains what to do.

    python ech_meter_calibration.py --selftest      checks the method on synthetic shots with a known ratio
    Talks to MDSplus (read only) and the terminal that runs it, nothing else: no controller, no hub, no network service.
    Needs numpy; matplotlib for the plot; MDSplus only to read shots.
"""
import argparse
import json
import os
import sys

import numpy as np

NODE_V = "ECH_PROC.GYROTRON_V:FILTERED"
NODE_I = "ECH_PROC.GYROTRON_I:FILTERED"
NODE_P = {"vessel": "ECH_PROC.WG_MONITOR_F:FILTERED", "dummy-load": "ECH_PROC.DL_MONITOR_F:FILTERED"}
NODE_TARGET = "ECH_PARAMS:ECH_TARGET"
NODE_BANK = "ECH_PARAMS:HVPS_V"
GRID_MS = 0.01
MAX_LAG_MS = 0.3


class CalError(Exception):
    pass


def target_of(text):
    s = (text or "").strip().lower().replace("_", "-")
    if s.startswith("dummy") or s == "dl":
        return "dummy-load"
    if s.startswith(("vessel", "plasma", "waveguide", "wg", "machine")):
        return "vessel"
    return None


# ----------------------------------------------------------------------------------------------------------------------
# Reading shots
# ----------------------------------------------------------------------------------------------------------------------
def read_mds(shot):
    try:
        import MDSplus as mds
    except ImportError:
        raise CalError("the MDSplus Python package is not installed (give CSV files instead of shot numbers)")
    try:
        t = mds.Tree("ech", int(shot), "readonly")
    except Exception as e:                                              # noqa: BLE001
        raise CalError(f"could not open the ech tree for shot {shot}: {e}")
    try:
        target = target_of(str(t.getNode(NODE_TARGET).data()))
    except Exception:                                                   # noqa: BLE001
        target = None
    if target is None:
        raise CalError(f"shot {shot}: ECH_PARAMS:ECH_TARGET does not say dummy load or vessel")

    def sig(path):
        n = t.getNode(path)
        return np.asarray(n.dim_of().data(), float) * 1e3, np.asarray(n.data(), float)

    try:
        tv, v = sig(NODE_V)
        ti, i = sig(NODE_I)
        tp, p = sig(NODE_P[target])
    except Exception as e:                                              # noqa: BLE001
        raise CalError(f"shot {shot}: could not read the gyrotron signals ({e})")
    bank = None
    try:
        bank = float(t.getNode(NODE_BANK).data())
    except Exception:                                                   # noqa: BLE001
        pass
    return {"name": str(shot), "target": target, "t": tv, "p": np.interp(tv, tp, p), "v": v / 1e3,
            "i": np.interp(tv, ti, i), "bank": bank}


def read_csv(path):
    """time_ms, power (kW), voltage (kV), current (A) in any column order, named in a header line; target from a
    '# target=' or '# ... target=...' comment."""
    target, bank, header, rows = None, None, None, []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            body = s.lstrip("#").strip()
            if "time_ms" in body.lower() and "," in body:              # the column names (commented or not)
                header = [c.strip().lower() for c in body.split(",")]
                continue
            if s.startswith("#"):                                       # key=value tokens anywhere in a comment
                for tok in body.split():
                    if "=" in tok:
                        k, val = tok.split("=", 1)
                        if k.lower() == "target":
                            target = target_of(val)
                        elif k.lower() in ("bank_v", "bank_volts"):
                            try:
                                bank = float(val)
                            except ValueError:
                                pass
                continue
            try:
                rows.append([float(c) for c in s.split(",")])
            except ValueError:
                continue
    if target is None:
        raise CalError(f"{path}: no '# target=dummy-load' or '# target=vessel' line")
    if header is None or len(rows) < 100:
        raise CalError(f"{path}: need a header line and data rows")

    def col(*keys):
        for k in keys:
            for j, h in enumerate(header):
                if k in h:
                    return j
        raise CalError(f"{path}: no column for {keys[0]} (header: {','.join(header)})")

    a = np.array(rows)
    jt, jp, jv, ji = col("time"), col("power"), col("_kv", "voltage", "gyrotron_v"), col("_a", "current", "gyrotron_i")
    a = a[np.argsort(a[:, jt])]
    return {"name": os.path.splitext(os.path.basename(path))[0], "target": target, "t": a[:, jt], "p": a[:, jp],
            "v": a[:, jv], "i": a[:, ji], "bank": bank}


def save_csv(s, path):
    header = (f"shot={s['name']} target={s['target']}" + (f" bank_v={s['bank']:g}" if s.get("bank") else "")
              + "\ntime_ms,forward_power_kw,gyrotron_v_kv,gyrotron_i_a")
    np.savetxt(path, np.column_stack([s["t"], s["p"], s["v"], s["i"]]), delimiter=",", header=header, comments="# ",
               fmt="%.6g")


# ----------------------------------------------------------------------------------------------------------------------
# The calibration
# ----------------------------------------------------------------------------------------------------------------------
def on_grid(s, grid):
    """Cell averages on the grid (the signals are sampled far finer than 10 us); power baseline removed."""
    t = s["t"]
    out = {}
    pre = t < -0.2
    base = float(np.mean(s["p"][pre])) if pre.sum() > 20 else 0.0
    for key, x in (("p", s["p"] - base), ("v", s["v"]), ("i", s["i"])):
        cum = np.concatenate([[0.0], np.cumsum(0.5 * (x[1:] + x[:-1]) * np.diff(t))])
        hi = np.interp(grid + GRID_MS / 2, t, cum)
        lo = np.interp(grid - GRID_MS / 2, t, cum)
        out[key] = (hi - lo) / GRID_MS
    out["baseline_kw"] = base
    return out


def flat_mask(grid, p, *, min_frac, settle_ms, end_ms=0.2):
    """Samples on the flat tops: power above min_frac of the peak, settle_ms after each pulse start, end_ms before its end."""
    on = p > min_frac * np.nanmax(p)
    m = np.zeros(len(grid), bool)
    idx = np.nonzero(np.diff(np.concatenate([[0], on.astype(int), [0]])))[0]
    for a, b in zip(idx[::2], idx[1::2]):
        t0, t1 = grid[a], grid[b - 1]
        m |= (grid >= t0 + settle_ms) & (grid <= t1 - end_ms) & on
    return m


def best_lag(a, b, max_lag):
    """How many samples b lags a (positive: b later), within +/-max_lag. Correlates the time derivatives (the edges carry
    the timing) over one fixed window, normalized, so the overlap length does not bias the answer towards zero."""
    da, db = np.gradient(a), np.gradient(b)
    n = len(a)
    x = da[max_lag:n - max_lag]
    x = (x - x.mean()) / (x.std() or 1.0)
    best, best_k = -np.inf, 0
    for k in range(-max_lag, max_lag + 1):
        y = db[max_lag + k:n - max_lag + k]
        c = float(np.mean(x * (y - y.mean())) / (y.std() or 1.0))
        if c > best:
            best, best_k = c, k
    return best_k


def shift(x, k):
    """x delayed by k samples (positive k: later), edges held."""
    if k == 0:
        return x
    if k > 0:
        return np.concatenate([np.full(k, x[0]), x[:-k]])
    return np.concatenate([x[-k:], np.full(-k, x[-1])])


def calibrate(shots, *, min_frac=0.5, settle_ms=1.0, max_mismatch_pct=1.0, force=False):
    dummy = [s for s in shots if s["target"] == "dummy-load"]
    vessel = [s for s in shots if s["target"] == "vessel"]
    if not dummy or not vessel:
        raise CalError(f"need shots into both targets: got {len(dummy)} dummy-load and {len(vessel)} vessel")
    t_lo = max(max(s["t"][0] for s in shots), -1.0)
    t_hi = min(min(s["t"][-1] for s in shots), 30.0)
    grid = np.arange(t_lo + GRID_MS, t_hi - GRID_MS, GRID_MS)
    gd = [on_grid(s, grid) for s in dummy]
    gv = [on_grid(s, grid) for s in vessel]
    mean = lambda gs, k: np.mean([g[k] for g in gs], axis=0)          # noqa: E731
    pd, pv = mean(gd, "p"), mean(gv, "p")
    vd, vv, id_, iv = mean(gd, "v"), mean(gv, "v"), mean(gd, "i"), mean(gv, "i")

    m = flat_mask(grid, pd, min_frac=min_frac, settle_ms=settle_ms)
    if m.sum() < 50:
        raise CalError("too few flat-top samples: are these the same profile? (check --min-frac / --settle-ms)")
    notes = []
    v_mis = 100 * np.sqrt(np.mean((vv[m] - vd[m]) ** 2)) / np.mean(vd[m])
    i_mis = 100 * np.sqrt(np.mean((iv[m] - id_[m]) ** 2)) / np.mean(id_[m])
    if max(v_mis, i_mis) > max_mismatch_pct:
        msg = (f"the gyrotron ran differently on the two targets (voltage differs by {v_mis:.2f} %, current by "
               f"{i_mis:.2f} % rms): the ratio would include that, not just the meters")
        if not force:
            raise CalError(msg + " (--force to accept it)")
        notes.append(msg)
    banks = {s["bank"] for s in shots if s.get("bank") is not None}
    if len(banks) > 1:
        notes.append(f"the shots were taken at different bank charges ({', '.join(f'{b:g}' for b in sorted(banks))} V)")

    lag = best_lag(pd, pv, int(round(MAX_LAG_MS / GRID_MS)))
    pv_al = shift(pv, -lag)                                            # vessel trace moved onto the dummy-load timing
    if abs(lag) * GRID_MS >= MAX_LAG_MS - GRID_MS:
        notes.append(f"the meters' timing differs by {lag * GRID_MS:+.2f} ms, the edge of the search: check the shots match")

    K = float(np.sum(pv_al[m]) / np.sum(pd[m]))
    A = np.column_stack([pd[m], np.ones(m.sum())])
    (a, b), *_ = np.linalg.lstsq(A, pv_al[m], rcond=None)
    # spread between individual vessel / dummy pairs: each vessel shot against each dummy shot
    pairs = []
    for x in gv:
        for y in gd:
            pairs.append(float(np.sum(shift(x["p"], -lag)[m]) / np.sum(y["p"][m])))
    spread = float(np.std(pairs, ddof=1)) if len(pairs) > 1 else float("nan")
    resid = pv_al[m] / pd[m] - K
    n_eff = max(1, int(m.sum() * GRID_MS / 0.1))                         # ~independent samples: one per 0.1 ms
    sample_err = float(np.std(resid) / np.sqrt(n_eff))
    unc = float(np.sqrt((spread / np.sqrt(len(pairs))) ** 2 + sample_err ** 2)) if np.isfinite(spread) else sample_err
    # ratio against power level, in bins, for the plot and the README's check
    edges = np.linspace(min_frac * np.nanmax(pd), np.nanmax(pd), 6)
    bins = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        k = m & (pd >= lo) & (pd < hi + 1e-9)
        if k.sum() >= 20:
            bins.append({"p_dummy_kw": float(np.mean(pd[k])), "ratio": float(np.sum(pv_al[k]) / np.sum(pd[k])),
                         "samples": int(k.sum())})
    if abs(b) > 0.03 * np.mean(pd[m]):
        notes.append(f"the straight-line fit has an offset of {b:.1f} kW: the ratio changes with power "
                     f"(see the plot); K is the power-weighted average over these levels")
    return {"power_scale": K, "uncertainty": unc, "per_shot_spread": spread, "fit_slope": float(a), "fit_offset_kw": float(b),
            "v_mismatch_pct": float(v_mis), "i_mismatch_pct": float(i_mis), "lag_ms": float(lag * GRID_MS),
            "dummy_shots": [s["name"] for s in dummy], "vessel_shots": [s["name"] for s in vessel],
            "samples": int(m.sum()), "bins": bins, "notes": notes,
            "baselines_kw": {s["name"]: g["baseline_kw"] for s, g in zip(dummy + vessel, gd + gv)},
            "_grid": grid, "_pd": pd, "_pv": pv_al, "_vd": vd, "_vv": vv, "_id": id_, "_iv": iv, "_mask": m}


def plot(path, r):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    g, m = r["_grid"], r["_mask"]
    fig, ax = plt.subplots(2, 2, figsize=(11, 7))
    ax[0, 0].plot(g, r["_pd"], lw=1, label="dummy-load meter (mean)")
    ax[0, 0].plot(g, r["_pv"], lw=1, label="experimental meter (mean, aligned)")
    ax[0, 0].plot(g, r["_pd"] * r["power_scale"], lw=0.8, ls="--", color="#444", label=f"dummy-load x {r['power_scale']:.3f}")
    ax[0, 0].set_ylabel("forward power (kW)")
    ax[0, 1].plot(g, r["_vd"], lw=1, label="voltage, dummy-load shots")
    ax[0, 1].plot(g, r["_vv"], lw=1, label="voltage, vessel shots")
    ax[0, 1].set_ylabel("gyrotron voltage (kV)")
    ax[1, 1].plot(g, r["_id"], lw=1, label="current, dummy-load shots")
    ax[1, 1].plot(g, r["_iv"], lw=1, label="current, vessel shots")
    ax[1, 1].set_ylabel("gyrotron current (A)")
    ax[1, 0].plot(r["_pd"][m], r["_pv"][m] / r["_pd"][m], ".", ms=1.5, alpha=0.4, color="#888", label="flat-top samples")
    if r["bins"]:
        ax[1, 0].plot([b["p_dummy_kw"] for b in r["bins"]], [b["ratio"] for b in r["bins"]], "o-", color="#d62728",
                      label="binned")
    ax[1, 0].axhline(r["power_scale"], color="#000", lw=1, ls="--", label=f"K = {r['power_scale']:.4f} +/- {r['uncertainty']:.4f}")
    ax[1, 0].set_xlabel("dummy-load meter (kW)")
    ax[1, 0].set_ylabel("experimental / dummy-load")
    ax[0, 0].plot(g, np.where(m, r["_pd"], np.nan), lw=2.5, alpha=0.25, color="#2ca02c", label="samples used")
    for a in (ax[0, 0], ax[0, 1], ax[1, 1]):
        a.set_xlabel("shot time (ms)")
    for a in ax.flat:
        a.grid(alpha=0.3)
        a.legend(fontsize=8, loc="best")
    fig.suptitle(f"ECH meter calibration: dummy load {', '.join(r['dummy_shots'])}; vessel {', '.join(r['vessel_shots'])}",
                 fontsize=10)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True


# ----------------------------------------------------------------------------------------------------------------------
# Self-test
# ----------------------------------------------------------------------------------------------------------------------
def synthetic(target, K, seed, *, offset_kw=0.0, v_shift=0.0, lag_ms=0.0):
    rng = np.random.default_rng(seed)
    t = np.arange(-2.0, 17.0, 0.0005)
    on = ((t > 0) & (t < 4)) | ((t > 5.5) & (t < 9.5)) | ((t > 11) & (t < 15))
    level = np.where(t < 5, 133.0, np.where(t < 10.5, 166.0, 200.0))
    tau = 0.4
    env = np.zeros_like(t)
    for k in range(1, len(t)):
        env[k] = env[k - 1] + (float(on[k]) - env[k - 1]) * (t[k] - t[k - 1]) / tau
    p_true = level * env * (1 + 0.02 * np.sin(2 * np.pi * t / 3.0))
    v = (40 + 25 * env) * (1 + v_shift) + rng.normal(0, 0.05, len(t))
    i = 20 * env + 4 * np.sin(2 * np.pi * 20.0 * t) * env + rng.normal(0, 0.1, len(t))
    p = p_true * (K if target == "vessel" else 1.0) + rng.normal(0, 1.0, len(t)) + 3.0
    if target == "vessel":
        p = p + offset_kw * env
        p = np.interp(t - lag_ms, t, p)
    return {"name": f"{target}-{seed}", "target": target, "t": t, "p": p, "v": v, "i": i, "bank": 520.0}


def selftest():
    ok = True
    for K, lag in ((1.0, 0.0), (1.07, 0.05), (0.93, -0.03)):
        shots = [synthetic("dummy-load", K, 1), synthetic("vessel", K, 2, lag_ms=lag), synthetic("dummy-load", K, 3),
                 synthetic("vessel", K, 4, lag_ms=lag)]
        r = calibrate(shots)
        good = abs(r["power_scale"] - K) < 0.01 and abs(r["lag_ms"] - lag) <= 0.02
        ok &= good
        print(f"  true K {K:.3f} lag {lag:+.2f} ms -> K {r['power_scale']:.4f} +/- {r['uncertainty']:.4f}, "
              f"lag {r['lag_ms']:+.2f} ms  {'ok' if good else 'FAIL'}")
    try:
        calibrate([synthetic("dummy-load", 1.0, 1), synthetic("vessel", 1.0, 2, v_shift=0.03)])
        print("  a 3 % voltage difference was accepted  FAIL")
        ok = False
    except CalError:
        print("  a 3 % voltage difference between targets is refused  ok")
    print("SELFTEST PASSED" if ok else "SELFTEST FAILED")
    return 0 if ok else 1


# ----------------------------------------------------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shots", nargs="*", help="MDS shot numbers and/or CSV files (dummy-load and vessel shots, any order)")
    ap.add_argument("--out", default="ech_meter_calibration", help="output prefix: <out>.json and <out>.png")
    ap.add_argument("--save-data", metavar="DIR", help="also write each shot's data as a CSV in DIR (to rerun offline)")
    ap.add_argument("--min-frac", type=float, default=0.5, help="flat-top samples: power above this fraction of the peak")
    ap.add_argument("--settle-ms", type=float, default=1.0, help="skip this long after each pulse start")
    ap.add_argument("--max-mismatch-pct", type=float, default=1.0,
                    help="refuse when the two targets' gyrotron voltage or current differ by more (rms %%)")
    ap.add_argument("--force", action="store_true", help="accept a voltage/current mismatch (it is reported)")
    ap.add_argument("--no-plot", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if len(a.shots) < 2:
        ap.error("give at least one dummy-load and one vessel shot")
    try:
        shots = [read_csv(x) if os.path.isfile(x) else read_mds(x) for x in a.shots]
        if a.save_data:
            os.makedirs(a.save_data, exist_ok=True)
            for s in shots:
                save_csv(s, os.path.join(a.save_data, f"shot_{s['name']}.csv"))
        r = calibrate(shots, min_frac=a.min_frac, settle_ms=a.settle_ms, max_mismatch_pct=a.max_mismatch_pct,
                      force=a.force)
    except (CalError, OSError, ValueError) as e:
        print(f"ERROR {e}", file=sys.stderr)
        print(f"RESULT status=error message=\"{str(e).splitlines()[0]}\"")
        return 1
    out = {k: v for k, v in r.items() if not k.startswith("_")}
    with open(a.out + ".json", "w") as f:
        json.dump(out, f, indent=2)
    plotted = (not a.no_plot) and plot(a.out + ".png", r)
    print(f"RESULT status=ok power_scale={r['power_scale']:.4f} uncertainty={r['uncertainty']:.4f} "
          f"per_shot_spread={r['per_shot_spread']:.4f} fit_slope={r['fit_slope']:.4f} fit_offset_kw={r['fit_offset_kw']:.1f} "
          f"v_mismatch_pct={r['v_mismatch_pct']:.2f} i_mismatch_pct={r['i_mismatch_pct']:.2f} lag_ms={r['lag_ms']:.3f} "
          f"dummy_shots={len(r['dummy_shots'])} vessel_shots={len(r['vessel_shots'])} samples={r['samples']}")
    for n in r["notes"]:
        print(f"WARNING {n}", file=sys.stderr)
    print(f"use: python ech_compensate.py <csv> --power-scale {r['power_scale']:.4f}   ({a.out}.json"
          + (f", {a.out}.png" if plotted else "") + ")", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

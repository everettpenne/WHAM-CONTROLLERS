"""shot_data.py -- what the operator console does with the data a controller logged during a shot: turn the raw log into a
recording (physical units), work out the numbers that matter, draw it, and keep it on disk.

The controllers' own waveform log (`LOG:ARM` / `LOG:DATA?`, both products) holds, per channel (XREX) or phase (HBRIDGE), three series
sampled on the control tick: the SETPOINT the loop was driving to, the MEASURED feedback, and the OUTPUT the controller wrote.
  XREX     all three in Hz (the supplies are driven and read back as pulse frequencies). Amps are derived with the controller's own
           linear calibration (TURNONHz, MAXFREQHz, MAXCURRent): A = (Hz - TURNON) / (MAXFREQ - TURNON) * MAXCURRENT. That calibration
           is a placeholder until it is measured on a real Transrex, and the report says so.
  HBRIDGE  target and measured in volts (milli-volts on the wire), output in Hz (HVPS) or % duty (Limiter) (milli-units on the wire).
           The controller logs only during a closed-loop run.

Everything here is pure (no sockets), so it is tested without hardware. A saved recording is a folder
    <shots_dir>/<controller>/<YYYYmmdd-HHMMSS>/   shot.json  shot.csv  report.html  report.md  summary.json
and its id ("sid") is "<controller>/<YYYYmmdd-HHMMSS>".
"""
import csv
import datetime
import html
import io
import json
import math
import os
import re

SID_RE = re.compile(r"^[A-Za-z0-9_.-]{1,80}/[0-9]{8}-[0-9]{6}(-[0-9]{1,3})?$")
FILES = ("shot.json", "shot.csv", "report.html", "report.md")
XREX_NOTE = ("Amps are estimated from the controller's linear frequency-to-current calibration (CONFig:TURNONHz, MAXFREQHz, MAXCURRent). "
             "That calibration is a placeholder until it is measured on a real Transrex, so treat the amps as indicative; the Hz values are exact.")


# ---- the log on the wire ---------------------------------------------------------------------------------------------
def parse_xrex_log(reply):
    """'OK <count> <rateHz> s1 m1 o1 s2 ...' -> (count, rate, [[s],[m],[o]]) or raises ValueError (short, garbled)."""
    parts = reply.split()
    if len(parts) < 3 or parts[0] != "OK":
        raise ValueError(f"not a log reply: {reply[:60]}")
    count, rate = int(parts[1]), float(parts[2])
    nums = [int(x) for x in parts[3:]]
    if len(nums) != 3 * count:
        raise ValueError(f"expected {3 * count} numbers for {count} samples, got {len(nums)}")
    return count, rate, [nums[0::3], nums[1::3], nums[2::3]]


def parse_hbridge_page(reply, start, want):
    """'OK <count> <rateHz> SUM=<s> a b c ...' (one page) -> (count, rate, [[s],[m],[o]]); checks SUM and the length."""
    parts = reply.split()
    if len(parts) < 4 or parts[0] != "OK" or not parts[3].startswith("SUM="):
        raise ValueError(f"not a log page: {reply[:60]}")
    count, rate, total = int(parts[1]), float(parts[2]), int(parts[3][4:])
    nums = [int(x) for x in parts[4:]]
    n = max(0, min(want, count - start))
    if len(nums) != 3 * n:
        raise ValueError(f"expected {3 * n} numbers, got {len(nums)}")
    if sum(x & 0xFFFFFFFF for x in nums) & 0xFFFFFFFF != total:
        raise ValueError("the page's check sum does not match (a character was lost on the link)")
    return count, rate, [nums[0::3], nums[1::3], nums[2::3]]


def choose_decimation(duration_s, tick_hz, max_samples, margin=1.15):
    """The smallest decimation that lets `max_samples` cover `duration_s` (+margin); returns (decim, sample rate, note or None)."""
    decim = max(1, math.ceil(duration_s * margin * tick_hz / max_samples))
    note = None
    if decim > 10000:
        decim = 10000
        note = "The shot is longer than the log can cover at its coarsest rate: the end of the shot is not recorded."
    return decim, tick_hz / decim, note


# ---- building a recording --------------------------------------------------------------------------------------------
def channel_name(product, n):
    return f"Channel {n}" if product == "xrex" else f"Phase {'UVW'[n - 1]}"


def build_recording(product, controller, outcome, rate_hz, decim, raw, calib=None, mode=None, meta=None, notes=None, started=None):
    """raw: {channel number: [[setpoint...], [measured...], [output...]]} straight from the log.
    Native units: XREX Hz; HBRIDGE volts and (HVPS) Hz or (Limiter) % duty."""
    started = started or datetime.datetime.now().astimezone()
    out_unit = "Hz" if product == "xrex" else ("kHz" if mode == "HVPS" else "%")
    chans = []
    for n in sorted(raw):
        s, m, o = raw[n]
        if product == "xrex":
            ser = {"sp": [float(x) for x in s], "pv": [float(x) for x in m], "out": [float(x) for x in o]}
            units = {"sp": "Hz", "pv": "Hz", "out": "Hz"}
        else:
            ser = {"sp": [x / 1000.0 for x in s], "pv": [x / 1000.0 for x in m], "out": [x / 1000.0 / (1000.0 if out_unit == "kHz" else 1.0) for x in o]}
            units = {"sp": "V", "pv": "V", "out": out_unit}
        chans.append({"n": n, "name": channel_name(product, n), "units": units, **ser})
    count = min((len(c["sp"]) for c in chans), default=0)
    rec = {"version": 1, "product": product, "controller": controller, "started": started.isoformat(timespec="seconds"),
           "outcome": outcome, "rate_hz": rate_hz, "decim": decim, "count": count, "duration_s": (count / rate_hz) if rate_hz else 0.0,
           "mode": mode, "calib": calib or {}, "meta": meta or {}, "notes": list(notes or []), "channels": chans}
    rec["notes"] += [n for n in diagnose(rec) if n not in rec["notes"]]
    return rec


def diagnose(rec):
    """Plain-language findings about a recording that look like a setup problem rather than a real result."""
    notes = []
    meta_ch = (rec.get("meta") or {}).get("channels") or {}
    for ch in rec["channels"]:
        n, name = ch["n"], ch["name"]
        sp, pv, out = ch["sp"], ch["pv"], ch["out"]
        if not sp:
            continue
        sp_rng, out_rng, pv_rng = max(sp) - min(sp), max(out) - min(out), max(pv) - min(pv)
        info = meta_ch.get(str(n)) or {}
        if rec["product"] != "xrex":
            continue
        if info.get("in_shot") is False and sp_rng == 0 and out_rng == 0:
            notes.append(f"{name} was not enabled for this shot (SOURce:ENAble {n} 0), so its log is empty.")
            continue
        if sp_rng > 1000 and out_rng < 0.02 * sp_rng:
            loop, gains = info.get("loop"), info.get("gains_kp_ki_kd")
            why = ("It was in closed loop with all PID gains zero (PID:GAINS), which holds the output at the minimum frequency." if loop == "closed" and gains and not any(gains)
                   else "It was in open loop, where the output should follow the setpoint: check that the channel is enabled and its output is running." if loop == "open"
                   else "Check its loop mode (PID:LOOPMODE) and PID gains (PID:GAINS): a closed loop with zero gains holds the output at the minimum frequency.")
            notes.append(f"{name}: the output did not follow the setpoint (it stayed near {out[len(out) // 2]:,.0f} Hz while the setpoint went from "
                         f"{min(sp):,.0f} to {max(sp):,.0f} Hz). {why}")
        elif out_rng > 1000 and pv_rng < 0.2 * out_rng:
            notes.append(f"{name}: the measured feedback barely moved while the output did. Check the feedback wiring (fibre or loopback to the PFM input).")
    return notes


def amps_from_hz(rec, n, hz_list):
    """The controller's linear calibration, or None when it is not known."""
    c = rec.get("calib") or {}
    try:
        t, m = float(c["turnon_hz"]), float(c["max_hz"])
        a = float((c.get("max_amps") or {})[str(n)] if str(n) in (c.get("max_amps") or {}) else c["max_amps"][n])
    except (KeyError, TypeError, ValueError):
        return None
    if m <= t or a <= 0:
        return None
    return [(h - t) / (m - t) * a for h in hz_list]


def view(rec, n, unit="native"):
    """One channel's series in the unit asked for ('A' for XREX when calibrated). Returns (series dict, units dict, unit label)."""
    ch = next(c for c in rec["channels"] if c["n"] == n)
    if rec["product"] == "xrex" and unit == "A":
        conv = {k: amps_from_hz(rec, n, ch[k]) for k in ("sp", "pv", "out")}
        if all(v is not None for v in conv.values()):
            return conv, {"sp": "A", "pv": "A", "out": "A"}, "A"
    return {k: ch[k] for k in ("sp", "pv", "out")}, dict(ch["units"]), "native"


# ---- the numbers -----------------------------------------------------------------------------------------------------
def _fmt(x, unit=""):
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return "-"
    a = abs(x)
    s = f"{x:,.0f}" if a >= 1000 else f"{x:.1f}" if a >= 100 else f"{x:.2f}" if a >= 1 else f"{x:.3f}"
    return f"{s} {unit}".strip()


def stats(rec, n, unit="native"):
    """The figures an operator reads off a shot, as [(label, text), ...] plus a dict of the raw values."""
    ser, units, _ = view(rec, n, unit)
    sp, pv, out, rate = ser["sp"], ser["pv"], ser["out"], rec["rate_hz"]
    u, uo = units["sp"], units["out"]
    N = len(sp)
    if N == 0:
        return [("Samples", "0")], {}
    err = [p - s for p, s in zip(pv, sp)]
    rms = math.sqrt(sum(e * e for e in err) / N)
    mean = sum(err) / N
    imax = max(range(N), key=lambda i: abs(err[i]))
    sp_pk, pv_pk = max(sp), max(pv)
    ipk = max(range(N), key=lambda i: pv[i])
    top = [i for i in range(N) if sp_pk > 0 and sp[i] >= 0.98 * sp_pk]
    raw = {"samples": N, "rms_error": rms, "mean_error": mean, "max_abs_error": abs(err[imax]), "max_abs_error_t": imax / rate,
           "setpoint_peak": sp_pk, "measured_peak": pv_pk, "measured_peak_t": ipk / rate, "output_min": min(out), "output_max": max(out)}
    rows = [("Samples", f"{N} at {rate:g} samples/s ({N / rate:.3g} s)"),
            ("Setpoint peak", _fmt(sp_pk, u)), ("Measured peak", f"{_fmt(pv_pk, u)} at {ipk / rate:.3g} s"),
            ("Error (measured - setpoint): rms", _fmt(rms, u)), ("Error: mean", _fmt(mean, u)),
            ("Error: largest", f"{_fmt(err[imax], u)} at {imax / rate:.3g} s")]
    if sp_pk > 0:
        raw["overshoot_pct"] = (pv_pk - sp_pk) / sp_pk * 100.0
        rows.append(("Overshoot of the setpoint peak", f"{raw['overshoot_pct']:+.1f} %"))
    if len(top) >= 3:
        pt = [pv[i] for i in top]
        mt = sum(pt) / len(pt)
        raw["flat_top_mean"], raw["flat_top_ripple_pp"] = mt, max(pt) - min(pt)
        raw["flat_top_error_pct"] = (mt - sum(sp[i] for i in top) / len(top)) / sp_pk * 100.0
        rows += [("Flat top (setpoint within 2 % of its peak): measured mean", f"{_fmt(mt, u)} ({raw['flat_top_error_pct']:+.2f} % of peak)"),
                 ("Flat top: measured ripple, peak to peak", _fmt(max(pt) - min(pt), u))]
    rows.append(("Output range", f"{_fmt(min(out), uo)} to {_fmt(max(out), uo)}"))
    return rows, raw


# ---- the picture -----------------------------------------------------------------------------------------------------
def _nice_ticks(lo, hi, target=5):
    if not math.isfinite(lo) or not math.isfinite(hi):
        return [0.0, 1.0]
    if hi <= lo:
        hi = lo + 1.0
    span = hi - lo
    raw = span / target
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw)
    t = math.floor(lo / step) * step
    ticks = []
    while t <= hi + step * 0.5 and len(ticks) < 20:
        ticks.append(round(t, 10))
        t += step
    return ticks


def _tick_label(v):
    if abs(v) >= 1000:
        return f"{v:,.0f}"
    return f"{v:.6g}"


SERIES_STYLE = {"sp": ("Setpoint", "sp"), "pv": ("Measured", "pv"), "out": ("Output", "out")}


def plot_svg(rec, n, unit="native", width=640):
    """An inline SVG of one channel. Colours come from CSS classes (.plot .sp/.pv/.out/.ax/.grid), so it follows the page's theme."""
    ser, units, label = view(rec, n, unit)
    rate, N = rec["rate_hz"], len(ser["sp"])
    if N == 0:
        return ""
    t_end = (N - 1) / rate if N > 1 else 1.0 / rate
    if rec["product"] == "xrex":
        panels = [(["sp", "pv", "out"], units["sp"], "")]
    else:
        panels = [(["sp", "pv"], units["sp"], "target and measured"), (["out"], units["out"], "output")]
    L, R, T, B, PH, GAP = 80, 12, 26, 38, 190, 34
    height = T + len(panels) * PH + (len(panels) - 1) * GAP + B
    X = lambda i: L + (width - L - R) * (i / rate) / (t_end or 1.0)
    parts = [f'<svg class="plot" viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(channel_name(rec["product"], n))}" '
             f'xmlns="http://www.w3.org/2000/svg">']
    parts.append(f'<text class="ttl" x="{L}" y="15">{html.escape(channel_name(rec["product"], n))}</text>')
    for p, (keys, u, sub) in enumerate(panels):
        top = T + p * (PH + GAP)
        vals = [v for k in keys for v in ser[k]]
        lo, hi = min(vals), max(vals)
        if hi - lo < 1e-9:
            lo, hi = lo - 0.5, hi + 0.5
        pad = (hi - lo) * 0.06
        lo_p = 0.0 if (lo >= 0 and lo - pad < 0) else lo - pad      # do not show negative amps/volts/% for data that never goes below zero
        yt = _nice_ticks(lo_p, hi + pad)
        ylo, yhi = min(yt[0], lo_p), max(yt[-1], hi + pad)
        Y = lambda v, top=top, ylo=ylo, yhi=yhi: top + PH - (PH * (v - ylo) / (yhi - ylo))
        for v in yt:
            y = Y(v)
            parts.append(f'<line class="grid" x1="{L}" x2="{width - R}" y1="{y:.1f}" y2="{y:.1f}"/>'
                         f'<text class="tick" x="{L - 6}" y="{y + 4:.1f}" text-anchor="end">{_tick_label(v)}</text>')
        for v in _nice_ticks(0, t_end):
            if v > t_end * 1.001:
                continue
            x = L + (width - L - R) * v / (t_end or 1.0)
            parts.append(f'<line class="grid v" x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{top + PH}"/>')
            if p == len(panels) - 1:
                parts.append(f'<text class="tick" x="{x:.1f}" y="{top + PH + 16}" text-anchor="middle">{_tick_label(v)}</text>')
        parts.append(f'<rect class="ax" x="{L}" y="{top}" width="{width - L - R}" height="{PH}" fill="none"/>')
        parts.append(f'<text class="ylab" x="13" y="{top + PH / 2:.0f}" transform="rotate(-90 13 {top + PH / 2:.0f})" text-anchor="middle">'
                     f'{html.escape((sub + " " if sub else "") + "(" + u + ")")}</text>')
        for k in keys:
            pts = " ".join(f"{X(i):.1f},{Y(v):.1f}" for i, v in enumerate(ser[k]))
            parts.append(f'<polyline class="{SERIES_STYLE[k][1]}" fill="none" points="{pts}"/>')
        lx = width - R - 8
        for j, k in enumerate(reversed(keys)):
            parts.append(f'<text class="leg {SERIES_STYLE[k][1]}" x="{lx}" y="{top + 16 + 15 * j}" text-anchor="end">{SERIES_STYLE[k][0]}</text>')
    parts.append(f'<text class="ylab" x="{(L + width - R) / 2:.0f}" y="{height - 4}" text-anchor="middle">time since the shot started (s)</text>')
    parts.append("</svg>")
    return "".join(parts)


# ---- files -----------------------------------------------------------------------------------------------------------
def to_csv(rec):
    """One row per sample. Native units, plus amps for a calibrated XREX."""
    cols, data = ["t_s"], []
    n_samples = rec["count"]
    for ch in rec["channels"]:
        n = ch["n"]
        tag = f"ch{n}_" if rec["product"] == "xrex" else f"phase{'UVW'[n - 1]}_"
        for k, name in (("sp", "setpoint"), ("pv", "measured"), ("out", "output")):
            cols.append(f"{tag}{name}_{ch['units'][k]}")
            data.append(ch[k])
        if rec["product"] == "xrex":
            for k, name in (("sp", "setpoint"), ("pv", "measured"), ("out", "output")):
                a = amps_from_hz(rec, n, ch[k])
                if a is not None:
                    cols.append(f"{tag}{name}_A")
                    data.append(a)
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(cols)
    for i in range(n_samples):
        w.writerow([f"{i / rec['rate_hz']:.6f}"] + [f"{col[i]:.6g}" for col in data])
    return buf.getvalue()


def _header_rows(rec):
    c = rec["controller"]
    rows = [("Controller", f"{c.get('label') or c.get('name') or '-'} ({c.get('board', '-')})"), ("Board serial", c.get("serial", "-")),
            ("Firmware", c.get("fw", "-")), ("Recorded", rec["started"]), ("Outcome", rec["outcome"]),
            ("Samples", f"{rec['count']} at {rec['rate_hz']:g} samples/s, {rec['duration_s']:.4g} s")]
    if rec.get("mode"):
        rows.insert(3, ("Supply", rec["mode"]))
    return rows


def to_markdown(rec, unit="native"):
    L = [f"# Shot data: {rec['controller'].get('label') or rec['controller'].get('name') or rec['controller'].get('serial', '')} {rec['started']}", ""]
    L += [f"- **{k}:** {v}" for k, v in _header_rows(rec)]
    if rec["product"] == "xrex":
        L += ["", f"_{XREX_NOTE}_"]
    for note in rec.get("notes", []):
        L += ["", f"> {note}"]
    meta = rec.get("meta") or {}
    if meta:
        L += ["", "## What was commanded", "", "```json", json.dumps(meta, indent=1)[:4000], "```"]
    for ch in rec["channels"]:
        rows, _ = stats(rec, ch["n"], unit)
        L += ["", f"## {ch['name']}", "", "| | |", "|---|---|"] + [f"| {a} | {b} |" for a, b in rows]
    return "\n".join(L) + "\n"


REPORT_CSS = """
:root{--bg:#f3f5f8;--panel:#fff;--ink:#1c2430;--muted:#5d6b7e;--line:#d8dee8;--sp:#1d5fd1;--pv:#c62828;--out:#2e8b57}
@media (prefers-color-scheme:dark){:root{--bg:#11161d;--panel:#1a212b;--ink:#e6ecf4;--muted:#93a1b5;--line:#2c3644;--sp:#6aa7ff;--pv:#ff8a80;--out:#6fd69a}}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,sans-serif}main{max-width:980px;margin:0 auto;padding:20px}
section{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 18px;margin:14px 0}
h1{font-size:1.4rem}h2{font-size:1.1rem;margin:.2rem 0 .6rem}table{border-collapse:collapse}td{padding:2px 14px 2px 0;vertical-align:top}td:first-child{color:var(--muted)}
.note{color:var(--muted);font-size:.9rem}svg.plot{width:100%;height:auto}
svg.plot .grid{stroke:var(--line);stroke-width:1}svg.plot .ax{stroke:var(--muted);stroke-width:1}svg.plot text{fill:var(--muted);font:13px system-ui,sans-serif}
svg.plot .ttl{fill:var(--ink);font-weight:600;font-size:15px}svg.plot .sp{stroke:var(--sp);stroke-width:1.6;stroke-dasharray:6 3}
svg.plot .pv{stroke:var(--pv);stroke-width:1.8}svg.plot .out{stroke:var(--out);stroke-width:1.4}
svg.plot text.leg.sp{fill:var(--sp)}svg.plot text.leg.pv{fill:var(--pv)}svg.plot text.leg.out{fill:var(--out)}
"""


def to_html(rec, unit="native"):
    c = rec["controller"]
    h = [f"<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         f"<title>Shot data {html.escape(rec['started'])}</title><style>{REPORT_CSS}</style><main>"
         f"<h1>Shot data: {html.escape(str(c.get('label') or c.get('name') or c.get('serial') or ''))}</h1><section><table>"]
    h += [f"<tr><td>{html.escape(k)}</td><td>{html.escape(str(v))}</td></tr>" for k, v in _header_rows(rec)]
    h.append("</table>")
    if rec["product"] == "xrex" and unit == "A":
        h.append(f"<p class=note>{html.escape(XREX_NOTE)}</p>")
    for note in rec.get("notes", []):
        h.append(f"<p class=note>{html.escape(note)}</p>")
    h.append("</section>")
    meta = rec.get("meta") or {}
    if meta:
        h.append(f"<section><h2>What was commanded</h2><pre>{html.escape(json.dumps(meta, indent=1)[:4000])}</pre></section>")
    for ch in rec["channels"]:
        rows, _ = stats(rec, ch["n"], unit)
        h.append(f"<section><h2>{html.escape(ch['name'])}</h2>{plot_svg(rec, ch['n'], unit)}<table>")
        h += [f"<tr><td>{html.escape(a)}</td><td>{html.escape(b)}</td></tr>" for a, b in rows]
        h.append("</table></section>")
    h.append("</main></html>")
    return "".join(h)


def summary_of(rec, sid):
    ch = rec["channels"]
    pk = {c["name"]: max(c["pv"]) if c["pv"] else None for c in ch}
    return {"sid": sid, "started": rec["started"], "outcome": rec["outcome"], "product": rec["product"], "count": rec["count"],
            "duration_s": rec["duration_s"], "rate_hz": rec["rate_hz"], "channels": [c["name"] for c in ch], "measured_peak": pk,
            "serial": rec["controller"].get("serial"), "label": rec["controller"].get("label")}


def controller_dir(controller):
    """A folder name for a controller: its board serial when it has one, else its name."""
    s = str(controller.get("serial") or "")
    if not s or s.upper() in ("UNSET", "?", "-"):
        s = "unassigned-" + str(controller.get("name") or controller.get("board") or "controller")
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", s).strip("-")[:60] or "controller"


def save(root, rec, unit="A"):
    """Write the recording and its reports under root; returns its sid. Never overwrites an earlier recording."""
    d = controller_dir(rec["controller"])
    stamp = datetime.datetime.fromisoformat(rec["started"]).strftime("%Y%m%d-%H%M%S")
    sid, k = f"{d}/{stamp}", 1
    while os.path.exists(os.path.join(root, sid)):
        k += 1
        sid = f"{d}/{stamp}-{k}"
    folder = os.path.join(root, sid)
    os.makedirs(folder)
    has_amps = rec["product"] == "xrex" and all(amps_from_hz(rec, c["n"], c["sp"]) is not None for c in rec["channels"])
    unit = "A" if has_amps else "native"
    with open(os.path.join(folder, "shot.json"), "w") as f:
        json.dump(rec, f)
    with open(os.path.join(folder, "shot.csv"), "w") as f:
        f.write(to_csv(rec))
    with open(os.path.join(folder, "report.html"), "w") as f:
        f.write(to_html(rec, unit))
    with open(os.path.join(folder, "report.md"), "w") as f:
        f.write(to_markdown(rec, unit))
    with open(os.path.join(folder, "summary.json"), "w") as f:
        json.dump(summary_of(rec, sid), f)
    return sid


def _folder(root, sid):
    if not SID_RE.match(sid or ""):
        raise KeyError("no such recording")
    p = os.path.realpath(os.path.join(root, sid))
    if not p.startswith(os.path.realpath(root) + os.sep) or not os.path.isdir(p):
        raise KeyError("no such recording")
    return p


def load(root, sid):
    with open(os.path.join(_folder(root, sid), "shot.json")) as f:
        return json.load(f)


def read_file(root, sid, name):
    if name not in FILES:
        raise KeyError("no such file")
    with open(os.path.join(_folder(root, sid), name), "rb") as f:
        return f.read()


def list_recordings(root, controller, limit=50):
    d = os.path.join(root, controller_dir(controller))
    out = []
    if os.path.isdir(d):
        for name in sorted(os.listdir(d), reverse=True)[:limit]:
            try:
                with open(os.path.join(d, name, "summary.json")) as f:
                    out.append(json.load(f))
            except (OSError, ValueError):
                pass
    return out


def describe(rec, sid, unit="A"):
    """Everything the page needs to show one recording."""
    has_amps = rec["product"] == "xrex" and all(amps_from_hz(rec, c["n"], c["sp"]) is not None for c in rec["channels"])
    unit = unit if (unit == "A" and has_amps) else "native"
    chans = []
    for ch in rec["channels"]:
        rows, raw = stats(rec, ch["n"], unit)
        chans.append({"n": ch["n"], "name": ch["name"], "svg": plot_svg(rec, ch["n"], unit), "stats": rows, "raw": raw})
    notes = list(rec.get("notes", [])) + [n for n in diagnose(rec) if n not in rec.get("notes", [])]     # recordings saved before diagnose() existed
    return {"sid": sid, "summary": summary_of(rec, sid), "header": _header_rows(rec), "notes": notes, "meta": rec.get("meta", {}),
            "unit": unit, "can_amps": has_amps, "amps_note": XREX_NOTE if rec["product"] == "xrex" else None, "channels": chans}

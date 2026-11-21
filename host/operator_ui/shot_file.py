"""shot_file.py -- read an HBRIDGE shot (the SHOT:STEP setpoints) from a file or pasted text, for the operator console.

Typing a profile in by hand is where shots go wrong (2026-11-18: decimals dropped, every frequency one row early, 19018 typed
for 20018 Hz). The console's shot table therefore loads the profile from what the tools write, and the operator checks and
sends it:

  * the CSV written by analysis/compensation/ech_compensate.py (time_us, demand_kw, carrier_hz, duty_pct; an 'off' row is 0 Hz);
  * the CSV written by analysis/compensation/limiter_compensate.py (time_us, carrier_hz, demand_duty*_pct, commanded_duty*_pct):
    the COMMANDED duty is what is uploaded, the demand only when the file has no commanded columns yet;
  * any CSV with a header naming a time, a frequency and a duty column (see _TIME, _FREQ below);
  * a block of SHOT:STEP lines, as printed in a logbook or a terminal ("SHOT:STEP <t_us> <hz> <duty> [<dutyV> <dutyW>]");
  * bare rows of numbers in SHOT:STEP order.

parse(text) returns {"rows": [...], "kind": ..., "meta": {...}, "notes": [...]} or raises ValueError with a message an
operator can act on. A row is {"t": ms, "f": kHz, "u": %, "v": %, "w": %, "off": bool}: the units of the console's table.
Nothing here checks a supply's limits: the console does that against the limits the controller reports. Standard library only.
"""
import re

MAX_ROWS = 500                      # SHOT_SETPOINT_MAX in the firmware
_STEP_RE = re.compile(r"SHOT:STEP\s+(.+)", re.I)
_NUM = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"

# column names (lower case) -> factor to the console's unit (ms, kHz)
_TIME = {"time_us": 1e-3, "t_us": 1e-3, "time_ms": 1.0, "t_ms": 1.0, "time_s": 1e3, "t_s": 1e3}
_FREQ = {"carrier_hz": 1e-3, "freq_hz": 1e-3, "frequency_hz": 1e-3, "f_hz": 1e-3,
         "carrier_khz": 1.0, "freq_khz": 1.0, "frequency_khz": 1.0, "f_khz": 1.0}
_OFF_WORDS = ("off", "gap", "-", "nan", "")


def _num(cell, what, line_no):
    try:
        v = float(cell)
    except ValueError:
        raise ValueError(f"line {line_no}: {what} is not a number: {cell.strip()!r}")
    if v != v or v in (float("inf"), float("-inf")):
        raise ValueError(f"line {line_no}: {what} is not a number: {cell.strip()!r}")
    return v


def _row(t_ms, f_khz, duties, line_no):
    if f_khz < 0 or any(d < 0 for d in duties):
        raise ValueError(f"line {line_no}: a frequency or duty is negative")
    off = f_khz == 0
    u, v, w = duties if len(duties) == 3 else (duties[0],) * 3
    return {"t": round(t_ms, 6), "f": 0.0 if off else round(f_khz, 9), "u": 0.0 if off else u, "v": 0.0 if off else v,
            "w": 0.0 if off else w, "off": off}


def _from_steps(lines):
    rows = []
    for no, line in lines:
        m = _STEP_RE.search(line)
        if not m:
            continue
        words = re.split(r"[\s,]+", m.group(1).split("->")[0].strip())
        words = [w for w in words if w]
        if len(words) not in (3, 5):
            raise ValueError(f"line {no}: SHOT:STEP needs a time, a frequency and one or three duties: {line.strip()!r}")
        vals = [_num(w, "a SHOT:STEP value", no) for w in words]
        rows.append(_row(vals[0] / 1000.0, vals[1] / 1000.0, vals[2:], no))
    return rows


def _pick_duty(header):
    """Which columns carry the duty to upload: (indices, note or None). Commanded before plain before demand."""
    def find(*names):
        return next((header.index(n) for n in names if n in header), None)

    for prefix, note in (("commanded_duty", None), ("duty", None),
                         ("demand_duty", "This file has no commanded duty yet (no shot has been corrected): the DEMAND duty was loaded.")):
        three = [find(f"{prefix}_{p}_pct", f"{prefix}_{p}") for p in "uvw"]
        if all(i is not None for i in three):
            return three, note
        one = find(f"{prefix}_pct", prefix)
        if one is not None:
            return [one], note
    return None, None


def _from_table(lines, header):
    ti = next((header.index(n) for n in _TIME if n in header), None)
    fi = next((header.index(n) for n in _FREQ if n in header), None)
    if ti is None and fi is None and not any("duty" in c for c in header):
        raise ValueError("this does not look like a shot: expected SHOT:STEP lines, rows of numbers (time in us, Hz, duty), "
                         "or a CSV whose first line names a time, a frequency and a duty column")
    if ti is None:
        raise ValueError("no time column: the header needs one of " + ", ".join(sorted(_TIME)))
    if fi is None:
        if "demand_kw" in header or "demand_hz" in header:
            raise ValueError("this file has no frequencies yet (only the demand): run analysis/compensation/ech_compensate.py on it "
                             "first, and load the _iterN.csv it writes")
        raise ValueError("no frequency column: the header needs one of " + ", ".join(sorted(_FREQ)))
    di, note = _pick_duty(header)
    if di is None:
        raise ValueError("no duty column: the header needs duty_pct, or duty_u_pct, duty_v_pct and duty_w_pct "
                         "(commanded_duty... is used when present)")
    tk, fk = _TIME[header[ti]], _FREQ[header[fi]]
    rows = []
    for no, line in lines:
        cells = [c.strip() for c in line.split(",")]
        if len(cells) <= max([ti, fi] + di):
            raise ValueError(f"line {no}: has {len(cells)} columns, the header has {len(header)}")
        fcell = cells[fi].lower()
        f = 0.0 if fcell in _OFF_WORDS else _num(cells[fi], "the frequency", no) * fk
        duties = [0.0] * len(di) if f == 0 else [_num(cells[i], "a duty", no) for i in di]
        rows.append(_row(_num(cells[ti], "the time", no) * tk, f, duties, no))
    return rows, note


def parse(text):
    if not isinstance(text, str) or not text.strip():
        raise ValueError("the file is empty")
    if len(text) > 400_000:
        raise ValueError("the file is too large to be a shot (more than 400 kB)")
    meta, body = {}, []
    for no, raw in enumerate(text.replace("\r\n", "\n").replace("\r", "\n").split("\n"), 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            m = re.match(r"^#\s*([A-Za-z_]\w*)=(.*)$", line)        # the tools write one "# key=value" per line
            if m:
                meta[m.group(1).lower()] = m.group(2).strip()
            if "time_" in line.lower() and "," in line:             # a commented header line
                body.append((no, line.lstrip("#").strip()))
            continue
        body.append((no, line))
    if not body:
        raise ValueError("the file has no rows")
    notes = []
    if any(_STEP_RE.search(l) for _, l in body):
        rows, kind = _from_steps(body), "SHOT:STEP lines"
        if not rows:
            raise ValueError("no SHOT:STEP lines found")
    else:
        first = [c.strip().lower() for c in body[0][1].split(",")]
        words0 = [w for w in re.split(r"[\s,;]+", body[0][1].strip().lower()) if w]
        if any(re.fullmatch(_NUM, w) is None and w not in _OFF_WORDS for w in words0):   # a header line: a named-column table
            rows, note = _from_table(body[1:], first)
            kind = "CSV"
            if note:
                notes.append(note)
            if "demand_kw" in first:
                kind = "ECH profile (ech_compensate.py)"
            elif any(c.startswith("commanded_duty") or c.startswith("demand_duty") for c in first):
                kind = "Limiter profile (limiter_compensate.py)"
        else:                                                       # bare numbers, SHOT:STEP order: t_us, Hz, duty[, dV, dW]
            rows = []
            for no, line in body:
                words = [w for w in re.split(r"[\s,;]+", line) if w]
                if len(words) not in (3, 5):
                    raise ValueError(f"line {no}: expected a time (us), a frequency (Hz) and one or three duties, as in SHOT:STEP")
                vals = [_num(w, "a value", no) for w in words]
                rows.append(_row(vals[0] / 1000.0, vals[1] / 1000.0, vals[2:], no))
            kind = "rows of numbers (time us, Hz, duty)"
    if len(rows) < 2:
        raise ValueError("a shot needs at least two setpoints")
    if len(rows) > MAX_ROWS:
        raise ValueError(f"{len(rows)} setpoints: the controller holds at most {MAX_ROWS}")
    for a, b in zip(rows, rows[1:]):
        if b["t"] <= a["t"]:
            raise ValueError(f"the times must increase: {b['t']:g} ms follows {a['t']:g} ms")
    if rows[0]["t"] != 0:
        notes.append(f"The first setpoint is at {rows[0]['t']:g} ms: the controller needs it at 0.")
    gaps = sum(1 for r in rows if r["off"])
    if gaps:
        notes.append(f"{gaps} output-off setpoint{'s' if gaps != 1 else ''} (0 Hz): the output is off from each until the next setpoint.")
    for key, label in (("power_scale", "power scale"),):
        if key in meta:
            notes.append(f"The file carries a {label} of {meta[key]} (demand in experimental-meter kW).")
    return {"rows": rows, "kind": kind, "meta": meta, "notes": notes}


def describe(parsed, name=""):
    """One line for the operator: what was loaded."""
    m, rows = parsed["meta"], parsed["rows"]
    bits = [f"{len(rows)} setpoints, {rows[-1]['t']:g} ms", parsed["kind"]]
    if m.get("iteration"):
        bits.append(f"iteration {m['iteration']}")
    if m.get("from_shot"):
        bits.append(f"corrected from shot {m['from_shot']}")
    if m.get("bank_volts"):
        bits.append(f"for a bank charge of {m['bank_volts']} V")
    if m.get("target"):
        bits.append(f"target {m['target']}")
    return (f"{name}: " if name else "") + "; ".join(bits)

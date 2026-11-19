"""pfm_capture.py -- the HBRIDGE controller's PFM_Input capture (PFMIN:*), turned into numbers: parsing the paged replies, decoding
both-edge captures into switching cycles, statistics, and the comparison with the programmed shot. Pure (no sockets, standard library
only), so it is tested without hardware. recorder.py talks to the controller; shot_data.py draws and saves what comes out of here.

WHAT THE CONTROLLER HOLDS (firmware/hbridge docs, PFMIN:*): per input (1..6; on the Limiter bench 1..3 are phases U, V, W) a capture
of up to 3800 cycles, started by the shot itself (PFMIN:CAPTURE <M> before ARM) and measured by a 170 MHz timer.
  RISING mode (default)  PFMIN:DATA? <ch> <start> <count>  -> OK <n> OVERCAP=<k> SUM=<s> <period ticks...>
                         rising edge to rising edge: the carrier period, no duty.
  BOTH mode              PFMIN:RAW?  <ch> <start> <count>  -> OK <n> OVERCAP=<k> LEVEL=<0|1> SUM=<s> <t0> <d1> <d2> ...
                         every edge: t0 = the first edge's raw timer value, then tick differences; LEVEL = the pin level before the
                         capture (so the first edge is the opposite transition and edges alternate). Gives period AND high time.
Replies can lose characters on the bridge: every page carries a check sum (the sum of its numbers modulo 2**32) and a wrong length or
sum raises ValueError, so the caller asks again.

Honest limits: a missing edge in the record shifts every later rising/falling assignment (edges are told apart only by alternation);
such a cycle shows as an odd period and is flagged, not repaired. The first cycle is flagged "startup": the capture starts just before
the output does. Duty is the measured high time of the input signal as the controller sees it (after the fibre/receiver), not the
commanded duty; the difference is reported when the programmed shot is known.
"""
import math

CLK_HZ = 170_000_000                       # the capture timer's clock: every PFMIN number is in ticks of this
PAGE = 100                                 # values per reply (the bridge loses characters in longer ones)
MAX_CYCLES = 3800                          # PFMIN:CAPTURE's upper limit
NUM_INPUTS = 6
INPUT_PHASE = {1: "U", 2: "V", 3: "W"}     # the bench wiring: PFM_Input 1..3 carry phases U, V, W


def input_name(n):
    return f"Input {n}" + (f" (phase {INPUT_PHASE[n]})" if n in INPUT_PHASE else "")


# ---- the replies on the wire -----------------------------------------------------------------------------------------
def _field(token, name):
    if not token.startswith(name + "="):
        raise ValueError(f"expected {name}= in the reply")
    return int(token[len(name) + 1:])


def _page(reply, start, count, header):
    """Common part of both page parsers. header: names of the fixed fields after the count, in order ('OVERCAP', 'SUM' ...)."""
    p = reply.split()
    if len(p) < 1 + 1 + len(header) or p[0] != "OK":
        raise ValueError("not an OK page")
    total = int(p[1])
    fields = {name: _field(tok, name) for name, tok in zip(header, p[2:2 + len(header)])}
    nums = [int(x) for x in p[2 + len(header):]]
    if total < 0 or len(nums) != max(0, min(count, total - start)):
        raise ValueError("wrong number of values (a character was lost on the link)")
    if sum(nums) & 0xFFFFFFFF != fields["SUM"]:
        raise ValueError("check sum mismatch (a character was lost on the link)")
    return total, fields, nums


def parse_data_page(reply, start, count=PAGE):
    """PFMIN:DATA? page -> (total periods, overcap, [period ticks])."""
    total, f, nums = _page(reply, start, count, ("OVERCAP", "SUM"))
    return total, f["OVERCAP"], nums


def parse_raw_page(reply, start, count=PAGE):
    """PFMIN:RAW? page -> (total edges, overcap, level, [values]); the first value of the whole capture is the raw t0."""
    total, f, nums = _page(reply, start, count, ("OVERCAP", "LEVEL", "SUM"))
    return total, f["OVERCAP"], f["LEVEL"], nums


# ---- decoding --------------------------------------------------------------------------------------------------------
def decode_cycles(level, values):
    """Every complete switching cycle of a both-edge capture (rising edge, falling edge, next rising edge) as
    (rising time in ticks from the first edge, period ticks, high ticks). values = [t0, d1, d2, ...]."""
    t, acc = [0], 0
    for d in values[1:]:
        acc += d
        t.append(acc)
    first_rising = (level == 0)
    rising = [i for i in range(len(t)) if (i % 2 == 0) == first_rising]
    return [(t[a], t[b] - t[a], t[a + 1] - t[a]) for a, b in zip(rising, rising[1:]) if b == a + 2]


def _median(xs):
    s = sorted(xs)
    n = len(s)
    return 0.0 if not n else (s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0)


def _flags(periods, startup):
    med = _median(periods)
    flags = ["odd period" if med and abs(p - med) > 0.25 * med else "" for p in periods]
    if startup and flags:
        flags[0] = "startup"
    return flags


def build_input(n, mode, overcap, data, level=0, clk=CLK_HZ):
    """One input as plain lists (JSON-able). mode: 'rising' (data = period ticks) or 'both' (data = [t0, d1, ...] from PFMIN:RAW?).
    t_s is the time of each cycle's start since the first captured one."""
    if mode == "both":
        cyc = decode_cycles(level, data)
        t0 = cyc[0][0] if cyc else 0
        t_s = [(c[0] - t0) / clk for c in cyc]
        periods = [c[1] for c in cyc]
        high_us = [c[2] / clk * 1e6 for c in cyc]
        duty = [100.0 * c[2] / c[1] if c[1] else 0.0 for c in cyc]
        flags = _flags(periods, True)
        edges = len(data)
    else:
        periods = list(data)
        t_s, acc = [], 0
        for p in periods:
            t_s.append(acc / clk)
            acc += p
        high_us, duty = None, None
        flags = _flags(periods, False)
        edges = None
    return {"n": n, "name": input_name(n), "mode": mode, "overcap": overcap, "edges": edges, "t_s": t_s,
            "period_us": [p / clk * 1e6 for p in periods], "freq_hz": [clk / p if p else 0.0 for p in periods],
            "high_us": high_us, "duty_pct": duty, "flag": flags}


# ---- the programmed shot ---------------------------------------------------------------------------------------------
def parse_points(replies):
    """SHOT:POINt? replies ('OK t_us f_hz d' or 'OK t_us f_hz dU dV dW') -> [[t_us, f_hz, dU, dV, dW], ...]; unreadable ones are skipped."""
    pts = []
    for r in replies:
        try:
            p = [float(x) for x in (r or "").split()[1:]]
        except ValueError:
            continue
        if len(p) == 3:
            pts.append([p[0], p[1], p[2], p[2], p[2]])
        elif len(p) == 5:
            pts.append(p)
    return pts


def interp(xs, ys, x):
    """Straight lines between the points, held flat outside them."""
    if not xs:
        return None
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    lo, hi = 0, len(xs) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if xs[mid] <= x:
            lo = mid
        else:
            hi = mid
    span = xs[hi] - xs[lo]
    return ys[lo] if span <= 0 else ys[lo] + (ys[hi] - ys[lo]) * (x - xs[lo]) / span


def programmed(points, n):
    """The programmed shot as seen by input n (phase U, V, W for n = 1..3): ([time s], [duty %], [carrier Hz]) or None."""
    if not points or n not in INPUT_PHASE:
        return None
    col = 1 + n                                     # [t_us, f_hz, dU, dV, dW]
    return [p[0] * 1e-6 for p in points], [p[col] for p in points], [p[1] for p in points]


def overlay(inp, points):
    """The programmed duty and carrier at each cycle of an input: ([duty], [freq Hz]) or (None, None)."""
    prog = programmed(points, inp["n"])
    if prog is None:
        return None, None
    ts, ds, fs = prog
    return [interp(ts, ds, t) for t in inp["t_s"]], [interp(ts, fs, t) for t in inp["t_s"]]


# ---- numbers ---------------------------------------------------------------------------------------------------------
def _good(inp):
    idx = [i for i, f in enumerate(inp["flag"]) if not f]
    return idx or list(range(len(inp["flag"])))


def _mean_std(xs):
    n = len(xs)
    if not n:
        return None, None
    m = sum(xs) / n
    return m, math.sqrt(sum((x - m) ** 2 for x in xs) / n)


def input_stats(inp, points=None):
    """(rows for the page and report, raw dict) for one input; points = the programmed shot (SHOT:POINt?) when known."""
    n = len(inp["t_s"])
    if n == 0:
        return [("Cycles", "0: nothing usable was captured")], {"cycles": 0}
    good = _good(inp)
    f = [inp["freq_hz"][i] for i in good]
    bad = n - len([i for i, x in enumerate(inp["flag"]) if not x])
    span_ms = (inp["t_s"][-1] + inp["period_us"][-1] * 1e-6) * 1e3
    rows = [("Capture", "both edges (period and duty)" if inp["mode"] == "both" else "rising edges only (period; no duty)"),
            ("Cycles", f"{n} over {span_ms:.3f} ms" + (f", {bad} flagged (start-up or a missing edge)" if bad else "")),
            ("Carrier", f"mean {sum(f) / len(f) / 1e3:.3f} kHz (min {min(f) / 1e3:.3f}, max {max(f) / 1e3:.3f})"),
            ("Missed captures (OVERCAP)", str(inp["overcap"]) + (" -- cycles were lost: the timer was not read in time" if inp["overcap"] else ""))]
    raw = {"cycles": n, "flagged": bad, "freq_mean_hz": sum(f) / len(f), "freq_min_hz": min(f), "freq_max_hz": max(f), "overcap": inp["overcap"]}
    pd, pf = overlay(inp, points)
    if inp["duty_pct"] is not None:
        d = [inp["duty_pct"][i] for i in good]
        rows.append(("Duty (measured)", f"{min(d):.2f} to {max(d):.2f} % (mean {sum(d) / len(d):.2f})"))
        raw.update(duty_min=min(d), duty_max=max(d), duty_mean=sum(d) / len(d))
        if pd is not None:
            m, s = _mean_std([inp["duty_pct"][i] - pd[i] for i in good])
            rows.append(("Duty, measured minus programmed", f"mean {m:+.2f}, std {s:.2f} percentage points"))
            raw.update(duty_error_mean=m, duty_error_std=s)
    if pf is not None:
        m, s = _mean_std([(inp["freq_hz"][i] - pf[i]) / 1e3 for i in good])
        rows.append(("Carrier, measured minus programmed", f"mean {m:+.3f}, std {s:.3f} kHz"))
        raw.update(freq_error_mean_khz=m, freq_error_std_khz=s)
    return rows, raw


def to_csv(pfm):
    """One row per cycle (long format: all inputs in one file)."""
    import csv
    import io
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["input", "mode", "cycle", "t_s", "period_us", "frequency_Hz", "high_us", "duty_pct", "programmed_duty_pct", "programmed_frequency_Hz", "flag"])
    points = pfm.get("programmed")
    for inp in pfm["inputs"]:
        pd, pf = overlay(inp, points)
        for i in range(len(inp["t_s"])):
            w.writerow([inp["n"], inp["mode"], i, f"{inp['t_s'][i]:.9f}", f"{inp['period_us'][i]:.4f}", f"{inp['freq_hz'][i]:.1f}",
                        "" if inp["high_us"] is None else f"{inp['high_us'][i]:.4f}", "" if inp["duty_pct"] is None else f"{inp['duty_pct'][i]:.3f}",
                        "" if pd is None else f"{pd[i]:.3f}", "" if pf is None else f"{pf[i]:.1f}", inp["flag"][i]])
    return buf.getvalue()


def auto_cycles(duration_s, max_freq_hz):
    """How many cycles to ask PFMIN:CAPTURE for so the whole shot is covered: (M, note or None). Capped at the firmware's limit."""
    need = int(math.ceil(duration_s * max_freq_hz * 1.03)) + 10
    if need <= MAX_CYCLES:
        return need, None
    covered_ms = MAX_CYCLES / max_freq_hz * 1e3
    return MAX_CYCLES, (f"At {max_freq_hz / 1e3:g} kHz the capture holds at most {MAX_CYCLES} cycles ({covered_ms:.1f} ms): the first "
                        f"{covered_ms:.1f} ms of the {duration_s * 1e3:.1f} ms shot is captured, not the end.")

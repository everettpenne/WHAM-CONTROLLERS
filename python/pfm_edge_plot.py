#!/usr/bin/env python3
"""pfm_edge_plot.py -- plot the raw both-edge captures saved by pfm_edge_test.py.
Rebuilds each input's digital waveform from its edge timestamps (LEVEL + alternation).
U/V share timer TIM2 and W/UN share TIM5, so only those pairs share a time axis.
    python3 python/pfm_edge_plot.py edges.json out.png
"""
import json, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CLK = 170e6
NAMES = {"1": "U", "2": "V", "3": "W", "4": "UN"}
COLORS = {"U": "tab:blue", "V": "tab:orange", "W": "tab:green", "UN": "tab:red"}


def edges(c):
    """[(time_us, level_after_edge)] in the timer's own ticks (first delta = raw
    timestamp of the first edge), so channels on one timer share a time axis."""
    t = c["deltas"][0]; lvl = 1 - c["lvl"]; out = [(t / CLK * 1e6, lvl)]
    for x in c["deltas"][1:]:
        t += x; lvl = 1 - lvl; out.append((t / CLK * 1e6, lvl))
    return out


def steps(ev, t0, t1, y0, h=0.8):
    xs, ys = [], []
    prev = 1 - ev[0][1]
    xs.append(t0); ys.append(prev)
    for t, l in ev:
        if t < t0: prev = l; xs[0] = t0; ys[0] = l; continue
        if t > t1: break
        xs += [t, t]; ys += [prev, l]; prev = l
    xs.append(t1); ys.append(prev)
    return xs, [y0 + h * v for v in ys]


def pulses(c):
    """(high_list, low_list) in ticks; deltas[0] is the first raw timestamp."""
    state = 1 - c["lvl"]; hi, lo = [], []
    for x in c["deltas"][1:]:
        (hi if state else lo).append(x); state = 1 - state
    return hi, lo


def sweep_plot(runs, out):
    """Frequency x duty sweep (list format from pfm_edge_test.py)."""
    import statistics as stt
    freqs = sorted({r["hz"] for r in runs})
    fig, axs = plt.subplots(2, 3, figsize=(18, 10))
    fcol = {f: c for f, c in zip(freqs, ("tab:blue", "tab:green", "tab:red"))}
    mark = {"1": "o", "2": "s", "3": "^", "4": "v"}
    # A: measured duty (high / period, median pulse) vs programmed
    ax = axs[0][0]
    for r in runs:
        per = r["per"] + 1
        for ch, c in r["ch"].items():
            hi, lo = pulses(c)
            if len(hi) < 3 or len(lo) < 3: continue
            h, l = stt.median(hi), stt.median(lo)
            if abs(h + l - per) > 3: continue          # stale / corrupt stream: not a measurement
            ax.plot(r["cmp"] / r["per"] * 100, 100 * (l if ch == "4" else h) / (h + l), mark[ch], color=fcol[r["hz"]], ms=5, mfc="none" if ch == "4" else None)
    ax.plot([0, 100], [0, 100], "k--", lw=.7); ax.set_xlabel("programmed duty % (compare / period)"); ax.set_ylabel("measured high / period %  (UN: low / period, its compare)")
    ax.set_title("A. measured vs programmed duty (U o, V s, W ^, UN v)"); ax.grid(alpha=.3)
    for f in freqs: ax.plot([], [], "o", color=fcol[f], label=f"{f/1000:.0f} kHz")
    ax.legend(fontsize=8)
    # B: high - expected, ticks
    ax = axs[0][1]
    for r in runs:
        per = r["per"] + 1
        for ch, c in r["ch"].items():
            hi, lo = pulses(c)
            if len(hi) < 3 or len(lo) < 3: continue
            h, l = stt.median(hi), stt.median(lo)
            if abs(h + l - per) > 3: continue
            exp = (r["per"] - r["cmp"]) if ch == "4" else r["cmp"]
            ax.plot(r["duty"] + {"1": -1.5, "2": -.5, "3": .5, "4": 1.5}[ch], h - exp, mark[ch], color=fcol[r["hz"]], ms=5, mfc="none" if ch == "4" else None)
    ax.set_xlabel("programmed duty %"); ax.set_ylabel("high time - expected compare (ticks @170 MHz)")
    ax.set_title("B. fixed offset: U -32, V -34, W -33, UN 0 (all freq, all duty)"); ax.grid(alpha=.3)
    # C: capture completeness
    ax = axs[0][2]; w = 0.2
    for k, ch in enumerate("1234"):
        for j, f in enumerate(freqs):
            ns = [r["ch"][ch]["n"] for r in runs if r["hz"] == f]
            ax.bar(j + (k - 1.5) * w, stt.mean(ns), w, color=("tab:blue", "tab:orange", "tab:green", "tab:red")[k], label=NAMES[ch] if j == 0 else None)
            ax.plot([j + (k - 1.5) * w] * 2, [min(ns), max(ns)], "k-", lw=1)
    ax.axhline(100, color="k", ls="--", lw=.7); ax.set_xticks(range(len(freqs))); ax.set_xticklabels([f"{f/1000:.0f} kHz" for f in freqs])
    ax.set_ylabel("edges captured (of ~100 in the shot)"); ax.set_title("C. capture completeness (mean, min-max over 7 duties)"); ax.legend(fontsize=8); ax.grid(alpha=.3, axis="y")
    # D-F: waveform snapshots at the extremes (U,V share TIM2)
    for ax, (hz, duty) in zip(axs[1], ((75000, 5), (125000, 5), (125000, 95))):
        r = [x for x in runs if x["hz"] == hz and x["duty"] == duty][0]
        T = (r["per"] + 1) / CLK * 1e6
        evs = [edges(r["ch"][ch]) for ch in "12"]
        w0 = max(e[10][0] for e in evs)
        for k, ch in enumerate("12"):
            x, y = steps(evs[k], w0, w0 + 4 * T, 1.2 * (1 - k))
            ax.plot([v - w0 for v in x], y, color=COLORS[NAMES[ch]], lw=1.6)
            ax.text(-0.02 * 4 * T, 1.2 * (1 - k) + 0.4, NAMES[ch], ha="right", color=COLORS[NAMES[ch]])
        for m in range(5): ax.axvline(m * T, color="gray", lw=.5, ls=":")
        ax.set_yticks([]); ax.set_xlabel("time (us)")
        ax.set_title(f"{['D','E','F'][list(((75000,5),(125000,5),(125000,95))).index((hz,duty))]}. {hz/1000:.0f} kHz, programmed {duty}%: U, V measured (pulse high = {r['cmp']-32} ticks = {(r['cmp']-32)/CLK*1e9:.0f} ns)" if False else f"{hz/1000:.0f} kHz, programmed {duty}%  -- U, V (dotted = period grid)")
    fig.suptitle("LIMITER-range sweep, raw both-edge capture on PFM_Input 1-4 (simulator, 50-period shots)", fontsize=13)
    fig.tight_layout(); fig.savefig(out, dpi=105); print("wrote", out)


def main():
    res = json.load(open(sys.argv[1])); out = sys.argv[2]
    if isinstance(res, list):
        return sweep_plot(res, out)
    fig, axs = plt.subplots(3, 3, figsize=(17, 10))
    for col, (duty, d) in enumerate(sorted(res.items(), key=lambda kv: int(kv[0]))):
        per = d["per"] + 1; T = per / CLK * 1e6
        for row, (pair, ttl) in enumerate((("12", "U, V (TIM2 timebase)"), ("34", "W, UN (TIM5 timebase)"))):
            ax = axs[row][col]
            # start 4 periods in, show 3 periods
            evs = [edges(d["ch"][ch]) for ch in pair]
            w0 = max(e[8][0] for e in evs)          # a common window on the shared timebase
            for k, ch in enumerate(pair):
                nm = NAMES[ch]
                x, y = steps(evs[k], w0, w0 + 3 * T, 1.2 * (1 - k))
                ax.plot([v - w0 for v in x], y, color=COLORS[nm], lw=1.6, label=nm)
                ax.text(-0.02 * 3 * T, 1.2 * (1 - k) + 0.4, nm, ha="right", va="center", color=COLORS[nm])
            for m in range(4):
                ax.axvline(m * T, color="gray", lw=0.5, ls=":")
            ax.set_yticks([]); ax.set_xlabel("time (us)   [dotted = 50 us period grid from the first window edge]"); ax.set_title(f"programmed {duty}% -- {ttl}", fontsize=10)
            if row == 0: pass
        ax = axs[2][col]
        for ch, c in d["ch"].items():
            nm = NAMES[ch]; dl = c["deltas"][1:]; lvl = 1 - c["lvl"]; hi = []
            i = 0
            while i + 1 < len(dl):
                # a high pulse followed by its low pulse
                h, l = (dl[i], dl[i + 1]) if lvl else (dl[i + 1], dl[i]) if False else (None, None)
                i += 1
            # per-cycle duty: pair (high, following low) using state alternation
            state = lvl; pulses = []
            for x in dl: pulses.append((state, x)); state = 1 - state
            duties = [100 * a[1] / (a[1] + b[1]) for a, b in zip(pulses, pulses[1:]) if a[0] == 1 and 3000 < a[1] < 6000 and 3000 < b[1] < 6000]
            ax.plot(duties, ".-", ms=3, lw=0.8, color=COLORS[nm], label=f"{nm}  mean {sum(duties)/len(duties):.2f}%")
        ax.axhline(int(duty), color="k", ls="--", lw=0.8, label=f"programmed {duty}%")
        ax.set_ylim(int(duty) - 25 if False else 30, 70); ax.set_xlabel("cycle"); ax.set_ylabel("duty %  (high / period)")
        ax.legend(fontsize=7, ncol=2); ax.grid(alpha=.3)
    fig.suptitle("PFM_Input raw both-edge capture -- HVPS 20 kHz constant frequency, WHAM-HBRIDGE simulator", fontsize=13)
    fig.tight_layout(); fig.savefig(out, dpi=110); print("wrote", out)


main()

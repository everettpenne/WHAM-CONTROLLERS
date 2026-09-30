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


def main():
    res = json.load(open(sys.argv[1])); out = sys.argv[2]
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

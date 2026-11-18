#!/usr/bin/env python3
"""Publication figure of one ECH shot: forward power, gyrotron voltage and gyrotron current on a shared time axis.

Two steps, so the figure can be regenerated anywhere without MDSplus:

    python3 plot_shot.py --fetch            # on a machine with MDSplus (andrew): MDS -> shot_<N>.csv
    python3 plot_shot.py                    # anywhere with numpy + matplotlib: shot_<N>.csv -> the figures below

Figures (each as .png at 600 dpi and .pdf): shot_<N>.png (forward power, gyrotron voltage, gyrotron current) and
shot_<N>_power.png (forward power alone).

--shot and --subtitle override the defaults below (SHOT, SUBTITLE), so the same script serves any shot.

Signals (MDS tree "ech", the post-processed :FILTERED nodes):
    forward power   ECH_PROC.DL_MONITOR_F:FILTERED (dummy load) or ECH_PROC.WG_MONITOR_F:FILTERED (vessel), kW,
                    chosen from ECH_PARAMS:ECH_TARGET unless --power-node is given
    voltage         ECH_PROC.GYROTRON_V:FILTERED, V (plotted in kV)
    current         ECH_PROC.GYROTRON_I:FILTERED, A
Time is the MDS time base (s, plotted in ms; t = 0 is the shot trigger).

Each panel shows the raw trace in light gray ("Raw") and, in black, the same trace filtered with a 0.1 ms
running mean ("Filtered"), with a legend (titled with the quantity) naming both. The 0.1 ms window is two periods of the ~20 kHz converter switching ripple, which dominates
the current trace. Layout is fixed in inches (title block and panel letters on the plots' left edge, legends on
the right), so every figure made with this script lines up the same way.

Suggested caption: "ECH shot <N> into the dummy load (HVPS bank 520 V): (a) forward power, (b) gyrotron voltage,
(c) gyrotron current. Gray: raw; black: filtered (0.1 ms running mean)."
"""
import argparse
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = 261118022
SUBTITLE = "3-pulse ECH staircase shot"
T_WINDOW_MS = (-1.0, 17.0)          # what the CSV keeps (and the figure shows unless XLIM_MS is set)
XLIM_MS = None                      # the figure's time range, ms; None = T_WINDOW_MS
AVG_MS = 0.1                        # running mean drawn over the raw trace: two periods of the ~20 kHz switching ripple


def running_mean(y, n):
    """Centred running mean over n samples; the ends average over what is there (no zero padding)."""
    if n <= 1:
        return y
    c = np.cumsum(np.insert(y, 0, 0.0))
    h = n // 2
    lo = np.clip(np.arange(len(y)) - h, 0, len(y))
    hi = np.clip(np.arange(len(y)) + n - h, 0, len(y))
    return (c[hi] - c[lo]) / (hi - lo)


def fetch(shot, out_csv, power_node=None):
    import MDSplus as mds
    t = mds.Tree("ech", shot, "readonly")
    if power_node is None:
        target = str(t.getNode("ECH_PARAMS:ECH_TARGET").data()).strip().lower()
        power_node = "ECH_PROC.DL_MONITOR_F:FILTERED" if "dummy" in target else "ECH_PROC.WG_MONITOR_F:FILTERED"

    def sig(path):
        n = t.getNode(path)
        return np.asarray(n.dim_of().data(), float) * 1e3, np.asarray(n.data(), float)

    tv, v = sig("ECH_PROC.GYROTRON_V:FILTERED")
    ti, i = sig("ECH_PROC.GYROTRON_I:FILTERED")
    tp, p = sig(power_node)
    i = np.interp(tv, ti, i)
    p = np.interp(tv, tp, p)
    k = (tv >= T_WINDOW_MS[0]) & (tv <= T_WINDOW_MS[1])
    header = (f"shot={shot} bank_v={float(t.getNode('ECH_PARAMS:HVPS_V').data()):g} "
              f"target={t.getNode('ECH_PARAMS:ECH_TARGET').data()} power_node={power_node}\n"
              "time_ms,forward_power_kw,gyrotron_v_kv,gyrotron_i_a")
    np.savetxt(out_csv, np.column_stack([tv[k], p[k], v[k] / 1e3, i[k]]), delimiter=",", header=header,
               comments="# ", fmt="%.6g")
    print(f"wrote {out_csv} ({k.sum()} samples)")


def plot(in_csv, out_base, shot, subtitle):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import AutoMinorLocator, MultipleLocator

    d = np.loadtxt(in_csv, delimiter=",", comments="#")
    t, p, v, i = d.T

    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["Helvetica", "Arial", "DejaVu Sans"],
        "mathtext.fontset": "dejavusans",
        "pdf.fonttype": 42,                                     # embed TrueType (editable text; what journals ask for)
        "font.size": 8,
        "axes.labelsize": 8,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "legend.fontsize": 6.5,
        "axes.linewidth": 0.6,
        "xtick.direction": "in",
        "ytick.direction": "in",
        "xtick.top": True,
        "ytick.right": True,
        "xtick.major.width": 0.6,
        "ytick.major.width": 0.6,
        "xtick.minor.width": 0.4,
        "ytick.minor.width": 0.4,
        "xtick.major.size": 3.0,
        "ytick.major.size": 3.0,
        "xtick.minor.size": 1.6,
        "ytick.minor.size": 1.6,
        "savefig.dpi": 600,
    })

    ink = "#1a1a1a"
    raw_ink = "#b8b8b8"
    n_avg = max(1, int(round(AVG_MS / np.median(np.diff(t)))))
    # (signal, y label, legend name, y limits, major tick): the top of each range leaves headroom for the legend
    power = (p, "Forward power (kW)", "Forward power", (0, 300), 50)
    voltage = (v, "Gyrotron voltage (kV)", "Gyrotron voltage", (0, 100), 20)
    current = (i, "Gyrotron current (A)", "Gyrotron current", (0, 40), 10)

    def add_legend(ax, name, x_anchor):
        leg = ax.legend(loc="upper left", bbox_to_anchor=(x_anchor, 0.985), borderaxespad=0.0, frameon=False,
                        title=name, alignment="left", handlelength=1.8, handletextpad=0.5, labelspacing=0.2)
        leg.get_title().set_fontsize(6.5)
        leg.get_title().set_fontweight("bold")
        for h in leg.legend_handles:
            h.set_linewidth(1.2)                                # legend swatches readable at print size
        return leg

    def figure(panels, panel_h, path_base):
        """One figure: the panels stacked on a shared time axis. Fixed layout in inches, so the title block, panel
        letters and legends line up the same way on every figure this script makes."""
        W = 3.4
        left, right, bottom, top, gap = 0.58, 0.10, 0.42, 0.50, 0.10
        n = len(panels)
        H = bottom + top + n * panel_h + (n - 1) * gap
        fig = plt.figure(figsize=(W, H))
        axes = []
        for k in range(n):
            y0 = H - top - (k + 1) * panel_h - k * gap
            axes.append(fig.add_axes([left / W, y0 / H, (W - left - right) / W, panel_h / H],
                                     sharex=axes[0] if axes else None))
        x_left = left / W                                       # every text block starts on the plots' left edge

        for k, (ax, (y, label, name, ylim, major)) in enumerate(zip(axes, panels)):
            # raw trace (with the 20 kHz switching ripple) in light gray, its AVG_MS running mean in ink on top;
            # ~56k samples each: rasterize the traces, keep axes and text vector in the PDF
            ax.plot(t, y, color=raw_ink, lw=0.4, rasterized=True, label="Raw")
            ax.plot(t, running_mean(y, n_avg), color=ink, lw=0.8, rasterized=True, label="Filtered")
            ax.set_ylabel(label)
            ax.set_ylim(*ylim)
            ax.yaxis.set_major_locator(MultipleLocator(major))
            ax.yaxis.set_minor_locator(AutoMinorLocator(2))
            ax.xaxis.set_minor_locator(AutoMinorLocator(2))
            if n > 1:                                           # panel letter at the top left (none on a single plot)
                ax.text(0.02, 0.96, f"({'abcdefgh'[k]})", transform=ax.transAxes, ha="left", va="top",
                        fontweight="bold")
            if ax is not axes[-1]:
                ax.tick_params(labelbottom=False)

        # the legend titles differ in length; measure the widest legend and start every legend at the same x, so
        # their titles and swatches form one column, the widest ending 1.5 % in from the right edge
        widths = []
        for ax, pn in zip(axes, panels):
            leg = add_legend(ax, pn[2], 0.0)
            fig.canvas.draw()
            widths.append(leg.get_window_extent().width / ax.get_window_extent().width)
            leg.remove()
        x_leg = 0.985 - max(widths)
        for ax, pn in zip(axes, panels):
            add_legend(ax, pn[2], x_leg)
        axes[-1].set_xlim(*(XLIM_MS or T_WINDOW_MS))
        axes[-1].xaxis.set_major_locator(MultipleLocator(2))
        axes[-1].set_xlabel("Time (ms)")
        fig.align_ylabels(axes)
        # title block: left-aligned on the plots' left edge, title over subtitle
        fig.text(x_left, 1 - 0.10 / H, f"Shot {shot}", ha="left", va="top", fontsize=9.5, fontweight="bold")
        fig.text(x_left, 1 - 0.27 / H, subtitle, ha="left", va="top", fontsize=8, color="#444444")
        for ext in ("png", "pdf"):
            fig.savefig(f"{path_base}.{ext}")
        plt.close(fig)
        print(f"wrote {path_base}.png and {path_base}.pdf")

    figure([power, voltage, current], 1.36, out_base)          # the three-panel figure
    figure([power], 2.30, out_base + "_power")                  # forward power alone


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shot", type=int, default=SHOT)
    ap.add_argument("--subtitle", default=SUBTITLE, help="the line under the title (default: %(default)r)")
    ap.add_argument("--fetch", action="store_true", help="read the shot from MDS into shot_<N>.csv (needs MDSplus)")
    ap.add_argument("--power-node", help="override the forward-power node")
    a = ap.parse_args()
    csv = os.path.join(HERE, f"shot_{a.shot}.csv")
    if a.fetch:
        fetch(a.shot, csv, a.power_node)
    else:
        plot(csv, os.path.join(HERE, f"shot_{a.shot}"), a.shot, a.subtitle)


if __name__ == "__main__":
    main()

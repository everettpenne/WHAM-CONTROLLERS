"""pfm_edge_test.py -- raw both-edge capture on PFM_Input_1..4 (2026-09-30 duty
re-exploration): fires constant-frequency shots over a frequency x duty sweep with
the four inputs in BOTH-edge mode (PFMIN:EDGes) and saves the raw streams to JSON
(a list of {hz, duty, per, cmp, ch:{input:{n, ov, lvl, deltas}}}).
Decode: LEVEL is the pin level before the first captured edge, so the first delta
after t0 is the time spent at the OPPOSITE level; states then alternate.

    python3 python/pfm_edge_test.py edges.json --mode HVPS --freqs 20000 --duties 40,50,60
    python3 python/pfm_edge_test.py lim.json --mode LIMITER --freqs 75000,100000,125000 \\
        --duties 5,15,35,50,65,85,95 [--url socket://localhost:5002] [--steps 50]
Needs an idle HBRIDGE board with U, V, W, UN fibred to PFM_Input_1..4.
"""
import argparse, os, sys, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pfm_shot_test as T

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--url", default="socket://localhost:5002")
ap.add_argument("--mode", required=True, choices=("LIMITER", "HVPS"))
ap.add_argument("--freqs", required=True)
ap.add_argument("--duties", required=True)
ap.add_argument("--steps", type=int, default=50)
a = ap.parse_args()
freqs = [int(x) for x in a.freqs.split(",")]
duties = [int(x) for x in a.duties.split(",")]

link = T.Link(a.url, "edges")
print(link.must("*IDN?")); assert link.must("STATE?") == "OK IDLE"
link.must(f"CONFig:MODE {a.mode}")
print(link.must("CONFig:LIMits?"))
CH = [1, 2, 3, 4]
LIMITS = {"LIMITER": (5, 95), "HVPS": (40, 60)}
for c in CH: link.must(f"PFMIN:EDGes {c} BOTH")


def read_raw(ch):
    last = None
    for _ in range(12):
        r = link.cmd(f"PFMIN:RAW? {ch}", timeout=6)
        try:
            t = r.split(); n = int(t[1]); vals = [int(x) for x in t[4:]]
            lvl = int(t[3].split("=")[1]); ov = t[2]
        except Exception:
            continue
        if len(vals) != n: continue
        rec = (n, ov, lvl, vals)
        if last == rec: return rec          # two identical reads = clean
        last = rec
    return last


def shot(hz, duty, steps):
    per = int(170e6 // hz) - 1      # floor: 75 kHz is not exactly representable, round toward lower per
    cmp_ = round(per * duty / 100)
    lo_pct, hi_pct = LIMITS[a.mode]              # the board checks duty with exact integer math
    cmp_ = min(max(cmp_, -(-per * lo_pct // 100)), per * hi_pct // 100)
    link.must("TABle:BEGin")
    for _ in range(steps):
        r = link.cmd(f"TABle:STEP {per} {cmp_} {cmp_} {cmp_}")
        assert r == "OK", (hz, duty, r)
    link.must("TABle:END")
    link.must(f"PFMIN:CAPTURE {steps}")
    link.must("ARM"); link.must("FIRE")
    for _ in range(100):
        time.sleep(0.1)
        if link.cmd("STATE?") == "OK IDLE": break
    else:
        sys.exit("shot did not finish")
    print(f"=== {hz} Hz duty {duty}%  per {per} cmp {cmp_} | counts {link.must('PFMIN:STATus?')}", flush=True)
    out = {}
    for ch in CH:
        n, ov, lvl, d = read_raw(ch)
        out[ch] = {"n": n, "ov": ov, "lvl": lvl, "deltas": d}
        print(f"  input {ch}: edges {n} {ov} level {lvl} first {d[:6]}", flush=True)
    return {"hz": hz, "duty": duty, "per": per, "cmp": cmp_, "ch": out}


res = []
for hz in freqs:
    for duty in duties:
        res.append(shot(hz, duty, a.steps))
        json.dump(res, open(a.out, "w"))

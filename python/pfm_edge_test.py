"""pfm_edge_test.py -- raw both-edge capture on PFM_Input_1..4 (2026-09-30 duty
re-exploration): fires constant-frequency shots at 40/50/60 % duty with the four
inputs in BOTH-edge mode (PFMIN:EDGes) and saves the raw streams to a JSON file.
Decode: LEVEL is the pin level before the first captured edge, so the first delta
after t0 is the time spent at the OPPOSITE level; states then alternate.

    python3 python/pfm_edge_test.py edges.json [socket://localhost:5002]
Needs an idle HBRIDGE board with U, V, W, UN fibred to PFM_Input_1..4.
"""
import os, sys, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pfm_shot_test as T

link = T.Link(sys.argv[2] if len(sys.argv) > 2 else "socket://localhost:5002", "edges")
print(link.must("*IDN?")); assert link.must("STATE?") == "OK IDLE"
link.must("CONFig:MODE HVPS")
CH = [1, 2, 3, 4]
for c in CH: link.must(f"PFMIN:EDGes {c} BOTH")
print(link.must("PFMIN:EDGes?"))

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

def shot(hz, duty, steps=80):
    per = round(170e6 / hz) - 1
    cmp_ = round(per * duty / 100)
    link.must("TABle:BEGin")
    for _ in range(steps):
        assert link.cmd(f"TABle:STEP {per} {cmp_} {cmp_} {cmp_}") == "OK"
    link.must("TABle:END")
    link.must(f"PFMIN:CAPTURE {steps}")
    link.must("ARM"); link.must("FIRE")
    for _ in range(100):
        time.sleep(0.1)
        if link.cmd("STATE?") == "OK IDLE": break
    print(f"\n=== {hz} Hz duty {duty}%  per {per} cmp {cmp_} | counts {link.must('PFMIN:STATus?')}")
    out = {}
    for ch in CH:
        n, ov, lvl, d = read_raw(ch)
        out[ch] = {"n": n, "ov": ov, "lvl": lvl, "deltas": d}
        print(f"  input {ch}: edges {n} {ov} start-level {lvl} first deltas {d[:8]}")
    return per, cmp_, out

res = {}
for duty in (40, 50, 60):
    per, cmp_, out = shot(20000, duty)
    res[duty] = {"per": per, "cmp": cmp_, "ch": out}
json.dump(res, open(sys.argv[1], "w"))

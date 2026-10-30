#!/usr/bin/env python3
"""wham_sim.py -- a SIMULATED WHAM controller for practice and for testing the operator console. No hardware.

It listens on a TCP port and answers like the firmware does for the commands the operator console uses: one reply line
per command (`OK ...` / `ERR <n> ...`), a state machine (IDLE -> ARMED -> FIRING -> IDLE, FAULT), a shot upload with the
supply mode's limits, the external trigger and (HBRIDGE) the CHARGE output. It is NOT the firmware: it does not model the
hardware, timing or any of the less common commands, and a command it does not know is answered `ERR 1`. A "shot" lasts at
least `min_fire_s` seconds so that the FIRING state is visible on screen.

    python3 host/operator_ui/wham_sim.py --product hbridge --port 5199
"""
import argparse
import os
import socketserver
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dangerous_commands as dc

LIMITS = {"LIMITER": (75000.0, 125000.0, 5.0, 95.0), "HVPS": (10000.0, 30000.0, 40.0, 60.0)}
SHOT_MAX_US = 1_000_000
SHOT_SETPOINT_MAX = 500


class SimState:
    def __init__(self, product, serial_no, label, min_fire_s):
        self.product, self.serial, self.label, self.min_fire_s = product, serial_no, label, min_fire_s
        self.lock = threading.RLock()
        self.state = "IDLE"
        self.fault = None                       # text after FAULT, e.g. "GENERAL"
        self.mode = None                        # HBRIDGE: LIMITER | HVPS
        self.phases = {"U": 1, "V": 1, "W": 1}
        self.shot = []                          # [(t_us, f_hz, [dU, dV, dW])]
        self.uploading = False
        self.trigger = 1
        self.trigger_in = 0
        self.edges = 0
        self.charge_hz = 0
        self.fire_until = 0.0
        self.channels = {n: {"enable": 1, "ena": 0, "contact": 0, "amps": 0.0} for n in (1, 2, 3, 4)}
        self.timing = None
        self.ext_enable = 1
        self.boot = time.time()
        self.pid_rate = 1000 if product == "xrex" else 10000
        self.loopmode = [0, 0, 0]                  # HBRIDGE: 1 = closed loop (only a closed-loop run is logged)
        self.log_arm = None                        # (loop or 0 = all, max samples, decim) from LOG:ARM
        self.log = {}                              # channel -> [(setpoint, measured, output), ...] in the wire's integer units
        self.log_rate = 0

    def tick(self):
        with self.lock:
            if self.state == "FIRING" and time.time() >= self.fire_until:
                self.state = "IDLE"


def uid():
    return "SIM000000000000000000"


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        sim = self.server.sim
        while True:
            try:
                raw = self.rfile.readline()
            except (ConnectionError, OSError):
                return
            if not raw:
                return
            for piece in raw.decode(errors="replace").replace("\r", "\n").split("\n"):
                cmd = piece.strip()
                if not cmd:
                    continue
                reply = self.server.dispatch(cmd)
                try:
                    self.wfile.write((reply + "\r\n").encode())
                    self.wfile.flush()
                except (ConnectionError, OSError):
                    return


class SimController(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, product="hbridge", port=0, serial_no="WB-SIM1-0", label="practice", min_fire_s=3.0, host="127.0.0.1"):
        super().__init__((host, port), Handler)
        self.sim = SimState(product, serial_no, label, min_fire_s)
        self.thread = None

    @property
    def port(self):
        return self.server_address[1]

    def start(self):
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        return self.port

    def stop(self):
        self.shutdown()
        self.server_close()

    # ---- the commands -------------------------------------------------------------------------------------------
    def dispatch(self, line):
        s = self.sim
        s.tick()
        parts = line.split()
        head, args = parts[0], parts[1:]
        query = head.endswith("?")
        path = head[:-1] if query else head
        with s.lock:
            for pattern, is_query, fn in self.table():
                if is_query == query and dc.path_matches(pattern, path):
                    try:
                        return fn(args)
                    except ValueError as e:
                        return f"ERR 12 {e}"
            if head.upper() == "*IDN?":
                return self.idn()
        return "ERR 1 Unknown command"

    def idn(self):
        s = self.sim
        name = "WHAM-HBRIDGE-PFMG474" if s.product == "hbridge" else "WHAM-XREX-PFMG474"
        return f"OK {name} REVA v0.11 sim0000 SN={s.serial} UID={uid()}"

    def table(self):
        s = self.sim
        T = [
            ("SYS:ID", True, self.sys_id),
            ("STATE", True, lambda a: "OK " + s.state + (f" {s.fault}" if s.state == "FAULT" and s.fault else "")),
            ("FAULT", True, lambda a: f"OK {1 if s.state == 'FAULT' else 0}"),
            ("FAULT:SOURces", True, lambda a: "OK PC10=0 GATEDRV=0 TABLE=0 TEST=%d OVP=0 FB=0" % (1 if s.state == "FAULT" else 0)),
            ("FAULT:CLEar", False, self.fault_clear),
            ("ARM", False, self.arm),
            ("DISARM", False, self.disarm),
            ("GENERAL:TEST:FAULT", False, self.test_fault),
            ("LOG:ARM", False, self.log_arm_cmd),
            ("LOG:DATA", True, self.log_data),
            ("CONFig:PIDRate", True, lambda a: f"OK {s.pid_rate}"),
            ("EXTernal:TRIGger", False, self.trigger_set),
            ("EXTernal:TRIGger", True, lambda a: f"OK {s.trigger}"),
            ("EXTernal:TRIGger:INPut", True, lambda a: f"OK {s.trigger_in}"),
            ("EXTernal:TRIGger:STATus", True, lambda a: f"OK enabled={s.trigger} input={s.trigger_in} edges={s.edges} started=0 "
                                                       "ignored_disabled=0 ignored_state=0 ignored_glitch=0 refused=0 busy=0 last=NONE"),
        ]
        if s.product == "hbridge":
            T += [
                ("CONFig:MODE", False, self.mode_set),
                ("CONFig:MODE", True, lambda a: "OK " + (s.mode or "NONE")),
                ("CONFig:LIMits", True, self.limits),
                ("PHASe:ENAble", True, lambda a: "OK " + " ".join(f"{p}={v}" for p, v in s.phases.items())),
                ("PHASe:SELect", False, self.phase_select),
                ("SHOT:BEGin", False, self.shot_begin),
                ("SHOT:STEP", False, self.shot_step),
                ("SHOT:END", False, self.shot_end),
                ("SHOT:CLEar", False, self.shot_clear),
                ("SHOT", True, lambda a: f"OK {len(s.shot)} {s.shot[-1][0] if s.shot else 0} PROFILE"),
                ("SHOT:POINt", True, self.shot_point),
                ("FIRE", False, self.fire),
                ("PID:LOOPMODE", False, self.loopmode_set),
                ("PID:LOOPMODE", True, lambda a: "OK " + " ".join(map(str, s.loopmode))),
                ("CHARGE:FREQuency", False, self.charge_set),
                ("CHARGE:FREQuency", True, lambda a: f"OK {s.charge_hz}"),
                ("CHARGE:STATus", True, self.charge_status),
            ]
        else:
            T += [
                ("SOURce:ENAble", False, self.src_enable),
                ("SOURce:ENAble", True, lambda a: f"OK {s.channels[self.ch(a)]['enable']}"),
                ("SOURce:STATus", True, lambda a: f"OK {1 if s.state == 'FIRING' else 0} 0 0 0"),
                ("SOURce:STOP", False, self.cmd_stop),
                ("XREX:CHANnel:STATus", True, lambda a: (self.ch(a), "OK WATER=HIGH TMP=HIGH ENERPRO=HIGH OCP=HIGH")[1]),
                ("XREX:CHANnel:ENAOut", False, lambda a: self.chan_out(a, "ena")),
                ("XREX:CHANnel:ENAOut", True, lambda a: f"OK {s.channels[self.ch(a)]['ena']}"),
                ("XREX:CHANnel:CONTactOut", False, lambda a: self.chan_out(a, "contact")),
                ("XREX:CHANnel:CONTactOut", True, lambda a: f"OK {s.channels[self.ch(a)]['contact']}"),
                ("SHOT:TIMing", False, self.timing_set),
                ("SHOT:TIMing", True, lambda a: "OK " + " ".join(map(str, s.timing)) if s.timing else "ERR 12 not configured"),
                ("SHOT:CURRent", False, self.current_set),
                ("SHOT:CURRent", True, lambda a: f"OK {s.channels[self.ch(a)]['amps']:g}"),
                ("SHOT:STARt", False, self.fire),
                ("CONFig:TURNONHz", True, lambda a: "OK 5000"),
                ("CONFig:MAXFREQHz", True, lambda a: "OK 100000"),
                ("CONFig:MAXCURRent", True, lambda a: (self.ch(a), "OK 6000")[1]),
                ("EXTernal:ENAble", True, lambda a: f"OK {s.ext_enable}"),
                ("EXTernal:INPut", True, lambda a: "OK 1"),
            ]
        return T

    def sys_id(self, a):
        s = self.sim
        prod = "WHAM-HBRIDGE-PFMG474" if s.product == "hbridge" else "WHAM-XREX-PFMG474"
        return (f"OK serial={s.serial} uid={uid()} flashuid=SIM0000000000000 product={prod} hw=REVA fw=v0.11 commit=sim0000 "
                f"label={s.label} verified=1 identity=ok")

    def ch(self, a):
        try:
            n = int(a[0])
        except (IndexError, ValueError):
            raise ValueError("needs a channel 1-4")
        if n not in self.sim.channels:
            raise ValueError("channel out of range")
        return n

    # state machine
    def arm(self, a):
        s = self.sim
        if s.state == "FAULT":
            return "ERR 6 Fault latched"
        if s.state != "IDLE":
            return "ERR 13 Not IDLE"
        s.state = "ARMED"
        return "OK"

    def disarm(self, a):
        s = self.sim
        if s.state != "ARMED":
            return "ERR 13 Not ARMED"
        s.state = "IDLE"
        return "OK"

    def cmd_stop(self, a):
        s = self.sim
        if s.state in ("ARMED", "FIRING"):
            s.state = "IDLE"
        return "OK"

    def test_fault(self, a):
        s = self.sim
        s.state, s.fault = "FAULT", "GENERAL"
        return "OK"

    def fault_clear(self, a):
        s = self.sim
        s.state, s.fault = "IDLE", None
        return "OK"

    def fire(self, a):
        s = self.sim
        if s.state == "FAULT":
            return "ERR 6 Fault latched"
        if s.state != "ARMED":
            return "ERR 13 Not ARMED"
        if s.product == "hbridge" and not s.shot:
            return "ERR 5 Nothing uploaded to play"
        if s.product == "xrex" and not s.timing:
            return "ERR 12 SHOT:TIMing was never set"
        dur = (s.shot[-1][0] / 1e6) if (s.product == "hbridge" and s.shot) else sum(s.timing or (0, 0, 0))
        s.state = "FIRING"
        s.fire_until = time.time() + max(s.min_fire_s, dur)
        self.make_log(dur)
        return "OK"

    def make_log(self, dur):
        """What the firmware's waveform log would hold after this shot (synthetic: a lagging measurement with a little ripple)."""
        s = self.sim
        s.log = {}
        if s.log_arm is None or (s.product == "hbridge" and not any(s.loopmode)):
            return                                # HBRIDGE logs only during a closed-loop run, like the firmware
        loop, cap, decim = s.log_arm
        rate = s.pid_rate / decim
        n = max(0, min(cap, int(dur * rate)))
        s.log_rate = int(rate)
        chans = [loop] if loop else (list(s.channels) if s.product == "xrex" else [1, 2, 3])
        import math
        for ch in chans:
            rows, pv = [], 0.0
            for i in range(n):
                t = i / rate
                if s.product == "xrex":
                    up, flat, down = s.timing
                    a = s.channels[ch]["amps"] or 3000.0
                    frac = t / up if t < up else 1.0 if t < up + flat else max(0.0, 1 - (t - up - flat) / down)
                    sp = 5000 + frac * (a / 6000.0) * 95000
                    pv += (sp - pv) * 0.25 + 40 * math.sin(i * 0.7 + ch)
                    rows.append((int(sp), int(pv), int(sp + 120 * math.sin(i * 0.3))))
                else:
                    tt = t * 1e6
                    sp = 300.0 * (tt / max(1, s.shot[-1][0]))
                    pv += (sp - pv) * 0.3
                    rows.append((int(sp * 1000), int((pv + 0.4 * math.sin(i * 0.9)) * 1000), int((50 + 0.01 * sp) * 1000)))
            s.log[ch] = rows

    def log_arm_cmd(self, a):
        s = self.sim
        try:
            loop, cap, decim = (int(x) for x in a)
        except ValueError:
            return "ERR 12 LOG:ARM needs <ch> <maxSamples> <decim>"
        top = 4 if s.product == "xrex" else 3
        if len(a) != 3 or not 0 <= loop <= top:
            return "ERR 12 LOG:ARM needs <ch> <maxSamples> <decim>"
        limit = (1750 if s.product == "xrex" else (333 if loop == 0 else 1000))
        if not 1 <= cap <= limit or not 1 <= decim <= 10000:
            return f"ERR 12 LOG:ARM needs <ch> <maxSamples 1-{limit}> <decim 1-10000>"
        s.log_arm, s.log = (loop, cap, decim), {}
        return "OK"

    def log_data(self, a):
        s = self.sim
        try:
            ch = int(a[0])
            start = int(a[1]) if len(a) > 1 else 0
            want = min(40, int(a[2])) if len(a) > 2 else 40
        except (IndexError, ValueError):
            return "ERR 12 LOG:DATA? <ch> [<start> [<count>]]"
        rows = s.log.get(ch)
        if rows is None:
            if s.log:
                return "ERR 12 That loop was not logged"
            return f"OK 0 {s.pid_rate // (s.log_arm[2] if s.log_arm else 1)}" + (" SUM=0" if s.product == "hbridge" else "")
        if s.product == "xrex":
            return f"OK {len(rows)} {s.log_rate} " + " ".join(f"{a} {b} {c}" for a, b, c in rows)
        page = rows[start:start + want]
        total = sum(x & 0xFFFFFFFF for r in page for x in r) & 0xFFFFFFFF
        return f"OK {len(rows)} {s.log_rate} SUM={total} " + " ".join(f"{a} {b} {c}" for a, b, c in page)

    def loopmode_set(self, a):
        s = self.sim
        try:
            loop, v = int(a[0]), int(a[1])
        except (IndexError, ValueError):
            return "ERR 12 PID:LOOPMODE needs <loop> <0|1>"
        for i in (range(3) if loop == 0 else [loop - 1]):
            s.loopmode[i] = v
        return "OK"

    def trigger_set(self, a):
        if len(a) != 1 or a[0] not in ("0", "1"):
            return "ERR 12 EXTernal:TRIGger needs one argument: 0|1"
        self.sim.trigger = int(a[0])
        return "OK"

    # HBRIDGE
    def mode_set(self, a):
        s = self.sim
        if s.state != "IDLE":
            return "ERR 13 Not IDLE"
        if len(a) != 1 or a[0].upper() not in ("LIMITER", "LIM", "HVPS"):
            return "ERR 12 CONFig:MODE needs LIMiter or HVPS"
        new = "LIMITER" if a[0].upper().startswith("LIM") else "HVPS"
        if new != s.mode:
            s.shot = []
        s.mode = new
        return "OK"

    def limits(self, a):
        s = self.sim
        if not s.mode:
            return "OK NONE"
        f0, f1, d0, d1 = LIMITS[s.mode]
        return f"OK {s.mode} FREQ {f0:g} {f1:g} DUTY {d0:g} {d1:g}"

    def phase_select(self, a):
        s = self.sim
        if s.state != "IDLE":
            return "ERR 13 Not IDLE"
        if len(a) != 1 or not a[0] or any(c not in "UVWuvw" for c in a[0]):
            return "ERR 12 PHASe:SELect needs a combination of U, V and W"
        chosen = a[0].upper()
        s.phases = {p: 1 if p in chosen else 0 for p in "UVW"}
        return "OK"

    def shot_begin(self, a):
        s = self.sim
        if not s.mode:
            return "ERR 13 No supply mode selected -- send CONFig:MODE LIMiter|HVPS first"
        if s.state == "FIRING":
            return "ERR 13 FIRING"
        s.shot, s.uploading = [], True
        return "OK"

    def shot_clear(self, a):
        s = self.sim
        s.shot, s.uploading = [], False
        return "OK"

    def shot_step(self, a):
        s = self.sim
        if not s.uploading:
            return "ERR 2 SHOT:STEP without SHOT:BEGin"
        if len(s.shot) >= SHOT_SETPOINT_MAX:
            return "ERR 3 Shot full"
        if len(a) not in (3, 5):
            return "ERR 4 SHOT:STEP needs <t_us> <freq_hz> <duty> or <t_us> <freq_hz> <dU> <dV> <dW>"
        try:
            t, f = int(a[0]), float(a[1])
            duty = [float(x) for x in a[2:]]
        except ValueError:
            return "ERR 12 not a number"
        if len(duty) == 1:
            duty = duty * 3
        if (not s.shot and t != 0) or (s.shot and t <= s.shot[-1][0]) or t > SHOT_MAX_US:
            return "ERR 4 time must start at 0, increase, and be at most 1000000 us"
        f0, f1, d0, d1 = LIMITS[s.mode]
        if not f0 <= f <= f1:
            return f"ERR 10 frequency {f:g} is outside the {s.mode} range ({f0:g}-{f1:g} Hz)"
        for name, d in zip("UVW", duty):
            if not d0 <= d <= d1:
                return f"ERR 10 duty of phase {name} is outside the {s.mode} range ({d0:g}-{d1:g} %)"
        s.shot.append((t, f, duty))
        return "OK"

    def shot_end(self, a):
        s = self.sim
        s.uploading = False
        if len(s.shot) < 2:
            s.shot = []
            return "ERR 4 at least two setpoints are needed"
        return f"OK {len(s.shot)} {s.shot[-1][0]}"

    def shot_point(self, a):
        s = self.sim
        try:
            i = int(a[0])
            t, f, d = s.shot[i]
        except (IndexError, ValueError):
            return "ERR 12 no such setpoint"
        if d[0] == d[1] == d[2]:
            return f"OK {t} {f:g} {d[0]:g}"
        return f"OK {t} {f:g} {d[0]:g} {d[1]:g} {d[2]:g}"

    def charge_set(self, a):
        s = self.sim
        if len(a) != 1:
            return "ERR 12 CHARGE:FREQuency needs one argument: <hz> (0 = off)"
        try:
            hz = int(a[0])
        except ValueError:
            return f"ERR 12 not a number: {a[0]}"
        if hz < 0 or hz > 100000:
            return "ERR 12 CHARGE:FREQuency must be 0 (off) or 1-100000 Hz"
        s.charge_hz = hz
        return "OK"

    def charge_status(self, a):
        s = self.sim
        hz = s.charge_hz
        return f"OK freq={hz} running={1 if hz else 0} actual={hz}.000 psc=0 arr={max(hz and 170000000 // hz - 1, 0)} ccr={max(hz and 85000000 // hz, 0)}"

    # XREX
    def src_enable(self, a):
        s = self.sim
        n = self.ch(a)
        if len(a) != 2 or a[1] not in ("0", "1"):
            return "ERR 12 SOURce:ENAble needs <ch> <0|1>"
        s.channels[n]["enable"] = int(a[1])
        return "OK"

    def chan_out(self, a, key):
        s = self.sim
        n = self.ch(a)
        if len(a) != 2 or a[1] not in ("0", "1"):
            return "ERR 12 needs <ch> <0|1>"
        s.channels[n][key] = int(a[1])
        return "OK"

    def timing_set(self, a):
        s = self.sim
        if len(a) != 3:
            return "ERR 12 SHOT:TIMing needs <up> <flat> <down> (seconds)"
        vals = tuple(float(x) for x in a)
        if any(v <= 0 for v in vals):
            return "ERR 12 all three must be > 0"
        s.timing = vals
        return "OK"

    def current_set(self, a):
        s = self.sim
        n = self.ch(a)
        s.channels[n]["amps"] = float(a[1])
        return "OK"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product", choices=("hbridge", "xrex"), default="hbridge")
    ap.add_argument("--port", type=int, default=5199)
    ap.add_argument("--min-fire-s", type=float, default=3.0)
    args = ap.parse_args()
    srv = SimController(args.product, args.port, min_fire_s=args.min_fire_s)
    print(f"simulated {args.product} controller on 127.0.0.1:{srv.port} (Ctrl-C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

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
        self.xloop = {n: 1 for n in (1, 2, 3, 4)}  # XREX: 1 = closed loop (the firmware's default)
        self.gains = {n: [0.0, 0.0, 0.0] for n in (1, 2, 3, 4)}   # XREX gains are zero at boot, as on the real controller
        self.loopmode = [0, 0, 0]                  # HBRIDGE: 1 = closed loop (only a closed-loop run is logged)
        self.log_arm = None                        # (loop or 0 = all, max samples, decim) from LOG:ARM
        self.log = {}                              # channel -> [(setpoint, measured, output), ...] in the wire's integer units
        self.log_rate = 0
        self.pfm_modes = ["R"] * 6                 # HBRIDGE PFMIN:EDGes: R = rising only, B = both edges
        self.pfm_arm = None                        # PFMIN:CAPTURE <M>: cycles to capture during the next shot
        self.pfm = {}                              # input -> ("R", [ticks]) or ("B", level, [t0, d1, ...])
        self.pfm_glitch_every = 0                  # test hook: damage every Nth PFMIN page reply (a lost character, as on the bridge)
        self.pfm_pages = 0
        self.pfm_duty_offset = -2.0                # measured duty minus programmed, percentage points (the real board reads about 2 low)
        # protection inputs (HBRIDGE): twelve gate-driver status inputs and the spark-gap input, as the firmware has them
        self.gds_raw = [1] * 12                    # 1 = HIGH. Tests and the practice mode change these to make a fault
        self.gds_enabled = [1] * 12
        self.gds_nick = ["-"] * 12
        self.gds_normally_low = False              # NORMALLY_HIGH (a LOW pin is the fault) is the HBRIDGE build's setting
        self.gds_last = 0                          # raw levels at the last gate-driver latch (GDS:LAST?), 0 = none since boot
        self.gds_checks = 1
        self.spark_in, self.spark_enabled = 1, 1
        self.bypass = 0                            # DEBUG:FAULT:BYPASS
        self.sources = {"GATEDRV": 0, "TEST": 0, "SPARK": 0}
        self.deadtime_ns = 100
        self.profiles = {}                         # slot -> {"label", "fw", "values": {...}, "shot": [...]}
        self.last_start = None                     # EXTernal:TRIGger:LATency? of the last start

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
            if head.upper() in ("*SAV", "*RCL"):
                try:
                    return self.profile_save(args) if head.upper() == "*SAV" else self.profile_recall(args)
                except ValueError as e:
                    return f"ERR 12 {e}"
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
            ("FAULT:SOURces", True, self.fault_sources),
            ("DEBUG:FAULT:BYPASS", False, self.bypass_set),
            ("DEBUG:FAULT:BYPASS", True, lambda a: f"OK {s.bypass}"),
            ("CONFig:DEADtime", False, self.deadtime_set),
            ("CONFig:DEADtime", True, lambda a: f"OK {s.deadtime_ns}"),
            ("CONFig:PROFile:CATalog", True, self.profile_catalog),
            ("CONFig:PROFile", True, self.profile_show),
            ("CONFig:PROFile:DIFF", True, self.profile_diff),
            ("CONFig:PROFile:CLEar", False, self.profile_clear),
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
                ("EXTernal:TRIGger:LATency", True, self.latency),
                ("GDS", True, lambda a: "OK " + " ".join(f"{i + 1:02d}={'HIGH' if v else 'LOW'}" for i, v in enumerate(s.gds_raw))),
                ("GDS:LAST", True, lambda a: f"OK {s.gds_last:03X} {s.gds_checks}"),
                ("GDS:POLarity", True, lambda a: "OK " + ("NORMALLY_LOW" if s.gds_normally_low else "NORMALLY_HIGH")),
                ("GDS:POLarity", False, self.gds_polarity),
                ("GDS:ENAble", True, lambda a: self.gds_list(a, s.gds_enabled, str)),
                ("GDS:ENAble", False, self.gds_enable),
                ("GDS:NICKname", True, lambda a: self.gds_list(a, s.gds_nick, str)),
                ("GDS:NICKname", False, self.gds_nickname),
                ("SPARKgap:INPut", True, lambda a: f"OK {s.spark_in}"),
                ("SPARKgap:ENAble", True, lambda a: f"OK {s.spark_enabled}"),
                ("SPARKgap:ENAble", False, self.spark_enable),
                ("PFMIN:CAPTURE", False, self.pfm_capture),
                ("PFMIN:STATus", True, lambda a: "OK " + " ".join(str(self.pfm_count(i)) for i in range(1, 7))),
                ("PFMIN:EDGes", False, self.pfm_edges_set),
                ("PFMIN:EDGes", True, lambda a: "OK " + " ".join(s.pfm_modes)),
                ("PFMIN:DATA", True, lambda a: self.pfm_page(a, "R")),
                ("PFMIN:RAW", True, lambda a: self.pfm_page(a, "B")),
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
                ("PID:LOOPMODE", False, self.xloop_set),
                ("PID:LOOPMODE", True, lambda a: f"OK {s.xloop[self.ch(a)]}"),
                ("PID:GAINS", False, self.gains_set),
                ("PID:GAINS", True, lambda a: "OK " + " ".join(f"{g:g}" for g in s.gains[self.ch(a)])),
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
        s.sources["TEST"] = 1
        s.state, s.fault = "FAULT", "GENERAL"
        return "OK"

    def fault_clear(self, a):
        s = self.sim
        s.sources = {k: 0 for k in s.sources}
        s.state, s.fault = "IDLE", None
        self.check_protection()                     # an input still in its fault state latches again, as on the real controller
        return "OK"

    # protection inputs (HBRIDGE): the firmware's rules, without the hardware
    def gds_fault_bits(self):
        s = self.sim
        return [bool(en) and (bool(raw) if s.gds_normally_low else not raw) for raw, en in zip(s.gds_raw, s.gds_enabled)]

    def check_protection(self):
        """Evaluate the fault inputs: latch FAULT GENERAL when a switched-on input is in its fault state and the bypass is off.
        Called after every change (a test or the practice page changing a pin, an enable, the polarity, a clear)."""
        s = self.sim
        if s.product != "hbridge":
            return
        s.gds_checks += 1
        gate = any(self.gds_fault_bits())
        spark = bool(s.spark_enabled) and not s.spark_in
        if gate:
            s.sources["GATEDRV"] = 1
            s.gds_last = sum(1 << i for i, v in enumerate(s.gds_raw) if v)
        if spark:
            s.sources["SPARK"] = 1
        if (gate or spark) and not s.bypass:
            s.state, s.fault = "FAULT", "GENERAL"

    def set_pin(self, n, level):
        """Test and practice hook: drive gate-driver status input n (1-12), or the spark-gap input (n = 'spark'), HIGH (1) or LOW (0)."""
        s = self.sim
        with s.lock:
            if n == "spark":
                s.spark_in = 1 if level else 0
            else:
                s.gds_raw[int(n) - 1] = 1 if level else 0
            self.check_protection()

    def fault_sources(self, a):
        s = self.sim
        gate = 1 if (s.sources["GATEDRV"] or any(self.gds_fault_bits())) else 0
        spark = 1 if (s.sources["SPARK"] or (s.spark_enabled and not s.spark_in)) else 0
        base = f"OK PC10=0 GATEDRV={gate} TABLE=0 TEST={s.sources['TEST']} OVP=0 FB=0"
        return base + (f" SPARK={spark}" if s.product == "hbridge" else "")

    def bypass_set(self, a):
        s = self.sim
        if not a or a[0] not in ("0", "1"):
            return "ERR 12 needs 0 or 1"
        s.bypass = int(a[0])
        if not s.bypass:
            self.check_protection()
        return "OK"

    def deadtime_set(self, a):
        s = self.sim
        try:
            ns = int(a[0])
        except (IndexError, ValueError):
            return "ERR 12 not a number"
        if not 50 <= ns <= 3000:
            return "ERR 12 dead time must be 50-3000 ns"
        if s.state == "FIRING":
            return "ERR 13 FIRING"
        counts = int(ns * 170 / 1000 + 0.5)                           # the 170 MHz timer counts in steps of 5.88 ns
        s.deadtime_ns = int(counts * 1000 / 170 + 0.5)                # read back as the firmware does: 150 -> 153
        return "OK"

    def gds_index(self, a, zero_ok=False):
        try:
            n = int(a[0])
        except (IndexError, ValueError):
            raise ValueError("needs an input number")
        if not (0 if zero_ok else 1) <= n <= 12:
            raise ValueError("input out of range")
        return n

    def gds_list(self, a, values, fmt):
        if a:
            return f"OK {fmt(values[self.gds_index(a) - 1])}"
        return "OK " + " ".join(f"{i + 1:02d}={fmt(v)}" for i, v in enumerate(values))

    def gds_enable(self, a):
        s = self.sim
        n = self.gds_index(a, zero_ok=True)
        if len(a) < 2 or a[1] not in ("0", "1"):
            return "ERR 12 needs <input 0-12> <0|1>"
        if a[1] == "0" and s.state in ("ARMED", "FIRING"):
            return "ERR 13 an input can be switched off only in IDLE or FAULT"
        for i in (range(12) if n == 0 else [n - 1]):
            s.gds_enabled[i] = int(a[1])
        self.check_protection()
        return "OK"

    def gds_nickname(self, a):
        s = self.sim
        n = self.gds_index(a)
        name = a[1] if len(a) > 1 else ""
        if name != "-" and not (1 <= len(name) <= 15 and all(c.isalnum() or c in "_.-+" for c in name)):
            return "ERR 14 invalid nickname"
        s.gds_nick[n - 1] = name
        return "OK"

    def gds_polarity(self, a):
        s = self.sim
        word = (a[0] if a else "").upper()
        if s.state not in ("IDLE", "FAULT"):
            return "ERR 13 only in IDLE or FAULT"
        if word == "NORMALLY_HIGH":
            s.gds_normally_low = False
        elif word == "NORMALLY_LOW":
            s.gds_normally_low = True
        elif word in ("INV", "INVERT"):
            s.gds_normally_low = not s.gds_normally_low
        else:
            return "ERR 12 needs NORMALLY_HIGH, NORMALLY_LOW or INVert"
        self.check_protection()
        return "OK"

    def spark_enable(self, a):
        s = self.sim
        if not a or a[0] not in ("0", "1"):
            return "ERR 12 needs 0 or 1"
        if a[0] == "0" and s.state not in ("IDLE", "FAULT"):
            return "ERR 13 only in IDLE or FAULT"
        s.spark_enabled = int(a[0])
        self.check_protection()
        return "OK"

    def latency(self, a):
        s = self.sim
        if not s.last_start:
            return "ERR 13 no shot has been started since boot"
        return "OK " + s.last_start

    # saved profiles: eight slots, the same commands and replies as the firmware (what is stored is a small subset)
    def profile_values(self):
        s = self.sim
        v = {}
        if s.product == "hbridge":
            v.update({"DEADtime": s.deadtime_ns, "MODE": {"LIMITER": 1, "HVPS": 2}.get(s.mode, 0), "PHASe:ENAble": sum(1 << i for i, p in enumerate("UVW") if s.phases[p]),
                      "GDS:ENAble": sum(1 << i for i, e in enumerate(s.gds_enabled) if e), "GDS:POLarity": 1 if s.gds_normally_low else 0,
                      "SPARKgap:ENAble": s.spark_enabled, "shot": len(s.shot)})
        else:
            # the firmware's own names (settings_codec.c): the shot times in ms, and per channel "ch<n>.<field>". An XREX
            # profile does not keep the dead time, the channel enables or the ready lines.
            t = s.timing or (0, 0, 0)
            v.update({"RampUpMs": int(round(t[0] * 1000)), "FlatTopMs": int(round(t[1] * 1000)), "RampDownMs": int(round(t[2] * 1000))})
            for n in (1, 2, 3, 4):
                v.update({f"ch{n}.kp": s.gains[n][0], f"ch{n}.ki": s.gains[n][1], f"ch{n}.kd": s.gains[n][2], f"ch{n}.ClosedLoop": s.xloop[n]})
        return v

    def profile_slot(self, a):
        try:
            n = int(a[0])
        except (IndexError, ValueError):
            raise ValueError("needs a slot 1-8")
        if not 1 <= n <= 8:
            raise ValueError("slot must be 1-8")
        return n

    def profile_save(self, a):
        s = self.sim
        n = self.profile_slot(a)
        label = a[1] if len(a) > 1 else "-"
        if len(a) > 1 and not (1 <= len(label) <= 15 and label != "-"):
            return "ERR 12 label must be 1-15 characters"
        if s.state != "IDLE":
            return "ERR 13 only in IDLE"
        s.profiles[n] = {"label": label, "fw": "v0.11", "values": self.profile_values(), "shot": list(s.shot),
                         "gds_nick": list(s.gds_nick), "timing": s.timing}
        return "OK"

    def profile_catalog(self, a):
        s = self.sim
        return "OK " + " ".join(f"{n}={s.profiles[n]['label']}/{s.profiles[n]['fw']}" if n in s.profiles else f"{n}=empty" for n in range(1, 9))

    def profile_show(self, a):
        s = self.sim
        n = self.profile_slot(a)
        if n not in s.profiles:
            return "ERR 18 slot is empty"
        p = s.profiles[n]
        return f"OK label={p['label']} fw={p['fw']} " + " ".join(f"{k}={v}" for k, v in p["values"].items())

    def profile_diff(self, a):
        s = self.sim
        n = self.profile_slot(a)
        if n not in s.profiles:
            return "ERR 18 slot is empty"
        live, saved = self.profile_values(), s.profiles[n]["values"]
        diff = [f"{k}={live.get(k)}>{v}" for k, v in saved.items() if live.get(k) != v]
        if not diff and s.product == "hbridge" and list(s.shot) != s.profiles[n]["shot"]:
            diff = [f"shot={len(s.shot)}>{len(s.profiles[n]['shot'])}*"]
        return "OK " + (" ".join(diff) if diff else "SAME")

    def profile_recall(self, a):
        s = self.sim
        n = self.profile_slot(a)
        if s.state != "IDLE":
            return "ERR 13 only in IDLE"
        if n not in s.profiles:
            return "ERR 18 slot is empty"
        p, v = s.profiles[n], s.profiles[n]["values"]
        extra = ""
        if s.product == "hbridge":
            s.deadtime_ns = v["DEADtime"]
            s.mode = {1: "LIMITER", 2: "HVPS"}.get(v["MODE"])
            s.phases = {ph: (v["PHASe:ENAble"] >> i) & 1 for i, ph in enumerate("UVW")}
            s.gds_enabled = [(v["GDS:ENAble"] >> i) & 1 for i in range(12)]
            s.gds_normally_low = bool(v["GDS:POLarity"])
            s.spark_enabled = v["SPARKgap:ENAble"]
            s.gds_nick = list(p["gds_nick"])
            s.shot = list(p["shot"])
            off = s.gds_enabled.count(0)
            extra = (f" GDSOFF={off}" if off else "") + ("" if s.spark_enabled else " SPARKOFF=1")
            self.check_protection()
        else:
            s.timing = p["timing"]
            for n in (1, 2, 3, 4):
                s.gains[n] = [v[f"ch{n}.kp"], v[f"ch{n}.ki"], v[f"ch{n}.kd"]]
                s.xloop[n] = v[f"ch{n}.ClosedLoop"]
        return f"OK APPLIED={len(v)} FAILED=0{extra}"

    def profile_clear(self, a):
        s = self.sim
        n = self.profile_slot(a)
        if s.state != "IDLE":
            return "ERR 13 only in IDLE"
        s.profiles.pop(n, None)
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
        s.last_start = "edge_us=NA ring_us=2.5 preload_us=5.1 capture_us=1.9 pwm_us=17.2 start_us=39.5 total_us=NA prepared=1"
        s.fire_until = time.time() + max(s.min_fire_s, dur)
        self.make_log(dur)
        self.make_pfm()
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
                    if s.xloop[ch] == 1 and not any(s.gains[ch]):
                        out = 3000.0                      # the real controller: closed loop with zero gains sits at the 3 kHz floor
                    else:
                        out = sp + 120 * math.sin(i * 0.3)
                    pv += (out - pv) * 0.25 + 40 * math.sin(i * 0.7 + ch)
                    rows.append((int(sp), int(pv), int(out)))
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

    # ---- PFM_Input capture (HBRIDGE) -----------------------------------------------------------------------------
    CLK = 170_000_000

    def pfm_count(self, ch):
        d = self.sim.pfm.get(ch)
        if not d:
            return 0
        return len(d[1]) if d[0] == "R" else len(d[2]) // 2

    def pfm_capture(self, a):
        s = self.sim
        try:
            m = int(a[0])
        except (IndexError, ValueError):
            return "ERR 9 PFMIN:CAPTURE needs one argument: M (1-3800)"
        if not 1 <= m <= 3800:
            return "ERR 9 M out of range (1-3800)"
        s.pfm_arm, s.pfm = m, {}
        return "OK"

    def pfm_edges_set(self, a):
        s = self.sim
        try:
            ch, word = int(a[0]), a[1].upper()
        except (IndexError, ValueError):
            return "ERR 12 PFMIN:EDGes needs <input 1-6> RISING|BOTH"
        if not 1 <= ch <= 6 or word not in ("RISING", "BOTH"):
            return "ERR 12 PFMIN:EDGes needs <input 1-6> RISING|BOTH"
        if s.state != "IDLE":
            return "ERR 13 PFMIN:EDGes can only be changed while IDLE"
        s.pfm_modes[ch - 1] = "B" if word == "BOTH" else "R"
        s.pfm.pop(ch, None)
        return "OK"

    def make_pfm(self):
        """What the capture would hold after this shot: inputs 1..3 carry phases U, V, W (the bench wiring), the others are unwired."""
        s = self.sim
        if not s.pfm_arm or not s.shot:
            return
        s.pfm = {}
        ts, fs = [p[0] * 1e-6 for p in s.shot], [p[1] for p in s.shot]
        end = ts[-1]
        import math

        def at(xs, ys, x):
            for i in range(1, len(xs)):
                if x <= xs[i]:
                    return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1])
            return ys[-1]
        for ch in (1, 2, 3):
            ds = [p[2][ch - 1] for p in s.shot]
            t, cycles, gap_ticks = 0.0, [], 0
            while t < end and len(cycles) < s.pfm_arm:
                seg = next(i for i in range(1, len(ts)) if t < ts[i] or i == len(ts) - 1)
                if fs[seg - 1] == 0:                       # an output-off gap (a 0 Hz setpoint): no pulses until the next setpoint
                    gap_ticks += round((ts[seg] - t) * self.CLK)
                    t = ts[seg]
                    continue
                f = fs[seg - 1] if fs[seg] == 0 else at(ts, fs, t)     # on -> off holds the start frequency
                period = max(1, round(self.CLK / f))
                duty = at(ts, ds, t) + s.pfm_duty_offset + 0.05 * math.sin(len(cycles) * 0.7)
                high = max(1, round(period * duty / 100.0))
                cycles.append((period + gap_ticks, high))   # the first pulse after a gap measures the gap too
                gap_ticks = 0
                t += period / self.CLK
            if s.pfm_modes[ch - 1] == "R":
                s.pfm[ch] = ("R", [c[0] for c in cycles])
            else:
                values = [12_345_678]                                   # t0, then the tick difference to each next edge: high, low, high, low ...
                for per, high in cycles:
                    values += [high, per - high]
                s.pfm[ch] = ("B", 0, values)

    def pfm_page(self, a, want):
        s = self.sim
        try:
            ch = int(a[0])
            start = int(a[1]) if len(a) > 1 else 0
            count = min(100, int(a[2])) if len(a) > 2 else 100
        except (IndexError, ValueError):
            return "ERR 12 PFMIN:DATA? <input> [<start> [<count>]]"
        mode = s.pfm_modes[ch - 1] if 1 <= ch <= 6 else "R"
        if mode != want:
            return ("ERR 13 input %d is in BOTH-edge mode: use PFMIN:RAW?" if want == "R" else "ERR 13 input %d is not in BOTH-edge mode") % ch
        d = s.pfm.get(ch)
        if want == "R":
            vals = d[1] if d else []
            page = vals[start:start + count]
            reply = f"OK {len(vals)} OVERCAP=0 SUM={sum(page) & 0xFFFFFFFF} " + " ".join(map(str, page))
        else:
            vals = d[2] if d else []
            page = vals[start:start + count]
            reply = f"OK {len(vals)} OVERCAP=0 LEVEL={d[1] if d else 0} SUM={sum(page) & 0xFFFFFFFF} " + " ".join(map(str, page))
        s.pfm_pages += 1
        if s.pfm_glitch_every and s.pfm_pages % s.pfm_glitch_every == 0 and len(reply) > 40:
            reply = reply[:30] + reply[31:]                             # one character lost
        return reply.rstrip()

    def xloop_set(self, a):
        s = self.sim
        n = self.ch(a)
        if len(a) != 2 or a[1] not in ("0", "1"):
            return "ERR 12 PID:LOOPMODE needs <ch> <0|1>"
        s.xloop[n] = int(a[1])
        return "OK"

    def gains_set(self, a):
        s = self.sim
        n = self.ch(a)
        try:
            g = [float(x) for x in a[1:]]
        except ValueError:
            return "ERR 12 not a number"
        if len(g) != 3 or any(x < 0 for x in g):
            return "ERR 12 PID:GAINS needs <ch> <kp> <ki> <kd>"
        s.gains[n] = g
        return "OK"

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
        if f == 0:                                       # an output-off gap: not first, no limits to meet, no duty kept
            if not s.shot:
                return "ERR 4 The first setpoint (t=0) cannot be an off (0 Hz) one"
            duty = [0.0, 0.0, 0.0]
        else:
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
        if len(s.shot) < 2 or s.shot[-1][1] == 0 or s.shot[-2][1] == 0:
            s.shot = []
            return "ERR 4 A shot needs at least two setpoints (t=0 and the end of the shot), the last two not 0 Hz"
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

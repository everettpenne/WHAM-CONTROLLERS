"""recorder.py -- collects the controller's waveform log around one shot and keeps it (see shot_data.py for what the data is).

The operator asks for a recording while the controller is ARMED (the page does it when ARM is pressed with "record data" ticked).
This module then
  1. arms the controller's own log: `LOG:ARM <ch|0> <samples> <decim>` with a decimation chosen so the expected shot length fits;
  2. watches the controller's state (the connection's heartbeat polls it): ARMED -> FIRING -> back out = the shot has happened;
  3. reads the log back (`LOG:DATA?`: one reply per channel on XREX; 40-sample pages with a check sum on HBRIDGE, each retried when a
     character was lost on the link) and builds, saves and reports a recording.
Collection runs on the connection's heartbeat thread and takes the connection's lock one command at a time, so an operator's
commands are not blocked for the whole read.

Optionally the same recording also arms the PFM_Input capture (HBRIDGE: PFMIN:EDGes + PFMIN:CAPTURE, before the shot) and reads it back
after (PFMIN:DATA? or PFMIN:RAW?, see pfm_capture.py). That works open loop too and needs no closed-loop run; a recording may hold only
it. The capture modes the page changed are put back afterwards (changing them needs the controller IDLE).

Honest limits: the log lives in the controller's RAM and is read after the shot, so a shot that ends the log early (a fault) gives a
shorter recording; the controller holds only the latest log; and HBRIDGE logs only during a CLOSED-LOOP run (an open-loop shot is
played from a table and records nothing), which this module reports instead of pretending.
"""
import re
import threading
import time

import pfm_capture as pc
import shot_data as sd

WAIT_ARM_S = 600.0                 # how long a request may wait for the controller to be ARMED
PAGE = 40                          # HBRIDGE: samples per LOG:DATA? reply (the firmware's own cap)
TRIES = 3
PFM_TRIES = 8                      # PFMIN pages are long (100 numbers) and the bridge loses characters: more attempts than the log's
MAX_POINTS = 100                   # the programmed shot is read back for the overlay only up to this many setpoints (one query each)
XREX_MAX_SAMPLES = 1000
HB_MAX_SAMPLES = {"one": 1000, "all": 333}
DEFAULT_TICK_HZ = {"xrex": 1000, "hbridge": 10000}


class RecordError(Exception):
    pass


def _ok_text(reply):
    return reply[2:].strip() if reply and reply.startswith("OK") else None


class Recorder:
    def __init__(self, conn, shots_dir):
        self.conn, self.shots_dir = conn, shots_dir
        self.rec = None
        self.mu = threading.Lock()

    # ---- what the page shows
    def active(self):
        return self.rec is not None and self.rec["state"] in ("waiting", "armed", "running", "collecting")

    def status(self):
        r = self.rec
        if r is None:
            return None
        return {k: r.get(k) for k in ("state", "message", "progress", "sid", "warnings", "channels", "decim", "rate_hz", "max_samples",
                                      "duration_s", "requested_at", "pfm", "pfm_cycles")}

    # ---- helpers
    def _x(self, cmd, show=False):
        c = self.conn
        with c.lock:
            return c._exchange(cmd, c._timeout_for(cmd), poll=not show)

    def _q(self, cmd):
        try:
            return _ok_text(self._x(cmd))
        except Exception:
            return None

    def _product(self):
        c = self.conn
        board = (c.ident or {}).get("board", "")
        return "hbridge" if "HBRIDGE" in board.upper() else "xrex" if "XREX" in board.upper() else None

    def _state_word(self):
        t = self.conn.state_text or ""
        return t.split()[0] if t else None

    # ---- request
    def arm(self, spec):
        """Arm the controller's log and/or PFM capture for the next shot. Returns the status dict; raises RecordError with a sentence
        for the operator."""
        product = self._product()
        if product is None:
            raise RecordError("This device is not a known WHAM controller, so there is no shot data to record.")
        if self.active():
            raise RecordError("A recording is already waiting for a shot: cancel it first.")
        top = 4 if product == "xrex" else 3
        want_pfm = spec.get("pfm") or None
        try:
            chans = sorted({int(x) for x in spec.get("channels", [])})
            dur = float(spec.get("duration_s", 0))
        except (TypeError, ValueError):
            raise RecordError("Choose the channels and the length of the shot.")
        if (not chans and not want_pfm) or any(not 1 <= n <= top for n in chans):
            raise RecordError(f"Choose at least one {'channel' if product == 'xrex' else 'phase'} to record" + ("." if product == "xrex" else ", or tick the PFM input capture."))
        if not 0 < dur <= 3600:
            raise RecordError("The expected shot length must be more than 0 and at most 3600 seconds.")
        warnings, decim, rate, maxs, tick = [], None, None, None, DEFAULT_TICK_HZ[product]
        if chans:
            q = self._q("CONFig:PIDRate?")
            if q and q.split()[0].isdigit() and int(q.split()[0]) > 0:
                tick = int(q.split()[0])
            every = len(chans) > 1
            maxs = XREX_MAX_SAMPLES if product == "xrex" else HB_MAX_SAMPLES["all" if every else "one"]
            decim, rate, note = sd.choose_decimation(dur, tick, maxs)
            if note:
                warnings.append(note)
            if product == "hbridge":
                lm = _ok_text(self._safe("PID:LOOPMODE? 0"))
                if lm:
                    modes = [int(x) for x in lm.split() if x.isdigit()]
                    if not any(modes[n - 1] for n in chans if n - 1 < len(modes)):
                        warnings.append("Every selected phase is in open loop. The controller logs its waveform only during a closed-loop run, so the "
                                        "waveform log will be empty" + (" (the PFM capture is recorded anyway)." if want_pfm else ". Close a loop (PID:LOOPMODE <phase> 1, with a target) to record."))
        pfm = self._arm_pfm(product, want_pfm, dur, warnings) if want_pfm else None
        if chans:
            reply = self._x(f"LOG:ARM {0 if len(chans) > 1 else chans[0]} {maxs} {decim}", show=True)
            if not reply.startswith("OK"):
                self._restore_modes(pfm)
                raise RecordError(f"The controller refused to arm its log: {reply}")
        r = {"state": "waiting", "message": "Armed. Waiting for the controller to be ARMED.", "progress": 0.0, "channels": chans, "decim": decim,
             "rate_hz": rate, "max_samples": maxs, "duration_s": dur, "meta": spec.get("meta") or {}, "warnings": warnings, "tick_hz": tick,
             "requested_at": time.time(), "sid": None, "pfm": pfm, "pfm_cycles": pfm["cycles"] if pfm else None}
        self.rec = r
        now = _ok_text(self._safe("STATE?"))               # the heartbeat's copy may be a moment old
        if now:
            self.conn.state_text = now
        self._advance(self._state_word())
        return self.status()

    # ---- the PFM capture
    def _pfm_inputs(self, req):
        try:
            inputs = sorted({int(x) for x in req.get("inputs", [])})
        except (TypeError, ValueError, AttributeError):
            raise RecordError("Choose which PFM inputs to capture.")
        if not inputs or any(not 1 <= n <= pc.NUM_INPUTS for n in inputs):
            raise RecordError(f"Choose at least one PFM input (1 to {pc.NUM_INPUTS}).")
        edges = req.get("edges") or "both"
        if edges not in ("both", "rising"):
            raise RecordError("The PFM capture edges must be 'both' (period and duty) or 'rising' (period only).")
        return inputs, edges

    def _read_points(self):
        """The programmed shot as the controller holds it: [[t_us, f_hz, dU, dV, dW], ...] ([] when none or too many to read quickly)."""
        head = _ok_text(self._safe("SHOT?"))
        try:
            n = int(head.split()[0]) if head else 0
        except ValueError:
            n = 0
        if not 0 < n <= MAX_POINTS:
            return []
        return pc.parse_points([self._safe(f"SHOT:POINt? {i}") for i in range(n)])

    def _arm_pfm(self, product, req, dur, warnings):
        """Set the capture mode of each chosen input, then PFMIN:CAPTURE <M>. Returns the capture's record; raises RecordError."""
        if product != "hbridge":
            raise RecordError("PFM input capture is available for the Limiter and HVPS (HBRIDGE) controllers only.")
        inputs, edges = self._pfm_inputs(req)
        cycles = req.get("cycles")
        if cycles in (None, "", 0):
            points = self._read_points()
            fmax = max((p[1] for p in points), default=0.0)
            if not fmax:
                fmax = 125000.0
                warnings.append("No shot is loaded in the controller yet, so the capture length assumes the highest Limiter carrier (125 kHz).")
            cycles, note = pc.auto_cycles(dur, fmax)
            if note:
                warnings.append(note)
        else:
            try:
                cycles = int(cycles)
            except (TypeError, ValueError):
                raise RecordError("The number of PFM cycles must be a whole number.")
            if not 1 <= cycles <= pc.MAX_CYCLES:
                raise RecordError(f"The number of PFM cycles must be 1 to {pc.MAX_CYCLES}.")
        letter, word = ("B", "BOTH") if edges == "both" else ("R", "RISING")
        prev = (_ok_text(self._safe("PFMIN:EDGes?")) or "").split()
        pfm = {"inputs": inputs, "edges": edges, "cycles": cycles, "prev": prev, "set": []}
        for n in inputs:
            if n - 1 < len(prev) and prev[n - 1] == letter:
                continue
            reply = self._x(f"PFMIN:EDGes {n} {word}", show=True)
            if not reply.startswith("OK"):
                self._restore_modes(pfm)
                raise RecordError(f"The controller refused PFMIN:EDGes {n} {word}: {reply}. The capture mode can only be changed while the "
                                  "controller is IDLE: DISARM it first, then tick Record again.")
            pfm["set"].append(n)
        reply = self._x(f"PFMIN:CAPTURE {cycles}", show=True)
        if not reply.startswith("OK"):
            self._restore_modes(pfm)
            raise RecordError(f"The controller refused PFMIN:CAPTURE {cycles}: {reply}")
        return pfm

    def _restore_modes(self, pfm):
        """Put back the capture modes this page changed (best effort: only possible while IDLE). Returns True when nothing is left changed."""
        if not pfm or not pfm.get("set") or not pfm.get("prev"):
            return True
        if (self._state_word() or "") != "IDLE":
            return False
        ok = True
        for n in pfm["set"]:
            was = pfm["prev"][n - 1] if n - 1 < len(pfm["prev"]) else "R"
            reply = self._safe(f"PFMIN:EDGes {n} {'BOTH' if was == 'B' else 'RISING'}")
            ok = ok and bool(reply and reply.startswith("OK"))
        if ok:
            pfm["set"] = []
        return ok

    def _safe(self, cmd):
        try:
            return self._x(cmd)
        except Exception:
            return None

    def cancel(self):
        if self.rec and self.rec["state"] in ("waiting", "armed", "running"):
            self.rec["state"], self.rec["message"] = "cancelled", "Recording cancelled. The controller's log stays armed but nothing will be saved."
            if self.rec.get("pfm") and not self._restore_modes(self.rec["pfm"]):
                self.rec["message"] += " The PFM capture modes were left as set (they can only be changed while IDLE)."
        return self.status()

    def collect_now(self, spec=None):
        """Read whatever the controller's log holds right now (the shot may have run without this page noticing)."""
        if self.rec is None or self.rec["state"] in ("done", "empty", "failed", "cancelled"):
            product = self._product()
            if product is None:
                raise RecordError("This device is not a known WHAM controller.")
            spec = spec or {}
            top = 4 if product == "xrex" else 3
            want_pfm = spec.get("pfm") or None
            chans = sorted({int(x) for x in spec.get("channels", [] if want_pfm else range(1, top + 1))})
            pfm = None
            if want_pfm:
                if product != "hbridge":
                    raise RecordError("PFM input capture is available for the Limiter and HVPS (HBRIDGE) controllers only.")
                inputs, edges = self._pfm_inputs(want_pfm)
                pfm = {"inputs": inputs, "edges": edges, "cycles": None, "prev": None, "set": []}
            if not chans and not pfm:
                raise RecordError("Choose at least one phase or PFM input to read.")
            self.rec = {"state": "armed", "message": "", "progress": 0.0, "channels": chans, "decim": None, "rate_hz": None, "max_samples": None,
                        "duration_s": spec.get("duration_s"), "meta": spec.get("meta") or {}, "warnings": [], "tick_hz": DEFAULT_TICK_HZ[product],
                        "requested_at": time.time(), "sid": None, "pfm": pfm, "pfm_cycles": None}
        self._collect(None)
        return self.status()

    # ---- following the shot (called by the connection's heartbeat with the state it just read)
    def on_state(self, word):
        if self.rec is not None:
            self._advance(word)

    def _advance(self, word):
        r = self.rec
        if r is None or word is None:
            return
        st = r["state"]
        if st == "waiting":
            if word == "ARMED":
                r["state"], r["message"] = "armed", "Armed with the recording running. Waiting for the shot to start."
            elif word == "FIRING":
                r["state"], r["message"] = "running", "The shot is running."
            elif word == "FAULT":
                r["state"], r["message"] = "failed", "The controller faulted before the shot: nothing was recorded."
            elif time.time() - r["requested_at"] > WAIT_ARM_S:
                r["state"], r["message"] = "cancelled", "Gave up waiting for the controller to be ARMED."
        elif st == "armed":
            if word == "FIRING":
                r["state"], r["message"] = "running", "The shot is running."
            elif word in ("IDLE", "FAULT"):
                self._collect(word)
        elif st == "running":
            if word != "FIRING":
                self._collect(word)

    # ---- reading it back
    def _collect(self, end_word):
        r = self.rec
        r["state"], r["message"], r["progress"] = "collecting", "The shot is over. Reading the data from the controller...", 0.0
        try:
            product = self._product()
            raw, rate, count_note = ({}, None, []) if not r["channels"] else self._fetch(product, r)
            pfm = self._fetch_pfm(r) if r.get("pfm") else None
        except Exception as e:                                            # a lost link, a refused command, ...
            r["state"], r["message"] = "failed", f"Could not read the data back: {e}"
            self._restore_modes(r.get("pfm"))
            return
        restored = self._restore_modes(r.get("pfm"))
        log_has = any(len(v[0]) for v in raw.values())
        pfm_has = bool(pfm and any(i["t_s"] for i in pfm["inputs"]))
        if not log_has and not pfm_has:
            hint = " An open-loop HBRIDGE shot is not logged: close a loop first." if (product == "hbridge" and r["channels"]) else ""
            what = "The controller's log and the PFM capture are" if (r["channels"] and pfm) else "The PFM capture is" if pfm else "The controller's log is"
            r["state"], r["message"] = "empty", f"{what} empty: the shot did not run, or was disarmed before it started.{hint}"
            return
        notes = list(r["warnings"])
        notes += count_note
        if r["channels"] and not log_has:
            notes.append("The waveform log is empty (HBRIDGE logs only a closed-loop run); the PFM capture was recorded.")
            raw = {}
        if not restored:
            notes.append("The PFM capture modes were left as set (they can only be changed while the controller is IDLE).")
        c = self.conn
        ident = c.ident or {}
        outcome = "completed"
        st_text = c.state_text or ""
        if end_word == "FAULT" or st_text.startswith("FAULT"):
            outcome = "fault"
            notes.append(f"The shot ended in a fault ({st_text or 'FAULT'}): the log stops where the fault stopped the loop (a ramp-down, if any, is included).")
        controller = {"name": c.spec.get("name") or c.target(), "serial": ident.get("serial"), "label": ident.get("label"), "board": ident.get("board"),
                      "fw": ident.get("fw"), "commit": ident.get("commit"), "uid": ident.get("uid"), "connection": c.mode}
        mode, calib = None, {}
        if product == "hbridge":
            mode = _ok_text(self._safe("CONFig:MODE?")) or None
            lm = _ok_text(self._safe("PID:LOOPMODE? 0"))
            if lm:
                r["meta"] = {**r["meta"], "loop_modes_U_V_W": lm}
        else:
            t, m = _ok_text(self._safe("CONFig:TURNONHz?")), _ok_text(self._safe("CONFig:MAXFREQHz?"))
            amps = {str(n): _ok_text(self._safe(f"CONFig:MAXCURRent? {n}")) for n in sorted(raw)}
            try:
                calib = {"turnon_hz": float(t), "max_hz": float(m), "max_amps": {k: float(v) for k, v in amps.items()}}
            except (TypeError, ValueError):
                calib = {}
        rec = sd.build_recording(product, controller, outcome, rate, r["decim"] or 0, raw, calib=calib, mode=mode, meta=r["meta"], notes=notes, pfm=pfm)
        try:
            sid = sd.save(self.shots_dir, rec)
        except OSError as e:
            r["state"], r["message"] = "failed", f"The data was read but could not be saved: {e}"
            return
        r["sid"], r["state"], r["progress"] = sid, "done", 1.0
        parts = ([f"{rec['count']} samples of the waveform log ({rec['duration_s']:.3g} s)"] if rec["channels"] else []) + \
                ([f"PFM capture of input{'s' if len(pfm['inputs']) > 1 else ''} {', '.join(str(i['n']) for i in pfm['inputs'])}"] if pfm_has else [])
        r["message"] = f"Recorded {' and '.join(parts)}. Saved as {sid}."
        self.conn.events.add("note", r["message"])

    def _fetch_pfm(self, r):
        """Read every chosen input back and return the recording's pfm section."""
        req = r["pfm"]
        modes = (_ok_text(self._safe("PFMIN:EDGes?")) or "").split()
        notes, out = [], []
        inputs = req["inputs"]
        for k, n in enumerate(inputs):
            letter = modes[n - 1] if n - 1 < len(modes) else ("B" if req["edges"] == "both" else "R")
            mode = "both" if letter == "B" else "rising"
            if mode != req["edges"]:
                notes.append(f"{pc.input_name(n)} is in {mode}-edge mode on the controller, not {req['edges']} as asked: read as {mode}.")
            r["message"] = f"Reading PFM input {n} ({k + 1} of {len(inputs)})..."
            got = self._pfm_pull(n, mode, r, k, len(inputs))
            if got is None:
                notes.append(f"{pc.input_name(n)}: nothing captured (not wired to a running output, or the shot did not reach it).")
                continue
            overcap, level, data = got
            inp = pc.build_input(n, mode, overcap, data, level)
            out.append(inp)
            if not inp["t_s"]:
                notes.append(f"{inp['name']}: the capture holds no complete cycle.")
            if overcap:
                notes.append(f"{inp['name']}: {overcap} captures were missed (OVERCAP): some cycles are absent from the record.")
            if req.get("cycles") and len(inp["t_s"]) >= req["cycles"] - 1:
                notes.append(f"{inp['name']}: the capture filled ({len(inp['t_s'])} cycles) before the shot ended, or just at its end: the tail may be missing.")
        points = self._read_points()
        if not points:
            notes.append("The programmed shot could not be read back (none loaded, or more than %d setpoints), so there is no programmed overlay." % MAX_POINTS)
        elif any(i["duty_pct"] is not None for i in out):
            notes.append("Programmed overlay: input n is compared with phase U, V, W for n = 1, 2, 3 (the bench wiring). The measured duty is the "
                         "high time of the signal at the PFM input, so a constant offset from the programmed duty is expected (dead time, receiver).")
        return {"clk_hz": pc.CLK_HZ, "requested": {"inputs": inputs, "edges": req["edges"], "cycles": req.get("cycles")},
                "inputs": out, "programmed": points or None, "notes": notes}

    def _pfm_pull(self, n, mode, r, k, total_inputs):
        """All values of one input: (overcap, level, [values]) or None when it captured nothing. Pages are checked and asked again."""
        cmd = "PFMIN:DATA?" if mode == "rising" else "PFMIN:RAW?"
        values, total, overcap, level = [], None, 0, 0
        while total is None or len(values) < total:
            start, last = len(values), None
            for _ in range(PFM_TRIES):
                reply = self._x(f"{cmd} {n} {start} {pc.PAGE}")
                if reply.startswith("ERR"):
                    raise RecordError(f"PFM input {n}: {reply}")
                try:
                    if mode == "rising":
                        tot, oc, nums = pc.parse_data_page(reply, start)
                        lv = 0
                    else:
                        tot, oc, lv, nums = pc.parse_raw_page(reply, start)
                    break
                except ValueError as e:
                    last = e
            else:
                raise RecordError(f"PFM input {n} from value {start}: {last}")
            total, overcap, level = tot, oc, lv
            if total == 0:
                return None
            if total > 20000:
                raise RecordError(f"PFM input {n}: the controller reports {total} values, more than expected.")
            values += nums
            r["progress"] = (k + len(values) / total) / total_inputs
        return overcap, level, values

    def _fetch(self, product, r):
        raw, rate, notes = {}, None, []
        chans = r["channels"]
        for k, n in enumerate(chans):
            r["message"] = f"Reading {sd.channel_name(product, n)} ({k + 1} of {len(chans)})..."
            if product == "xrex":
                cnt, rt, series = self._xrex_channel(n)
            else:
                cnt, rt, series = self._hbridge_channel(n, r, k, len(chans))
            raw[n], rate = series, rt
            r["progress"] = (k + 1) / len(chans)
            if cnt == 0:
                notes.append(f"{sd.channel_name(product, n)}: the controller logged nothing.")
        return raw, rate, notes

    def _xrex_channel(self, n):
        last = None
        for _ in range(TRIES):
            reply = self._x(f"LOG:DATA? {n}")
            if reply.startswith("ERR"):
                raise RecordError(reply)
            try:
                return sd.parse_xrex_log(reply)
            except ValueError as e:                                   # a character lost on the link: ask again
                last = e
        raise RecordError(f"{sd.channel_name('xrex', n)}: {last}")

    def _hbridge_channel(self, n, r, k, total):
        out, start, count, rate = [[], [], []], 0, None, None
        while count is None or start < count:
            for attempt in range(TRIES):
                reply = self._x(f"LOG:DATA? {n} {start} {PAGE}")
                if reply.startswith("ERR"):
                    raise RecordError(reply)
                try:
                    c, rt, page = sd.parse_hbridge_page(reply, start, PAGE)
                    break
                except ValueError as e:
                    if attempt == TRIES - 1:
                        raise RecordError(f"{sd.channel_name('hbridge', n)} from sample {start}: {e}")
            count, rate = c, rt
            for i in range(3):
                out[i] += page[i]
            start += PAGE
            if count:
                r["progress"] = (k + min(1.0, start / count)) / total
            if count == 0:
                break
        return count, rate, out

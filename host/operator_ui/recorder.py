"""recorder.py -- collects the controller's waveform log around one shot and keeps it (see shot_data.py for what the data is).

The operator asks for a recording while the controller is ARMED (the page does it when ARM is pressed with "record data" ticked).
This module then
  1. arms the controller's own log: `LOG:ARM <ch|0> <samples> <decim>` with a decimation chosen so the expected shot length fits;
  2. watches the controller's state (the connection's heartbeat polls it): ARMED -> FIRING -> back out = the shot has happened;
  3. reads the log back (`LOG:DATA?`: one reply per channel on XREX; 40-sample pages with a check sum on HBRIDGE, each retried when a
     character was lost on the link) and builds, saves and reports a recording.
Collection runs on the connection's heartbeat thread and takes the connection's lock one command at a time, so an operator's
commands are not blocked for the whole read.

Honest limits: the log lives in the controller's RAM and is read after the shot, so a shot that ends the log early (a fault) gives a
shorter recording; the controller holds only the latest log; and HBRIDGE logs only during a CLOSED-LOOP run (an open-loop shot is
played from a table and records nothing), which this module reports instead of pretending.
"""
import re
import threading
import time

import shot_data as sd

WAIT_ARM_S = 600.0                 # how long a request may wait for the controller to be ARMED
PAGE = 40                          # HBRIDGE: samples per LOG:DATA? reply (the firmware's own cap)
TRIES = 3
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
                                      "duration_s", "requested_at")}

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
        """Arm the controller's log for the next shot. Returns the status dict; raises RecordError with a sentence for the operator."""
        product = self._product()
        if product is None:
            raise RecordError("This device is not a known WHAM controller, so there is no shot data to record.")
        if self.active():
            raise RecordError("A recording is already waiting for a shot: cancel it first.")
        top = 4 if product == "xrex" else 3
        try:
            chans = sorted({int(x) for x in spec.get("channels", [])})
            dur = float(spec.get("duration_s", 0))
        except (TypeError, ValueError):
            raise RecordError("Choose the channels and the length of the shot.")
        if not chans or any(not 1 <= n <= top for n in chans):
            raise RecordError(f"Choose at least one {'channel' if product == 'xrex' else 'phase'} to record.")
        if not 0 < dur <= 3600:
            raise RecordError("The expected shot length must be more than 0 and at most 3600 seconds.")
        tick = DEFAULT_TICK_HZ[product]
        q = self._q("CONFig:PIDRate?")
        if q and q.split()[0].isdigit() and int(q.split()[0]) > 0:
            tick = int(q.split()[0])
        every = len(chans) > 1
        maxs = XREX_MAX_SAMPLES if product == "xrex" else HB_MAX_SAMPLES["all" if every else "one"]
        decim, rate, note = sd.choose_decimation(dur, tick, maxs)
        warnings = [note] if note else []
        if product == "hbridge":
            lm = _ok_text(self._safe("PID:LOOPMODE? 0"))
            if lm:
                modes = [int(x) for x in lm.split() if x.isdigit()]
                if not any(modes[n - 1] for n in chans if n - 1 < len(modes)):
                    warnings.append("Every selected phase is in open loop. The controller logs only during a closed-loop run, so this shot "
                                    "will record nothing. Close a loop (PID:LOOPMODE <phase> 1, with a target) to record.")
        reply = self._x(f"LOG:ARM {0 if every else chans[0]} {maxs} {decim}", show=True)
        if not reply.startswith("OK"):
            raise RecordError(f"The controller refused to arm its log: {reply}")
        r = {"state": "waiting", "message": "Log armed. Waiting for the controller to be ARMED.", "progress": 0.0, "channels": chans, "decim": decim,
             "rate_hz": rate, "max_samples": maxs, "duration_s": dur, "meta": spec.get("meta") or {}, "warnings": warnings, "tick_hz": tick,
             "requested_at": time.time(), "sid": None}
        self.rec = r
        now = _ok_text(self._safe("STATE?"))               # the heartbeat's copy may be a moment old
        if now:
            self.conn.state_text = now
        self._advance(self._state_word())
        return self.status()

    def _safe(self, cmd):
        try:
            return self._x(cmd)
        except Exception:
            return None

    def cancel(self):
        if self.rec and self.rec["state"] in ("waiting", "armed", "running"):
            self.rec["state"], self.rec["message"] = "cancelled", "Recording cancelled. The controller's log stays armed but nothing will be saved."
        return self.status()

    def collect_now(self, spec=None):
        """Read whatever the controller's log holds right now (the shot may have run without this page noticing)."""
        if self.rec is None or self.rec["state"] in ("done", "empty", "failed", "cancelled"):
            product = self._product()
            if product is None:
                raise RecordError("This device is not a known WHAM controller.")
            spec = spec or {}
            top = 4 if product == "xrex" else 3
            chans = sorted({int(x) for x in spec.get("channels", range(1, top + 1))})
            self.rec = {"state": "armed", "message": "", "progress": 0.0, "channels": chans, "decim": None, "rate_hz": None, "max_samples": None,
                        "duration_s": spec.get("duration_s"), "meta": spec.get("meta") or {}, "warnings": [], "tick_hz": DEFAULT_TICK_HZ[product],
                        "requested_at": time.time(), "sid": None}
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
                r["state"], r["message"] = "armed", "Armed with the log running. Waiting for the shot to start."
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
            raw, rate, count_note = self._fetch(product, r)
        except Exception as e:                                            # a lost link, a refused command, ...
            r["state"], r["message"] = "failed", f"Could not read the data back: {e}"
            return
        if not any(len(v[0]) for v in raw.values()):
            hint = " An open-loop HBRIDGE shot is not logged: close a loop first." if product == "hbridge" else ""
            r["state"], r["message"] = "empty", f"The controller's log is empty: the shot did not run, or was disarmed before it started.{hint}"
            return
        notes = list(r["warnings"])
        notes += count_note
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
        rec = sd.build_recording(product, controller, outcome, rate, r["decim"] or 0, raw, calib=calib, mode=mode, meta=r["meta"], notes=notes)
        try:
            sid = sd.save(self.shots_dir, rec)
        except OSError as e:
            r["state"], r["message"] = "failed", f"The data was read but could not be saved: {e}"
            return
        r["sid"], r["state"], r["progress"] = sid, "done", 1.0
        r["message"] = f"Recorded {rec['count']} samples ({rec['duration_s']:.3g} s). Saved as {sid}."
        self.conn.events.add("note", r["message"])

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

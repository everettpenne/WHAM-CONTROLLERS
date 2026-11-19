"""gen_command_flow.py -- draws the XREX command flowchart: docs/xrex_command_flow.{html,svg,md}, and the operator
console's copy (host/operator_ui/static/help/). Run it after changing the chart:

    python3 python/gen_command_flow.py

The drawing primitives and the page are in command_flow_kit.py (shared with firmware/hbridge). Every command and reply on
the chart is from docs/labview_commands.md, docs/labview_state_and_faults.md and firmware/xrex/docs/command_reference.md:
when one of those changes, change the chart here.
"""
import os
from command_flow_kit import *      # noqa: F401,F403  (the kit is this script's drawing vocabulary)

# ---- CONNECT
top = 12; y = top + 34
c1 = box(X0, y, SW, [('t', 'Open TCP to the hub, port 5101'), ('m', '@quiet <program-name>'), ('m', '*IDN?'), ('m', 'SYS:ID?')])
s0 = box(SX, y, SWD, [('t', 'Check who answered'), ('n', 'Product WHAM-XREX-PFMG474, serial WB-0003-4.'), ('n', 'Anything else: stop.')])
arrow(f'M{c1.r},{s0.cy} H{s0.x}'); T((c1.r + s0.x) // 2, s0.cy - 6, 'verify', 'el', 'middle')
d0 = diamond(CX, c1.b + 26 + 33, 'STATE?')
arrow(f'M{CX},{c1.b} V{d0.y}')
ls = [('t', 'Anything but IDLE'), ('n', 'OK ARMED or OK FIRING: another session'), ('n', 'is using it. Stop and look.'), ('n', 'OK FAULT <type>: take the fault path.')]
s1 = box(SX, d0.cy - height(ls) // 2, SWD, ls)
arrow(f'M{d0.r},{d0.cy} H{s1.x}')
bot = max(d0.b, s1.b) + 14
band(top, bot, 'CONNECT', 'one connection per program', 'bc')
stub_ys = [s1.cy]
line(f'M{s1.r},{s1.cy} H{BUS}', 'fl')

# ---- IDLE
top = bot + 22; y = top + 34
idle_top = top
arrow(f'M{CX},{d0.b} V{y}')
T(CX + 8, bot + 15, 'OK IDLE', 'el')
b0 = box(X0, y, SW, [('t', 'Optional: start from a saved profile'), ('m', 'CONFig:PROFile:DIFF? <slot>   *RCL <slot>'), ('n', 'IDLE only. Check FAILED=0. Enables and ready lines are not kept.')])
y = b0.b + GAP
arrow(f'M{CX},{b0.b} V{y}')
b1 = box(X0, y, SW, [('t', 'Choose channels'), ('m', 'SOURce:ENAble <ch> 1', True), ('n', 'Channels not in use: SOURce:ENAble <ch> 0')])
w1 = box(SX, y - 6, SWD, [('t', 'Fault pins are polled once enabled'), ('n', 'A pin already at fault level trips it within'), ('n', 'about a millisecond. Images built before'), ('n', '2026-11-09 missed this (seen on XR1).')], 'warn')
line(f'M{b1.r},{b1.cy} H{w1.x}', 'wl')
b2 = box(X0, b1.b + GAP, SW, [('t', 'Ready lines, each channel in use'), ('m', 'XREX:CHANnel:ENAOut <ch> 1', True), ('m', 'XREX:CHANnel:CONTactOut <ch> 1', True)])
b3 = box(X0, b2.b + GAP, SW, [('t', 'Control, each channel'), ('m', 'PID:LOOPMODE <ch> <0|1>'), ('m', 'PID:GAINS <ch> <kp> <ki> <kd>'), ('n', 'Loop mode 1 = closed loop, 0 = open loop')])
b4 = box(X0, b3.b + GAP, SW, [('t', 'Shot trapezoid'), ('m', 'SHOT:TIMing <up> <flat> <down>'), ('m', 'SHOT:CURRent <ch> <amps>'), ('n', 'Seconds, shared by all channels. Must be set after every reset.')])
b5 = box(X0, b4.b + GAP, SW, [('t', 'Read back, and arm the log if wanted'), ('m', 'SHOT:TIMing?   SHOT:CURRent? <ch>'), ('m', 'LOG:ARM <ch|0> <maxSamples> <decim>'), ('n', 'ch 0 = all channels together, up to 1750 samples')])
for a, b in ((b1, b2), (b2, b3), (b3, b4), (b4, b5)):
    arrow(f'M{CX},{a.b} V{b.y}')
d1 = diamond(CX, b5.b + 24 + 33, 'ARM', dang=True)
arrow(f'M{CX},{b5.b} V{d1.y}')
ls = [('t', 'ARM refused'), ('m', 'ERR 15  PF13 interlock LOW'), ('m', 'ERR 13  Channel <n> lines not set'), ('m', 'ERR 13  not currently IDLE'), ('n', 'Only the first failed check is reported.')]
s2 = box(SX, d1.cy - height(ls) + 24, SWD, ls)
arrow(f'M{d1.r},{d1.cy} H{s2.x}'); T(d1.r + 8, d1.cy - 6, 'ERR', 'el')
ry = s2.y + 26
arrow(f'M{s2.x},{ry} H504 V{b2.cy} H{b2.r}')
T(518, (ry + b2.cy) // 2, 'fix it, then ARM again', 'el', 'middle', True)
bot = max(d1.b, s2.b) + 14
band(top, bot, 'IDLE', 'configure here, output is off', 'bi')
stub_ys.append((top + bot) // 2); line(f'M{BR},{stub_ys[-1]} H{BUS}', 'fl')

# ---- ARMED
top = bot + 22; y = top + 34
st = box(240, y, 238, [('m', 'STATE?'), ('n', 'expect OK ARMED')])
di = box(X0, y, 112, [('m', 'DISARM'), ('n', 'stand down')])
arrow(f'M{CX},{d1.b} V{y}'); T(CX + 8, bot + 15, 'OK', 'el')
arrow(f'M{st.x},{st.cy} H{di.r}'); T((st.x + di.r) // 2, st.cy - 6, 'no shot', 'el', 'middle')
line(f'M{di.x},{di.cy} H{RAIL}'); dot(RAIL, di.cy)
T(RAIL + 8, di.cy - 6, 'IDLE', 'el')
y2 = st.b + 36
xs = (78, 215, 352); bw = 126
a1 = box(xs[0], y2, bw, [('m', 'SHOT:STARt', True), ('n', 'profiled shot,'), ('n', 'ends by itself')])
a2 = box(xs[1], y2, bw, [('m', 'SOURce:RUN', True), ('n', 'continuous, runs'), ('n', 'until SOURce:STOP')])
a3 = box(xs[2], y2, bw, [('t', 'PF15 rising edge'), ('n', 'hardware trigger,'), ('n', 'on by default')])
yf = st.b + 16
line(f'M{CX},{st.b} V{yf}'); line(f'M{a1.cx},{yf} H{a3.cx}')
for a in (a1, a2, a3):
    arrow(f'M{a.cx},{yf} V{a.y}')
yj = a1.b + 16
for a in (a1, a2, a3):
    line(f'M{a.cx},{a.b} V{yj}')
line(f'M{a1.cx},{yj} H{a3.cx}')
d2 = diamond(CX, yj + 18 + 33, 'Accepted?', 'nb')
arrow(f'M{CX},{yj} V{d2.y}')
ls = [('t', 'Start refused, state stays ARMED'), ('m', 'ERR 13  Must ARM first'), ('m', 'ERR 15  PF13 LOW at firing'), ('m', 'ERR 12  SHOT:TIMing not set'), ('n', 'A refused PF15 edge gives no reply.')]
s3 = box(SX, d2.cy - height(ls) // 2, SWD, ls)
arrow(f'M{d2.r},{d2.cy} H{s3.x}'); T(d2.r + 8, d2.cy - 6, 'no', 'el')
bot = max(d2.b, s3.b) + 14
band(top, bot, 'ARMED', 'nothing electrical yet', 'ba')
stub_ys.append((top + bot) // 2); line(f'M{BR},{stub_ys[-1]} H{BUS}', 'fl')

# ---- FIRING
top = bot + 22; y = top + 34
po = box(128, y, 300, [('t', 'Poll a few times a second'), ('m', 'STATE?'), ('m', 'SOURce:STATus? <ch>'), ('n', 'running, setpoint, measured, output (Hz)')])
arrow(f'M{CX},{d2.b} V{y}'); T(CX + 8, bot + 15, 'OK', 'el')
arrow(f'M{po.x},{po.cy+12} H104 V{po.cy-12} H{po.x}')
T(92, po.cy + 4, 'OK FIRING', 'el', 'middle', True)
ab = box(SX, po.cy - 25, SWD, [('m', 'SOURce:STOP'), ('n', 'Abort. Stops everything, back to IDLE.')])
arrow(f'M{po.r},{ab.cy} H{ab.x}'); T((po.r + ab.x) // 2, ab.cy - 6, 'abort', 'el', 'middle')
bot = max(po.b, ab.b) + 14
band(top, bot, 'FIRING', 'output is on', 'bf')
stub_ys.append((top + bot) // 2); line(f'M{BR},{stub_ys[-1]} H{BUS}', 'fl')

# ---- AFTER
top = bot + 22; y = top + 34
lg = box(X0, y, SW, [('t', 'Read the log, one channel at a time'), ('m', 'LOG:DATA? <ch>'), ('n', 'One long line: OK <count> <rateHz>, then setpoint, measured,'), ('n', 'output in Hz per sample. Allow a read timeout of 40 s or more.')])
arrow(f'M{CX},{po.b} V{y}'); T(CX + 8, bot + 15, 'OK IDLE, the shot ended by itself', 'el')
arrow(f'M{ab.cx},{ab.b} V{lg.cy} H{lg.r}')
line(f'M{lg.x},{lg.cy} H{RAIL}'); dot(RAIL, lg.cy)
T(RAIL + 8, lg.cy - 6, 'IDLE', 'el')
bot = lg.b + 14
band(top, bot, 'IDLE', 'shot over, output is off', 'bi')
after_bot = bot

# ---- FAULT column
y = 12
f1 = box(FX, y, FW, [('t', 'FAULT latched, from any state'), ('n', 'General or interlock fault while firing: all'), ('n', 'channels ramp down over the ramp time (1.0 s).'), ('n', 'One-channel fault while firing: that channel'), ('n', 'off at once, the others derate, then ramp down.')], 'fault')
line(f'M{BUS},{stub_ys[-1]} V{f1.cy}', 'fl'); arrow(f'M{BUS},{f1.cy} H{FX}', 'fl', 'af')
T(889, 330, 'a fault can arrive in any state', 'el fe', 'middle', True)
f2 = box(FX, f1.b + GAP, FW, [('t', 'Read the type, then the cause'), ('m', 'STATE?'),
    ('n', 'OK FAULT <type> [ch] ACTIVE=WATER1,OCP3'), ('n', 'ACTIVE= lists every fault measured.'),
    ('n', 'OK FAULT GENERAL (water, temp, hardware input)'), ('m', 'XREX:CHANnel:STATus? <ch>   GDS?'),
    ('n', 'OK FAULT OVERCURRENT <ch> or ENERPRO <ch>'), ('m', 'XREX:CHANnel:STATus? <ch>'),
    ('n', 'OK FAULT ENABLE_OUTPUT <ch>'), ('m', 'XREX:CHANnel:ENAOut? <ch>'), ('m', 'XREX:CHANnel:CONTactOut? <ch>'),
    ('n', 'OK FAULT EXTERNAL_ENABLE'), ('m', 'EXTernal:INPut?')])
f3 = box(FX, f2.b + GAP, FW, [('t', 'Wait out the ramp'), ('m', 'SOURce:STATus? <ch>'), ('n', 'First field (running) must be 0.')])
f4 = box(FX, f3.b + GAP, FW, [('t', 'A person finds and removes the cause'), ('m', 'SYS:EVLOG?'), ('n', 'Event order and times since boot. (Live !EVT'), ('n', 'lines are off unless SYS:EVENT 1 was sent.)'), ('n', 'Never auto-clear a fault that hit while firing.')])
f5 = box(FX, f4.b + GAP, FW, [('m', 'FAULT:CLEar', True), ('n', 'Replies OK whether or not it cleared.')])
for a, b in ((f1, f2), (f2, f3), (f3, f4), (f4, f5)):
    arrow(f'M{FCX},{a.b} V{b.y}')
d3 = diamond(FCX, f5.b + 24 + 33, 'STATE?')
arrow(f'M{FCX},{f5.b} V{d3.y}')
arrow(f'M{d3.r},{d3.cy} H1212 V{f4.cy} H{f4.r}')
T(1224, (d3.cy + f4.cy) // 2, 'OK FAULT: still latched', 'el', 'middle', True)
f6 = box(FX, d3.b + 28, FW, [('t', 'Re-check what the fault dropped'), ('m', 'SOURce:ENAble? <ch>'), ('m', 'XREX:CHANnel:ENAOut? <ch>'), ('m', 'XREX:CHANnel:CONTactOut? <ch>')])
arrow(f'M{FCX},{d3.b} V{f6.y}'); T(FCX + 8, d3.b + 18, 'OK IDLE', 'el')
HB = after_bot + 16
line(f'M{f6.x},{f6.cy} H{RET} V{HB} H{RAIL}')
arrow(f'M{RAIL},{HB} V{b0.cy} H{b0.x}')
T(26, (HB + b0.cy) // 2, 'Back to IDLE. Settings stay until a reset. Every shot needs its own ARM.', 'el', 'middle', True)
ls = [('t', 'Queries work in every state'), ('m', 'STATE?   FAULT?   GDS?'), ('m', 'XREX:CHANnel:STATus? <ch>'), ('m', 'SOURce:STATus? <ch>'), ('m', 'EXTernal:INPut?   SYS:EVLOG?')]
q = box(FX, after_bot - height(ls), FW, ls, 'plain')
assert q.y > f6.b + 10, (q.y, f6.b)
H = max(HB + 12, q.b + 12)


finish(
    name='xrex_command_flow', docs=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs'), H=H,
    title='XREX command flow',
    lede='The serial commands for one Transrex shot, in the order the controller accepts them, and the path out of a fault. Check the reply to each command before sending the next.',
    key=['<i class="k-d"></i>The hub logs and announces this as a dangerous command', '<i class="k-f"></i>Fault path', '<code>&lt;ch&gt;</code> is the channel, 1 to 4'],
    key_md='a filled orange dot marks a command the hub logs and announces as dangerous; a dashed line is the fault path; `<ch>` is the channel, 1 to 4.',
    caption='Each band is an operating state. Work moves down the bands, a fault from any band moves right, and every finish returns through the rail on the left to IDLE.',
    notes=[
        ('When fault pins are read', 'Water, temperature, Enerpro and overcurrent pins are all polled continuously (water, temperature and Enerpro since firmware built 2026-11-09; older images read them only on a pin change, at boot and in <code>FAULT:CLEar</code>). A disabled channel\'s pins are ignored. <code>FAULT:CLEar</code> in IDLE or ARMED does nothing. In FAULT, <code>STATE?</code> ends with <code>ACTIVE=</code>, the list of every fault measured.'),
        ('Commands that act outside IDLE', '<code>SOURce:ENAble</code>, <code>XREX:CHANnel:ENAOut</code> and <code>CONTactOut</code> take effect immediately in any state. While ARMED or FIRING, an enabled channel whose ready lines are not both HIGH faults, so set the lines before enabling.'),
        ('A reset forgets the setup', 'A <code>!BOOT</code> line can arrive at any time. Enables, gains and shot timing are gone: nothing is recalled at boot. Re-identify, tell the operator, and set up again or recall a saved profile (<code>*RCL &lt;slot&gt;</code>, eight slots, IDLE only; <code>CONFig:PROFile:DIFF? &lt;slot&gt;</code> shows first what it would change). Do not resend the old setup silently.'),
        ('Saved profiles and the dead time', 'A profile keeps the control rates and calibration, the fault polarities, the shot timing and, per channel, the gains, loop mode and currents. It does not keep channel enables, the ready lines or the external-enable setting. <code>CONFig:DEADtime &lt;ns&gt;</code> (50 to 3000, 100 at every boot) is announced as dangerous and is not in a profile on XREX.'),
        ('Events', 'The live <code>!EVT</code> lines are off at every boot since 2026-11-09 (<code>SYS:EVENT 1</code> turns them on for this boot). A program that polls <code>STATE?</code> needs nothing else; <code>SYS:EVLOG?</code> has the history either way.'),
        ('Not on this chart', '<code>DEBUG:FAULT:BYPASS 1</code> makes the controller ignore its fault detection. It is for a bench with nothing connected and is not a recovery step.'),
    ],
    sources='Sources: <code>docs/labview_commands.md</code>, <code>docs/labview_state_and_faults.md</code>, <code>firmware/xrex/docs/command_reference.md</code>, and the XREX firmware in WHAM-CONTROLLERS. Amps are a placeholder calibration until measured on a real Transrex.',
    aria='Flowchart of XREX serial commands. Connect and identify, confirm IDLE, configure channels, ready lines, control and shot settings (or recall a saved profile), ARM, start a shot, poll while firing, read the log, and return to IDLE. A fault from any state leads to reading its type, removing the cause, FAULT:CLEar, and a STATE? check before returning to IDLE.')

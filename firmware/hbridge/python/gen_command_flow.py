"""gen_command_flow.py -- draws the HBRIDGE command flowchart (Limiter and ECH-HVPS controllers):
docs/hbridge_command_flow.{html,svg,md}, and the operator console's copy (host/operator_ui/static/help/). Run it
after changing the chart:

    python3 python/gen_command_flow.py

The drawing primitives and the page are in command_flow_kit.py (shared with firmware/xrex). Every command and reply on
the chart is from docs/labview_commands.md (section 4 and 6), docs/labview_state_and_faults.md and
firmware/hbridge/docs/command_reference.md: when one of those changes, change the chart here.
"""
import os
from command_flow_kit import *      # noqa: F401,F403  (the kit is this script's drawing vocabulary)

# ---- CONNECT
top = 12; y = top + 34
c1 = box(X0, y, SW, [('t', 'Open TCP to the hub: port 5102 Limiter, 5103 ECH-HVPS'), ('m', '@quiet <program-name>'), ('m', '*IDN?'), ('m', 'SYS:ID?')])
s0 = box(SX, y, SWD, [('t', 'Check who answered'), ('n', 'Product WHAM-HBRIDGE-PFMG474. Serial'), ('n', 'WB-0002-6 Limiter, WB-0001-8 ECH-HVPS.'), ('n', 'Anything else: stop.')])
arrow(f'M{c1.r},{s0.cy} H{s0.x}'); T((c1.r + s0.x) // 2, s0.cy - 6, 'verify', 'el', 'middle')
d0 = diamond(CX, max(c1.b, s0.b) + 26 + 33, 'STATE?')
arrow(f'M{CX},{c1.b} V{d0.y}')
ls = [('t', 'Anything but IDLE'), ('n', 'OK ARMED or OK FIRING: another session'), ('n', 'is using it. Stop and look.'), ('n', 'OK FAULT GENERAL: take the fault path.')]
s1 = box(SX, d0.cy - height(ls) // 2 + 8, SWD, ls)
arrow(f'M{d0.r},{d0.cy} H{s1.x}')
bot = max(d0.b, s1.b) + 14
band(top, bot, 'CONNECT', 'one connection per program', 'bc')
stub_ys = [s1.cy]
line(f'M{s1.r},{s1.cy} H{BUS}', 'fl')

# ---- IDLE
top = bot + 22; y = top + 34
arrow(f'M{CX},{d0.b} V{y}')
T(CX + 8, bot + 15, 'OK IDLE', 'el')
b0 = box(X0, y, SW, [('t', 'Optional: start from a saved profile'), ('m', 'CONFig:PROFile:DIFF? <slot>   *RCL <slot>'), ('n', 'IDLE only. Then go on from "Read it back": a profile holds'), ('n', 'the supply, the phases, the shot and the fault-input settings.')])
ls = [('t', 'A recall can switch protections off'), ('n', 'Reply: OK APPLIED=n FAILED=k [GDSOFF=j]'), ('n', '[SPARKOFF=1]. FAILED must be 0. GDSOFF and'), ('n', 'SPARKOFF count fault inputs now switched off.'), ('n', 'It can also change phases and dead time.')]
w0 = box(SX, b0.cy - height(ls) // 2, SWD, ls, 'warn')
line(f'M{b0.r},{b0.cy} H{w0.x}', 'wl')
b1 = box(X0, max(b0.b, w0.b - 10) + GAP, SW, [('t', 'Choose the supply'), ('m', 'CONFig:MODE LIMiter|HVPS'), ('m', 'CONFig:LIMits?'), ('n', 'NONE after every reset. A change discards the uploaded shot.')])
ls = [('t', 'The limits the shot is checked against'), ('n', 'LIMITER  75-125 kHz, duty 5-95 %'), ('n', 'HVPS     10-30 kHz, duty 40-60 %'), ('n', 'Use what CONFig:LIMits? reports.')]
s4 = box(SX, b1.cy - height(ls) // 2, SWD, ls, 'plain')
b2 = box(X0, b1.b + GAP, SW, [('t', 'Phases that drive'), ('m', 'PHASe:SELect <UVW>', True), ('m', 'PHASe:ENAble?'), ('n', 'A phase that is off is never connected to its output pins.')])
b3 = box(X0, b2.b + GAP, SW, [('t', 'Fault inputs: look before every session'), ('m', 'FAULT:SOURces?   DEBUG:FAULT:BYPASS?'), ('m', 'GDS?   GDS:ENAble?   GDS:POLarity?'), ('m', 'SPARKgap:INPut?   SPARKgap:ENAble?'), ('n', 'Expect every input on and the bypass 0, unless agreed otherwise.')])
ls = [('t', 'Switching a protection off'), ('m', 'GDS:ENAble <input> 0', True), ('m', 'SPARKgap:ENAble 0', True), ('m', 'GDS:POLarity <word>', True), ('n', 'IDLE or FAULT only. On again at every reset.'), ('n', 'An unwired input can read as a fault.')]
s5 = box(SX, b3.cy - height(ls) // 2, SWD, ls)
line(f'M{b3.r},{b3.cy} H{s5.x}', 'wl')
b4 = box(X0, max(b3.b, s5.b - 30) + GAP, SW, [('t', 'Upload the shot: one line, one reply'), ('m', 'SHOT:BEGin'), ('m', 'SHOT:STEP <t_us> <hz> <duty> [<dutyV> <dutyW>]'), ('m', 'SHOT:END'), ('n', 'First at t = 0, times increasing, at most 500 setpoints and 1 s.'), ('n', 'SHOT:STEP <t_us> 0 0 = output off until the next setpoint.')])
ls = [('t', 'A setpoint is refused'), ('m', 'ERR 10  outside the limits'), ('m', 'ERR 4   order, format, or a gap rule'), ('m', 'ERR 13  no supply chosen, or FIRING'), ('n', 'Gap rules: the first setpoint and the last'), ('n', 'two are on. Start again from SHOT:BEGin.')]
s6 = box(SX, b4.cy - height(ls) // 2 + 14, SWD, ls)
arrow(f'M{b4.r},{b4.cy} H{s6.x}'); T((b4.r + s6.x) // 2, b4.cy - 6, 'ERR', 'el', 'middle')
b5 = box(X0, max(b4.b, s6.b - 40) + GAP, SW, [('t', 'Read it back'), ('m', 'SHOT?'), ('m', 'SHOT:POINt? <i>'), ('n', 'OK <setpoints> <duration_us> PROFILE. Compare every setpoint'), ('n', 'with what was meant: this, not the OK replies, is the proof.')])
for a, b in ((b0, b1), (b1, b2), (b2, b3), (b3, b4), (b4, b5)):
    arrow(f'M{CX},{a.b} V{b.y}')
d1 = diamond(CX, b5.b + 24 + 33, 'ARM', dang=True)
arrow(f'M{CX},{b5.b} V{d1.y}')
ls = [('t', 'ARM refused'), ('m', 'ERR 6   a fault is latched'), ('m', 'ERR 13  not IDLE, or no phase is on'), ('n', 'ARM does not look for a shot: FIRE does.'), ('n', 'ARM prepares the start, so the output'), ('n', 'begins tens of microseconds after FIRE.')]
s2 = box(SX, d1.cy - height(ls) + 30, SWD, ls)
arrow(f'M{d1.r},{d1.cy} H{s2.x}'); T(d1.r + 8, d1.cy - 6, 'ERR', 'el')
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
a1 = box(xs[0], y2, bw, [('m', 'FIRE', True), ('n', 'plays the shot,'), ('n', 'ends by itself')])
a2 = box(xs[1], y2, bw, [('t', 'PF15 rising edge'), ('n', 'hardware trigger,'), ('n', 'on by default')])
a3 = box(xs[2], y2, bw, [('m', 'SOURce:RUN', True), ('n', 'closed loop only,'), ('n', 'to SOURce:STOP')])
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
ls = [('t', 'Start refused, state stays ARMED'), ('m', 'ERR 6   a fault is latched'), ('m', 'ERR 13  not ARMED'), ('m', 'ERR 5   no shot to play'), ('n', 'A refused PF15 edge gives no reply:'), ('m', 'EXTernal:TRIGger:STATus?')]
s3 = box(SX, d2.cy - height(ls) // 2, SWD, ls)
arrow(f'M{d2.r},{d2.cy} H{s3.x}'); T(d2.r + 8, d2.cy - 6, 'no', 'el')
ls = [('t', 'While ARMED'), ('n', 'An edge on PF15 starts the shot with no'), ('n', 'command from you. EXTernal:TRIGger 0'), ('n', 'switches the trigger input off.')]
s7 = box(SX, st.y - 6, SWD, ls, 'warn')
line(f'M{st.r},{st.cy} H{s7.x}', 'wl')
assert s7.b + 8 <= s3.y, (s7.b, s3.y)
bot = max(d2.b, s3.b) + 14
band(top, bot, 'ARMED', 'nothing electrical yet', 'ba')
stub_ys.append((top + bot) // 2); line(f'M{BR},{stub_ys[-1]} H{BUS}', 'fl')

# ---- FIRING
top = bot + 22; y = top + 34
po = box(128, y, 300, [('t', 'Poll until the shot is over'), ('m', 'STATE?'), ('n', 'A shot lasts its duration_us: 1 s at most.')])
arrow(f'M{CX},{d2.b} V{y}'); T(CX + 8, bot + 15, 'OK', 'el')
arrow(f'M{po.x},{po.cy+12} H104 V{po.cy-12} H{po.x}')
T(92, po.cy + 4, 'OK FIRING', 'el', 'middle', True)
ab = box(SX, po.cy - 25, SWD, [('m', 'SOURce:STOP'), ('n', 'Abort. Stops the output, back to IDLE.')])
arrow(f'M{po.r},{ab.cy} H{ab.x}'); T((po.r + ab.x) // 2, ab.cy - 6, 'abort', 'el', 'middle')
bot = max(po.b, ab.b) + 22
band(top, bot, 'FIRING', 'output is on', 'bf')
stub_ys.append((top + bot) // 2); line(f'M{BR},{stub_ys[-1]} H{BUS}', 'fl')

# ---- AFTER
top = bot + 22; y = top + 34
lg = box(X0, y, SW, [('t', 'After the shot'), ('m', 'STATE?   FAULT?'), ('m', 'EXTernal:TRIGger:LATency?'), ('n', 'Expect OK IDLE and no fault. The shot stays loaded: ARM and'), ('n', 'FIRE repeat it. Compare the measured waveform with the demand'), ('n', 'and correct the next profile (analysis/compensation/README.md).')])
arrow(f'M{CX},{po.b} V{y}'); T(CX + 8, bot + 15, 'OK IDLE, the shot ended by itself', 'el')
arrow(f'M{ab.cx},{ab.b} V{lg.cy} H{lg.r}')
line(f'M{lg.x},{lg.cy} H{RAIL}'); dot(RAIL, lg.cy)
T(RAIL + 8, lg.cy - 6, 'IDLE', 'el')
bot = lg.b + 14
band(top, bot, 'IDLE', 'shot over, output is off', 'bi')
after_bot = bot

# ---- FAULT column
y = 12
f1 = box(FX, y, FW, [('t', 'FAULT latched, from any state'), ('n', 'The output stops at once: there is no ramp.'), ('n', 'One type only: STATE? says OK FAULT GENERAL.'), ('n', 'The CHARGE output keeps running.')], 'fault')
line(f'M{BUS},{stub_ys[-1]} V{f1.cy}', 'fl'); arrow(f'M{BUS},{f1.cy} H{FX}', 'fl', 'af')
T(889, 330, 'a fault can arrive in any state', 'el fe', 'middle', True)
f2 = box(FX, f1.b + GAP, FW, [('t', 'Read the source, then the cause'), ('m', 'FAULT:SOURces?'),
    ('n', 'OK PC10= GATEDRV= TABLE= TEST= OVP= FB='), ('n', 'SPARK=   A 1 names the source.'),
    ('n', 'GATEDRV=1: a gate-driver status input'), ('m', 'GDS:LAST?   GDS?   GDS:NICKname?'),
    ('n', 'SPARK=1: the spark-gap input read LOW'), ('m', 'SPARKgap:INPut?'),
    ('n', 'OVP=1 or FB=1: a closed loop stopped itself'), ('m', 'PID:FAULT?'),
    ('n', 'PC10=1: the hardware fault input tripped'),
    ('n', 'TABLE=1: the shot overran inside the controller')])
f3 = box(FX, f2.b + GAP, FW, [('t', 'A person finds and removes the cause'), ('n', 'Do not switch an input off to get going,'), ('n', 'unless that is agreed and written down.'), ('n', 'Never auto-clear a fault that hit while firing.')])
f4 = box(FX, f3.b + GAP, FW, [('m', 'FAULT:CLEar', True), ('n', 'Replies OK whether or not it cleared.')])
for a, b in ((f1, f2), (f2, f3), (f3, f4)):
    arrow(f'M{FCX},{a.b} V{b.y}')
d3 = diamond(FCX, f4.b + 24 + 33, 'STATE?')
arrow(f'M{FCX},{f4.b} V{d3.y}')
arrow(f'M{d3.r},{d3.cy} H1212 V{f3.cy} H{f3.r}')
T(1224, (d3.cy + f3.cy) // 2, 'OK FAULT: still latched', 'el', 'middle', True)
f5 = box(FX, d3.b + 28, FW, [('t', 'Re-check before arming again'), ('m', 'CONFig:MODE?   PHASe:ENAble?'), ('m', 'SHOT?   GDS:ENAble?'), ('n', 'After a reset (a !BOOT line) the supply, the'), ('n', 'shot and every setting are gone: start again.')])
arrow(f'M{FCX},{d3.b} V{f5.y}'); T(FCX + 8, d3.b + 18, 'OK IDLE', 'el')
HB = after_bot + 16
line(f'M{f5.x},{f5.cy} H{RET} V{HB} H{RAIL}')
arrow(f'M{RAIL},{HB} V{b0.cy} H{b0.x}')
T(26, (HB + b0.cy) // 2, 'Back to IDLE. Settings and the shot stay until a reset. Every shot needs its own ARM.', 'el', 'middle', True)
ls = [('t', 'Queries work in every state'), ('m', 'STATE?   FAULT?   FAULT:SOURces?'), ('m', 'GDS?   SPARKgap:INPut?'), ('m', 'SHOT?   CONFig:LIMits?'), ('m', 'EXTernal:TRIGger:STATus?')]
q = box(FX, after_bot - height(ls), FW, ls, 'plain')
ls = [('t', 'The charger is separate from the state'), ('m', 'CHARGE:FREQuency <hz>', True), ('m', 'CHARGE:STATus?'), ('n', '0 = off, otherwise 1 to 100000 Hz. Works in'), ('n', 'any state and stays on through a fault.'), ('n', 'Off after every reset.')]
ch = box(FX, q.y - GAP - height(ls), FW, ls)
assert ch.y > f5.b + 10, (ch.y, f5.b)
H = max(HB + 12, q.b + 12)

finish(
    name='hbridge_command_flow', docs=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs'), H=H,
    title='HBRIDGE command flow',
    lede='The serial commands for one shot on the Limiter or the ECH-HVPS controller (both run the HBRIDGE firmware), in the order the controller accepts them, and the path out of a fault. Check the reply to each command before sending the next.',
    key=['<i class="k-d"></i>The hub logs and announces this as a dangerous command', '<i class="k-f"></i>Fault path', 'Phases U, V, W are channels 1, 2, 3'],
    key_md='a filled orange dot marks a command the hub logs and announces as dangerous; a dashed line is the fault path; phases U, V, W are channels 1, 2, 3.',
    caption='Each band is an operating state. Work moves down the bands, a fault from any band moves right, and every finish returns through the rail on the left to IDLE.',
    notes=[
        ('The shot is a list of setpoints', 'Time in whole microseconds, frequency in Hz, duty in percent; the controller moves in straight lines between setpoints, one switching period at a time. One duty applies to all three phases, three duties give each phase its own. A setpoint at 0 Hz switches the outputs off until the next setpoint, which restarts them with no ramp; the setpoint before a gap holds its value up to the gap.'),
        ('Load the shot from a file', 'Typing a profile by hand is where shots go wrong. The compensation tools write the profile as a CSV (<code>analysis/compensation/README.md</code>); the operator console loads that file into its shot table, sends it and compares the read-back. A program should do the same: send from the file, then read every <code>SHOT:POINt?</code> back.'),
        ('What a recall can change', '<code>*RCL</code> puts back the supply, the shot and its tables, the phases that drive, the dead time, and the fault-input settings (which <code>GDS:ENAble</code> inputs are on, the polarity, the spark-gap enable). Show <code>CONFig:PROFile:DIFF? &lt;slot&gt;</code> to the operator first, and read <code>PHASe:ENAble?</code> and <code>GDS:ENAble?</code> afterwards. Nothing is recalled at boot.'),
        ('Which input is in fault', '<code>GDS:LAST?</code> replies <code>OK &lt;hex&gt; &lt;checks&gt;</code>: the levels of the twelve gate-driver inputs when the fault latched, bit 0 = input 01. Under <code>NORMALLY_HIGH</code> (the built-in polarity) the faulted inputs are the cleared bits: <code>FFB</code> is input 03. Read the polarity with <code>GDS:POLarity?</code>; do not assume it.'),
        ('A reset forgets the setup', 'A <code>!BOOT</code> line can arrive at any time. The supply mode is <code>NONE</code>, the shot is gone, every fault input is on again, the dead time is 100 ns and the charger is off. Re-identify, tell the operator, and set up again or recall a saved profile.'),
        ('Not on this chart', '<code>DEBUG:FAULT:BYPASS 1</code> makes the controller ignore PC10, the gate-driver inputs and the spark-gap input. It is for a bench with nothing connected and is not a recovery step. <code>CONFig:DEADtime &lt;ns&gt;</code> (50 to 3000, 100 at every boot) changes the pause between the two switches of a phase: too little risks shoot-through. Closed-loop set-up (<code>PID:*</code>, <code>TARGet:*</code>) is in <code>docs/labview_commands.md</code> section 4.4.'),
    ],
    sources='Sources: <code>docs/labview_commands.md</code>, <code>docs/labview_state_and_faults.md</code>, <code>firmware/hbridge/docs/command_reference.md</code>, and the HBRIDGE firmware in WHAM-CONTROLLERS. On the installed ECH-HVPS, nothing is armed or fired without the operator in charge agreeing.',
    aria='Flowchart of HBRIDGE serial commands for the Limiter and ECH-HVPS controllers. Connect and identify, confirm IDLE, optionally recall a saved profile, choose the supply and the phases, check the fault inputs, upload the shot and read it back, ARM, start with FIRE or the PF15 trigger, poll while firing, and return to IDLE. A fault from any state leads to FAULT:SOURces?, finding the input, removing the cause, FAULT:CLEar, and a STATE? check before returning to IDLE. The CHARGE output is separate from the state.')

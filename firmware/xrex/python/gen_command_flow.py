import html
e = html.escape
W = 1228
X0, SW, CX = 78, 400, 278
SX, SWD = 530, 300
BUS, RET, RAIL = 872, 884, 34
FX, FW = 896, 300
FCX = FX + FW // 2
BL, BR = 54, 850
bands, lines, boxes, texts = [], [], [], []
P = {'t': 18, 'm': 18, 'n': 16}
CW = {'t': 6.5, 'm': 7.2, 'n': 5.9}

def height(ls):
    return 12 + sum(P[l[0]] for l in ls) + 8

def T(x, y, s, cls, anchor=None, rot=False):
    a = f' text-anchor="{anchor}"' if anchor else ''
    r = f' transform="rotate(-90 {x} {y})"' if rot else ''
    texts.append(f'<text x="{x}" y="{y}" class="{cls}"{a}{r}>{e(s)}</text>')

class B:
    def __init__(s, x, y, w, h):
        s.x, s.y, s.w, s.h = x, y, w, h
        s.r, s.b, s.cx, s.cy = x + w, y + h, x + w // 2, y + h // 2

def box(x, y, w, ls, cls='box'):
    h = height(ls)
    boxes.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="5" class="{cls}"/>')
    danger = any(len(l) > 2 and l[2] for l in ls)
    cy = y + 10
    for l in ls:
        k, s = l[0], l[1]
        d = len(l) > 2 and l[2]
        ind = 13 if (danger and k == 'm') else 0
        limit = w - 22 - ind
        assert len(s) * CW[k] <= limit, (s, len(s) * CW[k], limit)
        base = cy + {'t': 13, 'm': 13, 'n': 12}[k]
        if d:
            boxes.append(f'<circle cx="{x + 15}" cy="{base - 4}" r="3.6" class="dg"/>')
        T(x + 11 + ind, base, s, {'t': 'tt', 'm': 'mm', 'n': 'nn'}[k])
        cy += P[k]
    return B(x, y, w, h)

def diamond(cx, cy, s, cls='mm', dang=False, w=170, h=66):
    boxes.append(f'<polygon points="{cx},{cy-h//2} {cx+w//2},{cy} {cx},{cy+h//2} {cx-w//2},{cy}" class="box"/>')
    T(cx + (6 if dang else 0), cy + 4, s, cls, 'middle')
    if dang:
        boxes.append(f'<circle cx="{cx-len(s)*3.6-6}" cy="{cy}" r="3.6" class="dg"/>')
    return B(cx - w // 2, cy - h // 2, w, h)

def arrow(d, cls='ln', head='a'):
    lines.append(f'<path d="{d}" class="{cls}" marker-end="url(#{head})"/>')
def line(d, cls='ln'):
    lines.append(f'<path d="{d}" class="{cls}"/>')
def dot(x, y):
    lines.append(f'<circle cx="{x}" cy="{y}" r="2.6" class="jt"/>')
def band(top, bottom, name, sub, cls):
    bands.append(f'<rect x="{BL}" y="{top}" width="{BR-BL}" height="{bottom-top}" rx="8" class="{cls}"/>')
    T(BL + 24, top + 22, name, 'tag ' + cls + 't')
    T(BL + 24 + len(name) * 10.5 + 10, top + 22, sub, 'nn')
GAP = 20

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
f4 = box(FX, f3.b + GAP, FW, [('t', 'A person finds and removes the cause'), ('m', 'SYS:EVLOG?'), ('n', 'Event order and times since boot.'), ('n', 'Never auto-clear a fault that hit while firing.')])
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
arrow(f'M{RAIL},{HB} V{b1.cy} H{b1.x}')
T(26, (HB + b1.cy) // 2, 'Back to IDLE. Settings stay until a reset. Every shot needs its own ARM.', 'el', 'middle', True)
ls = [('t', 'Queries work in every state'), ('m', 'STATE?   FAULT?   GDS?'), ('m', 'XREX:CHANnel:STATus? <ch>'), ('m', 'SOURce:STATus? <ch>'), ('m', 'EXTernal:INPut?   SYS:EVLOG?')]
q = box(FX, after_bot - height(ls), FW, ls, 'plain')
assert q.y > f6.b + 10, (q.y, f6.b)
H = HB + 12

defs = '''<defs>
<marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" class="ah"/></marker>
<marker id="af" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" class="ahf"/></marker>
</defs>'''
aria = 'Flowchart of XREX serial commands. Connect and identify, confirm IDLE, configure channels, ready lines, control and shot settings, ARM, start a shot, poll while firing, read the log, and return to IDLE. A fault from any state leads to reading its type, removing the cause, FAULT:CLEar, and a STATE? check before returning to IDLE.'
svg = f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{e(aria)}" xmlns="http://www.w3.org/2000/svg">{defs}' + ''.join(bands + lines + boxes + texts) + '</svg>'

page = '''<title>XREX Command Flow</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;500;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* Layout: one engineering sheet. State bands run down the page, the fault path runs beside them. */
:root{
  --bg:#edf0f1;--panel:#fbfcfc;--ink:#17232b;--muted:#54636d;--line:#47565f;--rule:#c5ced3;
  --bc:#e3e8ea;--bi:#dfe7ec;--ba:#f6ebcb;--bf:#f8ddd0;--ff:#f8dfe5;
  --bct:#3e4d57;--bit:#2a5a78;--bat:#835600;--bft:#ad3a12;--fault:#ad1240;--danger:#c8421a;
  --warn:#fff5da;--warnl:#a87200;
  --sans:"Barlow","Helvetica Neue",Arial,sans-serif;--mono:"IBM Plex Mono",ui-monospace,Menlo,Consolas,monospace;
}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){
  --bg:#10171c;--panel:#1a242b;--ink:#e6edf0;--muted:#9babb4;--line:#a7b5bd;--rule:#34434d;
  --bc:#182127;--bi:#17242d;--ba:#30281a;--bf:#372219;--ff:#361a24;
  --bct:#b4c1c9;--bit:#7fb9dc;--bat:#e8b95c;--bft:#f19470;--fault:#f07f9c;--danger:#f1794f;
  --warn:#33291a;--warnl:#e8b95c;color-scheme:dark}}
:root[data-theme="dark"]{
  --bg:#10171c;--panel:#1a242b;--ink:#e6edf0;--muted:#9babb4;--line:#a7b5bd;--rule:#34434d;
  --bc:#182127;--bi:#17242d;--ba:#30281a;--bf:#372219;--ff:#361a24;
  --bct:#b4c1c9;--bit:#7fb9dc;--bat:#e8b95c;--bft:#f19470;--fault:#f07f9c;--danger:#f1794f;
  --warn:#33291a;--warnl:#e8b95c;color-scheme:dark}
body{background:var(--bg);color:var(--ink);font:16px/1.5 var(--sans)}
.sheet{max-width:1280px;margin:0 auto;padding-block:28px 48px;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px));display:grid;gap:20px}
h1{font-size:30px;line-height:1.1;font-weight:700;margin:0;text-wrap:balance}
.lede{margin:6px 0 0;color:var(--muted);max-width:70ch}
.key{display:flex;flex-wrap:wrap;gap:8px 24px;font-size:14px;color:var(--muted);align-items:center}
.key span{display:inline-flex;gap:8px;align-items:center}
.k-d{width:9px;height:9px;border-radius:50%;background:var(--danger)}
.k-f{width:26px;border-top:2px dashed var(--fault)}
figure{margin:0;display:grid;gap:8px}
.scroll{overflow-x:auto;background:var(--panel);border:1px solid var(--rule);border-radius:8px;padding:12px}
svg{display:block;width:100%;min-width:1040px;height:auto;color:var(--line)}
figcaption{font-size:14px;color:var(--muted);max-width:90ch}
.notes{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,340px),1fr));gap:18px 28px}
.notes div{min-width:0;border-top:1px solid var(--rule);padding-top:10px}
h2{font-size:17px;font-weight:600;margin:0 0 4px}
p{margin:0}
code{font:13.5px var(--mono);overflow-wrap:anywhere}
.src{font-size:13.5px;color:var(--muted)}
text{font-family:var(--sans);fill:var(--ink)}
.tt{font-weight:600;font-size:13px}
.mm{font-family:var(--mono);font-size:12px}
.nn{font-size:12px;fill:var(--muted)}
.nb{font-size:13px;font-weight:600}
.el{font-size:11.5px;fill:var(--muted)}
.fe{fill:var(--fault)}
.tag{font-weight:700;font-size:15px;letter-spacing:.08em}
.bct{fill:var(--bct)}.bit{fill:var(--bit)}.bat{fill:var(--bat)}.bft{fill:var(--bft)}
.bc{fill:var(--bc)}.bi{fill:var(--bi)}.ba{fill:var(--ba)}.bf{fill:var(--bf)}
.box{fill:var(--panel);stroke:var(--line);stroke-width:1}
.plain{fill:none;stroke:var(--rule);stroke-width:1}
.fault{fill:var(--ff);stroke:var(--fault);stroke-width:1.4}
.warn{fill:var(--warn);stroke:var(--warnl);stroke-width:1.2}
.dg{fill:var(--danger)}
.ln{fill:none;stroke:var(--line);stroke-width:1.3}
.fl{fill:none;stroke:var(--fault);stroke-width:1.5;stroke-dasharray:6 4}
.wl{fill:none;stroke:var(--warnl);stroke-width:1.2;stroke-dasharray:2 3}
.jt{fill:var(--line)}
.ah{fill:var(--line)}.ahf{fill:var(--fault)}
</style>
<main class="sheet">
<header>
<h1>XREX command flow</h1>
<p class="lede">The serial commands for one Transrex shot, in the order the controller accepts them, and the path out of a fault. Check the reply to each command before sending the next.</p>
</header>
<div class="key">
<span><i class="k-d"></i>The hub logs and announces this as a dangerous command</span>
<span><i class="k-f"></i>Fault path</span>
<span><code>&lt;ch&gt;</code> is the channel, 1 to 4</span>
</div>
<figure>
<div class="scroll">''' + svg + '''</div>
<figcaption>Each band is an operating state. Work moves down the bands, a fault from any band moves right, and every finish returns through the rail on the left to IDLE.</figcaption>
</figure>
<section class="notes">
<div><h2>When fault pins are read</h2><p>Water, temperature, Enerpro and overcurrent pins are all polled continuously (water, temperature and Enerpro since firmware built 2026-11-09; older images read them only on a pin change, at boot and in <code>FAULT:CLEar</code>). A disabled channel's pins are ignored. <code>FAULT:CLEar</code> in IDLE or ARMED does nothing. In FAULT, <code>STATE?</code> ends with <code>ACTIVE=</code>, the list of every fault measured.</p></div>
<div><h2>Commands that act outside IDLE</h2><p><code>SOURce:ENAble</code>, <code>XREX:CHANnel:ENAOut</code> and <code>CONTactOut</code> take effect immediately in any state. While ARMED or FIRING, an enabled channel whose ready lines are not both HIGH faults, so set the lines before enabling.</p></div>
<div><h2>A reset forgets the setup</h2><p>A <code>!BOOT</code> line can arrive at any time. Enables, gains and shot timing are gone unless a stored profile is recalled. Re-identify, tell the operator, and do not resend the old setup silently.</p></div>
<div><h2>Not on this chart</h2><p><code>DEBUG:FAULT:BYPASS 1</code> makes the controller ignore its fault detection. It is for a bench with nothing connected and is not a recovery step.</p></div>
</section>
<p class="src">Sources: <code>docs/labview_commands.md</code>, <code>docs/labview_state_and_faults.md</code>, <code>firmware/xrex/docs/command_reference.md</code>, and the XREX firmware in WHAM-CONTROLLERS. Amps are a placeholder calibration until measured on a real Transrex.</p>
</main>
'''
import os, re
DOCS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs')
open(os.path.join(DOCS, 'xrex_command_flow.html'), 'w').write(page)

# --- GitHub renders Markdown and SVG files, not HTML: write the same chart as a standalone SVG
# (its own <style>, light and dark through prefers-color-scheme, the page's fonts replaced by system
# ones) and a Markdown page that embeds it, so docs/xrex_command_flow.md shows the chart on GitHub.
css = re.search(r'<style>(.*?)</style>', page, re.S).group(1)
def _vars(block):
    return dict(re.findall(r'--([a-z]+):([^;}]+)', block))
light = _vars(css[css.index(':root{'):css.index('@media')])
dark = _vars(css[css.index('@media'):css.index(':root[data-theme="dark"]')])
for d in (light, dark):
    d['sans'] = '"Helvetica Neue",Arial,sans-serif'
    d['mono'] = 'ui-monospace,Menlo,Consolas,monospace'
light_all = dict(light)
dark_all = dict(light, **dark)
rules_css = css[css.index('text{font-family'):]

def lit(rules, v):
    # literal colours, no var(): viewers that lack CSS variables (rsvg, some editors) render it too
    return re.sub(r'var\(--([a-z]+)\)', lambda m: v[m.group(1)], rules)

style = (lit(rules_css, light_all) + ':root{color:%s}' % light_all['line'] +
         '@media (prefers-color-scheme:dark){' + lit(rules_css, dark_all) + ':root{color:%s}.bgp{fill:%s}}' % (dark_all['line'], dark_all['panel']) +
         '.bgp{fill:%s}' % light_all['panel'])
standalone = svg.replace('<svg ', '<svg width="%d" height="%d" ' % (W, H), 1)
standalone = standalone.replace('>' + defs, '><style>' + style + '</style>' + defs +
                                '<rect class="bgp" width="%d" height="%d"/>' % (W, H), 1)
open(os.path.join(DOCS, 'xrex_command_flow.svg'), 'w').write('<?xml version="1.0" encoding="UTF-8"?>\n' + standalone + '\n')

def md(t):
    t = re.sub(r'</?code>', '`', t)
    t = re.sub(r'<[^>]+>', '', t)
    return t.replace('&lt;', '<').replace('&gt;', '>').replace('&amp;', '&')

notes = re.findall(r'<div><h2>(.*?)</h2><p>(.*?)</p></div>', page, re.S)
src = re.search(r'<p class="src">(.*?)</p>', page, re.S).group(1)
mdtext = ('# XREX command flow\n\n'
          'The serial commands for one Transrex shot, in the order the controller accepts them, and the path out of a fault. '
          'Check the reply to each command before sending the next.\n\n'
          'Key: a filled orange dot marks a command the hub logs and announces as dangerous; a dashed line is the fault path; '
          '`<ch>` is the channel, 1 to 4.\n\n'
          '![XREX command flow](xrex_command_flow.svg)\n\n'
          '*Each band is an operating state. Work moves down the bands, a fault from any band moves right, and every finish returns '
          'through the rail on the left to IDLE.* The chart is also available as a standalone page, '
          '[xrex_command_flow.html](xrex_command_flow.html), to open in a browser from a checkout, or as the '
          '[SVG](xrex_command_flow.svg) alone.\n\n'
          + ''.join('**%s.** %s\n\n' % (md(h), md(b)) for h, b in notes)
          + md(src) + '\n\n'
          '<!-- Generated by python/gen_command_flow.py; edit that script, not this file. -->\n')
open(os.path.join(DOCS, 'xrex_command_flow.md'), 'w').write(mdtext)
print('wrote docs/xrex_command_flow.{html,svg,md}  (%d bytes of svg)' % len(standalone))

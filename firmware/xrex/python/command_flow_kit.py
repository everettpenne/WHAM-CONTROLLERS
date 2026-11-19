"""command_flow_kit.py -- the drawing kit behind the command flowcharts (gen_command_flow.py).

SHARED: this file is byte-identical in firmware/xrex/python and firmware/hbridge/python (python/check_shared.py
compares the two). Each product's gen_command_flow.py lays its chart out with these primitives and calls finish(),
which writes, into that product's docs/ folder:

    <name>.html   the chart as a page (open it in a browser from a checkout)
    <name>.svg    the chart alone, with its own light and dark colours (GitHub shows it)
    <name>.md     a Markdown page that embeds the SVG, with the notes as text

and, when the repository has the operator console next to it, a self-contained copy of the page in
host/operator_ui/static/help/<name>.html, which the console lists under its menu. That copy loads nothing from
the network. Edit a chart in its generator and run it again; never edit the generated files.

The sheet is one fixed layout: operating-state bands run down the left (a main column of steps, a side column of
"what if" boxes), the fault path runs down the right, and a rail on the far left carries every return to IDLE.
A box is a list of lines: ('t', text) a title, ('m', text[, True]) a command in monospace (True marks a command
the hub announces as dangerous), ('n', text) a note. box() refuses a line that would not fit its width.

Standard library only.
"""
import html
import os
import re

e = html.escape
W = 1228                                  # sheet width
X0, SW, CX = 78, 400, 278                 # main column: left edge, width, centre line
SX, SWD = 530, 300                        # side column
BUS, RET, RAIL = 872, 884, 34             # fault bus, return line, the IDLE rail
FX, FW = 896, 300                         # fault column
FCX = FX + FW // 2
BL, BR = 54, 850                          # band left and right
GAP = 20
bands, lines, boxes, texts = [], [], [], []
P = {'t': 18, 'm': 18, 'n': 16}           # line pitch
CW = {'t': 6.5, 'm': 7.2, 'n': 5.9}       # width of one character, for the fit check


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
    # one text element, so the subtitle follows the name whatever the font's letter widths are
    texts.append(f'<text x="{BL + 24}" y="{top + 22}"><tspan class="tag {cls}t">{e(name)}</tspan><tspan dx="12" class="nn">{e(sub)}</tspan></text>')


DEFS = '''<defs>
<marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" class="ah"/></marker>
<marker id="af" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" class="ahf"/></marker>
</defs>'''

FONTS = '''<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;500;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap">
'''

CSS = '''/* Layout: one engineering sheet. State bands run down the page, the fault path runs beside them. */
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
.k-b{width:16px;height:12px;border-radius:3px;border:1px solid var(--rule)}
.k-b.i{background:var(--bi)}.k-b.a{background:var(--ba)}.k-b.f{background:var(--bf)}
figure{margin:0;display:grid;gap:8px}
.scroll{overflow-x:auto;background:var(--panel);border:1px solid var(--rule);border-radius:8px;padding:12px}
svg{display:block;width:100%;min-width:1040px;height:auto;color:var(--line)}
figcaption{font-size:14px;color:var(--muted);max-width:90ch}
.notes{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,340px),1fr));gap:18px 28px}
.notes div{min-width:0;border-top:1px solid var(--rule);padding-top:10px}
h2{font-size:17px;font-weight:600;margin:0 0 4px}
p{margin:0}
a{color:var(--bit)}
code{font:13.5px var(--mono);overflow-wrap:anywhere}
.src{font-size:13.5px;color:var(--muted)}
@media print{body{background:#fff}.sheet{padding:0;max-width:none}.scroll{overflow:visible;border:0;padding:0}svg{min-width:0}.nav{display:none}}
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
'''


def _md(t):
    t = re.sub(r'</?code>', '`', t)
    t = re.sub(r'<[^>]+>', '', t)
    return t.replace('&lt;', '<').replace('&gt;', '>').replace('&amp;', '&')


def finish(name, docs, H, title, lede, key, key_md, caption, notes, sources, aria, generator='python/gen_command_flow.py'):
    """Write the chart. name: the file stem; docs: the product's docs folder; H: the sheet height; key: [(html of one
    key entry)]; key_md: the key as one Markdown sentence; notes: [(heading, html paragraph)]; sources: html."""
    svg = (f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{e(aria)}" xmlns="http://www.w3.org/2000/svg">{DEFS}'
           + ''.join(bands + lines + boxes + texts) + '</svg>')
    body = ('<main class="sheet">\n<!--NAV--><header>\n<h1>' + e(title) + '</h1>\n<p class="lede">' + lede + '</p>\n</header>\n'
            '<div class="key">\n' + '\n'.join(f'<span>{k}</span>' for k in key) + '\n</div>\n'
            '<figure>\n<div class="scroll">' + svg + '</div>\n<figcaption>' + caption + '</figcaption>\n</figure>\n'
            '<section class="notes">\n' + '\n'.join(f'<div><h2>{h}</h2><p>{b}</p></div>' for h, b in notes) + '\n</section>\n'
            '<p class="src">' + sources + '</p>\n</main>\n')
    page = ('<title>' + e(title) + '</title>\n<meta name="claude:viewport-fit-sides" content="cover">\n' + FONTS +
            '<style>\n' + CSS + '</style>\n' + body.replace('<!--NAV-->', ''))
    with open(os.path.join(docs, name + '.html'), 'w') as f:
        f.write(page)

    # GitHub renders Markdown and SVG files, not HTML: the same chart as a standalone SVG (its own <style>, light and dark
    # through prefers-color-scheme, the page's fonts replaced by system ones) and a Markdown page that embeds it.
    def _vars(block):
        return dict(re.findall(r'--([a-z]+):([^;}]+)', block))
    light = _vars(CSS[CSS.index(':root{'):CSS.index('@media')])
    dark = dict(light, **_vars(CSS[CSS.index('@media'):CSS.index(':root[data-theme="dark"]')]))
    for d in (light, dark):
        d['sans'] = '"Helvetica Neue",Arial,sans-serif'
        d['mono'] = 'ui-monospace,Menlo,Consolas,monospace'
    rules = CSS[CSS.index('text{font-family'):]

    def lit(v):                           # literal colours, no var(): viewers without CSS variables (rsvg, some editors) render it too
        return re.sub(r'var\(--([a-z]+)\)', lambda m: v[m.group(1)], rules)
    # the light rules first: a rule after the @media block would win over the dark one (same specificity, later in the sheet)
    style = (lit(light) + ':root{color:%s}.bgp{fill:%s}' % (light['line'], light['panel']) +
             '@media (prefers-color-scheme:dark){' + lit(dark) + ':root{color:%s}.bgp{fill:%s}}' % (dark['line'], dark['panel']))
    alone = svg.replace('<svg ', '<svg width="%d" height="%d" ' % (W, H), 1)
    alone = alone.replace('>' + DEFS, '><style>' + style + '</style>' + DEFS + '<rect class="bgp" width="%d" height="%d"/>' % (W, H), 1)
    with open(os.path.join(docs, name + '.svg'), 'w') as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n' + alone + '\n')

    with open(os.path.join(docs, name + '.md'), 'w') as f:
        f.write('# ' + title + '\n\n' + _md(lede) + '\n\n' + 'Key: ' + key_md + '\n\n'
                '![' + title + '](' + name + '.svg)\n\n*' + _md(caption) + '* The chart is also available as a standalone page, '
                '[' + name + '.html](' + name + '.html), to open in a browser from a checkout, or as the [SVG](' + name + '.svg) alone. '
                'The operator console carries a copy under its menu.\n\n'
                + ''.join('**%s.** %s\n\n' % (_md(h), _md(b)) for h, b in notes) + _md(sources) + '\n\n'
                '<!-- Generated by ' + generator + '; edit that script, not this file. -->\n')

    # the operator console's copy: a whole document, nothing loaded from the network
    wrote = ['docs/' + name + '.{html,svg,md}']
    help_dir = os.path.normpath(os.path.join(docs, '..', '..', '..', 'host', 'operator_ui', 'static', 'help'))
    if os.path.isdir(os.path.dirname(help_dir)):
        os.makedirs(help_dir, exist_ok=True)
        nav = '<p class="src nav"><a href="/">&larr; Operator console</a></p>\n'
        with open(os.path.join(help_dir, name + '.html'), 'w') as f:
            f.write('<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
                    '<meta name="viewport" content="width=device-width, initial-scale=1">\n<title>' + e(title) + '</title>\n'
                    '<!-- Generated by firmware/*/' + generator + ': edit that script, not this file. -->\n'
                    '<style>\n*{box-sizing:border-box}body{margin:0}\n' + CSS + '</style>\n</head>\n<body>\n' + body.replace('<!--NAV-->', nav) + '</body>\n</html>\n')
        wrote.append('host/operator_ui/static/help/' + name + '.html')
    print('wrote ' + ', '.join(wrote) + '  (%d bytes of svg)' % len(alone))

"use strict";
/* WHAM operator console -- the page. Talks only to wham_ui.py on this computer (see its header comment).
   Safety: every command goes through send(); the SERVER decides which commands need an operator's confirmation (the project's
   dangerous-command list), answers 409, and this page then asks the operator and sends the same command again as confirmed. */

const $ = (sel, root = document) => root.querySelector(sel);
const h = (tag, props = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (k === "class") e.className = v;
    else if (k === "text") e.textContent = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v === true) e.setAttribute(k, "");
    else if (v !== false && v != null) e.setAttribute(k, v);
  }
  for (const kid of kids.flat()) if (kid != null) e.append(kid.nodeType ? kid : document.createTextNode(kid));
  return e;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const S = { conns: new Map(), active: null, presets: [], serial: { available: false, ports: [] }, demo: false, user: window.WHAM.user };

/* ---------- talking to the server ---------- */
async function api(path, body) {
  const opt = { headers: { "X-WHAM-Token": window.WHAM.token } };
  if (body !== undefined) { opt.method = "POST"; opt.headers["Content-Type"] = "application/json"; opt.body = JSON.stringify(body); }
  const r = await fetch(path, opt);
  let data = {};
  try { data = await r.json(); } catch (e) { /* not JSON */ }
  if (!r.ok) { const err = new Error(data.error || `HTTP ${r.status}`); err.status = r.status; err.data = data; throw err; }
  return data;
}
function toast(text, bad = false) {
  const t = h("div", { class: "t" + (bad ? " err" : ""), text });
  $("#toast").append(t);
  setTimeout(() => t.remove(), bad ? 9000 : 3500);
}
const ERR_HELP = {
  1: "The controller does not know this command (an older firmware may lack it).", 2: "Start a shot upload with SHOT:BEGin first.", 3: "The shot is full.",
  4: "The shot or its numbers are not acceptable (check the order of the times and the length).", 5: "There is no shot loaded: send a shot first.",
  6: "A fault is latched: clear it first (and find out why it happened).", 10: "A value is outside the limits of the selected supply.",
  11: "No such channel.", 12: "The command was refused: a value is missing, not a number, or out of range.",
  13: "Not allowed in the controller's current state (for example, it must be IDLE, or ARMED to fire).", 15: "The external-enable interlock is not satisfied.",
  19: "The board's identity could not be changed.",
};
function explain(reply) {
  const m = /^ERR (\d+)\s*(.*)$/.exec(reply || "");
  if (!m) return reply || "";
  return `${m[2] || "refused"} (ERR ${m[1]}).${ERR_HELP[m[1]] ? " " + ERR_HELP[m[1]] : ""}`;
}

/* ---------- confirmation of dangerous commands ---------- */
function describe(cmd, category) {
  const c = cmd.trim();
  if (/^ARM$/i.test(c)) return ["Arm the controller?", "ARM makes the controller ready to fire. Nothing is output yet, but from now on FIRE (and, if the external trigger is on, a rising edge on its input) starts the shot. Do not leave it armed."];
  if (/^(FIRE|SHOT:STAR\w*)$/i.test(c)) return ["Start the shot?", "The controller will start driving its outputs now, with the shot that is loaded. Make sure the equipment and everyone near it are ready."];
  if (/^FAULT:CLE\w*$/i.test(c)) return ["Clear the fault?", "This releases a latched protection. Only clear a fault after you know what caused it and that it is safe to continue."];
  const ch = /^CHARGE:FREQ\w*\s+(\d+)/i.exec(c);
  if (ch) return [`Turn the capacitor charger on at ${(+ch[1]).toLocaleString()} Hz?`, "The charger output is independent of the controller's state and stays on through a fault until you turn it off or the controller resets."];
  if (/^EXTernal:TRIGger\s+1/i.test(c) || /^EXT\w*:TRIG\w*\s+1/i.test(c)) return ["Turn the external trigger on?", "While the controller is ARMED, a rising edge on the trigger input will start the shot with nobody pressing anything."];
  if (/^PHASe:SELect/i.test(c) || /^PHAS\w*:SEL\w*/i.test(c)) return ["Change which phases drive?", "This chooses which of the phases U, V and W have active outputs."];
  if (/^(SOURce:ENAble|XREX:CHANnel:(ENAOut|CONTactOut))/i.test(c)) return ["Enable this output?", "This enables a channel or its enable/contactor line, which can allow real output."];
  return ["Send this command?", `This command is on the project's dangerous-command list (${category}): it can start output, arm, weaken a protection, clear a fault, or reset the controller.`];
}
function confirmDialog(conn, cmd, category) {
  return new Promise(resolve => {
    const dlg = $("#dlg-confirm");
    const [title, body] = describe(cmd, category);
    $("#confirm-title").textContent = title;
    $("#confirm-body").textContent = body;
    $("#confirm-cmd").textContent = cmd;
    const hub = conn.status && conn.status.hub;
    const others = hub ? Math.max(0, hub.clients - 1) : 0;
    const warn = $("#confirm-others");
    warn.hidden = others === 0;
    if (others) warn.textContent = `${others} other ${others === 1 ? "connection is" : "connections are"} on this controller (${hub.who.join(", ")}). Agree with them first.`;
    const done = v => { dlg.close(); resolve(v); };
    $("#confirm-cancel").onclick = () => done(false);
    $("#form-confirm").onsubmit = ev => { ev.preventDefault(); done(true); };
    dlg.oncancel = () => resolve(false);
    dlg.showModal();
    $("#confirm-cancel").focus();
  });
}

/* ---------- sending ---------- */
async function send(conn, command, { quiet = false } = {}) {
  let confirmed = false;
  for (;;) {
    try {
      const r = await api("/api/send", { id: conn.id, command, confirmed });
      if (r.error && !r.reply) { if (!quiet) toast(r.error, true); return { ok: false, reply: null, error: r.error }; }
      if (!r.ok && !quiet) toast(explain(r.reply), true);
      refreshSoon(conn);
      return r;
    } catch (e) {
      if (e.status === 409 && e.data.needs_confirm && !confirmed) {
        if (!(await confirmDialog(conn, command, e.data.category))) return { ok: false, cancelled: true, reply: null };
        confirmed = true;
        continue;
      }
      if (!quiet) toast(e.message, true);
      return { ok: false, reply: null, error: e.message };
    }
  }
}
async function query(conn, commands) {
  try {
    const r = await api("/api/query", { id: conn.id, commands });
    return r.results;
  } catch (e) { return commands.map(c => ({ command: c, reply: null, error: e.message })); }
}
const okval = r => (r && r.reply && r.reply.startsWith("OK") ? r.reply.slice(2).trim() : null);

/* ---------- connections and the event stream ---------- */
async function loadState() {
  let st;
  try { st = await api("/api/state"); } catch (e) { return; }
  window.WHAM.shotsDir = st.shots_dir;
  S.presets = st.presets; S.serial = st.serial; S.demo = st.demo; S.user = st.user;
  const ids = new Set(st.connections.map(c => c.id));
  for (const s of st.connections) {
    let conn = S.conns.get(s.id);
    if (!conn) { conn = newConn(s); S.conns.set(s.id, conn); eventLoop(conn); if (!S.active) S.active = s.id; }
    conn.status = s;
  }
  for (const [id, conn] of S.conns) if (!ids.has(id)) { conn.gone = true; conn.root.remove(); S.conns.delete(id); if (S.active === id) S.active = null; }
  renderAll();
}
async function eventLoop(conn) {
  while (!conn.gone) {
    try {
      const r = await api(`/api/events?id=${conn.id}&since=${conn.seq}&wait=20`);
      conn.seq = r.seq; conn.status = r.status;
      if (r.events.length) addLines(conn, r.events);
      updateStatus(conn);
      renderTabs();
    } catch (e) {
      if (e.status === 404) return;
      await sleep(1500);
    }
  }
}
async function connect(spec, { save = false } = {}) {
  const r = await api("/api/connect", spec);
  if (save) {
    const item = { name: spec.name || (spec.host ? `${spec.host}:${spec.port}` : spec.device), mode: spec.mode };
    for (const k of ["host", "port", "device"]) if (spec[k]) item[k] = spec[k];
    await api("/api/presets", { presets: [...S.presets, item] });
  }
  S.active = r.id;
  await loadState();
  return r;
}
async function disconnect(conn) {
  await api("/api/disconnect", { id: conn.id });
  if (S.active === conn.id) S.active = null;
  await loadState();
}

/* ---------- top-level rendering ---------- */
function renderAll() { renderTabs(); renderHome(); for (const conn of S.conns.values()) conn.root.classList.toggle("active", S.active === conn.id); $("#home").hidden = S.active != null; if (S.active) updateStatus(S.conns.get(S.active)); }
function renderTabs() {
  const nav = $("#tabs");
  nav.replaceChildren(h("div", { class: "tab" + (S.active == null ? " active" : ""), tabindex: 0, role: "button", onclick: () => { S.active = null; renderAll(); } }, "Start"));
  for (const conn of S.conns.values()) {
    const s = conn.status, cls = !s.link_open ? "bad" : s.link_ok ? "ok" : "wait";
    const label = (s.ident && (s.ident.label && s.ident.label !== "-" ? s.ident.label : s.ident.serial)) || s.name;
    nav.append(h("div", { class: "tab" + (S.active === conn.id ? " active" : ""), tabindex: 0, role: "button", onclick: () => { S.active = conn.id; renderAll(); } },
      h("span", { class: "dot " + cls }), label, s.state ? h("small", { text: s.state }) : null));
  }
}
function presetCard(p, idx) {
  const mode = p.mode === "hub" ? "through the host" : p.mode === "direct" ? "directly" : "USB cable";
  const target = p.mode === "serial" ? p.device : `${p.host}:${p.port}`;
  const open = [...S.conns.values()].find(c => c.status.mode === p.mode && c.status.target === target);
  return h("div", { class: "card" },
    h("h3", {}, p.name, h("span", { class: "chip " + p.mode, text: mode })),
    h("div", { class: "meta", text: target }),
    h("div", { class: "row spread" },
      open ? h("button", { class: "btn", onclick: () => { S.active = open.id; renderAll(); } }, "Open")
           : h("button", { class: "btn primary", onclick: async ev => { ev.target.disabled = true; try { await connect({ ...p }); } catch (e) { toast(e.message, true); } ev.target.disabled = false; } }, "Connect"),
      h("button", { class: "btn link", onclick: async () => { if (!confirm(`Remove "${p.name}" from the saved list?`)) return; const l = S.presets.filter((_, i) => i !== idx); await api("/api/presets", { presets: l }); loadState(); } }, "Remove")));
}
function renderHome() {
  const home = $("#home");
  if (home.dataset.sig === JSON.stringify([S.presets, S.demo, [...S.conns.keys()]])) return;
  home.dataset.sig = JSON.stringify([S.presets, S.demo, [...S.conns.keys()]]);
  home.replaceChildren(
    h("h1", {}, "Controllers"),
    h("p", { class: "lead" }, "Pick a controller from your saved list, or connect to a new one. You can have several open at once; each has its own tab above. Connecting only reads the controller's identity: nothing is armed or started."),
    h("div", { class: "cards" },
      ...S.presets.map(presetCard),
      h("div", { class: "card" }, h("h3", {}, "Another controller"), h("div", { class: "meta" }, "Through the host, directly over the network, or by USB cable."),
        h("button", { class: "btn primary", onclick: openConnect }, "Connect...")),
      S.demo ? h("div", { class: "card" }, h("h3", {}, "Practice (simulated)"), h("div", { class: "meta" }, "A simulated controller on this computer. No hardware. Good for learning the screens."),
        h("div", { class: "row" },
          h("button", { class: "btn", onclick: () => connect({ mode: "demo", product: "hbridge" }).catch(e => toast(e.message, true)) }, "HBRIDGE"),
          h("button", { class: "btn", onclick: () => connect({ mode: "demo", product: "xrex" }).catch(e => toast(e.message, true)) }, "XREX"))) : null));
}

/* ---------- the connect dialog ---------- */
function openConnect() {
  const dlg = $("#dlg-connect"), form = $("#form-connect");
  const err = $("#connect-error"); err.hidden = true;
  const sel = $("#f-device");
  sel.replaceChildren(...S.serial.ports.map(p => h("option", { value: p.device, text: `${p.device}  ${p.description}` })));
  $("#serial-hint").textContent = !S.serial.available ? "USB cables need the pyserial package: python3 -m pip install pyserial" : S.serial.ports.length ? "" : "No serial ports found: is the cable plugged in?";
  const sync = () => {
    const mode = form.elements.mode.value;
    $("#net-fields").hidden = mode === "serial"; $("#serial-fields").hidden = mode !== "serial";
    $("#f-host").placeholder = mode === "hub" ? "the host's address (127.0.0.1 through an ssh tunnel)" : "the controller's bridge address";
  };
  form.querySelectorAll("input[name=mode]").forEach(r => r.onchange = sync); sync();
  $("#connect-cancel").onclick = () => dlg.close();
  form.onsubmit = async ev => {
    ev.preventDefault();
    const mode = form.elements.mode.value;
    const spec = { mode, name: $("#f-name").value.trim() };
    if (mode === "serial") spec.device = $("#f-device").value; else { spec.host = $("#f-host").value.trim(); spec.port = $("#f-port").value.trim(); }
    $("#connect-go").disabled = true; err.hidden = true;
    try { await connect(spec, { save: $("#f-save").checked }); dlg.close(); }
    catch (e) { err.textContent = e.message; err.hidden = false; }
    $("#connect-go").disabled = false;
  };
  dlg.showModal();
}

/* ---------- one controller's view ---------- */
function newConn(status) {
  const conn = { id: status.id, status, seq: 0, lines: [], gone: false, history: [], histPos: 0, showPoll: false, panel: null, panelProduct: null,
                 lastExtras: 0, busy: false, interval: 5000, ui: {} };
  conn.root = buildView(conn);
  $("#views").append(conn.root);
  return conn;
}
function buildView(conn) {
  const u = conn.ui;
  u.idTitle = h("h2"); u.idList = h("dl", { class: "kv" }); u.idWarn = h("div"); u.linkBanner = h("div");
  u.badge = h("div", { class: "badge unknown", text: "?" }); u.stateNote = h("div", { class: "hint" }); u.faultText = h("div", { class: "error" });
  u.btnArm = h("button", { class: "btn big", onclick: () => armClicked(conn) }, "ARM");
  u.btnDisarm = h("button", { class: "btn big", onclick: () => send(conn, "DISARM") }, "DISARM");
  u.btnFire = h("button", { class: "btn big danger wide", onclick: () => fire(conn) }, "FIRE");
  u.btnClear = h("button", { class: "btn", onclick: () => send(conn, "FAULT:CLEar") }, "Clear fault");
  u.btnStop = h("button", { class: "btn", onclick: () => send(conn, "SOURce:STOP") }, "Stop");
  u.others = h("div");
  u.recCheck = h("input", { type: "checkbox", onchange: () => recordToggled(conn) }); u.recLine = h("div", { class: "hint reclines" });
  u.opPane = h("div", { class: "pane active" }); u.conPane = h("div", { class: "pane" }); u.dataPane = h("div", { class: "pane" });
  const tabOp = h("button", { class: "ptab active", onclick: () => pane("op") }, "Operate");
  const tabData = h("button", { class: "ptab", onclick: () => pane("data") }, "Shot data");
  const tabCon = h("button", { class: "ptab", onclick: () => pane("con") }, "Console");
  u.tabData = tabData;
  const pane = which => {
    conn.pane = which;
    for (const [name, tab, el] of [["op", tabOp, u.opPane], ["data", tabData, u.dataPane], ["con", tabCon, u.conPane]]) { tab.classList.toggle("active", which === name); el.classList.toggle("active", which === name); }
    if (which === "data") { tabData.classList.remove("flag"); syncRecordForm(conn); loadShots(conn); }
    else if (which === "con") scrollConsole(conn, true); else refreshSoon(conn, true);
  };
  conn.showPane = pane; conn.pane = "op";
  buildConsole(conn);
  buildShotData(conn);
  return h("div", { class: "view" },
    h("div", { class: "col" },
      h("div", { class: "card" }, u.idTitle, u.idList, u.idWarn, u.linkBanner,
        h("div", { class: "row" }, h("button", { class: "btn small", onclick: () => { if (confirm("Disconnect from this controller? Anything it is doing keeps going.")) disconnect(conn); } }, "Disconnect"))),
      h("div", { class: "card" }, h("h3", {}, "State"), h("div", { class: "state" }, u.badge, u.faultText), u.stateNote,
        h("div", { class: "runbtns" }, u.btnArm, u.btnDisarm, u.btnFire), h("div", { class: "row", style: "margin-top:10px" }, u.btnClear, u.btnStop),
        h("label", { class: "check rec" }, u.recCheck, " Record data for the next shot"), u.recLine),
      u.others),
    h("div", { class: "col" }, h("div", { class: "panel-tabs" }, tabOp, tabData, tabCon), u.opPane, u.dataPane, u.conPane));
}
async function fire(conn) {
  const cmd = conn.status.product === "xrex" ? "SHOT:STARt" : "FIRE";
  await send(conn, cmd);
}

const STATE_HINT = {
  IDLE: "Idle: nothing is being output. Set the shot up, then ARM.",
  ARMED: "Armed: the controller is ready. FIRE starts the shot. DISARM stands down.",
  FIRING: "Firing: a shot is running. It ends by itself.",
  FAULT: "Fault: output is stopped. Find out why, then clear the fault.",
};
function updateStatus(conn) {
  const s = conn.status, u = conn.ui, ident = s.ident || {};
  const label = ident.label && ident.label !== "-" ? ident.label : null;
  u.idTitle.textContent = label || (ident.board ? ident.board : s.name);
  const via = s.mode === "hub" ? "through the host" : s.mode === "direct" ? "directly over the network" : "USB cable";
  const rows = [["Controller", ident.board || "(not identified yet)"], ["Board serial", ident.serial && ident.serial !== "UNSET" ? ident.serial : (ident.serial === "UNSET" ? "not assigned" : "-")],
    ["Firmware", ident.fw ? `${ident.fw} (${ident.commit || "?"})` : "-"], ["Chip ID", ident.uid ? ident.uid : "-"], ["Connected", `${via}: ${s.target}`]];
  u.idList.replaceChildren(...rows.flatMap(([k, v]) => [h("dt", { text: k }), h("dd", { text: v })]));
  u.idWarn.replaceChildren();
  if (s.identified && s.product === "unknown") u.idWarn.append(h("div", { class: "banner err" }, "This device did not identify itself as a WHAM controller. Only the Console is available. Do not send commands unless you know what it is."));
  u.linkBanner.replaceChildren();
  if (!s.link_open) u.linkBanner.append(h("div", { class: "banner err" }, "The connection is closed. It will try to reconnect by itself."));
  else if (!s.identified) u.linkBanner.append(h("div", { class: "banner warn" }, s.error ? `Waiting for the controller: ${s.error}` : "Waiting for the controller to identify itself..."));
  else if (!s.link_ok) u.linkBanner.append(h("div", { class: "banner warn" }, "The controller is not answering. What you see may be out of date."));
  if (s.hub && !s.hub.connected) u.linkBanner.append(h("div", { class: "banner err" }, "The host cannot reach the controller (power or network cable?)."));
  const st = s.state, known = !!st;
  u.badge.className = "badge " + (known ? st : "unknown"); u.badge.textContent = known ? st : "?";
  u.faultText.textContent = s.fault ? s.fault : "";
  u.stateNote.textContent = known ? (STATE_HINT[st] || "") : "No state yet.";
  const operable = s.identified && s.product !== "unknown" && s.link_ok;
  u.btnArm.disabled = !(operable && st === "IDLE"); u.btnDisarm.disabled = !(operable && st === "ARMED");
  u.btnFire.disabled = !(operable && st === "ARMED"); u.btnFire.textContent = s.product === "xrex" ? "START SHOT" : "FIRE";
  u.btnClear.disabled = !(operable && st === "FAULT"); u.btnStop.hidden = s.product !== "xrex"; u.btnStop.disabled = !(operable && (st === "FIRING" || st === "ARMED"));
  // other people on the same controller
  u.others.replaceChildren();
  if (s.hub) {
    const n = Math.max(0, s.hub.clients - 1);
    u.others.append(h("div", { class: "card" }, h("h3", {}, "People on this controller"),
      n ? h("div", { class: "banner warn" }, `${n} other ${n === 1 ? "connection" : "connections"}: ${s.hub.who.join(", ")}. Agree who is operating before you arm or fire.`) : h("div", { class: "hint" }, "You are the only one connected."),
      h("div", { class: "hint" }, s.hub.confirm ? "The host asks for confirmation of dangerous commands." : "The host announces dangerous commands to everyone and logs them.")));
  } else if (s.mode !== "hub") {
    u.others.append(h("div", { class: "card" }, h("h3", {}, "Direct connection"), h("div", { class: "hint" }, "No host: nothing is shared or logged, and nobody else can use this controller's bridge while you are connected. Use the Console's Save log if you need a record.")));
  }
  // the product panel appears once the controller says what it is
  if (operable && conn.panelProduct !== s.product) {
    conn.panelProduct = s.product;
    conn.panel = s.product === "hbridge" ? buildHbridge(conn) : buildXrex(conn);
    conn.ui.opPane.replaceChildren(conn.panel.el);
    conn.showPane("op");
  } else if (!s.identified || s.product === "unknown") {
    if (conn.panelProduct) { conn.panelProduct = null; conn.panel = null; }
    conn.ui.opPane.replaceChildren(h("div", { class: "section" }, s.product === "unknown" ? "No controls are offered for an unknown device. Use the Console tab." : "Controls appear when the controller has identified itself."));
  }
  if (conn.panel && conn.panel.onStatus) conn.panel.onStatus();
  renderRec(conn); watchRec(conn);
  if (conn.pane === "data") syncRecordForm(conn);
}

/* refreshing the readings on the Operate tab */
function refreshSoon(conn, immediate = false) { conn.wantRefresh = true; if (immediate) conn.lastExtras = 0; }
setInterval(async () => {
  const conn = S.conns.get(S.active);
  if (!conn || !conn.panel || conn.busy || document.visibilityState !== "visible") return;
  if (!conn.ui.opPane.classList.contains("active") || !conn.status.link_ok) return;
  const due = conn.wantRefresh ? 400 : conn.interval;
  if (!due || Date.now() - conn.lastExtras < due) return;
  conn.wantRefresh = false; conn.lastExtras = Date.now(); conn.busy = true;
  try { await conn.panel.refresh(); } finally { conn.busy = false; }
}, 500);
function intervalPicker(conn, def) {
  conn.interval = def;
  const sel = h("select", { onchange: () => { conn.interval = +sel.value; } }, ...[[0, "off"], [2000, "every 2 s"], [5000, "every 5 s"], [10000, "every 10 s"], [30000, "every 30 s"]].map(([v, t]) => h("option", { value: v, text: t })));
  sel.value = String(def);
  return h("label", { class: "hint" }, "Refresh the readings below ", sel, " ", h("button", { class: "btn small", onclick: () => refreshSoon(conn, true) }, "Refresh now"));
}
function section(title, step, ...kids) { return h("div", { class: "section" }, h("h3", {}, step ? h("span", { class: "step", text: step }) : null, title), ...kids); }

/* ---------- the console ---------- */
const QUICK = {
  common: [["Who are you?", "*IDN?"], ["State", "STATE?"], ["Faults", "FAULT?"], ["Fault sources", "FAULT:SOURces?"], ["Gate-driver inputs", "GDS?"], ["Board identity", "SYS:ID?"]],
  hbridge: [["Supply mode", "CONFig:MODE?"], ["Limits", "CONFig:LIMits?"], ["Phases", "PHASe:ENAble?"], ["Shot", "SHOT?"], ["Trigger", "EXTernal:TRIGger:STATus?"], ["Charger", "CHARGE:STATus?"], ["Last fault pins", "GDS:LAST?"]],
  xrex: [["Shot timing", "SHOT:TIMing?"], ["Channel 1 status", "SOURce:STATus? 1"], ["Channel 1 fault pins", "XREX:CHANnel:STATus? 1"], ["Interlock", "EXTernal:ENAble?"], ["Trigger", "EXTernal:TRIGger?"]],
  hub: [["Hub status", "!hub status"], ["Who is connected", "!hub clients"]],
};
function buildConsole(conn) {
  const u = conn.ui;
  const out = h("div", { class: "consolebox" });
  u.out = out;
  const input = h("input", { type: "text", placeholder: "Type a command and press Enter, for example *IDN?", autocomplete: "off", spellcheck: "false", "aria-label": "Command" });
  u.input = input;
  const submit = async () => {
    const text = input.value.trim(); if (!text) return;
    conn.history.push(text); conn.histPos = conn.history.length; input.value = "";
    await send(conn, text, { quiet: false });
  };
  input.addEventListener("keydown", ev => {
    if (ev.key === "Enter") submit();
    else if (ev.key === "ArrowUp") { conn.histPos = Math.max(0, conn.histPos - 1); input.value = conn.history[conn.histPos] || ""; ev.preventDefault(); }
    else if (ev.key === "ArrowDown") { conn.histPos = Math.min(conn.history.length, conn.histPos + 1); input.value = conn.history[conn.histPos] || ""; ev.preventDefault(); }
  });
  const quick = h("div", { class: "quick" });
  u.quick = quick;
  const showPoll = h("input", { type: "checkbox", onchange: ev => { conn.showPoll = ev.target.checked; rebuildConsole(conn); } });
  u.conPane.append(
    h("div", { class: "row spread" }, h("h3", {}, "Console"), h("div", { class: "row" },
      h("label", { class: "hint" }, showPoll, " show automatic status requests"),
      h("button", { class: "btn small", onclick: () => { conn.lines = []; rebuildConsole(conn); } }, "Clear"),
      h("button", { class: "btn small", onclick: () => saveLog(conn) }, "Save log"))),
    out, h("div", { class: "cmdrow" }, input, h("button", { class: "btn primary", onclick: submit }, "Send")), quick,
    h("p", { class: "hint" }, "Commands that start output, arm, or weaken a protection ask for confirmation first. Press Up for earlier commands."));
}
function fillQuick(conn) {
  const s = conn.status, u = conn.ui;
  const sets = [...QUICK.common, ...(s.product === "hbridge" ? QUICK.hbridge : s.product === "xrex" ? QUICK.xrex : []), ...(s.mode === "hub" ? QUICK.hub : [])];
  const sig = sets.map(x => x[1]).join("|");
  if (u.quick.dataset.sig === sig) return;
  u.quick.dataset.sig = sig;
  u.quick.replaceChildren(...sets.map(([label, cmd]) => h("button", { title: cmd, onclick: () => { u.input.value = cmd; u.input.focus(); } }, label)));
}
function lineEl(conn, e) {
  const d = new Date(e.t * 1000), ts = d.toTimeString().slice(0, 8) + "." + String(d.getMilliseconds()).padStart(3, "0");
  const arrow = { tx: "→ ", rx: "← " }[e.dir] || "";
  const cls = e.dir + (e.dir === "rx" && e.text.startsWith("ERR") ? " errl" : "");
  return h("div", { class: "ln " + cls }, h("span", { class: "t", text: ts }), arrow + e.text);
}
function addLines(conn, events) {
  conn.lines.push(...events);
  if (conn.lines.length > 4000) conn.lines.splice(0, conn.lines.length - 4000);
  const out = conn.ui.out, near = out.scrollHeight - out.scrollTop - out.clientHeight < 60;
  for (const e of events) if (conn.showPoll || !e.poll) out.append(lineEl(conn, e));
  while (out.childElementCount > 3000) out.firstChild.remove();
  if (near) out.scrollTop = out.scrollHeight;
  fillQuick(conn);
}
function rebuildConsole(conn) { conn.ui.out.replaceChildren(...conn.lines.filter(e => conn.showPoll || !e.poll).map(e => lineEl(conn, e))); scrollConsole(conn, true); }
function scrollConsole(conn, force) { const o = conn.ui.out; if (force) o.scrollTop = o.scrollHeight; }
function saveLog(conn) {
  const text = conn.lines.map(e => { const d = new Date(e.t * 1000); return `${d.toISOString()} ${({ tx: "TX", rx: "RX" })[e.dir] || e.dir.toUpperCase()} ${e.text}`; }).join("\n") + "\n";
  const name = `wham-console-${(conn.status.ident && conn.status.ident.serial) || conn.id}-${new Date().toISOString().replace(/[:.]/g, "-")}.txt`;
  const a = h("a", { href: URL.createObjectURL(new Blob([text], { type: "text/plain" })), download: name }); document.body.append(a); a.click(); a.remove();
}

/* ---------- HBRIDGE panel ---------- */
const HB_PRESETS = {
  perphase: { label: "Three different duty profiles (example)", build: (lim) => { const f = clampF(lim, 100); return [{ t: 0, f, u: 20, v: 80, w: 50 }, { t: 5, f, u: 50, v: 50, w: 90 }, { t: 10, f, u: 80, v: 20, w: 50 }]; } },
  flat: { label: "Flat: mid-range, 10 ms", build: lim => { const f = clampF(lim, 100), d = mid(lim, "duty", 50); return [{ t: 0, f, u: d, v: d, w: d }, { t: 10, f, u: d, v: d, w: d }]; } },
  ramp: { label: "Duty ramp up, 20 ms", build: lim => { const f = clampF(lim, 100), a = lim ? lim.d0 + (lim.d1 - lim.d0) * 0.25 : 25, b = lim ? lim.d0 + (lim.d1 - lim.d0) * 0.75 : 75; return [{ t: 0, f, u: a, v: a, w: a }, { t: 20, f, u: b, v: b, w: b }]; } },
};
function clampF(lim, kHz) { if (!lim) return kHz; return Math.min(lim.f1 / 1000, Math.max(lim.f0 / 1000, kHz)); }
function mid(lim, what, dflt) { return lim ? Math.round((lim.d0 + lim.d1) / 2 * 100) / 100 : dflt; }
const fmt = (x, n) => String(+(+x).toFixed(n));

function buildHbridge(conn) {
  const P = { rows: HB_PRESETS.perphase.build(null), limits: null, mode: null, phases: { U: 1, V: 1, W: 1 }, phaseWant: null, charge: null, chargeErr: false };
  const el = h("div");
  const state = () => conn.status.state;

  // 1 supply
  const segLim = h("button", { onclick: () => setMode("LIMiter") }, "Limiter"), segHv = h("button", { onclick: () => setMode("HVPS") }, "HVPS");
  const modeNote = h("div", { class: "hint" });
  async function setMode(m) { const r = await send(conn, `CONFig:MODE ${m}`); if (r.ok) toast(`Supply set to ${m === "HVPS" ? "HVPS" : "Limiter"}. Any shot already loaded was discarded.`); await P.refresh(); }
  // 2 phases
  const chips = {};
  for (const p of "UVW") chips[p] = h("button", { class: "toggle on", onclick: () => { P.phaseWant = P.phaseWant || { ...P.phases }; P.phaseWant[p] = P.phaseWant[p] ? 0 : 1; paintPhases(); } }, p);
  const phaseApply = h("button", { class: "btn small", onclick: async () => { const sel = "UVW".split("").filter(p => P.phaseWant[p]).join(""); if (!sel) { toast("Choose at least one phase.", true); return; } const r = await send(conn, `PHASe:SELect ${sel}`); if (r.ok) P.phaseWant = null; await P.refresh(); } }, "Apply");
  function paintPhases() { const w = P.phaseWant || P.phases; for (const p of "UVW") { chips[p].classList.toggle("on", !!w[p]); chips[p].classList.toggle("off", !w[p]); chips[p].className = "toggle " + (w[p] ? "on" : "off") + " " + p.toLowerCase(); } phaseApply.disabled = !P.phaseWant || state() !== "IDLE"; }
  // 3 shot
  const tbody = h("tbody"), shotMsg = h("div"), canvas = h("canvas", { class: "chart", width: 900, height: 220 });
  const presetSel = h("select", { onchange: () => { if (!presetSel.value) return; P.rows = HB_PRESETS[presetSel.value].build(P.limits); presetSel.value = ""; drawTable(); } },
    h("option", { value: "", text: "Start from an example..." }), ...Object.entries(HB_PRESETS).map(([k, v]) => h("option", { value: k, text: v.label })));
  const uploadBtn = h("button", { class: "btn primary", onclick: () => upload() }, "Send shot to controller");
  function cell(row, key, step) {
    const i = h("input", { type: "number", step, value: row[key], "aria-label": key });
    i.addEventListener("input", () => { row[key] = i.value === "" ? NaN : +i.value; validate(); drawChart(); });
    i.dataset.key = key; return i;
  }
  function drawTable() {
    tbody.replaceChildren(...P.rows.map((row, idx) => h("tr", {},
      h("td", {}, cell(row, "t", "any")), h("td", {}, cell(row, "f", "any")), h("td", {}, cell(row, "u", "any")), h("td", {}, cell(row, "v", "any")), h("td", {}, cell(row, "w", "any")),
      h("td", {}, h("button", { class: "btn small", title: "Remove this setpoint", disabled: P.rows.length <= 2, onclick: () => { P.rows.splice(idx, 1); drawTable(); } }, "✕")))));
    validate(); drawChart();
  }
  function validate() {
    const msgs = [], lim = P.limits, rows = P.rows;
    const flag = (idx, key, bad) => { const inp = tbody.children[idx] && tbody.children[idx].querySelector(`input[data-key=${key}]`); if (inp) inp.classList.toggle("bad", bad); };
    rows.forEach((r, i) => {
      let bt = !Number.isFinite(r.t) || r.t < 0 || (i === 0 && r.t !== 0) || (i > 0 && r.t <= rows[i - 1].t) || r.t > 1000;
      if (i === 0 && r.t !== 0) msgs.push("The first setpoint must be at time 0.");
      if (i > 0 && r.t <= rows[i - 1].t) msgs.push(`Setpoint ${i + 1}: the time must be later than the one before.`);
      if (r.t > 1000) msgs.push("A shot can be at most 1000 ms long.");
      flag(i, "t", bt);
      const bf = !Number.isFinite(r.f) || (lim && (r.f * 1000 < lim.f0 - 1e-6 || r.f * 1000 > lim.f1 + 1e-6));
      if (bf && lim) msgs.push(`Setpoint ${i + 1}: frequency ${fmt(r.f, 3)} kHz is outside ${lim.f0 / 1000}-${lim.f1 / 1000} kHz.`);
      flag(i, "f", bf);
      for (const k of ["u", "v", "w"]) { const bd = !Number.isFinite(r[k]) || (lim && (r[k] < lim.d0 - 1e-9 || r[k] > lim.d1 + 1e-9)); if (bd && lim) msgs.push(`Setpoint ${i + 1}: duty ${k.toUpperCase()} ${fmt(r[k], 2)} % is outside ${lim.d0}-${lim.d1} %.`); flag(i, k, bd); }
    });
    if (rows.length < 2) msgs.push("A shot needs at least two setpoints.");
    if (rows.length > 500) msgs.push("At most 500 setpoints.");
    if (!lim) msgs.push("Choose the supply (Limiter or HVPS) first, so the limits are known.");
    P.problems = [...new Set(msgs)];
    shotMsg.replaceChildren(...P.problems.map(m => h("div", { class: "banner warn", text: m })), P.shotResult ? P.shotResult : "");
    uploadBtn.disabled = P.problems.length > 0 || state() !== "IDLE";
    uploadBtn.title = state() !== "IDLE" ? "The controller must be IDLE to receive a shot." : "";
  }
  function drawChart() {
    const c = canvas, g = c.getContext("2d"), W = c.width, H = c.height, pad = { l: 46, r: 12, t: 12, b: 26 };
    const cs = getComputedStyle(document.documentElement);
    g.clearRect(0, 0, W, H);
    const rows = P.rows.filter(r => Number.isFinite(r.t));
    const tmax = Math.max(1, ...rows.map(r => r.t)), lim = P.limits, lo = lim ? lim.d0 : 0, hi = lim ? lim.d1 : 100;
    const X = t => pad.l + (W - pad.l - pad.r) * t / tmax, Y = d => H - pad.b - (H - pad.t - pad.b) * (d - lo) / (hi - lo || 1);
    g.strokeStyle = cs.getPropertyValue("--line"); g.fillStyle = cs.getPropertyValue("--muted"); g.font = "12px system-ui"; g.lineWidth = 1;
    for (let i = 0; i <= 4; i++) { const d = lo + (hi - lo) * i / 4, y = Y(d); g.beginPath(); g.moveTo(pad.l, y); g.lineTo(W - pad.r, y); g.stroke(); g.fillText(fmt(d, 0) + " %", 4, y + 4); }
    for (let i = 0; i <= 5; i++) { const t = tmax * i / 5; g.fillText(fmt(t, 1) + " ms", X(t) - 14, H - 8); }
    [["u", "#d6342b"], ["v", "#1a8f3c"], ["w", "#1d5fd1"]].forEach(([k, col]) => {
      g.strokeStyle = col; g.fillStyle = col; g.lineWidth = 2.2; g.beginPath();
      rows.forEach((r, i) => { const x = X(r.t), y = Y(r[k]); if (!Number.isFinite(y)) return; if (i === 0) g.moveTo(x, y); else g.lineTo(x, y); }); g.stroke();
      rows.forEach(r => { if (Number.isFinite(r[k])) { g.beginPath(); g.arc(X(r.t), Y(r[k]), 3.2, 0, 7); g.fill(); } });
    });
  }
  async function upload() {
    if (P.problems.length) return;
    uploadBtn.disabled = true; conn.busy = true; P.shotResult = null;
    const fail = (what, reply) => { P.shotResult = h("div", { class: "banner err", text: `${what}: ${explain(reply)}` }); };
    try {
      let r = await send(conn, "SHOT:BEGin", { quiet: true });
      if (!r.ok) return fail("The controller refused to start a new shot", r.reply || r.error);
      for (let i = 0; i < P.rows.length; i++) {
        const w = P.rows[i];
        r = await send(conn, `SHOT:STEP ${Math.round(w.t * 1000)} ${fmt(w.f * 1000, 3)} ${fmt(w.u, 2)} ${fmt(w.v, 2)} ${fmt(w.w, 2)}`, { quiet: true });
        if (!r.ok) return fail(`Setpoint ${i + 1} was refused`, r.reply || r.error);
      }
      r = await send(conn, "SHOT:END", { quiet: true });
      if (!r.ok) return fail("The controller refused to finish the shot", r.reply || r.error);
      // read the shot back and compare with what was sent: that, not the OK replies, is the proof of what it holds
      const res = await query(conn, ["SHOT?", ...P.rows.map((_, i) => `SHOT:POINt? ${i}`)]);
      const head = okval(res[0]);
      let bad = null;
      if (!head || +head.split(" ")[0] !== P.rows.length) bad = `the controller reports ${head || "nothing"} but ${P.rows.length} setpoints were sent`;
      P.rows.forEach((w, i) => {
        const v = (okval(res[i + 1]) || "").split(/\s+/).map(Number);
        const want = [Math.round(w.t * 1000), w.f * 1000, ...(v.length === 3 ? [w.u] : [w.u, w.v, w.w])];
        if (v.length < 3 || want.some((x, k) => Math.abs(x - v[k]) > Math.max(0.011, Math.abs(x) * 1e-5))) bad = bad || `setpoint ${i + 1} reads back as "${okval(res[i + 1])}"`;
      });
      if (bad) return fail("Read-back does not match", "ERR " + bad);
      P.shotResult = h("div", { class: "banner ok", text: `Shot loaded and checked by reading it back: ${P.rows.length} setpoints, ${fmt(P.rows[P.rows.length - 1].t, 3)} ms. ARM, then FIRE, when ready.` });
    } finally { conn.busy = false; validate(); refreshSoon(conn, true); }
  }
  // control loops
  const loopBtns = [0, 1, 2].map(i => h("button", { class: "toggle", onclick: async () => { await send(conn, `PID:LOOPMODE ${i + 1} ${P.loops && P.loops[i] ? 0 : 1}`); await P.refresh(); } }));
  // trigger
  const trigBtn = h("button", { class: "toggle", onclick: async () => { const on = P.trig && P.trig.enabled === "1"; await send(conn, `EXTernal:TRIGger ${on ? 0 : 1}`); await P.refresh(); } });
  const trigInfo = h("div", { class: "hint" }), trigWarn = h("div");
  // charger
  const chFreq = h("input", { type: "number", min: 0, max: 100, step: "any", value: 100, "aria-label": "Charger frequency in kilohertz", style: "width:110px" });
  const chInfo = h("div"), chNote = h("div", { class: "hint" });
  const chOn = h("button", { class: "btn primary", onclick: async () => { const hz = Math.round(+chFreq.value * 1000); if (!(hz >= 1 && hz <= 100000)) { toast("Enter a frequency from 0.001 to 100 kHz.", true); return; } await send(conn, `CHARGE:FREQuency ${hz}`); await P.refresh(); } }, "Start / change");
  const chOff = h("button", { class: "btn danger", onclick: async () => { await send(conn, "CHARGE:FREQuency 0"); await P.refresh(); } }, "OFF");

  el.append(
    h("div", { class: "row spread", style: "margin-bottom:10px" }, h("h2", {}, "Operate"), intervalPicker(conn, 5000)),
    section("Choose the supply", "1", h("div", { class: "row" }, h("div", { class: "seg" }, segLim, segHv), modeNote), h("p", { class: "hint" }, "Only possible while the controller is IDLE. Changing the supply discards a loaded shot.")),
    section("Which phases drive", "2", h("div", { class: "row" }, ...["U", "V", "W"].map(p => chips[p]), phaseApply), h("p", { class: "hint" }, "A phase that is switched off has no output. Changing this asks for confirmation.")),
    section("Build and send the shot", "3",
      h("div", { class: "row spread" }, presetSel, h("div", { class: "row" }, h("button", { class: "btn small", onclick: () => { const last = P.rows[P.rows.length - 1]; P.rows.push({ ...last, t: last.t + 5 }); drawTable(); } }, "+ Add setpoint"), uploadBtn)),
      h("table", { class: "shot" }, h("thead", {}, h("tr", {}, h("th", { text: "Time (ms)" }), h("th", { text: "Frequency (kHz)" }), h("th", { class: "u", text: "Duty U (%)" }), h("th", { class: "v", text: "Duty V (%)" }), h("th", { class: "w", text: "Duty W (%)" }), h("th"))), tbody),
      h("p", { class: "hint" }, "The controller moves frequency and duty in straight lines between setpoints. The first setpoint is at 0 ms. Values outside the supply's limits are marked and cannot be sent."),
      shotMsg, canvas),
    section("Control loop (per phase)", "", h("div", { class: "row" }, ...["U", "V", "W"].map((p, i) => h("span", { class: "row" }, h("span", { class: p.toLowerCase() }, p), loopBtns[i]))),
      h("p", { class: "hint" }, "Open loop: the phase plays the duty you set in the shot. Closed loop: a PID holds the phase's output voltage to a target using the feedback input; it needs gains (PID:GAINS) and a target (TARGet:*), which are set in the Console for now. The controller records data (Shot data tab) only for a run with a closed loop.")),
    section("Run", "4", h("p", {}, "Use ", h("b", {}, "ARM"), " then ", h("b", {}, "FIRE"), " in the State box on the left. FIRE asks for confirmation."),
      h("div", { class: "row" }, h("span", {}, "External trigger:"), trigBtn), trigInfo, trigWarn),
    section("Capacitor charger (CHARGE)", "", h("p", { class: "hint" }, "A separate 50 % square wave that drives the frequency-to-voltage converter charging the capacitor bank. It is independent of the controller's state: you can leave it running, and a fault does not stop it."),
      h("div", { class: "row" }, h("label", {}, "Frequency ", chFreq, " kHz"), chOn, chOff, h("button", { class: "btn small", onclick: () => { chFreq.value = 100; } }, "100 kHz")), chInfo, chNote));

  P.onStatus = () => {
    const s = state(), editable = s === "IDLE";
    segLim.disabled = segHv.disabled = !editable; validate(); paintPhases();
    trigWarn.replaceChildren(s === "ARMED" && P.trig && P.trig.enabled === "1" ? h("div", { class: "banner warn", text: "ARMED with the external trigger ON: a rising edge on the trigger input will start the shot." }) : "");
  };
  P.refresh = async () => {
    const res = await query(conn, ["CONFig:MODE?", "CONFig:LIMits?", "PHASe:ENAble?", "EXTernal:TRIGger:STATus?", "CHARGE:STATus?", "SHOT?", "PID:LOOPMODE? 0"]);
    const [mode, limits, phases, trig, charge, shotInfo, loops] = res.map(okval);
    P.loops = loops ? loops.split(/\s+/).map(Number) : null;
    loopBtns.forEach((b, i) => { const v = P.loops ? P.loops[i] : null; b.textContent = v === 1 ? "Closed" : v === 0 ? "Open" : "?"; b.className = "toggle " + (v === 1 ? "on" : "off"); });
    P.heldShotS = shotInfo && /^\d+ \d+/.test(shotInfo) && +shotInfo.split(" ")[0] > 0 ? +shotInfo.split(" ")[1] / 1e6 : null;   // what the controller holds, not what the table shows
    P.mode = mode; modeNote.textContent = "";
    segLim.classList.toggle("on", mode === "LIMITER"); segHv.classList.toggle("on", mode === "HVPS");
    const lm = limits && /^(\w+) FREQ ([\d.]+) ([\d.]+) DUTY ([\d.]+) ([\d.]+)$/.exec(limits);
    P.limits = lm ? { mode: lm[1], f0: +lm[2], f1: +lm[3], d0: +lm[4], d1: +lm[5] } : null;
    modeNote.textContent = P.limits ? `${P.limits.mode}: ${P.limits.f0 / 1000}-${P.limits.f1 / 1000} kHz, duty ${P.limits.d0}-${P.limits.d1} %` : "No supply chosen yet.";
    if (phases) { const m = {}; phases.split(/\s+/).forEach(t => { const [k, v] = t.split("="); m[k] = +v; }); if (!P.phaseWant) P.phases = { U: m.U, V: m.V, W: m.W }; }
    if (trig) { P.trig = Object.fromEntries(trig.split(/\s+/).map(t => t.split("="))); trigBtn.textContent = P.trig.enabled === "1" ? "ON" : "OFF"; trigBtn.className = "toggle " + (P.trig.enabled === "1" ? "on" : "off");
      trigInfo.textContent = `Input level ${P.trig.input}; edges seen ${P.trig.edges}; shots started by an edge ${P.trig.started}; last result ${P.trig.last}. While ARMED, a rising edge on the trigger input starts the shot just as FIRE does.`; }
    if (charge) { const c = Object.fromEntries(charge.split(/\s+/).map(t => t.split("="))); chInfo.replaceChildren(h("div", { class: "banner " + (c.running === "1" ? "ok" : ""), text: c.running === "1" ? `Charger ON: ${(+c.freq).toLocaleString()} Hz (the timer produces ${c.actual} Hz).` : "Charger OFF." })); chNote.textContent = ""; }
    else if (res[4] && res[4].reply && res[4].reply.startsWith("ERR 1")) { chInfo.replaceChildren(h("div", { class: "banner warn", text: "This controller's firmware has no CHARGE output (needs firmware v0.11 or later)." })); }
    paintPhases(); P.onStatus(); validate(); drawChart();
  };
  P.recordDefaults = () => {
    const ph = ["U", "V", "W"].map((p, i) => (P.phases[p] ? i + 1 : 0)).filter(Boolean);
    const last = P.rows.length ? P.rows[P.rows.length - 1].t : 10;
    return { channels: ph.length ? ph : [1, 2, 3], duration_s: P.heldShotS || Math.max(0.001, (Number.isFinite(last) ? last : 10) / 1000),
             meta: { supply: P.mode, loop_modes_U_V_W: P.loops ? P.loops.map(v => (v ? "closed" : "open")).join(" ") : null, phases_on: ph.map(i => "UVW"[i - 1]).join(""), shot_setpoints: P.rows.map(r => ({ t_ms: r.t, f_kHz: r.f, duty_U: r.u, duty_V: r.v, duty_W: r.w })) } };
  };
  drawTable(); P.el = el;
  conn.lastExtras = 0; setTimeout(() => P.refresh(), 100);
  return P;
}

/* ---------- XREX panel ---------- */
function buildXrex(conn) {
  const P = { ch: {}, timing: null };
  const el = h("div"), grid = h("div", { class: "chan-grid" });
  const cards = {};
  const gainInput = label => h("input", { type: "number", min: 0, step: "any", style: "width:78px", "aria-label": label, oninput: ev => { ev.target.dataset.touched = "1"; } });
  const gainsTouched = (c, v) => [c.kp, c.ki, c.kd].forEach(i => { if (v) i.dataset.touched = "1"; else delete i.dataset.touched; });
  const zeroGains = c => c.gains && c.gains.every(x => x === 0);
  for (const n of [1, 2, 3, 4]) {
    const c = { n };
    c.title = h("h3", {}, `Channel ${n}`); c.run = h("div", { class: "hint" });
    /* a click shows its result at once (from the controller's own OK), then the full refresh confirms it */
    const flip = (cmd, key, btn, on, off) => async () => {
      const want = c[key] === "1" ? "0" : "1";
      const r = await send(conn, `${cmd} ${n} ${want}`);
      if (r && r.ok) { c[key] = want; P.paint(btn || { enaV: c.ena, conV: c.con, en: c.enable }[key], want, on, off); }
      await P.refresh();
    };
    c.enable = h("button", { class: "toggle", onclick: flip("SOURce:ENAble", "en", null, "ENABLED", "OFF") });
    c.ena = h("button", { class: "toggle", onclick: flip("XREX:CHANnel:ENAOut", "enaV", null, "ON", "OFF") });
    c.con = h("button", { class: "toggle", onclick: flip("XREX:CHANnel:CONTactOut", "conV", null, "ON", "OFF") });
    c.amps = h("input", { type: "number", min: 0, step: "any", style: "width:100px", "aria-label": `Peak current for channel ${n}` });
    c.pins = h("div", { class: "pins" });
    c.loop = h("button", { class: "toggle", title: "Closed loop: the PID corrects the output from the feedback. Open loop: the output is the setpoint, feedback only reported.",
      onclick: async () => { const want = c.loopV === "1" ? "0" : "1"; const r = await send(conn, `PID:LOOPMODE ${n} ${want}`); if (r && r.ok) { c.loopV = want; c.loop.textContent = want === "1" ? "Closed loop" : "Open loop"; c.loop.className = "toggle " + (want === "1" ? "on" : "off"); } await P.refresh(); } });
    c.kp = gainInput(`Kp of channel ${n}`); c.ki = gainInput(`Ki of channel ${n}`); c.kd = gainInput(`Kd of channel ${n}`);
    c.warn = h("div");
    c.el = h("div", { class: "card" }, c.title, c.run, c.warn,
      h("div", { class: "row" }, h("span", {}, "Control:"), c.loop),
      h("div", { class: "row", style: "margin-top:6px" }, h("span", {}, "PID gains"), h("label", {}, "Kp ", c.kp), h("label", {}, "Ki ", c.ki), h("label", {}, "Kd ", c.kd),
        h("button", { class: "btn small", onclick: async () => { const v = [c.kp, c.ki, c.kd].map(i => i.value.trim()); if (v.some(x => x === "" || !Number.isFinite(+x) || +x < 0)) { toast("Enter Kp, Ki and Kd as numbers (0 or more).", true); return; } await send(conn, `PID:GAINS ${n} ${v.join(" ")}`); gainsTouched(c, false); await P.refresh(); } }, "Set")),
      h("div", { class: "row" }, h("span", {}, "In shot:"), c.enable), h("div", { class: "row" }, h("span", {}, "Enable line:"), c.ena, h("span", {}, "Contactor:"), c.con),
      h("div", { class: "row", style: "margin-top:8px" }, h("label", {}, "Peak current ", c.amps, " A"), h("button", { class: "btn small", onclick: async () => { const v = +c.amps.value; if (!(v >= 0)) { toast("Enter a current in amps.", true); return; } await send(conn, `SHOT:CURRent ${n} ${v}`); await P.refresh(); } }, "Set")),
      h("div", { class: "hint", style: "margin-top:8px" }, "Fault inputs, raw levels (what each level means depends on the polarity settings):"), c.pins);
    cards[n] = c; grid.append(c.el);
  }
  const tUp = h("input", { type: "number", min: 0, step: "any", style: "width:90px" }), tFlat = h("input", { type: "number", min: 0, step: "any", style: "width:90px" }), tDown = h("input", { type: "number", min: 0, step: "any", style: "width:90px" });
  const timingInfo = h("div", { class: "hint" }), interlock = h("div", { class: "hint" });
  el.append(h("div", { class: "row spread", style: "margin-bottom:10px" }, h("h2", {}, "Operate"), intervalPicker(conn, 10000)),
    section("Shot timing", "1", h("div", { class: "row" }, h("label", {}, "Ramp up ", tUp, " s"), h("label", {}, "Flat top ", tFlat, " s"), h("label", {}, "Ramp down ", tDown, " s"),
      h("button", { class: "btn small", onclick: async () => { await send(conn, `SHOT:TIMing ${tUp.value} ${tFlat.value} ${tDown.value}`); await P.refresh(); } }, "Apply")), timingInfo),
    section("Channels", "2", grid),
    section("Run", "3", h("p", {}, "Use ", h("b", {}, "ARM"), " then ", h("b", {}, "START SHOT"), " in the State box on the left. ARM is refused unless every channel that is in the shot has its enable line and contactor on."), interlock));
  const paint = (btn, v, on, off) => { btn.textContent = v === "1" ? on : off; btn.className = "toggle " + (v === "1" ? "on" : "off"); };
  P.paint = paint;
  P.refresh = async () => {
    const cmds = ["SHOT:TIMing?", "EXTernal:ENAble?", "EXTernal:INPut?", "EXTernal:TRIGger?"];
    for (const n of [1, 2, 3, 4]) cmds.push(`SOURce:ENAble? ${n}`, `SOURce:STATus? ${n}`, `XREX:CHANnel:STATus? ${n}`, `XREX:CHANnel:ENAOut? ${n}`, `XREX:CHANnel:CONTactOut? ${n}`, `SHOT:CURRent? ${n}`, `PID:LOOPMODE? ${n}`, `PID:GAINS? ${n}`);
    const res = await query(conn, cmds), val = i => okval(res[i]);
    const tm = val(0); P.timingVals = tm ? tm.split(/\s+/).map(Number) : null; if (tm) { const [a, b, c] = tm.split(/\s+/); if (document.activeElement !== tUp && document.activeElement !== tFlat && document.activeElement !== tDown) { tUp.value = a; tFlat.value = b; tDown.value = c; } timingInfo.textContent = `Set in the controller: ${a} s up, ${b} s flat, ${c} s down.`; }
    else timingInfo.textContent = "No shot timing has been set since the controller started: set it before starting a shot.";
    interlock.textContent = `External enable interlock: ${val(1) === "1" ? "on" : "off"}; its input reads ${val(2) === "1" ? "HIGH" : "LOW"}. External trigger: ${val(3) === "1" ? "on" : "off"}.`;
    [1, 2, 3, 4].forEach((n, k) => {
      const c = cards[n], o = 4 + k * 8;
      c.en = val(o); c.enaV = val(o + 3); c.conV = val(o + 4);
      paint(c.enable, c.en, "ENABLED", "OFF"); paint(c.ena, c.enaV, "ON", "OFF"); paint(c.con, c.conV, "ON", "OFF");
      const st = val(o + 1); if (st) { const [run, sp, meas, outp] = st.split(/\s+/); c.run.textContent = `${run === "1" ? "Running" : "Not running"}: setpoint ${sp} Hz, measured ${meas} Hz, output ${outp} Hz.`; }
      if (document.activeElement !== c.amps && val(o + 5) != null) c.amps.value = val(o + 5);
      c.loopV = val(o + 6); c.loop.textContent = c.loopV === "1" ? "Closed loop" : c.loopV === "0" ? "Open loop" : "?"; c.loop.className = "toggle " + (c.loopV === "1" ? "on" : "off");
      const g = val(o + 7); c.gains = g ? g.split(/\s+/).map(Number) : null;
      if (c.gains) for (const [inp, v] of [[c.kp, c.gains[0]], [c.ki, c.gains[1]], [c.kd, c.gains[2]]]) if (document.activeElement !== inp && !inp.dataset.touched) inp.value = String(v);
      c.warn.replaceChildren(c.loopV === "1" && zeroGains(c) ? h("div", { class: "banner err", text: "Closed loop with all gains zero: the output will sit at the minimum frequency (3 kHz) and never follow the setpoint. Set the gains, or switch to open loop." }) : "");
      const pins = val(o + 2); c.pins.replaceChildren(...(pins ? pins.split(/\s+/).map(t => { const [k2, v] = t.split("="); return h("div", { class: "pin" }, h("b", { text: k2 }), v); }) : []));
    });
  };
  P.el = el; P.onStatus = () => {};
  P.preArmWarnings = () => {
    const none = [1, 2, 3, 4].every(n => cards[n].en !== "1");
    if (none) return ["No channel is enabled for the shot (\"In shot\" is OFF on every channel), so nothing will be output."];
    const bad = [1, 2, 3, 4].filter(n => cards[n].en === "1" && cards[n].loopV === "1" && zeroGains(cards[n]));
    return bad.length ? [`Channel${bad.length > 1 ? "s" : ""} ${bad.join(", ")} ${bad.length > 1 ? "are" : "is"} in the shot, in closed loop, with all PID gains zero. The output would stay at the minimum frequency and not follow the setpoint.`] : [];
  };
  P.recordDefaults = () => {
    const on = [1, 2, 3, 4].filter(n => cards[n].en === "1");
    const t = P.timingVals || [3, 0, 0];
    return { channels: on.length ? on : [1, 2, 3, 4], duration_s: t.reduce((a, b) => a + b, 0) || 3,
             meta: { shot_timing_s: { up: t[0], flat: t[1], down: t[2] }, channels: Object.fromEntries([1, 2, 3, 4].map(n => [n, { in_shot: cards[n].en === "1", peak_current_A: +cards[n].amps.value || null, loop: cards[n].loopV === "1" ? "closed" : cards[n].loopV === "0" ? "open" : null, gains_kp_ki_kd: cards[n].gains || null }])) } };
  };
  setTimeout(() => P.refresh(), 100);
  return P;
}


/* ---------- shot data: record around a shot, then report and plot it ---------- */
const REC_ACTIVE = ["waiting", "armed", "running", "collecting"];
const recActive = conn => !!(conn.status.rec && REC_ACTIVE.includes(conn.status.rec.state));
const REC_LABEL = { waiting: "Waiting for ARM", armed: "Recording armed", running: "Shot running", collecting: "Reading the data", done: "Done", empty: "Nothing recorded", failed: "Failed", cancelled: "Cancelled" };

function recDefaults(conn) {
  const p = conn.panel && conn.panel.recordDefaults ? conn.panel.recordDefaults() : null;
  return p || { channels: conn.status.product === "xrex" ? [1, 2, 3, 4] : [1, 2, 3], duration_s: 3, meta: {} };
}
function buildShotData(conn) {
  const u = conn.ui;
  u.recChans = h("div", { class: "row" });
  u.recDur = h("input", { type: "number", min: 0.001, max: 3600, step: "any", style: "width:90px", "aria-label": "Expected shot length in seconds",
    oninput: () => { conn.recDurTouched = true; } });
  u.recStatus = h("div"); u.recWarn = h("div");
  u.recBtns = h("div", { class: "row" },
    h("button", { class: "btn small", onclick: () => cancelRecording(conn) }, "Cancel recording"),
    h("button", { class: "btn small", title: "Read whatever the controller's log holds now (a shot that ran without being recorded by this page)", onclick: () => collectNow(conn) }, "Read the controller's log now"));
  u.shotList = h("div", { class: "shotlist" }); u.shotView = h("div");
  u.dataPane.append(
    section("Record the next shot", "", h("p", { class: "hint" }, "Tick “Record data for the next shot” (left, under the state), then ARM. The controller logs its setpoint, the measured feedback and its output during the shot; when the shot ends the page reads the data back, saves it, and shows the plots and numbers here."),
      h("div", { class: "row" }, h("span", {}, "Record:"), u.recChans, h("label", {}, "Expected shot length ", u.recDur, " s")),
      h("p", { class: "hint" }, "The log has room for about 1000 samples, so a longer shot is recorded at a lower sample rate. Set the length a little long rather than short."),
      u.recWarn, u.recStatus, u.recBtns),
    section("Recorded shots", "", u.shotList), u.shotView);
}
function syncRecordForm(conn) {
  const u = conn.ui, product = conn.status.product;
  if (!product || product === "unknown") return;
  const n = product === "xrex" ? 4 : 3, def = recDefaults(conn);
  if (u.recChans.dataset.product !== product) {
    u.recChans.dataset.product = product;
    u.recChans.replaceChildren(...Array.from({ length: n }, (_, i) => {
      const box = h("input", { type: "checkbox", "data-ch": i + 1, onchange: () => { conn.recChanTouched = true; } });
      return h("label", { class: "chk" }, box, " ", product === "xrex" ? `Ch ${i + 1}` : `Phase ${"UVW"[i]}`);
    }));
  }
  if (!conn.recChanTouched) u.recChans.querySelectorAll("input").forEach(b => { b.checked = def.channels.includes(+b.dataset.ch); });
  if (!conn.recDurTouched && document.activeElement !== u.recDur) u.recDur.value = String(+(+def.duration_s).toFixed(3));
}
function recSpec(conn) {
  const u = conn.ui, def = recDefaults(conn);
  return { id: conn.id, channels: [...u.recChans.querySelectorAll("input")].filter(b => b.checked).map(b => +b.dataset.ch), duration_s: +u.recDur.value, meta: def.meta };
}
async function armRecording(conn) {
  syncRecordForm(conn);
  const spec = recSpec(conn);
  try {
    const r = await api("/api/record/arm", spec);
    conn.status.rec = r.rec; renderRec(conn);
    for (const w of r.rec.warnings || []) toast(w, true);
    return true;
  } catch (e) {
    toast("Recording not armed: " + e.message, true);
    conn.ui.recCheck.checked = false;
    return false;
  }
}
async function armClicked(conn) {
  const warn = conn.panel && conn.panel.preArmWarnings ? conn.panel.preArmWarnings() : [];
  if (warn.length && !confirm(warn.join("\n\n") + "\n\nArm anyway?")) return;
  const r = await send(conn, "ARM");
  if (r.ok && conn.ui.recCheck.checked && !recActive(conn)) await armRecording(conn);
}
async function recordToggled(conn) {
  const on = conn.ui.recCheck.checked;
  if (on && conn.status.state === "ARMED" && !recActive(conn)) await armRecording(conn);
  else if (!on && recActive(conn)) await cancelRecording(conn);
  renderRec(conn);
}
async function cancelRecording(conn) {
  try { const r = await api("/api/record/cancel", { id: conn.id }); conn.status.rec = r.rec; conn.ui.recCheck.checked = false; renderRec(conn); } catch (e) { toast(e.message, true); }
}
async function collectNow(conn) {
  syncRecordForm(conn);
  try { const r = await api("/api/record/collect", recSpec(conn)); conn.status.rec = r.rec; renderRec(conn); watchRec(conn); }
  catch (e) { toast(e.message, true); }
}
function renderRec(conn) {
  const u = conn.ui, rec = conn.status.rec, ok = conn.status.identified && conn.status.product !== "unknown";
  u.recCheck.disabled = !ok || (recActive(conn) && !u.recCheck.checked);
  const cls = rec ? "rec-" + rec.state : "";
  const text = rec ? `${REC_LABEL[rec.state] || rec.state}: ${rec.message}` : "";
  const prog = rec && rec.state === "collecting" ? ` (${Math.round((rec.progress || 0) * 100)} %)` : "";
  u.recLine.className = "hint reclines " + cls; u.recLine.textContent = text ? text + prog : (u.recCheck.checked ? "Will be armed when you press ARM." : "");
  u.recStatus.className = "recstatus " + cls; u.recStatus.textContent = rec ? text + prog : "No recording requested.";
  u.recWarn.replaceChildren(...((rec && recActive(conn) ? rec.warnings : []) || []).map(w => h("div", { class: "banner warn", text: w })));
  u.recBtns.firstChild.hidden = !recActive(conn) || rec.state === "collecting";
}
function watchRec(conn) {
  const rec = conn.status.rec, last = conn.lastRec || "";
  if (!rec) return;
  const key = rec.requested_at + ":" + rec.state;
  if (key === last) return;
  conn.lastRec = key;
  if (rec.state === "done") {
    conn.ui.recCheck.checked = false;
    conn.ui.tabData.classList.add("flag");
    toast("Shot data recorded and saved.");
    conn.selectSid = rec.sid;
    if (conn.pane === "op") conn.showPane("data"); else if (conn.pane === "data") loadShots(conn);
  } else if (rec.state === "empty" || rec.state === "failed") {
    conn.ui.recCheck.checked = false;
    toast(rec.message, true);
  }
}
async function loadShots(conn) {
  const u = conn.ui;
  let list = [];
  try { list = (await api(`/api/shots?id=${conn.id}`)).shots; } catch (e) { return; }
  conn.shots = list;
  if (!list.length) { u.shotList.replaceChildren(h("div", { class: "hint" }, "No recorded shots for this controller yet.")); u.shotView.replaceChildren(); return; }
  if (!conn.selectSid || !list.find(x => x.sid === conn.selectSid)) conn.selectSid = list[0].sid;
  u.shotList.replaceChildren(...list.slice(0, 12).map(x => h("button", { class: "shotitem" + (x.sid === conn.selectSid ? " on" : ""), onclick: () => { conn.selectSid = x.sid; loadShots(conn); } },
    h("b", {}, x.started.replace("T", " ").slice(0, 19)), h("span", { class: "tag " + x.outcome }, x.outcome), `${(+x.duration_s).toPrecision(3)} s`, `${x.channels.length} ch`)));
  showShot(conn, conn.selectSid);
}
async function showShot(conn, sid) {
  const u = conn.ui;
  let d;
  try { d = await api(`/api/shot?sid=${encodeURIComponent(sid)}&unit=${conn.shotUnit || "A"}`); } catch (e) { u.shotView.replaceChildren(h("div", { class: "banner err" }, e.message)); return; }
  const unitSeg = d.summary.product === "xrex" ? h("div", { class: "seg" },
    ...[["A", "Amps"], ["native", "Hz"]].map(([v, t]) => h("button", { class: d.unit === v ? "on" : "", disabled: v === "A" && !d.can_amps, onclick: () => { conn.shotUnit = v; showShot(conn, sid); } }, t))) : null;
  const dl = name => h("button", { class: "btn small", onclick: () => downloadShot(conn, sid, name) }, name === "shot.csv" ? "Data (CSV)" : name === "report.html" ? "Report (HTML)" : name === "report.md" ? "Report (text)" : "Raw (JSON)");
  u.shotView.replaceChildren(
    h("div", { class: "section" },
      h("div", { class: "row spread" }, h("h3", {}, "Shot " + d.summary.started.replace("T", " ").slice(0, 19)), unitSeg),
      h("dl", { class: "kv" }, ...d.header.flatMap(([k, v]) => [h("dt", { text: k }), h("dd", { text: v })])),
      ...d.notes.map(n => h("div", { class: "banner warn", text: n })),
      d.unit === "A" && d.amps_note ? h("p", { class: "hint", text: d.amps_note }) : null,
      h("div", { class: "row" }, dl("shot.csv"), dl("report.html"), dl("report.md"), dl("shot.json")),
      h("div", { class: "hint" }, `Saved in ${window.WHAM.shotsDir || "the shots folder"}/${sid}`)),
    ...d.channels.map(c => { const box = h("div", { class: "section" }, h("h3", {}, c.name)); const fig = h("div", { class: "fig" }); fig.innerHTML = c.svg; box.append(fig, h("table", { class: "stats" }, ...c.stats.map(([k, v]) => h("tr", {}, h("td", { text: k }), h("td", { text: v }))))); return box; }));
}
async function downloadShot(conn, sid, name) {
  try {
    const r = await fetch(`/api/shot/file?sid=${encodeURIComponent(sid)}&name=${name}`, { headers: { "X-WHAM-Token": window.WHAM.token } });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const a = h("a", { href: URL.createObjectURL(await r.blob()), download: sid.replace("/", "_") + "_" + name });
    document.body.append(a); a.click(); a.remove();
  } catch (e) { toast("Download failed: " + e.message, true); }
}

/* ---------- start ---------- */
$("#btn-new").addEventListener("click", openConnect);
$(".brand").addEventListener("click", () => { S.active = null; renderAll(); });
loadState();
setInterval(loadState, 2500);

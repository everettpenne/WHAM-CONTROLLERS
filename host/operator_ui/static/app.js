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
const S = { tunnel: null, tunnelBusy: false, conns: new Map(), active: null, presets: [], serial: { available: false, ports: [] }, demo: false, user: window.WHAM.user, help: [], stopped: false };

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
  const gd = /^GDS:ENA\w*\s+(\d+)\s+0/i.exec(c);
  if (gd) return [+gd[1] === 0 ? "Switch ALL twelve gate-driver fault inputs off?" : `Switch gate-driver fault input ${gd[1]} off?`, "A switched-off input can no longer stop the output or latch a fault, whatever its pin reads. Only switch off an input that is not wired, or that you know is giving a false reading."];
  if (/^SPARK\w*:ENA\w*\s+0/i.test(c)) return ["Switch the spark-gap fault input off?", "With it off, a spark-gap signal no longer stops the output or latches a fault."];
  if (/^GDS:POL\w*\s+/i.test(c)) return ["Change the fault polarity of all twelve gate-driver inputs?", "A wrong polarity turns healthy inputs into faults, or hides real faults. The inputs are checked again at once."];
  if (/^CONF\w*:DEAD\w*\s+/i.test(c)) return ["Change the dead time?", "The dead time keeps the two switches of a phase from conducting together. Too short a value risks shoot-through in the power stage."];
  if (/^DEBUG:FAULT:BYPASS\s+1/i.test(c)) return ["Bypass ALL fault detection?", "With the bypass on, the controller ignores its fault inputs. It is for a bench with nothing connected, never for a supply that can deliver power."];
  return ["Send this command?", `This command is on the project's dangerous-command list (${category}): it can start output, arm, weaken a protection, clear a fault, or reset the controller.`];
}
/* A yes/no question in the page's own dialog. body: text or nodes. Resolves true only on the yes button; Esc and Cancel are no. */
function askDialog({ title, body, yes = "Yes", danger = true, command = null, others = null }) {
  return new Promise(resolve => {
    const dlg = $("#dlg-confirm");
    $("#confirm-title").textContent = title;
    $("#confirm-body").replaceChildren(...[body].flat().filter(x => x != null).map(x => (x.nodeType ? x : h("p", { text: String(x) }))));
    $("#confirm-cmdline").hidden = !command; $("#confirm-cmd").textContent = command || "";
    const warn = $("#confirm-others"); warn.hidden = !others; warn.textContent = others || "";
    const yesBtn = $("#confirm-yes"); yesBtn.textContent = yes; yesBtn.className = "btn " + (danger ? "danger" : "primary");
    let done = false;
    const finish = v => { if (done) return; done = true; if (dlg.open) dlg.close(); resolve(v); };
    $("#confirm-cancel").onclick = () => finish(false);
    $("#form-confirm").onsubmit = ev => { ev.preventDefault(); finish(true); };
    dlg.oncancel = () => finish(false);
    dlg.onclose = () => finish(false);
    dlg.showModal();
    $("#confirm-cancel").focus();
  });
}
/* A text prompt (one line or a text area). validate(text) may return an error message, or a promise of one. Resolves the text, or null. */
function textDialog({ title, hint = "", value = "", multiline = false, ok = "OK", placeholder = "", validate = null }) {
  return new Promise(resolve => {
    const dlg = $("#dlg-text"), area = $("#text-area"), line = $("#text-line"), err = $("#text-error");
    const box = multiline ? area : line;
    $("#text-title").textContent = title; $("#text-hint").textContent = hint; $("#text-hint").hidden = !hint;
    area.hidden = !multiline; line.hidden = multiline; box.value = value; box.placeholder = placeholder;
    $("#text-ok").textContent = ok; err.hidden = true;
    let done = false;
    const finish = v => { if (done) return; done = true; if (dlg.open) dlg.close(); resolve(v); };
    $("#text-cancel").onclick = () => finish(null);
    $("#form-text").onsubmit = async ev => {
      ev.preventDefault();
      const text = multiline ? box.value : box.value.trim();
      const problem = validate ? await validate(text) : null;
      if (problem) { err.textContent = problem; err.hidden = false; return; }
      finish(text);
    };
    dlg.oncancel = () => finish(null);
    dlg.onclose = () => finish(null);
    dlg.showModal();
    box.focus(); if (!multiline) box.select();
  });
}
function confirmDialog(conn, cmd, category) {
  const [title, body] = describe(cmd, category);
  const hub = conn.status && conn.status.hub;
  const n = hub ? Math.max(0, hub.clients - 1) : 0;
  return askDialog({ title, body, yes: "Yes, send it", command: cmd,
    others: n ? `${n} other ${n === 1 ? "connection is" : "connections are"} on this controller (${hub.who.join(", ")}). Agree with them first.` : null });
}
function download(name, text, type = "text/plain") {
  const a = h("a", { href: URL.createObjectURL(new Blob([text], { type })), download: name });
  document.body.append(a); a.click(); a.remove();
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
  if (S.stopped) return;
  try { st = await api("/api/state"); S.lost = 0; }
  catch (e) { S.lost = (S.lost || 0) + 1; if (S.lost === 3) toast("The console program on this computer is not answering. Was its window closed? Start it again and reload this page.", true); return; }
  window.WHAM.shotsDir = st.shots_dir;
  S.presets = st.presets; S.serial = st.serial; S.demo = st.demo; S.user = st.user; S.tunnel = st.tunnel || null;
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
const connLabel = conn => { const s = conn.status, i = s.ident; return (i && (i.label && i.label !== "-" ? i.label : i.serial)) || s.name; };
/* What to say before closing a connection: a controller that is armed or firing keeps going with nobody watching from this page. */
function leaveWarning(conns) {
  const busy = conns.filter(c => ["ARMED", "FIRING"].includes(c.status.state));
  const rec = conns.filter(c => recActive(c));
  return [
    busy.length ? h("div", { class: "banner warn", text: `${busy.map(c => `${connLabel(c)} is ${c.status.state}`).join("; ")}. Disconnecting does not disarm or stop it: it keeps going, with this page no longer watching.` }) : null,
    rec.length ? h("div", { class: "banner warn", text: `A shot recording is waiting on ${rec.map(connLabel).join(", ")}: it is lost if you disconnect now.` }) : null];
}
async function askDisconnect(conn) {
  const ok = await askDialog({ title: `Disconnect from ${connLabel(conn)}?`, yes: "Disconnect", danger: false,
    body: ["This closes this page's connection only. Whatever the controller is doing keeps going, and other people stay connected.", ...leaveWarning([conn])] });
  if (ok) { try { await disconnect(conn); toast(`Disconnected from ${connLabel(conn)}.`); } catch (e) { toast(e.message, true); } }
}
async function disconnectAll({ ask = true } = {}) {
  const conns = [...S.conns.values()];
  if (!conns.length) return true;
  if (ask && !(await askDialog({ title: `Disconnect from ${conns.length === 1 ? "the controller" : `all ${conns.length} controllers`}?`, yes: "Disconnect all", danger: false,
    body: [`This closes this page's ${conns.length === 1 ? "connection" : "connections"} (${conns.map(connLabel).join(", ")}). Whatever each controller is doing keeps going.`, ...leaveWarning(conns)] }))) return false;
  try { await api("/api/disconnect_all", {}); S.active = null; await loadState(); toast("Disconnected."); return true; }
  catch (e) { toast(e.message, true); return false; }
}

/* ---------- top-level rendering ---------- */
function renderAll() {
  renderTabs(); renderTop(); renderHome();
  for (const conn of S.conns.values()) conn.root.classList.toggle("active", S.active === conn.id);
  $("#home").hidden = S.active != null;
  if (S.active) updateStatus(S.conns.get(S.active));
}
function renderTabs() {
  const nav = $("#tabs");
  nav.replaceChildren(h("div", { class: "tab" + (S.active == null ? " active" : ""), tabindex: 0, role: "button", title: "Tunnel and saved controllers", onclick: () => { S.active = null; renderAll(); } }, "Start"));
  for (const conn of S.conns.values()) {
    const s = conn.status, cls = !s.link_open ? "bad" : s.link_ok ? "ok" : "wait";
    nav.append(h("div", { class: "tab" + (S.active === conn.id ? " active" : ""), tabindex: 0, role: "button", onclick: () => { S.active = conn.id; renderAll(); } },
      h("span", { class: "dot " + cls, title: cls === "ok" ? "answering" : cls === "wait" ? "waiting for the controller" : "connection closed" }), connLabel(conn),
      s.state ? h("small", { class: s.state, text: s.state }) : null,
      h("button", { class: "x", title: `Disconnect from ${connLabel(conn)}`, "aria-label": `Disconnect from ${connLabel(conn)}`, onclick: ev => { ev.stopPropagation(); askDisconnect(conn); } }, "×")));
  }
  const act = S.conns.get(S.active);
  document.title = act ? `${act.status.state || "?"} – ${connLabel(act)} – WHAM` : "WHAM Operator Console";
}
/* The top bar's right side: the tunnel (always visible, with its button) and Disconnect all. */
function tunnelWords(t) {
  if (!t) return ["", "", null];
  if (S.tunnelBusy || t.state === "starting") return ["", S.tunnelBusy === "closing" ? "Closing the tunnel…" : `Opening the tunnel to ${t.host}…`, null];
  if (t.state === "open") return ["ok", `Tunnel to ${t.host}: open`, "close"];
  if (t.state === "open-elsewhere") return ["ok", `Tunnel to ${t.host}: open`, "close"];
  if (t.state === "partial") return ["warn", `Tunnel: only port ${t.listening.join(", ")} of ${t.ports.join(", ")}`, "close"];
  return [t.error ? "err" : "", `Tunnel to ${t.host}: closed`, "open"];
}
function renderTop() {
  const t = S.tunnel, pill = $("#tunnel-pill");
  const sig = JSON.stringify([t, S.tunnelBusy, S.conns.size]);
  if (pill.dataset.sig !== sig) {
    pill.dataset.sig = sig;
    const [cls, text, action] = tunnelWords(t);
    pill.hidden = !t; pill.className = "pill " + cls;
    pill.title = t ? (t.state === "open-elsewhere" ? "This tunnel was opened outside this page (for example ssh in a terminal). It is used as it is." : t.error || "The ssh tunnel that carries the connections to the lab host") : "";
    pill.replaceChildren(h("span", { class: "dot " + (cls === "ok" ? "ok" : cls === "warn" ? "wait" : cls === "err" ? "bad" : "") }), h("span", { class: "txt", text: text }),
      action ? h("button", { class: "btn", disabled: !!S.tunnelBusy || (action === "open" && !t.ssh_found), onclick: () => tunnelToggle(action === "open") }, action === "open" ? "Open tunnel" : "Close tunnel") : null);
    $("#btn-disc-all").hidden = S.conns.size === 0;
    $("#btn-disc-all").textContent = S.conns.size > 1 ? `Disconnect all (${S.conns.size})` : "Disconnect";
  }
}
const viaTunnel = x => x.mode === "hub" && ["127.0.0.1", "localhost"].includes(String(x.host));
function presetCard(p, idx) {
  const mode = p.mode === "hub" ? "through the host" : p.mode === "direct" ? "directly" : "USB cable";
  const target = p.mode === "serial" ? p.device : `${p.host}:${p.port}`;
  const open = [...S.conns.values()].find(c => c.status.mode === p.mode && c.status.target === target);
  const t = S.tunnel;
  // a controller reached through this computer's own address needs its port to answer: the tunnel (or a local hub) provides it
  const reach = viaTunnel(p) && t ? ((t.listening || []).includes(+p.port) ? ["ok", "port answering"] : t.ports.includes(+p.port) ? ["bad", "tunnel closed: open it first"] : null) : null;
  return h("div", { class: "card preset" },
    h("h3", {}, p.name, h("span", { class: "chip " + p.mode, text: mode })),
    h("div", { class: "meta" }, open ? h("span", { class: "dot ok" }) : reach ? h("span", { class: "dot " + reach[0] }) : null, target, open ? " – connected" : reach ? ` – ${reach[1]}` : ""),
    p.note ? h("div", { class: "hint", text: p.note }) : null,
    h("div", { class: "row spread" },
      h("div", { class: "row tight" },
        open ? h("button", { class: "btn primary", onclick: () => { S.active = open.id; renderAll(); } }, "Open")
             : h("button", { class: "btn primary", onclick: async ev => { ev.target.disabled = true; try { await connect({ ...p }); } catch (e) { toast(e.message, true); } ev.target.disabled = false; } }, "Connect"),
        open ? h("button", { class: "btn", onclick: () => askDisconnect(open) }, "Disconnect") : null),
      h("div", { class: "row tight" },
        h("button", { class: "btn link", onclick: () => openConnect({ edit: idx }) }, "Edit"),
        h("button", { class: "btn link", onclick: async () => { if (!(await askDialog({ title: `Remove "${p.name}" from the saved list?`, body: "Only the entry on this start screen is removed. Nothing is sent to the controller.", yes: "Remove", danger: false }))) return; const l = S.presets.filter((_, i) => i !== idx); await api("/api/presets", { presets: l }); loadState(); } }, "Remove"))));
}
function renderHome() {
  const home = $("#home");
  const sig = JSON.stringify([S.presets, S.demo, [...S.conns.values()].map(c => [c.id, c.status.target]), S.tunnel, S.tunnelBusy]);
  if (home.dataset.sig === sig) return;
  home.dataset.sig = sig;
  const wasOpen = home.querySelector("details.help") && home.querySelector("details.help").open;
  home.replaceChildren(
    h("h1", {}, "Controllers"),
    tunnelCard(),
    h("p", { class: "lead" }, "Pick a controller from your saved list, or connect to a new one. You can have several open at once; each has its own tab above. Connecting only reads the controller's identity: nothing is armed or started."),
    h("div", { class: "cards" },
      ...S.presets.map(presetCard),
      h("div", { class: "card preset" }, h("h3", {}, "Another controller"), h("div", { class: "meta" }, "Through the host, directly over the network, or by USB cable."),
        h("div", { class: "row spread" }, h("button", { class: "btn", onclick: () => openConnect() }, "Connect…"))),
      S.demo ? h("div", { class: "card preset" }, h("h3", {}, "Practice (simulated)"), h("div", { class: "meta" }, "A simulated controller on this computer. No hardware. Good for learning the screens."),
        h("div", { class: "row spread" }, h("div", { class: "row tight" },
          h("button", { class: "btn", onclick: () => connect({ mode: "demo", product: "hbridge" }).catch(e => toast(e.message, true)) }, "HBRIDGE"),
          h("button", { class: "btn", onclick: () => connect({ mode: "demo", product: "xrex" }).catch(e => toast(e.message, true)) }, "XREX")))) : null),
    h("details", { class: "help", open: wasOpen || false }, h("summary", {}, "Using this console on another computer"),
      h("ol", {},
        h("li", {}, "Copy the ", h("code", {}, "host"), " folder of the WHAM-CONTROLLERS repository to that computer (or clone the repository). Python 3 is all it needs; nothing has to be installed with pip."),
        h("li", {}, "Start it: double-click the launcher in ", h("code", {}, "host/operator_ui"), " (", h("code", {}, "start_wham_ui.command"), " on macOS, ", h("code", {}, "start_wham_ui.bat"), " on Windows, ", h("code", {}, "start_wham_ui.sh"), " on Linux), or run ", h("code", {}, "python3 host/operator_ui/wham_ui.py"), "."),
        h("li", {}, "A tunnel to the lab host must be open on that computer. If one already is (opened in a terminal, or by another program), this page finds it by its ports and uses it: the bar above says “open”. Otherwise press Open tunnel: that runs ssh to ", h("code", {}, S.tunnel ? S.tunnel.host : "the host"), ", which must log in with a key, without a password. A different ssh name: start the console with ", h("code", {}, "--tunnel-host user@address"), "."),
        h("li", {}, "The saved controllers above expect the tunnel on this computer's ports ", h("code", {}, S.tunnel ? S.tunnel.ports.join(", ") : "5101, 5102, 5103"), ". If your tunnel uses other local ports, press Edit on a controller and change its port."),
        h("li", {}, "When you are done: Disconnect (top right), then Close tunnel. Closing the console's window does both."))));
}

/* ---------- the connect dialog (also edits a saved controller) ---------- */
function openConnect({ edit = null } = {}) {
  const dlg = $("#dlg-connect"), form = $("#form-connect");
  const err = $("#connect-error"); err.hidden = true;
  const old = edit != null ? S.presets[edit] : null;
  $("#connect-title").textContent = old ? "Edit a saved controller" : "Connect to a controller";
  $("#connect-go").textContent = old ? "Save" : "Connect";
  $("#f-save-wrap").hidden = !!old; $("#f-note-wrap").hidden = !old;
  const sel = $("#f-device");
  sel.replaceChildren(...S.serial.ports.map(p => h("option", { value: p.device, text: `${p.device}  ${p.description}` })));
  if (old && old.device && !S.serial.ports.find(p => p.device === old.device)) sel.append(h("option", { value: old.device, text: old.device }));
  $("#serial-hint").textContent = !S.serial.available ? "USB cables need the pyserial package: python3 -m pip install pyserial" : S.serial.ports.length ? "" : "No serial ports found: is the cable plugged in?";
  form.elements.mode.value = old ? old.mode : "hub";
  $("#f-host").value = old ? old.host || "" : ""; $("#f-port").value = old ? old.port || "" : ""; $("#f-name").value = old ? old.name || "" : "";
  $("#f-note").value = old ? old.note || "" : ""; if (old && old.device) sel.value = old.device;
  const sync = () => {
    const mode = form.elements.mode.value;
    $("#net-fields").hidden = mode === "serial"; $("#serial-fields").hidden = mode !== "serial";
    $("#f-host").placeholder = mode === "hub" ? "the host's address (127.0.0.1 through the tunnel)" : "the controller's bridge address";
  };
  form.querySelectorAll("input[name=mode]").forEach(r => r.onchange = sync); sync();
  $("#connect-cancel").onclick = () => dlg.close();
  form.onsubmit = async ev => {
    ev.preventDefault();
    const mode = form.elements.mode.value;
    const spec = { mode, name: $("#f-name").value.trim() };
    if (mode === "serial") spec.device = $("#f-device").value; else { spec.host = $("#f-host").value.trim(); spec.port = $("#f-port").value.trim(); }
    $("#connect-go").disabled = true; err.hidden = true;
    try {
      if (old) {
        if (mode !== "serial" && (!spec.host || !/^\d+$/.test(spec.port))) throw new Error("A host name or address and a port number are needed.");
        const item = { name: spec.name || (spec.host ? `${spec.host}:${spec.port}` : spec.device), mode };
        for (const k of ["host", "port", "device"]) if (spec[k]) item[k] = k === "port" ? +spec[k] : spec[k];
        if ($("#f-note").value.trim()) item.note = $("#f-note").value.trim();
        await api("/api/presets", { presets: S.presets.map((x, i) => (i === edit ? item : x)) });
        await loadState();
      } else await connect(spec, { save: $("#f-save").checked });
      dlg.close();
    } catch (e) { err.textContent = e.message; err.hidden = false; }
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
  u.badge = h("div", { class: "badge unknown", text: "?" }); u.stateNote = h("div", { class: "hint" }); u.faultText = h("div", { class: "error" }); u.faultWhy = h("div", { class: "faultwhy" });
  u.btnArm = h("button", { class: "btn big", onclick: () => armClicked(conn) }, "ARM");
  u.btnDisarm = h("button", { class: "btn big", onclick: () => send(conn, "DISARM") }, "DISARM");
  u.btnFire = h("button", { class: "btn big danger wide", onclick: () => fire(conn) }, "FIRE");
  u.btnClear = h("button", { class: "btn", onclick: () => send(conn, "FAULT:CLEar") }, "Clear fault");
  u.btnStop = h("button", { class: "btn", onclick: () => send(conn, "SOURce:STOP") }, "Stop");
  u.others = h("div");
  u.recCheck = h("input", { type: "checkbox", onchange: () => recordToggled(conn) }); u.recLine = h("div", { class: "hint reclines" });
  u.opPane = h("div", { class: "pane active" }); u.conPane = h("div", { class: "pane" }); u.dataPane = h("div", { class: "pane" }); u.logPane = h("div", { class: "pane" });
  const tabOp = h("button", { class: "ptab active", onclick: () => pane("op") }, "Operate");
  const tabData = h("button", { class: "ptab", onclick: () => pane("data") }, "Shot data");
  const tabCon = h("button", { class: "ptab", onclick: () => pane("con") }, "Console");
  const tabLog = h("button", { class: "ptab", title: "What the host service logged for this controller: commands, replies, events, notes", onclick: () => pane("log") }, "Host log");
  u.tabData = tabData;
  const pane = which => {
    conn.pane = which;
    for (const [name, tab, el] of [["op", tabOp, u.opPane], ["data", tabData, u.dataPane], ["con", tabCon, u.conPane], ["log", tabLog, u.logPane]]) { tab.classList.toggle("active", which === name); el.classList.toggle("active", which === name); }
    hostLogVisible(conn, which === "log");
    if (which === "log") { /* loaded by hostLogVisible */ }
    else if (which === "data") { tabData.classList.remove("flag"); syncRecordForm(conn); loadShots(conn); }
    else if (which === "con") scrollConsole(conn, true); else refreshSoon(conn, true);
  };
  conn.showPane = pane; conn.pane = "op";
  buildConsole(conn);
  buildShotData(conn);
  buildHostLog(conn);
  return h("div", { class: "view" },
    h("div", { class: "col side" },
      h("div", { class: "card" }, h("div", { class: "row spread" }, u.idTitle, h("button", { class: "btn small", title: "Close this page's connection. What the controller is doing keeps going.", onclick: () => askDisconnect(conn) }, "Disconnect")),
        u.idList, u.idWarn, u.linkBanner),
      h("div", { class: "card" }, h("h3", {}, "State"), h("div", { class: "state" }, u.badge, u.faultText), u.faultWhy, u.stateNote,
        h("div", { class: "runbtns" }, u.btnArm, u.btnDisarm, u.btnFire), h("div", { class: "row", style: "margin-top:10px" }, u.btnClear, u.btnStop),
        h("label", { class: "check rec" }, u.recCheck, " Record data for the next shot"), u.recLine),
      u.others),
    h("div", { class: "col" }, h("div", { class: "panel-tabs" }, tabOp, tabData, tabCon, tabLog), u.opPane, u.dataPane, u.conPane, u.logPane));
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
  u.faultWhy.replaceChildren(...(st === "FAULT" && conn.panel && conn.panel.faultWhy ? conn.panel.faultWhy() : []));
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
/* A titled block of the Operate tab. It folds; whether it was open is remembered on this computer. sectionC(...).sum(text) sets the
   one-line summary shown next to the title, so a folded block still says what matters. */
function sectionC(title, step, opts, ...kids) {
  const key = "wham.sec." + (opts.key || title);
  let open = opts.open !== false;
  try { const saved = localStorage.getItem(key); if (saved != null) open = saved === "1"; } catch (e) { /* private window */ }
  const sum = h("span", { class: "sum" });
  const d = h("details", { class: "section", open }, h("summary", {}, h("h3", {}, step ? h("span", { class: "step", text: step }) : null, title), sum), h("div", { class: "body" }, ...kids));
  d.addEventListener("toggle", () => { try { localStorage.setItem(key, d.open ? "1" : "0"); } catch (e) { /* ignore */ } if (d.open && opts.onOpen) opts.onOpen(); });
  d.sum = text => { sum.textContent = text || ""; };
  return d;
}
function section(title, step, ...kids) { return sectionC(title, step, {}, ...kids); }

/* ---------- the console ---------- */
const QUICK = {
  common: [["Who are you?", "*IDN?"], ["State", "STATE?"], ["Faults", "FAULT?"], ["Fault sources", "FAULT:SOURces?"], ["Gate-driver inputs", "GDS?"], ["Board identity", "SYS:ID?"], ["Saved profiles", "CONFig:PROFile:CATalog?"], ["Dead time", "CONFig:DEADtime?"], ["Fault bypass", "DEBUG:FAULT:BYPASS?"], ["Serial link health", "DIAGnostic:UART?"]],
  hbridge: [["Supply mode", "CONFig:MODE?"], ["Limits", "CONFig:LIMits?"], ["Phases", "PHASe:ENAble?"], ["Shot", "SHOT?"], ["Trigger", "EXTernal:TRIGger:STATus?"], ["Start latency", "EXTernal:TRIGger:LATency?"], ["Charger", "CHARGE:STATus?"], ["Last fault pins", "GDS:LAST?"], ["Gate-driver polarity", "GDS:POLarity?"], ["Gate-driver on/off", "GDS:ENAble?"], ["Gate-driver names", "GDS:NICKname?"], ["Spark-gap input", "SPARKgap:INPut?"], ["Spark-gap on/off", "SPARKgap:ENAble?"]],
  xrex: [["Shot timing", "SHOT:TIMing?"], ["Channel 1 status", "SOURce:STATus? 1"], ["Channel 1 fault pins", "XREX:CHANnel:STATus? 1"], ["Interlock", "EXTernal:ENAble?"], ["Trigger", "EXTernal:TRIGger?"], ["Event log", "SYS:EVLOG?"]],
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
  download(`wham-console-${(conn.status.ident && conn.status.ident.serial) || conn.id}-${new Date().toISOString().replace(/[:.]/g, "-")}.txt`, text);
}

/* ---------- shared by both products: saved profiles and the dead time ---------- */
/* Settings in a profile that change a protection, a limit or what drives: shown in red before a recall. The names are the firmware's own
   (settings_codec.c): HBRIDGE GDS:ENAble, GDS:POLarity, SPARKgap:ENAble, PHASe:ENAble, DEADtime; XREX Polarity.<input>, MAXFREQHz, SLEWRate,
   FaultRampTime, ch<n>.MaxCurrent and the two board enable outputs GPOut, PWMAlt. */
const PROT_WORDS = /^(ch\d+\.)?(GDS:(ENAble|POLarity)|SPARK|PHASe:ENAble|DEADtime|CONFig:DEADtime|Polarity\.|MAXFREQHz|SLEWRate|FaultRampTime|MaxCurrent|GPOut|PWMAlt)/i;
function buildProfiles(conn, after) {
  const box = h("div", { class: "slots" }), msg = h("div");
  const idle = () => conn.status.state === "IDLE";
  const X = { slots: [], supported: true };
  const say = (cls, text) => msg.replaceChildren(h("div", { class: "banner " + cls, text }));
  async function save(n, used) {
    const label = await textDialog({ title: `Save the current settings into slot ${n}`, ok: "Save", placeholder: "a short name, for example ech-200kW",
      hint: (used ? `Slot ${n} is in use (${used}): saving replaces it. ` : "") + "Everything set on the controller now is saved: the supply, the shot, the phases, the dead time and the fault-input settings. The name is optional: 1 to 15 letters, digits or - _ . without spaces.",
      validate: t => (t && !/^[A-Za-z0-9_.+-]{1,15}$/.test(t) ? "Use 1 to 15 letters, digits or - _ . + without spaces (or leave it empty)." : t === "-" ? "A single - is not a name." : null) });
    if (label == null) return;
    const r = await send(conn, `*SAV ${n}${label ? " " + label : ""}`);
    if (r.ok) say("ok", `Saved into slot ${n}${label ? ` as "${label}"` : ""}.`);
    await X.refresh();
  }
  async function diff(n) {
    const r = (await query(conn, [`CONFig:PROFile:DIFF? ${n}`]))[0], v = okval(r);
    if (v == null) { say("err", `Could not compare slot ${n}: ${explain(r.reply || r.error)}`); return null; }
    return v === "SAME" ? [] : v.split(/\s+/);
  }
  const diffList = items => h("ul", { class: "diff" }, ...items.map(t => { const m = /^([^=]+)=(.*)>(.*)$/.exec(t); return h("li", { class: PROT_WORDS.test(t) ? "prot" : "" }, m ? `${m[1]}: ${m[2]} → ${m[3]}` : t); }));
  async function compare(n) {
    const d = await diff(n); if (d == null) return;
    msg.replaceChildren(h("div", { class: "banner " + (d.length ? "warn" : "ok") }, d.length ? `Recalling slot ${n} would change ${d.length} setting${d.length === 1 ? "" : "s"} (now → saved):` : `Slot ${n} is the same as what is set now.`, d.length ? diffList(d) : null));
  }
  async function recall(n, name) {
    const d = await diff(n); if (d == null) return;
    if (!d.length) { say("ok", `Slot ${n} is the same as what is set now: nothing to recall.`); return; }
    const prot = d.filter(t => PROT_WORDS.test(t));
    const ok = await askDialog({ title: `Recall slot ${n}${name ? ` (${name})` : ""}?`, yes: "Recall", danger: prot.length > 0, command: `*RCL ${n}`, body: [
      `This replaces ${d.length} setting${d.length === 1 ? "" : "s"} on the controller with the saved one${d.length === 1 ? "" : "s"} (now → saved). A table marked * has the same length but different points.`, diffList(d),
      prot.length ? h("div", { class: "banner err", text: "The lines in red change a protection or a limit: which fault inputs are on and their polarity, the dead time, which phases drive, a maximum current or frequency, the slew or fault-ramp limit, or one of the board's enable outputs. Check them before you recall." }) : null] });
    if (!ok) return;
    const r = await send(conn, `*RCL ${n}`);
    if (r.ok) {
      const kv = Object.fromEntries((r.reply || "").split(/\s+/).filter(t => t.includes("=")).map(t => t.split("=")));
      const warn = [+kv.FAILED > 0 ? `${kv.FAILED} value(s) were refused: ${(r.reply.split("FAILED=" + kv.FAILED)[1] || "").replace(/\b(GDSOFF|SPARKOFF|IGNORED)=\S+/g, "").trim() || "see the Console"}` : null,
        kv.GDSOFF ? `${kv.GDSOFF} gate-driver fault input(s) are now switched OFF` : null, kv.SPARKOFF ? "the spark-gap fault input is now switched OFF" : null].filter(Boolean);
      say(warn.length ? "warn" : "ok", `Recalled slot ${n}: ${kv.APPLIED || "?"} settings applied.` + (warn.length ? " " + warn.join("; ") + "." : ""));
    }
    await X.refresh(); if (after) await after();
  }
  async function clear(n, name) {
    if (!(await askDialog({ title: `Erase slot ${n}${name ? ` (${name})` : ""}?`, body: "The saved profile is removed from the controller's memory. The settings in use now are not changed.", yes: "Erase", danger: true, command: `CONFig:PROFile:CLEar ${n}` }))) return;
    const r = await send(conn, `CONFig:PROFile:CLEar ${n}`);
    if (r.ok) say("ok", `Slot ${n} erased.`);
    await X.refresh();
  }
  X.paint = () => {
    if (!X.supported) { box.replaceChildren(h("div", { class: "hint", text: "This controller's firmware has no saved profiles." })); return; }
    box.replaceChildren(...X.slots.map(([n, v]) => {
      const bad = v === "corrupt", empty = v === "empty" || bad, [label, fw] = empty ? ["", ""] : v.split("/"), name = label && label !== "-" ? label : "";
      return h("div", { class: "slot" + (empty ? " empty" : "") },
        h("div", { class: "name" }, `${n}: `, bad ? h("span", { class: "error", text: "damaged" }) : empty ? h("span", { class: "hint", text: "empty" }) : name || "(no name)"),
        bad ? h("div", { class: "hint", text: "The saved data does not check out and cannot be recalled. Save here again, or erase it." }) : null,
        empty ? null : h("div", { class: "hint", text: fw && fw !== "-" ? `saved by firmware ${fw}` : "" }),
        h("div", { class: "acts" },
          h("button", { class: "btn small", disabled: !idle(), title: idle() ? "" : "Only while the controller is IDLE", onclick: () => save(n, empty ? null : name || "no name") }, empty ? "Save here" : "Replace"),
          empty ? null : h("button", { class: "btn small", onclick: () => compare(n) }, "Compare"),
          empty ? null : h("button", { class: "btn small", disabled: !idle(), title: idle() ? "" : "Only while the controller is IDLE", onclick: () => recall(n, name) }, "Recall"),
          empty && !bad ? null : h("button", { class: "btn small", disabled: !idle(), onclick: () => clear(n, bad ? "damaged" : name) }, "Erase")));
    }));
  };
  X.refresh = async () => {
    const r = (await query(conn, ["CONFig:PROFile:CATalog?"]))[0], v = okval(r);
    if (v == null) { X.supported = !(r.reply && r.reply.startsWith("ERR 1")); if (X.supported) return; }
    else { X.supported = true; X.slots = v.split(/\s+/).map(t => { const i = t.indexOf("="); return [+t.slice(0, i), t.slice(i + 1)]; }).filter(x => x[0]); }
    X.paint();
    if (X.sec) X.sec.sum(X.supported ? `${X.slots.filter(x => x[1] !== "empty" && x[1] !== "corrupt").length} of ${X.slots.length} slots used` + (X.slots.some(x => x[1] === "corrupt") ? "; one damaged" : "") : "not available");
  };
  X.sec = sectionC("Saved profiles", "", { open: false, onOpen: () => X.refresh() },
    h("p", { class: "hint" }, "Eight slots in the controller's own memory keep a complete set-up (settings, the shot and its tables, fault-input settings). Nothing is recalled by itself, not even at power-up. Save, Recall and Erase work only while the controller is IDLE."),
    msg, box);
  return X;
}
function buildDeadtime(conn) {
  const inp = h("input", { type: "number", min: 50, max: 3000, step: 1, style: "width:100px", "aria-label": "Dead time in nanoseconds" }), now = h("span", { class: "hint" });
  const D = { value: null };
  D.row = h("div", { class: "row" }, h("label", {}, "Dead time ", inp, " ns"),
    h("button", { class: "btn small", onclick: async () => { const v = Math.round(+inp.value); if (!(v >= 50 && v <= 3000)) { toast("Enter a dead time from 50 to 3000 ns.", true); return; } await send(conn, `CONFig:DEADtime ${v}`); delete inp.dataset.touched; await D.refresh(); } }, "Set"), now);
  inp.addEventListener("input", () => { inp.dataset.touched = "1"; });
  D.note = h("p", { class: "hint" }, "The pause between one switch of a phase turning off and the other turning on, the same on every phase. The timer counts in steps of 5.9 ns, so the value in force can differ slightly from what was asked. 100 ns at every power-up; changing it asks for confirmation; not possible while FIRING.");
  D.refresh = async () => {
    const r = (await query(conn, ["CONFig:DEADtime?"]))[0], v = okval(r);
    D.value = v;
    if (v == null) { now.textContent = r.reply && r.reply.startsWith("ERR 1") ? "This firmware has a fixed dead time (100 ns)." : ""; inp.disabled = !!(r.reply && r.reply.startsWith("ERR 1")); return; }
    now.textContent = `in force: ${v} ns`;
    if (document.activeElement !== inp && !inp.dataset.touched) inp.value = v;
  };
  return D;
}

/* ---------- HBRIDGE panel ---------- */
const HB_PRESETS = {
  flat: { label: "Flat: mid-range, 10 ms", build: lim => { const f = clampF(lim, 100), d = mid(lim, "duty", 50); return [{ t: 0, f, u: d, v: d, w: d }, { t: 10, f, u: d, v: d, w: d }]; } },
  ramp: { label: "Duty ramp up, 20 ms", build: lim => { const f = clampF(lim, 100), a = lim ? lim.d0 + (lim.d1 - lim.d0) * 0.25 : 25, b = lim ? lim.d0 + (lim.d1 - lim.d0) * 0.75 : 75; return [{ t: 0, f, u: a, v: a, w: a }, { t: 20, f, u: b, v: b, w: b }]; } },
  perphase: { label: "Three different duty profiles (example)", build: lim => { const f = clampF(lim, 100), c = x => (lim ? Math.min(lim.d1, Math.max(lim.d0, x)) : x); return [{ t: 0, f, u: c(20), v: c(80), w: c(50) }, { t: 5, f, u: c(50), v: c(50), w: c(90) }, { t: 10, f, u: c(80), v: c(20), w: c(50) }]; } },
  gap: { label: "Two pulses with an output-off gap", build: lim => { const f = clampF(lim, 100), d = mid(lim, "duty", 50); return [{ t: 0, f, u: d, v: d, w: d }, { t: 4, f, u: d, v: d, w: d }, { t: 4.01, f: 0, u: 0, v: 0, w: 0, off: true }, { t: 8, f, u: d, v: d, w: d }, { t: 12, f, u: d, v: d, w: d }]; } },
};
function clampF(lim, kHz) { if (!lim) return kHz; return Math.min(lim.f1 / 1000, Math.max(lim.f0 / 1000, kHz)); }
function mid(lim, what, dflt) { return lim ? Math.round((lim.d0 + lim.d1) / 2 * 100) / 100 : dflt; }
const fmt = (x, n) => String(+(+x).toFixed(n));
const isOff = r => !!r.off || r.f === 0;
/* One setpoint as the controller reports it (SHOT:POINt?): "t_us f_hz duty" or "t_us f_hz dU dV dW"; 0 Hz is an off setpoint. */
function pointToRow(text) {
  const v = (text || "").split(/\s+/).map(Number);
  if (v.length < 3 || v.some(x => !Number.isFinite(x))) return null;
  const off = v[1] === 0, d = v.length >= 5 ? v.slice(2, 5) : [v[2], v[2], v[2]];
  return { t: v[0] / 1000, f: v[1] / 1000, u: off ? 0 : d[0], v: off ? 0 : d[1], w: off ? 0 : d[2], off };
}
/* Do two setpoints say the same thing, to the resolution the controller keeps (1 us, 0.001 Hz, 0.01 %)? */
function sameRow(a, b) {
  if (Math.round(a.t * 1000) !== Math.round(b.t * 1000) || isOff(a) !== isOff(b)) return false;
  if (isOff(a)) return true;
  const close = (x, y, tol) => Math.abs(x - y) <= Math.max(tol, Math.abs(x) * 1e-5);
  return close(a.f * 1000, b.f * 1000, 0.011) && close(a.u, b.u, 0.011) && close(a.v, b.v, 0.011) && close(a.w, b.w, 0.011);
}
const rowText = r => (isOff(r) ? `${fmt(r.t, 3)} ms: output off` : `${fmt(r.t, 3)} ms: ${fmt(r.f * 1000, 3)} Hz, duty ${r.u === r.v && r.v === r.w ? fmt(r.u, 2) : [r.u, r.v, r.w].map(x => fmt(x, 2)).join("/")} %`);
const stepCommand = r => (isOff(r) ? `SHOT:STEP ${Math.round(r.t * 1000)} 0 0` : `SHOT:STEP ${Math.round(r.t * 1000)} ${fmt(r.f * 1000, 3)} ${fmt(r.u, 2)} ${fmt(r.v, 2)} ${fmt(r.w, 2)}`);

function buildHbridge(conn) {
  const P = { rows: HB_PRESETS.flat.build(null), limits: null, mode: null, phases: { U: 1, V: 1, W: 1 }, phaseWant: null, perPhase: false, loaded: null, held: null, ticks: 0, src: null, bypass: null, pristine: true };
  const el = h("div");
  const state = () => conn.status.state;
  /* The table is "checked" when it was compared with the controller's shot, found identical, and neither has changed since
     (the controller's side is judged by its setpoint count and length; a recall or a supply change drops the mark). */
  const tableSig = () => JSON.stringify(P.rows.map(r => (isOff(r) ? [Math.round(r.t * 1000)] : [Math.round(r.t * 1000), r.f, r.u, r.v, r.w])));
  const markChecked = () => { P.checked = P.held ? { sig: tableSig(), n: P.rows.length, us: Math.round(P.rows[P.rows.length - 1].t * 1000) } : null; };
  const isChecked = () => !!(P.checked && P.held && P.held.n === P.checked.n && P.held.us === P.checked.us && tableSig() === P.checked.sig);

  // 1 supply
  const segLim = h("button", { onclick: () => setMode("LIMiter") }, "Limiter"), segHv = h("button", { onclick: () => setMode("HVPS") }, "HVPS");
  const modeNote = h("div", { class: "hint" });
  async function setMode(m) {
    if (P.held && P.held.n > 0 && !(await askDialog({ title: `Switch the supply to ${m === "HVPS" ? "HVPS" : "Limiter"}?`, body: `The controller holds a shot of ${P.held.n} setpoints. Changing the supply discards it.`, yes: "Switch", danger: false }))) return;
    const r = await send(conn, `CONFig:MODE ${m}`); if (r.ok) { P.checked = null; toast(`Supply set to ${m === "HVPS" ? "HVPS" : "Limiter"}.`); } await P.refresh();
  }
  // 2 phases
  const chips = {};
  for (const p of "UVW") chips[p] = h("button", { class: "toggle on", onclick: () => { P.phaseWant = P.phaseWant || { ...P.phases }; P.phaseWant[p] = P.phaseWant[p] ? 0 : 1; paintPhases(); } }, p);
  const phaseApply = h("button", { class: "btn small", onclick: async () => { const sel = "UVW".split("").filter(p => P.phaseWant[p]).join(""); if (!sel) { toast("Choose at least one phase.", true); return; } const r = await send(conn, `PHASe:SELect ${sel}`); if (r.ok) P.phaseWant = null; await P.refresh(); } }, "Apply");
  const phaseUndo = h("button", { class: "btn small quiet", onclick: () => { P.phaseWant = null; paintPhases(); } }, "Undo");
  function paintPhases() { const w = P.phaseWant || P.phases; for (const p of "UVW") chips[p].className = "toggle " + (w[p] ? "on" : "off") + " " + p.toLowerCase(); phaseApply.disabled = !P.phaseWant || state() !== "IDLE"; phaseApply.hidden = phaseUndo.hidden = !P.phaseWant; }

  // 3 shot
  const tbody = h("tbody"), thead = h("thead"), shotMsg = h("div"), loadedMsg = h("div", { class: "loaded" }), heldMsg = h("div", { class: "hint" });
  const canvas = h("canvas", { class: "chart", width: 1000, height: 330 });
  const presetSel = h("select", { "aria-label": "Start from an example", onchange: () => { if (!presetSel.value) return; setRows(HB_PRESETS[presetSel.value].build(P.limits), null); presetSel.value = ""; } },
    h("option", { value: "", text: "Start from an example…" }), ...Object.entries(HB_PRESETS).map(([k, v]) => h("option", { value: k, text: v.label })));
  const perPhaseBox = h("input", { type: "checkbox", onchange: () => { P.perPhase = perPhaseBox.checked; if (!P.perPhase) for (const r of P.rows) r.v = r.w = r.u; drawTable(); } });
  const fileInput = h("input", { type: "file", accept: ".csv,.txt,text/csv,text/plain", hidden: true, onchange: async () => { const f = fileInput.files[0]; fileInput.value = ""; if (f) loadText(await f.text(), f.name); } });
  const uploadBtn = h("button", { class: "btn primary", onclick: () => upload() }, "Send shot to controller");
  function setRows(rows, loaded) {
    P.rows = rows.map(r => ({ t: r.t, f: r.f, u: r.u, v: r.v, w: r.w, off: isOff(r) }));
    P.perPhase = P.rows.some(r => !isOff(r) && (r.u !== r.v || r.v !== r.w)); perPhaseBox.checked = P.perPhase;
    P.loaded = loaded; P.shotResult = null; P.pristine = false; drawTable();
  }
  async function loadText(text, name) {
    try {
      const r = await api("/api/shot/parse", { text, name });
      setRows(r.rows, { summary: r.summary, notes: r.notes });
      toast(`Loaded ${r.rows.length} setpoints. Check them, then send.`);
    } catch (e) { toast("Could not read that as a shot: " + e.message, true); }
  }
  async function pasteShot() {
    let parsed = null;
    const text = await textDialog({ title: "Paste a shot", multiline: true, ok: "Load", placeholder: "SHOT:STEP 0 20086 50\nSHOT:STEP 1000 20117 50\n…",
      hint: "Paste SHOT:STEP lines (as in a logbook entry), the rows of a profile CSV, or plain rows of: time in µs, frequency in Hz, duty in % (one duty, or three for U V W). 0 Hz is an output-off setpoint.",
      validate: async t => { try { parsed = await api("/api/shot/parse", { text: t, name: "pasted" }); return null; } catch (e) { return e.message; } } });
    if (text == null || !parsed) return;
    setRows(parsed.rows, { summary: parsed.summary, notes: parsed.notes });
    toast(`Loaded ${parsed.rows.length} setpoints. Check them, then send.`);
  }
  function cell(row, key) {
    const i = h("input", { type: "number", step: "any", value: Number.isFinite(row[key]) ? row[key] : "", "aria-label": key === "t" ? "time in ms" : key === "f" ? "frequency in kHz" : `duty ${key.toUpperCase()} in percent` });
    i.addEventListener("input", () => { row[key] = i.value === "" ? NaN : +i.value; if (key === "u" && !P.perPhase) row.v = row.w = row.u; P.shotResult = null; P.pristine = false; validate(); drawChart(); });
    i.dataset.key = key; return i;
  }
  function drawTable() {
    thead.replaceChildren(h("tr", {}, h("th", { text: "#" }), h("th", { text: "Time (ms)" }), h("th", { class: "fq", text: "Frequency (kHz)" }),
      ...(P.perPhase ? [h("th", { class: "u", text: "Duty U (%)" }), h("th", { class: "v", text: "Duty V (%)" }), h("th", { class: "w", text: "Duty W (%)" })] : [h("th", { text: "Duty (%)" })]),
      h("th", { title: "An off setpoint: the output is off from this time until the next setpoint", text: "Off" }), h("th")));
    tbody.replaceChildren(...P.rows.map((row, idx) => {
      const offBox = h("input", { type: "checkbox", checked: isOff(row), disabled: idx === 0, "aria-label": `Setpoint ${idx + 1}: output off`, title: idx === 0 ? "The first setpoint cannot be off" : "Output off from this time until the next setpoint",
        onchange: () => { row.off = offBox.checked; if (row.off) { row.f = 0; row.u = row.v = row.w = 0; } else { const src = P.rows.slice(0, idx).reverse().find(r => !isOff(r)) || P.rows.find(r => !isOff(r)); row.f = src ? src.f : clampF(P.limits, 100); row.u = src ? src.u : mid(P.limits, "duty", 50); row.v = src ? src.v : row.u; row.w = src ? src.w : row.u; } P.shotResult = null; P.pristine = false; drawTable(); } });
      return h("tr", { class: isOff(row) ? "off" : "" }, h("td", { class: "n", text: String(idx + 1) }),
        h("td", {}, cell(row, "t")), h("td", {}, cell(row, "f")), ...(P.perPhase ? ["u", "v", "w"] : ["u"]).map(k => h("td", {}, cell(row, k))),
        h("td", { class: "offcell" }, h("label", { class: "chk" }, offBox, isOff(row) ? " off" : "")),
        h("td", {}, h("button", { class: "btn small", title: "Remove this setpoint", "aria-label": `Remove setpoint ${idx + 1}`, disabled: P.rows.length <= 2, onclick: () => { P.rows.splice(idx, 1); P.shotResult = null; P.pristine = false; drawTable(); } }, "×")));
    }));
    validate(); drawChart();
  }
  function validate() {
    const msgs = [], lim = P.limits, rows = P.rows;
    const flag = (idx, key, bad) => { const inp = tbody.children[idx] && tbody.children[idx].querySelector(`input[data-key=${key}]`); if (inp) inp.classList.toggle("bad", bad); };
    rows.forEach((r, i) => {
      const bt = !Number.isFinite(r.t) || r.t < 0 || (i === 0 && r.t !== 0) || (i > 0 && r.t <= rows[i - 1].t) || r.t > 1000;
      if (i === 0 && r.t !== 0) msgs.push("The first setpoint must be at time 0.");
      if (i > 0 && r.t <= rows[i - 1].t) msgs.push(`Setpoint ${i + 1}: the time must be later than the one before.`);
      if (r.t > 1000) msgs.push("A shot can be at most 1000 ms long.");
      flag(i, "t", bt);
      if (isOff(r)) { if (i === 0) msgs.push("The first setpoint cannot be an off one."); if (i >= rows.length - 2) msgs.push("The last two setpoints must be on: after a gap, the shot needs a setpoint to turn back on and one to end."); return; }
      const bf = !Number.isFinite(r.f) || (lim && (r.f * 1000 < lim.f0 - 1e-6 || r.f * 1000 > lim.f1 + 1e-6));
      if (bf) msgs.push(lim && Number.isFinite(r.f) ? `Setpoint ${i + 1}: frequency ${fmt(r.f, 3)} kHz is outside ${lim.f0 / 1000}-${lim.f1 / 1000} kHz.` : `Setpoint ${i + 1}: the frequency is missing.`);
      flag(i, "f", bf);
      for (const k of P.perPhase ? ["u", "v", "w"] : ["u"]) { const bd = !Number.isFinite(r[k]) || (lim && (r[k] < lim.d0 - 1e-9 || r[k] > lim.d1 + 1e-9)); if (bd) msgs.push(lim && Number.isFinite(r[k]) ? `Setpoint ${i + 1}: duty ${P.perPhase ? k.toUpperCase() + " " : ""}${fmt(r[k], 2)} % is outside ${lim.d0}-${lim.d1} %.` : `Setpoint ${i + 1}: a duty is missing.`); flag(i, k, bd); }
    });
    if (rows.length < 2) msgs.push("A shot needs at least two setpoints.");
    if (rows.length > 500) msgs.push(`${rows.length} setpoints: the controller holds at most 500.`);
    if (!lim) msgs.push("Choose the supply (Limiter or HVPS) first, so the limits are known.");
    const uniq = [...new Set(msgs)];
    P.problems = uniq;
    shotMsg.replaceChildren(...uniq.slice(0, 6).map(m => h("div", { class: "banner warn", text: m })), uniq.length > 6 ? h("div", { class: "hint", text: `…and ${uniq.length - 6} more.` }) : "", P.shotResult ? P.shotResult : "");
    loadedMsg.replaceChildren(...(P.loaded ? [h("div", { class: "banner" }, h("b", {}, "Loaded, not sent yet: "), P.loaded.summary), ...P.loaded.notes.map(n => h("div", { class: "banner warn", text: n }))] : []));
    uploadBtn.disabled = uniq.length > 0 || state() !== "IDLE" || P.sending;
    uploadBtn.title = state() !== "IDLE" ? "The controller must be IDLE to receive a shot." : "";
    if (P.secShot) P.secShot.sum(`${rows.length} setpoints, ${fmt(rows[rows.length - 1].t || 0, 3)} ms in the table` + (P.held ? `; the controller holds ${P.held.n ? `${P.held.n} setpoints, ${fmt(P.held.us / 1000, 3)} ms` + (isChecked() ? " (the same shot)" : "") : "no shot"}` : ""));
    heldMsg.replaceChildren(...(!P.held ? [] : P.held.n === 0 ? ["The controller holds no shot."]
      : P.held.kind === "TABLE" ? [`The controller holds a TABle upload (${P.held.n} entries), not a shot of setpoints.`]
      : isChecked() ? [h("span", { class: "okmark", text: "\u2713 " }), `The controller holds this shot: ${P.held.n} setpoints, ${fmt(P.held.us / 1000, 3)} ms (checked by reading it back).`]
      : [`The controller holds a shot of ${P.held.n} setpoints, ${fmt(P.held.us / 1000, 3)} ms. It has not been compared with this table. `,
         h("button", { class: "btn link", onclick: loadFromController }, "Show it in the table"), " or ", h("button", { class: "btn link", onclick: compareWithController }, "compare")]));
  }
  /* Two panels on one time axis: frequency above, duty below. Drawn the way the controller plays it: straight lines between on
     setpoints; an on setpoint followed by an off one holds its value until the gap; nothing in a gap (shaded). */
  function drawChart() {
    const c = canvas, g = c.getContext("2d"), W = c.width, H = c.height, pad = { l: 66, r: 14, t: 10, b: 28 }, mid_ = 14;
    const cs = getComputedStyle(document.documentElement), col = n => cs.getPropertyValue(n).trim();
    g.clearRect(0, 0, W, H);
    const rows = P.rows.filter(r => Number.isFinite(r.t)), on = rows.filter(r => !isOff(r));
    const tmax = Math.max(1e-3, ...rows.map(r => r.t));
    const ph = (H - pad.t - pad.b - mid_) / 2, X = t => pad.l + (W - pad.l - pad.r) * t / tmax;
    const range = (vals, lo, hi) => { const a = vals.filter(Number.isFinite); let mn = Math.min(...a), mx = Math.max(...a); if (!a.length) { mn = lo; mx = hi; } if (mx - mn < 1e-9) { const p = Math.max(Math.abs(mx) * 0.01, 0.05); mn -= p; mx += p; } else { const p = (mx - mn) * 0.12; mn -= p; mx += p; } return [mn, mx]; };
    const panels = [
      { top: pad.t, keys: [["f", col("--freq")]], unit: "kHz", name: "Frequency", rng: range(on.map(r => r.f), 0, 1), dec: 3 },
      { top: pad.t + ph + mid_, keys: P.perPhase ? [["u", col("--pu")], ["v", col("--pv")], ["w", col("--pw")]] : [["u", col("--ink")]], unit: "%", name: "Duty", rng: range(on.flatMap(r => (P.perPhase ? [r.u, r.v, r.w] : [r.u])), 0, 100), dec: 1 }];
    g.font = "12px system-ui"; g.lineWidth = 1;
    for (const pn of panels) {
      const [lo, hi] = pn.rng, Y = v => pn.top + ph - ph * (v - lo) / (hi - lo);
      g.strokeStyle = col("--line"); g.fillStyle = col("--muted");
      for (let i = 0; i <= 3; i++) { const v = lo + (hi - lo) * i / 3, y = Y(v); g.beginPath(); g.moveTo(pad.l, y); g.lineTo(W - pad.r, y); g.stroke(); g.fillText(fmt(v, pn.dec) + " " + pn.unit, 4, y + 4); }
      g.fillStyle = col("--muted"); g.fillText(pn.name, pad.l + 6, pn.top + 13);
      // gaps: from an off setpoint to the next setpoint
      rows.forEach((r, i) => { if (isOff(r) && rows[i + 1]) { g.fillStyle = col("--line"); g.globalAlpha = 0.55; g.fillRect(X(r.t), pn.top, X(rows[i + 1].t) - X(r.t), ph); g.globalAlpha = 1; g.fillStyle = col("--muted"); if (X(rows[i + 1].t) - X(r.t) > 26) g.fillText("off", X(r.t) + 4, pn.top + ph - 6); } });
      for (const [k, colr] of pn.keys) {
        g.strokeStyle = colr; g.fillStyle = colr; g.lineWidth = 2; g.beginPath();
        let pen = false;
        rows.forEach((r, i) => {
          if (isOff(r) || !Number.isFinite(r[k])) { pen = false; return; }
          const x = X(r.t), y = Y(r[k]);
          if (!pen) { g.moveTo(x, y); pen = true; } else g.lineTo(x, y);
          if (rows[i + 1] && isOff(rows[i + 1])) g.lineTo(X(rows[i + 1].t), y);       // held until the gap
        });
        g.stroke();
        if (rows.length <= 80) rows.forEach(r => { if (!isOff(r) && Number.isFinite(r[k])) { g.beginPath(); g.arc(X(r.t), Y(r[k]), 2.8, 0, 7); g.fill(); } });
      }
    }
    g.fillStyle = col("--muted");
    for (let i = 0; i <= 5; i++) { const t = tmax * i / 5; g.fillText(fmt(t, 2) + " ms", Math.min(X(t) - 14, W - 54), H - 8); }
  }
  async function readController() {
    const head = okval((await query(conn, ["SHOT?"]))[0]);
    if (head == null) throw new Error("the controller did not answer SHOT?");
    const n = +head.split(" ")[0];
    if (/TABLE/.test(head)) throw new Error("the controller holds a TABle upload, not a SHOT: it cannot be read back as setpoints");
    const rows = [];
    for (let i = 0; i < n; i += 40) {
      const res = await query(conn, Array.from({ length: Math.min(40, n - i) }, (_, k) => `SHOT:POINt? ${i + k}`));
      for (const r of res) { const row = pointToRow(okval(r)); if (!row) throw new Error(`setpoint ${rows.length + 1} could not be read (${r.reply || r.error})`); rows.push(row); }
    }
    return rows;
  }
  async function busyWhile(fn) { conn.busy = true; try { return await fn(); } finally { conn.busy = false; } }
  async function loadFromController() {
    try {
      const rows = await busyWhile(readController);
      if (!rows.length) { toast("The controller holds no shot.", true); return; }
      setRows(rows, null);
      P.held = { n: rows.length, us: Math.round(rows[rows.length - 1].t * 1000), kind: "PROFILE" }; markChecked();
      P.shotResult = h("div", { class: "banner ok", text: `This is the shot the controller holds now: ${rows.length} setpoints, ${fmt(rows[rows.length - 1].t, 3)} ms.` }); validate();
    } catch (e) { toast("Could not read the shot: " + e.message, true); }
  }
  async function compareWithController() {
    try {
      const got = await busyWhile(readController), want = P.rows, diffs = [];
      for (let i = 0; i < Math.max(got.length, want.length); i++) {
        if (!want[i]) diffs.push(`${i + 1}: only in the controller: ${rowText(got[i])}`);
        else if (!got[i]) diffs.push(`${i + 1}: only in the table: ${rowText(want[i])}`);
        else if (!sameRow(want[i], got[i])) diffs.push(`${i + 1}: table ${rowText(want[i])}  |  controller ${rowText(got[i])}`);
      }
      P.shotResult = diffs.length
        ? h("div", { class: "banner err" }, h("b", {}, got.length ? `The controller's shot differs from the table in ${diffs.length} setpoint${diffs.length === 1 ? "" : "s"}:` : "The controller holds no shot."), got.length ? h("ul", { class: "diff" }, ...diffs.slice(0, 30).map(d => h("li", { text: d })), diffs.length > 30 ? h("li", { text: `…and ${diffs.length - 30} more` }) : null) : null)
        : h("div", { class: "banner ok", text: `The controller holds exactly this shot: ${got.length} setpoints, ${fmt(got[got.length - 1].t, 3)} ms.` });
      if (!diffs.length) { P.held = { n: got.length, us: Math.round(got[got.length - 1].t * 1000), kind: "PROFILE" }; markChecked(); } else P.checked = null;
      validate();
    } catch (e) { toast("Could not compare: " + e.message, true); }
  }
  function saveCsv() {
    const lines = ["# WHAM operator console: HBRIDGE shot" + (P.mode ? `, supply ${P.mode}` : ""), "time_us,carrier_hz,duty_u_pct,duty_v_pct,duty_w_pct",
      ...P.rows.map(r => (isOff(r) ? `${Math.round(r.t * 1000)},0,0,0,0` : `${Math.round(r.t * 1000)},${fmt(r.f * 1000, 3)},${fmt(r.u, 2)},${fmt(r.v, 2)},${fmt(r.w, 2)}`))];
    download(`wham-shot-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-")}.csv`, lines.join("\n") + "\n", "text/csv");
  }
  async function copyCommands() {
    const text = ["SHOT:BEGin", ...P.rows.map(stepCommand), "SHOT:END"].join("\n") + "\n";
    try { await navigator.clipboard.writeText(text); toast("The SHOT commands are on the clipboard."); }
    catch (e) { await textDialog({ title: "The shot as commands", multiline: true, value: text, ok: "Close", hint: "Select all and copy." }); }
  }
  async function upload() {
    if (P.problems.length) return;
    if (P.held && P.held.n > 0 && !isChecked() && !(await askDialog({ title: "Replace the shot in the controller?", body: `The controller holds a shot of ${P.held.n} setpoints, ${fmt(P.held.us / 1000, 3)} ms (perhaps sent by someone else or by LabVIEW). Sending replaces it with the ${P.rows.length} setpoints in the table.`, yes: "Replace it", danger: false }))) return;
    P.sending = true; uploadBtn.disabled = true; conn.busy = true; P.shotResult = null;
    const fail = (what, reply) => { P.shotResult = h("div", { class: "banner err", text: `${what}: ${explain(reply)}` }); };
    const progress = h("div", { class: "banner", text: "Sending…" }); shotMsg.replaceChildren(progress);
    try {
      let r = await send(conn, "SHOT:BEGin", { quiet: true });
      if (!r.ok) return fail("The controller refused to start a new shot", r.reply || r.error);
      for (let i = 0; i < P.rows.length; i++) {
        r = await send(conn, stepCommand(P.rows[i]), { quiet: true });
        if (!r.ok) return fail(`Setpoint ${i + 1} was refused (nothing is loaded: the upload was abandoned)`, r.reply || r.error);
        if (i % 10 === 9) progress.textContent = `Sending… ${i + 1} of ${P.rows.length}`;
      }
      r = await send(conn, "SHOT:END", { quiet: true });
      if (!r.ok) return fail("The controller refused to finish the shot", r.reply || r.error);
      // read the shot back and compare with what was sent: that, not the OK replies, is the proof of what it holds
      progress.textContent = "Reading it back…";
      const got = await readController();
      const bad = got.length !== P.rows.length ? `the controller reports ${got.length} setpoints but ${P.rows.length} were sent` : (() => { const i = P.rows.findIndex((w, k) => !sameRow(w, got[k])); return i < 0 ? null : `setpoint ${i + 1} reads back as "${rowText(got[i])}"`; })();
      if (bad) return fail("Read-back does not match. Do not arm; send it again", "ERR " + bad);
      const st = okval((await query(conn, ["STATE?"]))[0]);
      P.loaded = null; P.held = { n: got.length, us: Math.round(got[got.length - 1].t * 1000), kind: "PROFILE" }; markChecked();
      P.shotResult = h("div", { class: "banner " + (st && st !== "IDLE" ? "warn" : "ok"), text: `Shot loaded and checked by reading it back: ${P.rows.length} setpoints, ${fmt(P.rows[P.rows.length - 1].t, 3)} ms.` + (st && st !== "IDLE" ? ` Note: the controller is ${st} (someone armed it during the upload).` : " ARM, then FIRE, when ready.") });
    } catch (e) { fail("The upload did not finish", "ERR " + e.message); }
    finally { P.sending = false; conn.busy = false; validate(); refreshSoon(conn, true); }
  }
  // control loops
  const loopBtns = [0, 1, 2].map(i => h("button", { class: "toggle", onclick: async () => { await send(conn, `PID:LOOPMODE ${i + 1} ${P.loops && P.loops[i] ? 0 : 1}`); await P.refresh(); } }));
  // trigger
  const trigBtn = h("button", { class: "toggle", onclick: async () => { const on = P.trig && P.trig.enabled === "1"; await send(conn, `EXTernal:TRIGger ${on ? 0 : 1}`); await P.refresh(); } });
  const trigInfo = h("div", { class: "hint" }), trigWarn = h("div"), latInfo = h("div", { class: "hint" });
  // charger
  const chFreq = h("input", { type: "number", min: 0, max: 100, step: "any", value: 100, "aria-label": "Charger frequency in kilohertz", style: "width:110px" });
  const chInfo = h("div"), chNote = h("div", { class: "hint" });
  const chOn = h("button", { class: "btn primary", onclick: async () => { const hz = Math.round(+chFreq.value * 1000); if (!(hz >= 1 && hz <= 100000)) { toast("Enter a frequency from 0.001 to 100 kHz.", true); return; } await send(conn, `CHARGE:FREQuency ${hz}`); await P.refresh(); } }, "Start / change");
  const chOff = h("button", { class: "btn danger", onclick: async () => { await send(conn, "CHARGE:FREQuency 0"); await P.refresh(); } }, "OFF");
  // protection: fault sources, the bypass, the twelve gate-driver inputs, the spark-gap input
  const srcRow = h("div", { class: "srcrow" }), bypassBox = h("div"), pinGrid = h("div", { class: "pingrid" }), polNote = h("div", { class: "hint" }), sparkRow = h("div", { class: "row" }), protNote = h("div");
  const SRC_NAME = { PC10: "hardware fault input (PC10)", GATEDRV: "gate-driver status input", TABLE: "shot table ran out", TEST: "test fault (GENERAL:TEST:FAULT)", OVP: "overvoltage", FB: "feedback lost", SPARK: "spark-gap input" };
  async function gdsToggle(n, on) { await send(conn, `GDS:ENAble ${n} ${on ? 1 : 0}`); await P.refreshProtection(); refreshSoon(conn, true); }
  async function gdsRename(n, old) {
    const name = await textDialog({ title: `Name for gate-driver input ${n}`, value: old && old !== "-" ? old : "", ok: "Set", placeholder: "for example PhaseU_Hi (empty: no name)",
      hint: "1 to 15 letters, digits or _ . - + without spaces. The name is only a label for people: it changes nothing in the controller's behaviour. It is lost at power-off unless a profile is saved.",
      validate: t => (t && !/^[A-Za-z0-9_.+-]{1,15}$/.test(t) ? "Use 1 to 15 letters, digits or _ . - + without spaces." : null) });
    if (name == null) return;
    await send(conn, `GDS:NICKname ${n} ${name || "-"}`); await P.refreshProtection();
  }
  const deadtime = buildDeadtime(conn);
  const profiles = buildProfiles(conn, async () => { P.checked = null; await P.refresh(); await P.refreshProtection(); });

  const secSupply = section("Choose the supply", "1", h("div", { class: "row" }, h("div", { class: "seg" }, segLim, segHv), modeNote), h("p", { class: "hint" }, "Only possible while the controller is IDLE. Changing the supply discards a loaded shot."));
  const secPhase = section("Which phases drive", "2", h("div", { class: "row" }, ...["U", "V", "W"].map(p => chips[p]), phaseApply, phaseUndo), h("p", { class: "hint" }, "A phase that is switched off has no output. Click the phases, then Apply: that asks for confirmation."));
  P.secShot = section("Build and send the shot", "3",
    h("div", { class: "toolbar" },
      h("button", { class: "btn small", title: "A profile written by the compensation tools (_iterN.csv), any CSV with time, frequency and duty columns, or a file of SHOT:STEP lines", onclick: () => fileInput.click() }, "Load file…"), fileInput,
      h("button", { class: "btn small", onclick: pasteShot }, "Paste…"),
      h("button", { class: "btn small", title: "Put the shot the controller holds now into the table", onclick: loadFromController }, "Read from controller"),
      presetSel, h("span", { class: "sp" }),
      h("button", { class: "btn small", onclick: saveCsv }, "Save as CSV"), h("button", { class: "btn small", onclick: copyCommands }, "Copy as commands")),
    loadedMsg,
    h("div", { class: "shotwrap" }, h("table", { class: "shot" }, thead, tbody)),
    h("div", { class: "toolbar" },
      h("button", { class: "btn small", onclick: () => { const last = P.rows[P.rows.length - 1], prev = P.rows[P.rows.length - 2]; const dt = prev && last.t > prev.t ? last.t - prev.t : 5; P.rows.push({ ...last, t: +(last.t + dt).toFixed(3) }); P.shotResult = null; P.pristine = false; drawTable(); } }, "+ Add setpoint"),
      h("label", { class: "chk hint" }, perPhaseBox, " a different duty for each phase"), h("span", { class: "sp" }),
      h("button", { class: "btn", title: "Read the controller's shot and list every setpoint that differs from this table", onclick: compareWithController }, "Compare with controller"), uploadBtn),
    heldMsg, shotMsg, canvas,
    h("p", { class: "hint" }, "The controller moves frequency and duty in straight lines between setpoints. Tick Off for an output-off setpoint: the output is off from that time until the next setpoint, and the setpoint before it holds its value up to the gap. The first setpoint is at 0 ms; the last two must be on. Values outside the supply's limits are marked and cannot be sent. Sending reads the shot back and compares it."));
  const secRun = section("Run", "4", h("p", {}, "Use ", h("b", {}, "ARM"), " then ", h("b", {}, "FIRE"), " in the State box on the left. Both ask for confirmation. ARM prepares the start, so the output begins within tens of microseconds of FIRE or the trigger."),
    h("div", { class: "row" }, h("span", {}, "External trigger:"), trigBtn), trigInfo, trigWarn, latInfo);
  const secProt = sectionC("Protection: fault inputs", "", { key: "protection", onOpen: () => P.refreshProtection() },
    bypassBox, h("div", { class: "hint", text: "Fault sources (red = latched or active now):" }), srcRow, protNote,
    h("h4", {}, "Gate-driver status inputs"), polNote, pinGrid,
    h("h4", {}, "Spark-gap input"), sparkRow,
    h("p", { class: "hint" }, "A switched-off input is not looked at: it cannot stop the output or latch a fault. Switching an input off asks for confirmation and is not possible while ARMED or FIRING. After a power-up every input is on again, unless a saved profile is recalled."));
  const secSet = sectionC("Settings: dead time, control loop", "", { open: false, key: "settings" }, deadtime.row, deadtime.note,
    h("h4", {}, "Control loop (per phase)"), h("div", { class: "row" }, ...["U", "V", "W"].map((p, i) => h("span", { class: "row tight" }, h("span", { class: p.toLowerCase() }, p), loopBtns[i]))),
    h("p", { class: "hint" }, "Open loop: the phase plays the duty you set in the shot. Closed loop: a PID holds the phase's output voltage to a target using the feedback input; it needs gains (PID:GAINS) and a target (TARGet:*), which are set in the Console for now. The controller logs its waveform (Shot data tab) only for a run with a closed loop; the PFM input capture there works open loop too."));
  const secCharge = sectionC("Capacitor charger (CHARGE)", "", { open: false, key: "charge" }, h("p", { class: "hint" }, "A separate 50 % square wave that drives the frequency-to-voltage converter charging the capacitor bank. It is independent of the controller's state: you can leave it running, and a fault does not stop it."),
    h("div", { class: "row" }, h("label", {}, "Frequency ", chFreq, " kHz"), chOn, chOff, h("button", { class: "btn small", onclick: () => { chFreq.value = 100; } }, "100 kHz")), chInfo, chNote);
  el.append(h("div", { class: "row spread", style: "margin-bottom:10px" }, h("h2", {}, "Operate"), intervalPicker(conn, 5000)),
    secSupply, secPhase, P.secShot, secRun, secProt, profiles.sec, secSet, secCharge);

  P.onStatus = () => {
    const s = state(), editable = s === "IDLE";
    segLim.disabled = segHv.disabled = !editable; validate(); paintPhases(); profiles.paint();
    trigWarn.replaceChildren(s === "ARMED" && P.trig && P.trig.enabled === "1" ? h("div", { class: "banner warn", text: "ARMED with the external trigger ON: a rising edge on the trigger input will start the shot." }) : "");
    if (s === "FAULT" && P.lastState !== "FAULT") { refreshSoon(conn, true); P.refreshProtection(); }
    P.lastState = s;
  };
  /* What the State box shows under FAULT: which sources are latched, and which inputs. */
  P.faultWhy = () => {
    if (!P.src) return [];
    const on = Object.entries(P.src).filter(([, v]) => v === "1").map(([k]) => SRC_NAME[k] || k);
    const pins = P.gds ? P.gds.filter(g => g.fault).map(g => `input ${g.n}${g.nick && g.nick !== "-" ? ` (${g.nick})` : ""}`) : [];
    return [h("div", {}, on.length ? "Cause: " + on.join(", ") + (pins.length ? ` – ${pins.join(", ")} in fault now` : "") + "." : "No fault source is active now: the fault is latched from earlier."),
      h("button", { class: "btn link", onclick: () => { conn.showPane("op"); secProt.open = true; secProt.scrollIntoView({ behavior: "smooth", block: "start" }); } }, "Show the fault inputs")];
  };
  P.refreshProtection = async () => {
    const res = await query(conn, ["GDS?", "GDS:ENAble?", "GDS:NICKname?", "GDS:POLarity?", "GDS:LAST?", "SPARKgap:INPut?", "SPARKgap:ENAble?"]);
    const [raw, en, nick, pol, last, spIn, spEn] = res.map(okval);
    const list = t => (t ? Object.fromEntries(t.split(/\s+/).map(x => { const i = x.indexOf("="); return [+x.slice(0, i), x.slice(i + 1)]; })) : null);
    const R = list(raw), E = list(en), N = list(nick), low = pol === "NORMALLY_LOW";
    if (!R) { pinGrid.replaceChildren(h("div", { class: "hint", text: "The gate-driver inputs could not be read." })); return; }
    P.gds = Array.from({ length: 12 }, (_, i) => { const n = i + 1, high = R[n] === "HIGH", enabled = !E || E[n] === "1"; return { n, high, enabled, nick: N ? N[n] : "-", fault: enabled && pol != null && (low ? high : !high) }; });
    const idleOrFault = ["IDLE", "FAULT"].includes(state());
    polNote.textContent = pol ? `Polarity in force: ${pol === "NORMALLY_HIGH" ? "normally HIGH (a LOW input is a fault)" : "normally LOW (a HIGH input is a fault)"}. An input with nothing wired to it reads LOW.` : "This firmware does not report a polarity.";
    const bits = last ? parseInt(last.split(" ")[0], 16) : NaN;
    const atLatch = P.src && P.src.GATEDRV === "1" && Number.isFinite(bits) && pol ? P.gds.filter(g => g.enabled && (((bits >> (g.n - 1)) & 1) === 1) === low).map(g => `${g.n}${g.nick && g.nick !== "-" ? ` (${g.nick})` : ""}`) : null;
    protNote.replaceChildren(atLatch ? h("div", { class: "banner warn", text: atLatch.length ? `When the gate-driver fault latched, ${atLatch.length === 1 ? "input" : "inputs"} ${atLatch.join(", ")} read as a fault (GDS:LAST? ${last.split(" ")[0]}).` : `A gate-driver fault is latched, but no input that is switched on read as a fault at that moment (GDS:LAST? ${last.split(" ")[0]}).` }) : "");
    pinGrid.replaceChildren(...P.gds.map(g => h("div", { class: "pincard" + (g.fault ? " fault" : "") + (g.enabled ? "" : " offp") },
      h("div", { class: "top" }, h("span", { class: "num", text: `Input ${g.n}` }), h("span", { class: "lvl", text: g.high ? "HIGH" : "LOW" })),
      h("div", { class: "nick", text: g.nick && g.nick !== "-" ? g.nick : "no name", title: g.nick && g.nick !== "-" ? g.nick : "" }),
      h("div", { class: "top" }, h("span", { class: g.fault ? "error" : "hint", text: !g.enabled ? "switched off" : g.fault ? "FAULT" : "healthy" }),
        E ? h("span", { class: "acts" }, h("button", { class: "btn small", title: "Give this input a name", onclick: () => gdsRename(g.n, g.nick) }, "Name"),
          h("button", { class: "toggle " + (g.enabled ? "on" : "off"), disabled: g.enabled && !idleOrFault, title: g.enabled ? "On: this input can stop the output. Click to switch it off (asks first)." : "Off: this input is ignored. Click to switch it on.", onclick: () => gdsToggle(g.n, !g.enabled) }, g.enabled ? "On" : "Off")) : null))));
    const sparkOn = spEn === "1";
    sparkRow.replaceChildren(...(spIn == null ? [h("span", { class: "hint", text: "This controller's firmware has no spark-gap input." })] : [
      h("span", { class: "chip " + (spIn === "0" && sparkOn ? "on" : spIn === "1" ? "good" : ""), text: spIn === "1" ? "reads HIGH (healthy)" : "reads LOW" + (sparkOn ? " (FAULT)" : "") }),
      h("span", {}, "Input:"), h("button", { class: "toggle " + (sparkOn ? "on" : "off"), disabled: sparkOn && !idleOrFault, onclick: async () => { await send(conn, `SPARKgap:ENAble ${sparkOn ? 0 : 1}`); await P.refreshProtection(); refreshSoon(conn, true); } }, sparkOn ? "On" : "Off"),
      h("span", { class: "hint", text: "Normally HIGH; a LOW level stops the output and latches a fault. An unwired input may read LOW." })]));
    const off = P.gds.filter(g => !g.enabled).length;
    secProt.sum([P.bypass === "1" ? "FAULT BYPASS ON" : null, `${P.gds.filter(g => g.fault).length} of 12 inputs in fault`, off ? `${off} switched off` : null, spEn === "0" ? "spark-gap input off" : null].filter(Boolean).join("; "));
    if (state() === "FAULT") updateStatus(conn);
  };
  P.refresh = async () => {
    const res = await query(conn, ["CONFig:MODE?", "CONFig:LIMits?", "PHASe:ENAble?", "EXTernal:TRIGger:STATus?", "CHARGE:STATus?", "SHOT?", "PID:LOOPMODE? 0", "FAULT:SOURces?", "DEBUG:FAULT:BYPASS?", "EXTernal:TRIGger:LATency?"]);
    const [mode, limits, phases, trig, charge, shotInfo, loops, sources, bypass, lat] = res.map(okval);
    P.loops = loops ? loops.split(/\s+/).map(Number) : null;
    loopBtns.forEach((b, i) => { const v = P.loops ? P.loops[i] : null; b.textContent = v === 1 ? "Closed" : v === 0 ? "Open" : "?"; b.className = "toggle " + (v === 1 ? "on" : "off"); });
    const sh = shotInfo && /^\d+ \d+/.test(shotInfo) ? shotInfo.split(" ") : null;
    P.held = sh ? { n: +sh[0], us: +sh[1], kind: sh[2] || "" } : null;
    P.heldShotS = P.held && P.held.n > 0 ? P.held.us / 1e6 : null;   // what the controller holds, not what the table shows
    P.mode = mode;
    segLim.classList.toggle("on", mode === "LIMITER"); segHv.classList.toggle("on", mode === "HVPS");
    const lm = limits && /^(\w+) FREQ ([\d.]+) ([\d.]+) DUTY ([\d.]+) ([\d.]+)$/.exec(limits);
    P.limits = lm ? { mode: lm[1], f0: +lm[2], f1: +lm[3], d0: +lm[4], d1: +lm[5] } : null;
    modeNote.textContent = P.limits ? `${P.limits.mode}: ${P.limits.f0 / 1000}-${P.limits.f1 / 1000} kHz, duty ${P.limits.d0}-${P.limits.d1} %` : "No supply chosen yet.";
    secSupply.sum(P.limits ? `${mode === "HVPS" ? "HVPS" : "Limiter"}: ${P.limits.f0 / 1000}-${P.limits.f1 / 1000} kHz, duty ${P.limits.d0}-${P.limits.d1} %` : "not chosen");
    if (phases) { const m = {}; phases.split(/\s+/).forEach(t => { const [k, v] = t.split("="); m[k] = +v; }); if (!P.phaseWant) P.phases = { U: m.U, V: m.V, W: m.W }; secPhase.sum("UVW".split("").filter(p => m[p]).join(" ") || "none"); }
    if (trig) { P.trig = Object.fromEntries(trig.split(/\s+/).map(t => t.split("="))); trigBtn.textContent = P.trig.enabled === "1" ? "ON" : "OFF"; trigBtn.className = "toggle " + (P.trig.enabled === "1" ? "on" : "off");
      trigInfo.textContent = `Input level ${P.trig.input}; edges seen ${P.trig.edges}; shots started by an edge ${P.trig.started}; last result ${P.trig.last}. While ARMED, a rising edge on the trigger input starts the shot just as FIRE does.`;
      secRun.sum(`external trigger ${P.trig.enabled === "1" ? "ON" : "off"}`); }
    if (lat) { const L = Object.fromEntries(lat.split(/\s+/).map(t => t.split("="))); latInfo.textContent = `Last start: ${L.start_us} µs from the start command to the outputs connected` + (L.total_us && L.total_us !== "NA" ? `, ${L.total_us} µs from the trigger edge` : " (started by FIRE, not by the trigger)") + (L.prepared === "1" ? "; prepared at ARM." : L.prepared === "0" ? "; not prepared at ARM (the shot or supply changed while ARMED, or a closed-loop start)." : "."); }
    else latInfo.textContent = res[9].reply && res[9].reply.startsWith("ERR 13") ? "No shot has been started since the controller was powered up." : "";
    if (charge) { const c = Object.fromEntries(charge.split(/\s+/).map(t => t.split("="))); chInfo.replaceChildren(h("div", { class: "banner " + (c.running === "1" ? "ok" : ""), text: c.running === "1" ? `Charger ON: ${(+c.freq).toLocaleString()} Hz (the timer produces ${c.actual} Hz).` : "Charger OFF." })); chNote.textContent = ""; secCharge.sum(c.running === "1" ? `ON, ${(+c.freq).toLocaleString()} Hz` : "off"); }
    else if (res[4] && res[4].reply && res[4].reply.startsWith("ERR 1")) { chInfo.replaceChildren(h("div", { class: "banner warn", text: "This controller's firmware has no CHARGE output (needs firmware v0.11 or later)." })); secCharge.sum("not available"); }
    P.src = sources ? Object.fromEntries(sources.split(/\s+/).map(t => t.split("="))) : null; P.bypass = bypass;
    srcRow.replaceChildren(...(P.src ? Object.entries(P.src).map(([k, v]) => h("span", { class: "chip " + (v === "1" ? "on" : ""), title: SRC_NAME[k] || k, text: k })) : [h("span", { class: "hint", text: "not reported" })]));
    bypassBox.replaceChildren(bypass === "1" ? h("div", { class: "banner err" }, h("b", {}, "FAULT BYPASS IS ON. "), "The controller ignores its fault inputs: nothing here protects the supply. This is for a bench with nothing connected. ",
      h("button", { class: "btn small", onclick: async () => { await send(conn, "DEBUG:FAULT:BYPASS 0"); await P.refresh(); await P.refreshProtection(); } }, "Turn the bypass off")) : "");
    P.ticks++;
    if (P.pristine && P.limits && P.pristineFor !== P.mode) {          // nobody has touched the table: an example inside this supply's limits
      P.pristineFor = P.mode; P.rows = HB_PRESETS.flat.build(P.limits); drawTable();
    }
    if (secProt.open || P.ticks % 6 === 1) await P.refreshProtection();
    if (secSet.open || P.ticks % 6 === 1) { await deadtime.refresh(); secSet.sum(deadtime.value ? `dead time ${deadtime.value} ns` : ""); }
    if (profiles.sec.open && P.ticks % 6 === 2) await profiles.refresh();
    if (P.ticks === 1) await profiles.refresh();
    paintPhases(); P.onStatus(); validate(); drawChart();
    if (state() === "FAULT") updateStatus(conn);
  };
  P.recordDefaults = () => {
    const ph = ["U", "V", "W"].map((p, i) => (P.phases[p] ? i + 1 : 0)).filter(Boolean);
    const last = P.rows.length ? P.rows[P.rows.length - 1].t : 10;
    return { channels: ph.length ? ph : [1, 2, 3], pfm_inputs: ph.length ? ph : [1, 2, 3], duration_s: P.heldShotS || Math.max(0.001, (Number.isFinite(last) ? last : 10) / 1000),
             meta: { supply: P.mode, loop_modes_U_V_W: P.loops ? P.loops.map(v => (v ? "closed" : "open")).join(" ") : null, phases_on: ph.map(i => "UVW"[i - 1]).join(""), shot_setpoints: P.rows.map(r => ({ t_ms: r.t, f_kHz: r.f, duty_U: r.u, duty_V: r.v, duty_W: r.w })) } };
  };
  P.preArmWarnings = () => {
    const w = [];
    if (P.held && P.held.n === 0) w.push("The controller holds no shot: FIRE would be refused. Send a shot first.");
    if (P.bypass === "1") w.push("The fault bypass is ON: the controller will not react to its fault inputs.");
    const off = P.gds ? P.gds.filter(g => !g.enabled).length : 0;
    if (off) w.push(`${off} of the 12 gate-driver fault inputs ${off === 1 ? "is" : "are"} switched off.`);
    if (P.held && P.held.n > 0 && !isChecked()) w.push(P.loaded ? "The table holds a shot that was loaded but not sent: the controller will fire the shot it holds, not the one in the table."
      : `The table has not been compared with the controller's shot (${P.held.n} setpoints, ${fmt(P.held.us / 1000, 3)} ms) since one of them last changed. The controller fires the shot it holds, whatever the table shows: use Compare with controller if you need to be sure.`);
    return w;
  };
  drawTable(); P.el = el;
  try { matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => drawChart()); } catch (e) { /* an old browser: the chart is redrawn at the next refresh */ }
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
  const timingInfo = h("div", { class: "hint" }), interlock = h("div", { class: "hint" }), bypassBox = h("div");
  const deadtime = buildDeadtime(conn), profiles = buildProfiles(conn, () => P.refresh());
  const secSet = sectionC("Settings: dead time", "", { open: false, key: "xrex-settings", onOpen: () => deadtime.refresh() }, deadtime.row, deadtime.note);
  el.append(h("div", { class: "row spread", style: "margin-bottom:10px" }, h("h2", {}, "Operate"), intervalPicker(conn, 10000)),
    section("Shot timing", "1", h("div", { class: "row" }, h("label", {}, "Ramp up ", tUp, " s"), h("label", {}, "Flat top ", tFlat, " s"), h("label", {}, "Ramp down ", tDown, " s"),
      h("button", { class: "btn small", onclick: async () => { await send(conn, `SHOT:TIMing ${tUp.value} ${tFlat.value} ${tDown.value}`); await P.refresh(); } }, "Apply")), timingInfo),
    section("Channels", "2", grid),
    section("Run", "3", h("p", {}, "Use ", h("b", {}, "ARM"), " then ", h("b", {}, "START SHOT"), " in the State box on the left. ARM is refused unless every channel that is in the shot has its enable line and contactor on."), interlock, bypassBox),
    profiles.sec, secSet);
  const paint = (btn, v, on, off) => { btn.textContent = v === "1" ? on : off; btn.className = "toggle " + (v === "1" ? "on" : "off"); };
  P.paint = paint;
  P.refresh = async () => {
    const cmds = ["SHOT:TIMing?", "EXTernal:ENAble?", "EXTernal:INPut?", "EXTernal:TRIGger?"];
    P.ticks = (P.ticks || 0) + 1;
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
    if (P.ticks % 4 === 1) await slow();
  };
  P.el = el; P.onStatus = () => profiles.paint();
  /* the slower extras: the fault bypass, the dead time, the saved profiles */
  const slow = async () => {
    P.bypass = okval((await query(conn, ["DEBUG:FAULT:BYPASS?"]))[0]);
    bypassBox.replaceChildren(P.bypass === "1" ? h("div", { class: "banner err" }, h("b", {}, "FAULT BYPASS IS ON. "), "The controller ignores its fault inputs and interlocks. This is for a bench with nothing connected. ",
      h("button", { class: "btn small", onclick: async () => { await send(conn, "DEBUG:FAULT:BYPASS 0"); await slow(); } }, "Turn the bypass off")) : "");
    await deadtime.refresh(); secSet.sum(deadtime.value ? `${deadtime.value} ns` : "");
    if (P.ticks === 1 || profiles.sec.open) await profiles.refresh();
  };
  P.preArmWarnings = () => {
    const w = [];
    if (P.bypass === "1") w.push("The fault bypass is ON: the controller will not react to its fault inputs or interlocks.");
    if ([1, 2, 3, 4].every(n => cards[n].en !== "1")) w.push("No channel is enabled for the shot (\"In shot\" is OFF on every channel), so nothing will be output.");
    const bad = [1, 2, 3, 4].filter(n => cards[n].en === "1" && cards[n].loopV === "1" && zeroGains(cards[n]));
    if (bad.length) w.push(`Channel${bad.length > 1 ? "s" : ""} ${bad.join(", ")} ${bad.length > 1 ? "are" : "is"} in the shot, in closed loop, with all PID gains zero. The output would stay at the minimum frequency and not follow the setpoint.`);
    return w;
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
  u.recPfm = h("input", { type: "checkbox", "aria-label": "Record the PFM input capture", onchange: () => { conn.recPfmTouched = true; } });
  u.recPfmIn = h("span", { class: "row" });
  u.recPfmEdges = h("select", { "aria-label": "Edges to capture" }, h("option", { value: "both" }, "Both edges: period and duty"), h("option", { value: "rising" }, "Rising edges: period only"));
  u.recPfmCycles = h("input", { type: "number", min: 1, max: 3800, step: 1, placeholder: "auto", style: "width:80px", "aria-label": "PFM cycles to capture (empty: from the shot length)" });
  u.recPfmBox = h("div", {},
    h("div", { class: "row" }, h("label", { class: "chk" }, u.recPfm, h("b", {}, " PFM input capture"), " (works open loop too):"), u.recPfmIn, u.recPfmEdges,
      h("label", {}, "cycles ", u.recPfmCycles)),
    h("p", { class: "hint" }, "The PFM_Input timer measures every switching cycle of the signal fed back to each input (inputs 1 to 3 carry phases U, V and W on the bench): the carrier period and, with both edges, the duty. It is armed before the shot and starts with it: tick Record, then press ARM here (the page arms the capture first, because its modes can only be set while the controller is IDLE; if the controller is already ARMED, DISARM first). Leave cycles empty to size it from the shot (at most 3800 cycles, about 38 ms at 100 kHz). The capture modes are put back afterwards.")); 
  u.recStatus = h("div"); u.recWarn = h("div");
  u.recBtns = h("div", { class: "row" },
    h("button", { class: "btn small", onclick: () => cancelRecording(conn) }, "Cancel recording"),
    h("button", { class: "btn small", title: "Read whatever the controller's log holds now (a shot that ran without being recorded by this page)", onclick: () => collectNow(conn) }, "Read the controller's log now"));
  u.shotList = h("div", { class: "shotlist" }); u.shotView = h("div");
  u.dataPane.append(
    section("Record the next shot", "", h("p", { class: "hint" }, "Tick “Record data for the next shot” (left, under the state), then ARM. The controller logs its setpoint, the measured feedback and its output during a closed-loop shot, and (Limiter and HVPS) can also measure the carrier and duty at its PFM inputs, open loop or closed. When the shot ends the page reads the data back, saves it, and shows the plots and numbers here."),
      h("div", { class: "row" }, h("span", {}, "Waveform log:"), u.recChans, h("label", {}, "Expected shot length ", u.recDur, " s")),
      h("p", { class: "hint" }, "The log has room for about 1000 samples, so a longer shot is recorded at a lower sample rate. Set the length a little long rather than short. Untick every phase to record only the PFM capture."),
      u.recPfmBox,
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
  u.recPfmBox.hidden = product !== "hbridge";
  if (product === "hbridge" && u.recPfmIn.dataset.product !== product) {
    u.recPfmIn.dataset.product = product;
    u.recPfmIn.replaceChildren(...[1, 2, 3, 4, 5, 6].map(i => h("label", { class: "chk" }, h("input", { type: "checkbox", "data-in": i, onchange: () => { conn.recPfmTouched = true; } }), " ", i <= 3 ? `In ${i} (${"UVW"[i - 1]})` : `In ${i}`)));
  }
  if (product === "hbridge" && !conn.recPfmTouched) u.recPfmIn.querySelectorAll("input").forEach(b => { b.checked = (def.pfm_inputs || [1, 2, 3]).includes(+b.dataset.in); });
  if (!conn.recChanTouched) u.recChans.querySelectorAll("input").forEach(b => { b.checked = def.channels.includes(+b.dataset.ch); });
  if (!conn.recDurTouched && document.activeElement !== u.recDur) u.recDur.value = String(+(+def.duration_s).toFixed(3));
}
function recSpec(conn) {
  const u = conn.ui, def = recDefaults(conn);
  const spec = { id: conn.id, channels: [...u.recChans.querySelectorAll("input")].filter(b => b.checked).map(b => +b.dataset.ch), duration_s: +u.recDur.value, meta: def.meta };
  if (conn.status.product === "hbridge" && u.recPfm.checked) {
    spec.pfm = { inputs: [...u.recPfmIn.querySelectorAll("input")].filter(b => b.checked).map(b => +b.dataset.in), edges: u.recPfmEdges.value, cycles: u.recPfmCycles.value ? +u.recPfmCycles.value : null };
  }
  return spec;
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
  if (warn.length && !(await askDialog({ title: "Arm anyway?", body: warn.map(w => h("div", { class: "banner warn", text: w })), yes: "Go on to ARM", danger: false }))) return;
  const wantRec = conn.ui.recCheck.checked && !recActive(conn);
  const usesPfm = wantRec && conn.status.product === "hbridge" && conn.ui.recPfm.checked;
  // The PFM capture modes can only be set while the controller is IDLE, so with PFM ticked the recording is armed BEFORE ARM is sent.
  if (usesPfm && conn.status.state === "IDLE" && !(await armRecording(conn))) { toast("The controller was NOT armed, because the recording could not be armed.", true); return; }
  const r = await send(conn, "ARM");
  if (r.ok && wantRec && !usesPfm && !recActive(conn)) await armRecording(conn);
  else if (!r.ok && usesPfm && recActive(conn)) await cancelRecording(conn);       // ARM was refused: do not leave a recording waiting
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
    h("b", {}, x.started.replace("T", " ").slice(0, 19)), h("span", { class: "tag " + x.outcome }, x.outcome), x.channels.length ? `${(+x.duration_s).toPrecision(3)} s` : "", x.channels.length ? `${x.channels.length} ch` : "", (x.pfm_inputs || []).length ? `PFM ${(x.pfm_inputs || []).join(",")}` : "")));
  showShot(conn, conn.selectSid);
}
async function showShot(conn, sid) {
  const u = conn.ui;
  let d;
  try { d = await api(`/api/shot?sid=${encodeURIComponent(sid)}&unit=${conn.shotUnit || "A"}`); } catch (e) { u.shotView.replaceChildren(h("div", { class: "banner err" }, e.message)); return; }
  const unitSeg = d.summary.product === "xrex" ? h("div", { class: "seg" },
    ...[["A", "Amps"], ["native", "Hz"]].map(([v, t]) => h("button", { class: d.unit === v ? "on" : "", disabled: v === "A" && !d.can_amps, onclick: () => { conn.shotUnit = v; showShot(conn, sid); } }, t))) : null;
  const dl = name => h("button", { class: "btn small", onclick: () => downloadShot(conn, sid, name) }, name === "shot.csv" ? "Data (CSV)" : name === "pfm.csv" ? "PFM data (CSV)" : name === "report.html" ? "Report (HTML)" : name === "report.md" ? "Report (text)" : "Raw (JSON)");
  u.shotView.replaceChildren(
    h("div", { class: "section" },
      h("div", { class: "row spread" }, h("h3", {}, "Shot " + d.summary.started.replace("T", " ").slice(0, 19)), unitSeg),
      h("dl", { class: "kv" }, ...d.header.flatMap(([k, v]) => [h("dt", { text: k }), h("dd", { text: v })])),
      ...d.notes.map(n => h("div", { class: "banner warn", text: n })),
      d.unit === "A" && d.amps_note ? h("p", { class: "hint", text: d.amps_note }) : null,
      h("div", { class: "row" }, ...(d.files || ["shot.csv", "report.html", "report.md", "shot.json"]).map(dl)),
      h("div", { class: "hint" }, `Saved in ${window.WHAM.shotsDir || "the shots folder"}/${sid}`)),
    ...d.channels.map(c => { const box = h("div", { class: "section" }, h("h3", {}, c.name)); const fig = h("div", { class: "fig" }); fig.innerHTML = c.svg; box.append(fig, h("table", { class: "stats" }, ...c.stats.map(([k, v]) => h("tr", {}, h("td", { text: k }), h("td", { text: v }))))); return box; }),
    d.pfm ? pfmSection(d.pfm) : null);
}
function pfmSection(p) {
  const box = h("div", { class: "section" }, h("h3", {}, "PFM input capture"), ...p.notes.map(n => h("div", { class: "banner warn", text: n })));
  for (const svg of [p.svg_duty, p.svg_freq]) { if (!svg) continue; const fig = h("div", { class: "fig" }); fig.innerHTML = svg; box.append(fig); }
  for (const i of p.inputs) box.append(h("h4", {}, i.name), h("table", { class: "stats" }, ...i.stats.map(([k, v]) => h("tr", {}, h("td", { text: k }), h("td", { text: v })))));
  return box;
}
async function downloadShot(conn, sid, name) {
  try {
    const r = await fetch(`/api/shot/file?sid=${encodeURIComponent(sid)}&name=${name}`, { headers: { "X-WHAM-Token": window.WHAM.token } });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const a = h("a", { href: URL.createObjectURL(await r.blob()), download: sid.replace("/", "_") + "_" + name });
    document.body.append(a); a.click(); a.remove();
  } catch (e) { toast("Download failed: " + e.message, true); }
}

/* ---------- the ssh tunnel to the host (the saved controllers are reached through it) ---------- */
function tunnelCard() {
  const t = S.tunnel;
  if (!t) return null;
  const busy = !!S.tunnelBusy || t.state === "starting";
  const open = t.state === "open" || t.state === "open-elsewhere";
  const needs = S.presets.some(viaTunnel);
  const text = busy ? (S.tunnelBusy === "closing" ? "Closing…" : `Opening the tunnel to ${t.host}…`)
    : t.state === "open" ? `Open. This computer's ports reach the controllers' hubs on ${t.host}. This page opened it, and closes it when the console stops.`
    : t.state === "open-elsewhere" ? `Open. It was opened outside this page (for example ssh in a terminal) and is used as it is. Close tunnel here ends it for every program on this computer.`
    : t.state === "partial" ? `Only part of the tunnel is up: port ${t.listening.join(", ")} answers, ${t.ports.filter(p => !t.listening.includes(p)).join(", ")} ${t.ports.length - t.listening.length === 1 ? "does" : "do"} not. Controllers on the answering ports can be used. To get all of them, close this tunnel and open it again here.`
    : needs ? `Closed. The saved controllers below are reached through ssh to ${t.host}: open the tunnel first, then pick one.` : `Closed. Only needed to reach controllers through the host (${t.host}) from outside the lab.`;
  const holders = (t.foreign || []).map(f => `${f.name} (process ${f.pid}) on port ${f.ports.join(", ")}`).join("; ");
  return h("div", { class: "card tunnelcard" },
    h("div", {}, h("h3", {}, h("span", {}, "Tunnel to the host ", h("code", {}, t.host)), h("span", { class: "chip " + (open ? "good" : t.state === "partial" ? "direct" : ""), text: busy ? "working" : open ? "open" : t.state === "partial" ? "partly open" : "closed" })),
      h("div", { text: text }),
      t.error ? h("div", { class: "banner err", text: t.error }) : null,
      !t.ssh_found && !open ? h("div", { class: "hint", text: "ssh is not installed on this computer, so this page cannot open the tunnel itself. A tunnel opened another way is still found and used." }) : null,
      h("div", { class: "ports" }, ...t.ports.map(p => h("span", { class: "portchip", title: `this computer's port ${p} → ${t.host} port ${p}` }, h("span", { class: "dot " + ((t.listening || []).includes(p) ? "ok" : "") }), String(p)))),
      holders ? h("div", { class: "hint", text: `Held by ${holders}.` }) : null),
    h("div", { class: "row" },
      open || t.state === "partial" ? h("button", { class: "btn", disabled: busy, onclick: () => tunnelToggle(false) }, "Close tunnel") : null,
      !open ? h("button", { class: "btn primary", disabled: busy || !t.ssh_found || t.state === "partial", title: t.state === "partial" ? "Close the partly open tunnel first" : "", onclick: () => tunnelToggle(true) }, "Open tunnel") : null));
}
async function tunnelToggle(on) {
  const t = S.tunnel;
  let force = false;
  if (!on) {
    const through = [...S.conns.values()].filter(c => viaTunnel({ mode: c.status.mode, host: c.status.target.split(":")[0] }) && t.ports.includes(+c.status.target.split(":")[1]));
    const foreign = t.state === "open-elsewhere" || t.state === "partial";
    if (foreign && t.closable === false) {
      const who = (t.foreign || []).map(f => `${f.name} (process ${f.pid}) on port ${f.ports.join(", ")}`).join("; ");
      await askDialog({ title: "This page cannot close that tunnel", yes: "OK", danger: false,
        body: who ? `The tunnel's ports are held by ${who}, not by ssh, so they are left alone. Close that program yourself.` : "The program that holds the tunnel's ports could not be found. Close the tunnel in the terminal it was started from (Ctrl-C there)." });
      return;
    }
    if (through.length || foreign) {
      const ok = await askDialog({ title: "Close the tunnel?", yes: "Close tunnel", danger: false, body: [
        foreign ? h("p", { text: `This tunnel was not opened by this page: ${(t.foreign || []).map(f => `ssh process ${f.pid} (port ${f.ports.join(", ")})`).join(", ")}. Closing it ends it for every program on this computer that uses it (a terminal session, another console).` }) : null,
        through.length ? h("p", { text: `${through.map(connLabel).join(", ")} ${through.length === 1 ? "is" : "are"} connected through it and will be disconnected first. What the controllers are doing keeps going.` }) : null,
        ...leaveWarning(through)] });
      if (!ok) return;
      for (const c of through) { try { await api("/api/disconnect", { id: c.id }); } catch (e) { /* closing anyway */ } }
      if (through.find(c => c.id === S.active)) S.active = null;
      force = foreign;
    }
  }
  S.tunnelBusy = on ? "opening" : "closing"; renderTop(); renderHome();
  try {
    const r = await api("/api/tunnel", { on, force }); S.tunnel = r.tunnel;
    if (on && r.tunnel.state === "open") toast(`Tunnel to ${r.tunnel.host} is open.`);
    else if (on && r.tunnel.error) toast(r.tunnel.error, true);
    else if (!on && r.tunnel.state === "closed") toast("Tunnel closed.");
    else if (!on && r.tunnel.error) toast(r.tunnel.error, true);
  } catch (e) { toast("Tunnel: " + e.message, true); }
  finally { S.tunnelBusy = false; await loadState(); renderTop(); renderHome(); }
}

/* ---------- the menu: guides (the command flowcharts) and quit ---------- */
async function buildMenu() {
  try { S.help = (await api("/api/help")).pages; } catch (e) { S.help = []; }
  const list = $("#menu-list"), btn = $("#btn-more");
  const close = () => { list.hidden = true; btn.setAttribute("aria-expanded", "false"); };
  list.replaceChildren(
    ...(S.help.length ? [h("div", { class: "lbl", text: "Guides (open in a new tab)" }), ...S.help.map(p => h("a", { href: `/static/help/${p.file}`, target: "_blank", rel: "noopener", onclick: close }, p.title)), h("div", { class: "sep" })] : []),
    h("button", { onclick: () => { close(); S.active = null; renderAll(); const d = $("#home details.help"); if (d) { d.open = true; d.scrollIntoView({ behavior: "smooth" }); } } }, "Using this console on another computer"),
    h("div", { class: "sep" }),
    h("button", { onclick: () => { close(); quitConsole(); } }, "Quit the console…"));
  btn.onclick = ev => { ev.stopPropagation(); list.hidden = !list.hidden; btn.setAttribute("aria-expanded", String(!list.hidden)); };
  document.addEventListener("click", ev => { if (!list.hidden && !list.contains(ev.target)) close(); });
  document.addEventListener("keydown", ev => { if (ev.key === "Escape") close(); });
}
async function quitConsole() {
  const conns = [...S.conns.values()], t = S.tunnel;
  const ok = await askDialog({ title: "Quit the console?", yes: "Quit", danger: false, body: [
    "This disconnects from every controller, closes the tunnel this console opened, and stops the console program on this computer. What the controllers are doing keeps going.",
    t && (t.state === "open-elsewhere" || t.state === "partial") ? h("p", { class: "hint", text: "The tunnel that was opened outside this page stays open. Use Close tunnel first if you want it closed too." }) : null,
    ...leaveWarning(conns)] });
  if (!ok) return;
  S.stopped = true;
  try { await api("/api/quit", {}); } catch (e) { /* it stops while answering */ }
  for (const c of S.conns.values()) c.gone = true;
  $("#stopped").hidden = false;
}

/* ---------- host log: what the host service recorded for this controller ---------- */
const LOG_GROUPS = [
  ["cmd", "Commands", ["cmd", "hub-cmd", "danger-held"], "What people and programs sent to the controller."],
  ["reply", "Replies", ["reply", "hub-reply", "data"], "What the controller answered."],
  ["event", "Events", ["event"], "The controller's own !EVT lines (only when its event stream is on)."],
  ["boot", "Restarts", ["boot"], "The controller started (!BOOT)."],
  ["note", "Host notes", ["hub", "identity", "refused", "danger-cancelled", "danger-expired"], "Connections, who joined or left, timeouts, refused commands."],
];
const LOG_RANGES = [["15m", "last 15 minutes"], ["1h", "last hour"], ["6h", "last 6 hours"], ["1d", "last day"], ["7d", "last 7 days"], ["", "everything kept"]];
const KIND_LABEL = { cmd: "command", reply: "reply", event: "event", boot: "restart", hub: "host", "hub-cmd": "host cmd", "hub-reply": "host reply", refused: "refused", identity: "identity", data: "data", "danger-held": "held", "danger-cancelled": "cancelled", "danger-expired": "expired" };
const PROBLEM_WORDS = /not connected|no answer|timed out|timeout|lost|closed|refused|not reading|disconnect|failed|error/i;
function logIsProblem(r) {
  return r.kind === "refused" || r.kind === "boot" || (r.kind === "reply" && /^ERR/.test(r.line)) || (r.kind === "hub-reply" && /^ERR/.test(r.line)) ||
    (r.kind === "hub" && PROBLEM_WORDS.test(r.line)) || r.kind === "danger-expired";
}
function buildHostLog(conn) {
  const u = conn.ui, L = conn.log = { rows: [], info: null, groups: new Set(LOG_GROUPS.map(g => g[0])), range: "1h", limit: 500, text: "", client: "", problems: false, all: false, auto: true, timer: null, busy: false, more: false };
  u.logBody = h("div"); u.logSummary = h("div"); u.logMsg = h("div"); u.logTable = h("div", { class: "hosttable" }); u.logFoot = h("div", { class: "row" });
  const chips = h("div", { class: "row" }, ...LOG_GROUPS.map(([key, label, , tip]) => {
    const box = h("input", { type: "checkbox", checked: true, onchange: () => { box.checked ? L.groups.add(key) : L.groups.delete(key); hostLogLoad(conn); } });
    return h("label", { class: "chk", title: tip }, box, " ", label);
  }));
  const range = h("select", { "aria-label": "How far back", onchange: ev => { L.range = ev.target.value; hostLogLoad(conn); } }, ...LOG_RANGES.map(([v, t]) => h("option", { value: v, text: t })));
  range.value = L.range;
  const limit = h("select", { "aria-label": "Most rows", onchange: ev => { L.limit = +ev.target.value; hostLogLoad(conn); } }, ...[200, 500, 1000, 1500].map(v => h("option", { value: v, text: `up to ${v} rows` })));
  limit.value = String(L.limit);
  u.logClient = h("select", { "aria-label": "Who", onchange: ev => { L.client = ev.target.value; hostLogLoad(conn); } }, h("option", { value: "", text: "everyone" }));
  const find = h("input", { type: "search", placeholder: "find text, e.g. ARM or ERR", "aria-label": "Find text", style: "width:200px" });
  let findTimer = null;
  find.addEventListener("input", () => { clearTimeout(findTimer); findTimer = setTimeout(() => { L.text = find.value.trim(); hostLogLoad(conn); }, 400); });
  const problems = h("input", { type: "checkbox", onchange: ev => { L.problems = ev.target.checked; hostLogLoad(conn); } });
  const all = h("input", { type: "checkbox", onchange: ev => { L.all = ev.target.checked; hostLogLoad(conn); } });
  const auto = h("input", { type: "checkbox", checked: true, onchange: ev => { L.auto = ev.target.checked; hostLogSchedule(conn); } });
  u.logPane.append(
    h("div", { class: "row spread" }, h("h3", {}, "Host log"),
      h("div", { class: "row" }, h("label", { class: "hint" }, auto, " update every 5 s"), h("button", { class: "btn small", onclick: () => hostLogLoad(conn) }, "Refresh now"),
        h("button", { class: "btn small", onclick: () => hostLogSave(conn) }, "Save as text"))),
    h("p", { class: "hint" }, "Everything the host service saw for this controller, from every program and person using it: not just this page. Newest at the bottom."),
    h("div", { class: "row logfilters" }, range, limit, u.logClient, find),
    chips,
    h("div", { class: "row" }, h("label", { class: "chk", title: "Only restarts, ERR replies, refused commands and host notes about lost connections or timeouts" }, problems, " problems only"),
      h("label", { class: "chk", title: "The host's own *IDN? heartbeats and its handshake queries, and web pages asking !hub status" }, all, " include the host's automatic queries")),
    u.logMsg, u.logSummary, u.logTable, u.logFoot);
}
function hostLogVisible(conn, on) {
  const L = conn.log; if (!L) return;
  L.visible = on;
  if (on) hostLogLoad(conn); else { clearTimeout(L.timer); L.timer = null; }
}
function hostLogSchedule(conn) {
  const L = conn.log; clearTimeout(L.timer); L.timer = null;
  if (L.visible && L.auto && !conn.gone) L.timer = setTimeout(() => hostLogLoad(conn, { quiet: true }), 5000);
}
function hostLogQuery(conn, before) {
  const L = conn.log, kinds = LOG_GROUPS.filter(g => L.groups.has(g[0])).flatMap(g => g[2]), all = L.groups.size === LOG_GROUPS.length;
  const p = new URLSearchParams({ id: conn.id, n: L.problems ? 1500 : L.limit });
  if (L.range) p.set("since", L.range);
  if (!all) p.set("kind", kinds.length ? kinds.join(",") : "none");
  if (L.client) p.set("client", L.client);
  if (L.text) p.set("grep", L.text);
  if (L.all) p.set("all", "1");
  if (before) p.set("before", before);
  return `/api/hostlog?${p}`;
}
async function hostLogLoad(conn, { quiet = false, older = false } = {}) {
  const L = conn.log, u = conn.ui;
  if (L.busy) return;
  if (conn.status.mode !== "hub") {
    u.logMsg.replaceChildren(h("div", { class: "banner warn" }, "The host log is only available when this controller is connected through the host (not directly or over a cable). Disconnect and connect again, choosing “Through the host”."));
    u.logSummary.replaceChildren(); u.logTable.replaceChildren(); u.logFoot.replaceChildren(); return;
  }
  L.busy = true;
  const box = u.logTable, atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40, oldHeight = box.scrollHeight;
  try {
    const r = await api(hostLogQuery(conn, older && L.rows.length ? L.rows[0].t : null));
    L.info = r.info;
    L.rows = older ? r.rows.concat(L.rows) : r.rows;
    L.more = !!r.info.truncated;
    u.logMsg.replaceChildren();
    hostLogRender(conn);
    if (older) box.scrollTop = box.scrollHeight - oldHeight; else if (atBottom || !quiet) box.scrollTop = box.scrollHeight;
  } catch (e) {
    u.logMsg.replaceChildren(h("div", { class: "banner err" }, "Could not read the host log: " + e.message));
  } finally { L.busy = false; hostLogSchedule(conn); }
}
function hostLogRows(conn) { const L = conn.log; return L.problems ? L.rows.filter(logIsProblem) : L.rows; }
function logTime(r, withDate) {
  const d = new Date(r.t * 1000), pad = (n, w = 2) => String(n).padStart(w, "0");
  return (withDate ? `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` : "") + `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
}
function hostLogRender(conn) {
  const L = conn.log, u = conn.ui, rows = hostLogRows(conn);
  // the "who" menu keeps every name seen, so a filter that hides a name does not remove it from the menu
  const names = new Set([L.client, ...L.rows.map(r => r.client).filter(Boolean)].filter(Boolean));
  const have = new Set([...u.logClient.options].map(o => o.value));
  for (const n of [...names].sort()) if (!have.has(n)) u.logClient.append(h("option", { value: n, text: n }));
  u.logClient.value = L.client;
  // summary: counts that answer "was anything wrong?" at a glance
  const count = f => L.rows.filter(f).length, problems = L.rows.filter(logIsProblem);
  const clients = new Set(L.rows.filter(r => r.kind === "cmd" && r.client).map(r => r.client));
  const first = L.rows[0], last = L.rows[L.rows.length - 1];
  const stat = (label, value, bad) => h("div", { class: "stat" + (bad ? " bad" : "") }, h("b", { text: String(value) }), h("span", { text: label }));
  u.logSummary.replaceChildren(...[h("div", { class: "statrow" },
    stat("rows read", L.rows.length), stat("commands", count(r => r.kind === "cmd")), stat("ERR replies", count(r => r.kind === "reply" && /^ERR/.test(r.line)), count(r => r.kind === "reply" && /^ERR/.test(r.line)) > 0),
    stat("restarts", count(r => r.kind === "boot"), count(r => r.kind === "boot") > 0), stat("connection problems", count(r => r.kind === "hub" && PROBLEM_WORDS.test(r.line)), count(r => r.kind === "hub" && PROBLEM_WORDS.test(r.line)) > 0),
    stat("programs/people", clients.size)),
    first ? h("div", { class: "hint" }, `${logTime(first, true)} to ${logTime(last, true)}` + (L.info && L.info.truncated ? ". There is more earlier than this." : ". This is everything in the chosen range.") + (L.problems ? ` Showing the ${problems.length} problem rows only.` : "")) : null].filter(Boolean));
  let day = "";
  const out = [];
  for (const r of rows) {
    const d = new Date(r.t * 1000).toDateString();
    if (d !== day) { day = d; out.push(h("div", { class: "dayrow", text: new Date(r.t * 1000).toLocaleDateString(undefined, { weekday: "long", year: "numeric", month: "long", day: "numeric" }) })); }
    const bad = logIsProblem(r);
    out.push(h("div", { class: "hrow k-" + r.kind + (bad ? " problem" : "") },
      h("span", { class: "t", text: logTime(r) }), h("span", { class: "kind", text: KIND_LABEL[r.kind] || r.kind }), h("span", { class: "who", text: r.client || "" }),
      h("span", { class: "txt", text: r.line + (r.danger ? `  [DANGEROUS: ${r.danger}]` : "") + (r.truncated ? ` ...(+${r.truncated} characters not logged)` : "") })));
  }
  if (!out.length) out.push(h("div", { class: "hint", style: "padding:12px" }, L.rows.length ? "Nothing here is a problem." : "Nothing logged in that range with those filters."));
  u.logTable.replaceChildren(...out);
  u.logFoot.replaceChildren(...[L.more && L.rows.length ? h("button", { class: "btn small", onclick: () => hostLogLoad(conn, { older: true }) }, "Load earlier rows") : null,
    h("span", { class: "hint" }, L.info ? `Host log for ${L.info.dev}.` : "")].filter(Boolean));
}
function hostLogSave(conn) {
  const rows = hostLogRows(conn);
  const text = rows.map(r => `${new Date(r.t * 1000).toISOString()}\t${r.kind}\t${r.client || ""}\t${r.line}`).join("\n") + "\n";
  const a = h("a", { href: URL.createObjectURL(new Blob([text], { type: "text/plain" })), download: `wham-host-log-${(conn.log.info && conn.log.info.dev) || conn.id}-${new Date().toISOString().replace(/[:.]/g, "-")}.txt` });
  document.body.append(a); a.click(); a.remove();
}

/* ---------- start ---------- */
$("#btn-new").addEventListener("click", () => openConnect());
$("#btn-disc-all").addEventListener("click", () => disconnectAll());
$(".brand").addEventListener("click", () => { S.active = null; renderAll(); });
$(".brand").addEventListener("keydown", ev => { if (ev.key === "Enter" || ev.key === " ") { S.active = null; renderAll(); } });
buildMenu();
loadState();
setInterval(loadState, 2500);

#pragma once

// Movement remote served at "/" by WebSocketControlServer. Its buttons send the
// same self.otto.action / self.otto.stop calls over /ws as trigger_tictactoe.py.
inline constexpr char kOttoWebPage[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Otto Control</title>
<link rel="icon" href="data:,">
<style>
:root {
  color-scheme: light dark;
  --bg: #f3f4f6; --card: #ffffff; --text: #111827; --muted: #6b7280; --line: #e5e7eb;
  --btn: #eceef1; --btn-press: #dde0e5; --accent: #2563eb; --stop: #dc2626;
  --ok: #15803d; --bad: #b91c1c;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0f1115; --card: #181b21; --text: #e5e7eb; --muted: #9ca3af; --line: #262a31;
    --btn: #242830; --btn-press: #30353e; --accent: #60a5fa; --ok: #4ade80; --bad: #f87171;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 15px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
main { max-width: 520px; margin: 0 auto; padding: 12px 16px 32px; }
header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
h1 { font-size: 22px; margin: 0; }
h2 {
  font-size: 12px; font-weight: 600; letter-spacing: .06em; text-transform: uppercase;
  color: var(--muted); margin: 0 0 10px;
}
section {
  background: var(--card); border: 1px solid var(--line); border-radius: 14px;
  padding: 14px; margin-bottom: 12px;
}
.pill { font-size: 13px; padding: 4px 10px; border-radius: 999px; background: var(--btn); }
.pill.on { color: var(--ok); }
.pill.off { color: var(--bad); }
button {
  font: inherit; color: inherit; background: var(--btn); border: 0; border-radius: 10px;
  min-height: 46px; padding: 8px 6px; cursor: pointer; hyphens: auto; overflow-wrap: anywhere;
  touch-action: manipulation; -webkit-user-select: none; user-select: none;
}
button:active { background: var(--btn-press); }
#stop {
  display: block; width: 100%; min-height: 58px; margin-bottom: 12px;
  background: var(--stop); color: #fff; font-size: 18px; font-weight: 700; letter-spacing: .05em;
}
#stop:active { filter: brightness(.85); }
.pad {
  display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px;
  max-width: 300px; margin: 0 auto;
}
.pad button { min-height: 64px; font-size: 22px; }
.pad .home { font-size: 15px; }
.grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
.row { display: flex; align-items: center; gap: 8px; margin-top: 8px; }
.row span { flex: 1; }
.row button { flex: 0 0 54px; }
.slider {
  display: grid; grid-template-columns: 80px 1fr 72px; align-items: center;
  gap: 10px; margin: 4px 0;
}
.slider output { text-align: right; color: var(--muted); font-variant-numeric: tabular-nums; }
input[type=range] { width: 100%; accent-color: var(--accent); }
.hint { color: var(--muted); font-size: 13px; margin: 10px 0 0; }
#log {
  margin: 0; max-height: 160px; overflow-y: auto; white-space: pre-wrap; word-break: break-all;
  color: var(--muted); font: 12px/1.5 ui-monospace, Menlo, Consolas, monospace;
}
</style>
</head>
<body>
<main>
  <header>
    <h1>Otto</h1>
    <span id="conn" class="pill off">Connecting…</span>
  </header>

  <button id="stop" type="button">STOP</button>

  <section>
    <h2>Drive</h2>
    <div class="pad">
      <span></span>
      <button type="button" data-act="walk" data-dir="1" aria-label="Walk forward">▲</button>
      <span></span>
      <button type="button" data-act="turn" data-dir="1" aria-label="Turn left">◀</button>
      <button type="button" data-act="home" class="home">Home</button>
      <button type="button" data-act="turn" data-dir="-1" aria-label="Turn right">▶</button>
      <span></span>
      <button type="button" data-act="walk" data-dir="-1" aria-label="Walk backward">▼</button>
      <span></span>
    </div>
    <p class="hint">Each tap queues one move. STOP clears the queue and stands Otto up.</p>
  </section>

  <section>
    <h2>Settings</h2>
    <label class="slider">Steps
      <input id="steps" type="range" min="1" max="20" value="3"><output></output></label>
    <label class="slider">Speed
      <input id="speed" type="range" min="1" max="10" value="6"><output></output></label>
    <label class="slider">Amount
      <input id="amount" type="range" min="10" max="90" step="5" value="30"><output></output></label>
    <label class="slider">Arm swing
      <input id="arm_swing" type="range" min="0" max="90" step="5" value="50"><output></output></label>
    <p class="hint">Amount is how big swings, bounces and arm moves are. Arm swing is for walking
      and turning. Left and right are Otto's own.</p>
  </section>

  <section>
    <h2>Moves</h2>
    <div class="grid">
      <button type="button" data-act="jump">Jump</button>
      <button type="button" data-act="swing">Swing</button>
      <button type="button" data-act="updown">Up &amp; down</button>
      <button type="button" data-act="whirlwind_leg">Whirlwind</button>
      <button type="button" data-act="sit">Sit</button>
      <button type="button" data-act="showcase">Showcase</button>
    </div>
    <div class="row"><span>Moonwalk</span>
      <button type="button" data-act="moonwalk" data-dir="1">Left</button>
      <button type="button" data-act="moonwalk" data-dir="-1">Right</button></div>
    <div class="row"><span>Bend</span>
      <button type="button" data-act="bend" data-dir="1">Left</button>
      <button type="button" data-act="bend" data-dir="-1">Right</button></div>
    <div class="row"><span>Shake leg</span>
      <button type="button" data-act="shake_leg" data-dir="1">Left</button>
      <button type="button" data-act="shake_leg" data-dir="-1">Right</button></div>
  </section>

  <section>
    <h2>Arms</h2>
    <div class="grid">
      <button type="button" data-act="hand_wave" data-dir="0">Wave</button>
      <button type="button" data-act="windmill">Windmill</button>
      <button type="button" data-act="takeoff">Takeoff</button>
      <button type="button" data-act="fitness">Fitness</button>
      <button type="button" data-act="radio_calisthenics">Calis&shy;thenics</button>
      <button type="button" data-act="magic_circle">Magic circle</button>
    </div>
    <div class="row"><span>Hands up</span>
      <button type="button" data-act="hands_up" data-dir="1">Left</button>
      <button type="button" data-act="hands_up" data-dir="0">Both</button>
      <button type="button" data-act="hands_up" data-dir="-1">Right</button></div>
    <div class="row"><span>Hands down</span>
      <button type="button" data-act="hands_down" data-dir="1">Left</button>
      <button type="button" data-act="hands_down" data-dir="0">Both</button>
      <button type="button" data-act="hands_down" data-dir="-1">Right</button></div>
    <div class="row"><span>Greeting</span>
      <button type="button" data-act="greeting" data-dir="1">Left</button>
      <button type="button" data-act="greeting" data-dir="-1">Right</button></div>
    <div class="row"><span>Shy</span>
      <button type="button" data-act="shy" data-dir="1">Left</button>
      <button type="button" data-act="shy" data-dir="-1">Right</button></div>
  </section>

  <section>
    <h2>Log</h2>
    <pre id="log"></pre>
  </section>
</main>
<script>
"use strict";
// The tool's `speed` is a period in ms (lower = faster); Speed 1..10 maps onto it.
const SPEED_MS = [3000, 2400, 1900, 1500, 1200, 1000, 800, 650, 500, 400];
const SLIDERS = ["steps", "speed", "amount", "arm_swing"];
const SAVE_KEY = "otto-move-settings";
const KEYS = {
  ArrowUp: ["walk", 1, "Walk forward"],
  ArrowDown: ["walk", -1, "Walk backward"],
  ArrowLeft: ["turn", 1, "Turn left"],
  ArrowRight: ["turn", -1, "Turn right"],
};
const connEl = document.getElementById("conn");
const logEl = document.getElementById("log");
let ws = null;
let nextId = 1;
let retryTimer = null;

function log(line) {
  const time = new Date().toLocaleTimeString([], { hour12: false });
  const lines = logEl.textContent ? logEl.textContent.split("\n") : [];
  lines.unshift(time + "  " + line);
  logEl.textContent = lines.slice(0, 40).join("\n");
}

function setConn(online, text) {
  connEl.textContent = text;
  connEl.className = "pill " + (online ? "on" : "off");
}

function connect() {
  clearTimeout(retryTimer);
  setConn(false, "Connecting…");
  const sock = new WebSocket("ws://" + location.host + "/ws");
  ws = sock;
  sock.onopen = () => setConn(true, "Connected");
  sock.onmessage = (e) => log("<< " + e.data);
  sock.onerror = () => sock.close();
  sock.onclose = () => {
    if (ws !== sock) return;
    setConn(false, "Offline, retrying…");
    retryTimer = setTimeout(connect, 2000);
  };
}

function sendTool(name, args, label) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    log("Not connected, " + label + " not sent");
    return;
  }
  ws.send(JSON.stringify({
    jsonrpc: "2.0",
    method: "tools/call",
    params: { name: name, arguments: args },
    id: nextId++,
  }));
  log(">> " + label);
}

function value(id) {
  return Number(document.getElementById(id).value);
}

function runAction(action, dir, label) {
  const args = {
    action: action,
    steps: value("steps"),
    speed: SPEED_MS[value("speed") - 1],
    amount: value("amount"),
    arm_swing: value("arm_swing"),
  };
  if (dir !== undefined) args.direction = dir;
  sendTool("self.otto.action", args, label);
}

function stop() {
  sendTool("self.otto.stop", {}, "STOP");
}

let saved = {};
try {
  saved = JSON.parse(localStorage.getItem(SAVE_KEY)) || {};
} catch (e) {}

for (const id of SLIDERS) {
  const input = document.getElementById(id);
  const out = input.nextElementSibling;
  if (saved[id] !== undefined) input.value = saved[id];
  const show = () => {
    out.textContent = id === "speed"
        ? input.value + " · " + (SPEED_MS[input.value - 1] / 1000).toFixed(1) + "s"
        : input.value;
  };
  input.addEventListener("input", () => {
    show();
    saved[id] = input.value;
    try {
      localStorage.setItem(SAVE_KEY, JSON.stringify(saved));
    } catch (e) {}
  });
  show();
}

document.addEventListener("click", (e) => {
  const b = e.target.closest("button");
  if (!b) return;
  if (b.id === "stop") {
    stop();
    return;
  }
  const row = b.closest(".row");
  const label = row ? row.firstElementChild.textContent + " " + b.textContent
                    : b.getAttribute("aria-label") || b.textContent;
  const dir = b.dataset.dir === undefined ? undefined : Number(b.dataset.dir);
  runAction(b.dataset.act, dir, label);
});

document.addEventListener("keydown", (e) => {
  if (e.repeat || e.target.tagName === "INPUT") return;
  if (e.key === "Escape") {
    stop();
    return;
  }
  const key = KEYS[e.key];
  if (key) {
    e.preventDefault();
    runAction(key[0], key[1], key[2]);
  }
});

document.addEventListener("visibilitychange", () => {
  if (!document.hidden && (!ws || ws.readyState === WebSocket.CLOSED)) connect();
});

connect();
</script>
</body>
</html>
)html";

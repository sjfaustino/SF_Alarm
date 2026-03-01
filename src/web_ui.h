#ifndef SF_ALARM_WEB_UI_H
#define SF_ALARM_WEB_UI_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Embedded single-page dashboard — HTML + CSS + JS
// Stored as a const string literal in flash.
// ---------------------------------------------------------------------------

static const char WEB_UI_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SF_Alarm Dashboard</title>
<style>
:root {
  --bg: #0f1117;
  --surface: #1a1d27;
  --surface2: #242835;
  --border: #2e3345;
  --text: #e4e6ed;
  --text2: #8b8fa3;
  --accent: #6c8cff;
  --accent2: #4a6bdf;
  --green: #34d399;
  --green-bg: rgba(52,211,153,.12);
  --red: #f87171;
  --red-bg: rgba(248,113,113,.12);
  --yellow: #fbbf24;
  --yellow-bg: rgba(251,191,36,.12);
  --orange: #fb923c;
  --orange-bg: rgba(251,146,60,.12);
  --gray: #6b7280;
  --radius: 12px;
  --shadow: 0 4px 24px rgba(0,0,0,.35);
}
* { margin:0; padding:0; box-sizing:border-box; }
body {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  background: var(--bg);
  color: var(--text);
  min-height: 100vh;
  line-height: 1.5;
}
/* Header */
.header {
  background: linear-gradient(135deg, #1e2235 0%, #161926 100%);
  border-bottom: 1px solid var(--border);
  padding: 16px 24px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  position: sticky;
  top: 0;
  z-index: 100;
}
.header h1 {
  font-size: 1.25rem;
  font-weight: 700;
  letter-spacing: -.02em;
  display: flex;
  align-items: center;
  gap: 10px;
}
.header h1 .icon { font-size: 1.5rem; }
.header .conn {
  display: flex; align-items: center; gap: 8px;
  font-size: .8rem; color: var(--text2);
}
.conn-dot {
  width: 8px; height: 8px; border-radius: 50%;
  background: var(--red);
  transition: background .3s;
}
.conn-dot.ok { background: var(--green); }

/* Layout */
.container {
  max-width: 1200px;
  margin: 0 auto;
  padding: 20px;
  display: flex;
  flex-direction: column;
  gap: 20px;
}

/* Cards */
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
  overflow: hidden;
}
.card-header {
  padding: 14px 18px;
  border-bottom: 1px solid var(--border);
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: var(--surface2);
}
.card-header h2 {
  font-size: .9rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: .06em;
  color: var(--text2);
}
.card-body { padding: 18px; }

/* Alarm Status */
.alarm-status {
  display: flex;
  align-items: center;
  gap: 20px;
  flex-wrap: wrap;
}
.alarm-badge {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 10px 22px;
  border-radius: 8px;
  font-size: 1.1rem;
  font-weight: 700;
  letter-spacing: .04em;
  text-transform: uppercase;
  transition: all .3s;
}
.alarm-badge.disarmed { background: var(--green-bg); color: var(--green); border: 1px solid rgba(52,211,153,.25); }
.alarm-badge.armed    { background: var(--orange-bg); color: var(--orange); border: 1px solid rgba(251,146,60,.25); }
.alarm-badge.triggered{ background: var(--red-bg); color: var(--red); border: 1px solid rgba(248,113,113,.25); animation: pulse 1s infinite; }
.alarm-badge.delay    { background: var(--yellow-bg); color: var(--yellow); border: 1px solid rgba(251,191,36,.25); }

@keyframes pulse {
  0%,100% { opacity: 1; }
  50% { opacity: .6; }
}

.alarm-actions {
  display: flex; gap: 8px; flex-wrap: wrap; margin-left: auto;
}
.btn {
  padding: 8px 18px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--surface2);
  color: var(--text);
  font-size: .85rem;
  font-weight: 600;
  cursor: pointer;
  transition: all .15s;
  display: inline-flex;
  align-items: center;
  gap: 6px;
}
.btn:hover { background: var(--border); transform: translateY(-1px); }
.btn:active { transform: translateY(0); }
.btn.arm { border-color: var(--orange); color: var(--orange); }
.btn.arm:hover { background: var(--orange-bg); }
.btn.disarm { border-color: var(--green); color: var(--green); }
.btn.disarm:hover { background: var(--green-bg); }
.btn.mute { border-color: var(--yellow); color: var(--yellow); }
.btn.mute:hover { background: var(--yellow-bg); }
.btn.danger { border-color: var(--red); color: var(--red); }
.btn.danger:hover { background: var(--red-bg); }

.delay-info {
  font-size: 1.6rem;
  font-weight: 700;
  color: var(--yellow);
  font-variant-numeric: tabular-nums;
}

/* Zone Grid */
.zone-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(250px, 1fr));
  gap: 12px;
}
.zone-card {
  background: var(--surface2);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 14px;
  transition: all .2s;
  position: relative;
  overflow: hidden;
}
.zone-card::before {
  content: '';
  position: absolute;
  left: 0; top: 0; bottom: 0;
  width: 4px;
  border-radius: 4px 0 0 4px;
  transition: background .3s;
}
.zone-card.normal::before   { background: var(--green); }
.zone-card.triggered::before{ background: var(--red); }
.zone-card.bypassed::before { background: var(--yellow); }
.zone-card.disabled::before { background: var(--gray); }
.zone-card.fault::before    { background: var(--orange); }

.zone-card:hover { border-color: var(--accent); transform: translateY(-2px); }
.zone-top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}
.zone-num {
  font-size: .7rem;
  font-weight: 700;
  color: var(--text2);
  background: var(--bg);
  padding: 2px 8px;
  border-radius: 4px;
}
.zone-state-pill {
  font-size: .7rem;
  font-weight: 600;
  padding: 2px 10px;
  border-radius: 20px;
  text-transform: uppercase;
  letter-spacing: .04em;
}
.zone-state-pill.normal   { background: var(--green-bg); color: var(--green); }
.zone-state-pill.triggered{ background: var(--red-bg); color: var(--red); }
.zone-state-pill.bypassed { background: var(--yellow-bg); color: var(--yellow); }
.zone-state-pill.disabled { background: rgba(107,114,128,.15); color: var(--gray); }
.zone-state-pill.fault    { background: var(--orange-bg); color: var(--orange); }
.zone-state-pill.tamper   { background: var(--red-bg); color: var(--red); }

.zone-name {
  font-size: .95rem;
  font-weight: 600;
  margin-bottom: 6px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.zone-meta {
  font-size: .75rem;
  color: var(--text2);
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.zone-actions { margin-top: 8px; }
.zone-actions .btn { font-size: .75rem; padding: 4px 12px; }

/* Outputs Grid */
.output-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
  gap: 10px;
}
.output-item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: var(--surface2);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 10px 14px;
  transition: all .2s;
}
.output-item:hover { border-color: var(--accent); }
.output-label {
  font-size: .8rem;
  font-weight: 600;
  color: var(--text2);
}
/* Toggle switch */
.toggle {
  position: relative;
  width: 40px; height: 22px;
  cursor: pointer;
}
.toggle input { display: none; }
.toggle .slider {
  position: absolute;
  inset: 0;
  background: var(--border);
  border-radius: 11px;
  transition: background .2s;
}
.toggle .slider::before {
  content: '';
  position: absolute;
  width: 16px; height: 16px;
  left: 3px; bottom: 3px;
  background: var(--text2);
  border-radius: 50%;
  transition: all .2s;
}
.toggle input:checked + .slider { background: var(--accent); }
.toggle input:checked + .slider::before { transform: translateX(18px); background: #fff; }

/* System Info */
.sys-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
  gap: 12px;
}
.sys-item {
  text-align: center;
  padding: 10px;
}
.sys-val {
  font-size: 1.2rem;
  font-weight: 700;
  color: var(--accent);
  font-variant-numeric: tabular-nums;
}
.sys-label {
  font-size: .7rem;
  color: var(--text2);
  text-transform: uppercase;
  letter-spacing: .06em;
  margin-top: 2px;
}

/* Modal */
.modal-overlay {
  display: none;
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,.6);
  backdrop-filter: blur(4px);
  z-index: 200;
  align-items: center;
  justify-content: center;
}
.modal-overlay.show { display: flex; }
.modal {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 28px;
  width: 340px;
  max-width: 90vw;
  box-shadow: var(--shadow);
}
.modal h3 {
  font-size: 1rem;
  margin-bottom: 16px;
}
.modal input {
  width: 100%;
  padding: 10px 14px;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 8px;
  color: var(--text);
  font-size: 1rem;
  margin-bottom: 16px;
  outline: none;
  transition: border-color .2s;
}
.modal input:focus { border-color: var(--accent); }
.modal-btns {
  display: flex;
  gap: 8px;
  justify-content: flex-end;
}

/* Toast */
.toast-container {
  position: fixed;
  bottom: 24px;
  right: 24px;
  z-index: 300;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.toast {
  padding: 12px 20px;
  border-radius: 8px;
  font-size: .85rem;
  font-weight: 500;
  box-shadow: var(--shadow);
  animation: slideIn .3s ease;
  max-width: 320px;
}
.toast.success { background: #065f46; color: #d1fae5; }
.toast.error   { background: #7f1d1d; color: #fecaca; }
.toast.info    { background: #1e3a5f; color: #bfdbfe; }
@keyframes slideIn {
  from { opacity: 0; transform: translateX(30px); }
  to   { opacity: 1; transform: translateX(0); }
}

/* Responsive */
@media (max-width: 600px) {
  .container { padding: 12px; gap: 14px; }
  .alarm-status { flex-direction: column; align-items: flex-start; }
  .alarm-actions { margin-left: 0; }
  .zone-grid { grid-template-columns: 1fr; }
}
</style>
</head>
<body>

<div class="header">
  <h1><span class="icon">🛡️</span> SF_Alarm</h1>
  <div class="conn">
    <div class="conn-dot" id="connDot"></div>
    <span id="connLabel">Connecting…</span>
    <span id="fwVer"></span>
  </div>
</div>

<div class="container">

  <!-- Alarm Status -->
  <div class="card">
    <div class="card-header"><h2>Alarm Status</h2></div>
    <div class="card-body">
      <div class="alarm-status">
        <div class="alarm-badge disarmed" id="alarmBadge">DISARMED</div>
        <div class="delay-info" id="delayInfo" style="display:none">--s</div>
        <div class="alarm-actions">
          <button class="btn arm" onclick="doAction('arm','away')">🔒 Arm Away</button>
          <button class="btn arm" onclick="doAction('arm','home')">🏠 Arm Home</button>
          <button class="btn disarm" onclick="doAction('disarm')">🔓 Disarm</button>
          <button class="btn mute" onclick="doMute()">🔇 Mute</button>
        </div>
      </div>
    </div>
  </div>

  <!-- Zones -->
  <div class="card">
    <div class="card-header"><h2>Zones</h2></div>
    <div class="card-body">
      <div class="zone-grid" id="zoneGrid"></div>
    </div>
  </div>

  <!-- Outputs -->
  <div class="card">
    <div class="card-header"><h2>Outputs</h2></div>
    <div class="card-body">
      <div class="output-grid" id="outputGrid"></div>
    </div>
  </div>

  <!-- System Info -->
  <div class="card">
    <div class="card-header"><h2>System</h2></div>
    <div class="card-body">
      <div class="sys-grid" id="sysGrid">
        <div class="sys-item"><div class="sys-val" id="sysIp">--</div><div class="sys-label">IP Address</div></div>
        <div class="sys-item"><div class="sys-val" id="sysRssi">--</div><div class="sys-label">RSSI (dBm)</div></div>
        <div class="sys-item"><div class="sys-val" id="sysUptime">--</div><div class="sys-label">Uptime</div></div>
        <div class="sys-item"><div class="sys-val" id="sysHeap">--</div><div class="sys-label">Free Heap</div></div>
      </div>
    </div>
  </div>

</div>

<!-- PIN Modal -->
<div class="modal-overlay" id="pinModal">
  <div class="modal">
    <h3 id="modalTitle">Enter PIN</h3>
    <input type="password" id="pinInput" placeholder="PIN" maxlength="8" autocomplete="off">
    <div class="modal-btns">
      <button class="btn" onclick="closeModal()">Cancel</button>
      <button class="btn arm" id="modalOk" onclick="submitModal()">OK</button>
    </div>
  </div>
</div>

<!-- Toast -->
<div class="toast-container" id="toasts"></div>

<script>
let pendingAction = null;
let pollTimer = null;
let connected = false;

// --- Toast ---
function toast(msg, type='info') {
  const c = document.getElementById('toasts');
  const t = document.createElement('div');
  t.className = 'toast ' + type;
  t.textContent = msg;
  c.appendChild(t);
  setTimeout(() => { t.style.opacity='0'; setTimeout(()=>t.remove(),300); }, 3500);
}

// --- Modal ---
function openModal(title, cb) {
  document.getElementById('modalTitle').textContent = title;
  document.getElementById('pinInput').value = '';
  pendingAction = cb;
  document.getElementById('pinModal').classList.add('show');
  setTimeout(()=>document.getElementById('pinInput').focus(), 100);
}
function closeModal() {
  document.getElementById('pinModal').classList.remove('show');
  pendingAction = null;
}
function submitModal() {
  const pin = document.getElementById('pinInput').value;
  if (!pin) { toast('PIN is required','error'); return; }
  if (pendingAction) pendingAction(pin);
  closeModal();
}
document.getElementById('pinInput').addEventListener('keydown', e => {
  if (e.key === 'Enter') submitModal();
  if (e.key === 'Escape') closeModal();
});

// --- API ---
async function apiPost(url, body) {
  try {
    const r = await fetch(url, {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify(body)
    });
    const j = await r.json();
    if (j.ok) toast(j.msg || 'OK', 'success');
    else toast(j.msg || 'Failed', 'error');
    refresh();
  } catch (e) { toast('Network error','error'); }
}

function doAction(action, mode) {
  if (action === 'arm') {
    openModal('Enter PIN to Arm ' + (mode==='home'?'Home':'Away'), pin => {
      apiPost('/api/arm', {pin, mode});
    });
  } else if (action === 'disarm') {
    openModal('Enter PIN to Disarm', pin => {
      apiPost('/api/disarm', {pin});
    });
  }
}
function doMute() { apiPost('/api/mute', {}); }

function doBypass(zone, bypass) {
  apiPost('/api/bypass', {zone, bypass});
}

function doOutput(ch, state) {
  apiPost('/api/output', {channel: ch, state});
}

// --- Status ---
const zoneStateMap = ['NORMAL','TRIGGERED','TAMPER','FAULT','BYPASSED'];
const zoneStateClass = ['normal','triggered','tamper','fault','bypassed'];
const zoneTypeMap = ['Instant','Delayed','24-Hour','Follower'];
const alarmStates = ['DISARMED','EXIT DELAY','ARMED AWAY','ARMED HOME','ENTRY DELAY','TRIGGERED'];
const alarmClass  = ['disarmed','delay','armed','armed','delay','triggered'];

function fmtUptime(s) {
  const d = Math.floor(s/86400); s %= 86400;
  const h = Math.floor(s/3600); s %= 3600;
  const m = Math.floor(s/60); s %= 60;
  let r = '';
  if (d) r += d+'d ';
  r += String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(Math.floor(s)).padStart(2,'0');
  return r;
}

function renderZones(zones) {
  const g = document.getElementById('zoneGrid');
  let html = '';
  for (const z of zones) {
    const sc = z.enabled ? zoneStateClass[z.stateCode] || 'normal' : 'disabled';
    const sl = z.enabled ? (zoneStateMap[z.stateCode] || 'UNKNOWN') : 'DISABLED';
    const isBypassed = z.stateCode === 4;
    html += `<div class="zone-card ${sc}">
      <div class="zone-top">
        <span class="zone-num">Z${z.index+1}</span>
        <span class="zone-state-pill ${sc}">${sl}</span>
      </div>
      <div class="zone-name">${escHtml(z.name)}</div>
      <div class="zone-meta">
        <span>${zoneTypeMap[z.typeCode]||'?'} · ${z.wiring}</span>
        <span>${z.rawInput?'⚡ Active':'—'}</span>
      </div>
      <div class="zone-actions">
        ${z.enabled ? (isBypassed
          ? `<button class="btn" onclick="doBypass(${z.index},false)">Unbypass</button>`
          : `<button class="btn" onclick="doBypass(${z.index},true)">Bypass</button>`
        ) : ''}
      </div>
    </div>`;
  }
  g.innerHTML = html;
}

function renderOutputs(mask) {
  const g = document.getElementById('outputGrid');
  let html = '';
  for (let i = 0; i < 16; i++) {
    const on = (mask >> i) & 1;
    html += `<div class="output-item">
      <span class="output-label">OUT ${i+1}</span>
      <label class="toggle">
        <input type="checkbox" ${on?'checked':''} onchange="doOutput(${i},this.checked)">
        <span class="slider"></span>
      </label>
    </div>`;
  }
  g.innerHTML = html;
}

function escHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

async function refresh() {
  try {
    const r = await fetch('/api/status');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const d = await r.json();

    // Connection
    if (!connected) { connected = true; toast('Connected','success'); }
    document.getElementById('connDot').classList.add('ok');
    document.getElementById('connLabel').textContent = 'Connected';

    // Alarm
    const ab = document.getElementById('alarmBadge');
    const ac = alarmClass[d.alarm.stateCode] || 'disarmed';
    ab.className = 'alarm-badge ' + ac;
    ab.textContent = alarmStates[d.alarm.stateCode] || d.alarm.state;

    const di = document.getElementById('delayInfo');
    if (d.alarm.stateCode === 1 || d.alarm.stateCode === 4) {
      di.style.display = '';
      di.textContent = d.alarm.delayRemaining + 's';
    } else {
      di.style.display = 'none';
    }

    // Zones
    renderZones(d.zones);

    // Outputs
    renderOutputs(d.outputs);

    // System
    document.getElementById('sysIp').textContent = d.network.ip;
    document.getElementById('sysRssi').textContent = d.network.rssi;
    document.getElementById('sysUptime').textContent = fmtUptime(d.system.uptime);
    document.getElementById('sysHeap').textContent = (d.system.freeHeap/1024).toFixed(1)+'KB';
    document.getElementById('fwVer').textContent = 'v'+d.system.version;

  } catch (e) {
    if (connected) { connected = false; toast('Connection lost','error'); }
    document.getElementById('connDot').classList.remove('ok');
    document.getElementById('connLabel').textContent = 'Disconnected';
  }
}

// Start
refresh();
pollTimer = setInterval(refresh, 2000);
</script>
</body>
</html>
)rawliteral";

#endif // SF_ALARM_WEB_UI_H

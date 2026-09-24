#pragma once

// Served in AP mode (setup): 2-step wizard — WiFi, then printer.
static const char SETUP_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bambu LED Status Setup</title>
<style>
  :root{--bg:#12161c;--card:#1b212b;--text:#eef1f5;--muted:#8b93a1;--accent:#00c37a;--border:#2a3240;--danger:#e5484d}
  *{box-sizing:border-box}
  body{margin:0;padding:24px 16px 60px;background:var(--bg);color:var(--text);font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
  .card{max-width:440px;margin:0 auto 16px;background:var(--card);border:1px solid var(--border);border-radius:14px;padding:22px 20px}
  .card.disabled{opacity:.45;pointer-events:none}
  .card.hidden{display:none}
  h1{font-size:20px;margin:0 0 4px}
  .subtitle{color:var(--muted);font-size:13px;margin:0 0 18px}
  h2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:0 0 12px;display:flex;align-items:center;gap:8px}
  .step-num{width:20px;height:20px;border-radius:50%;background:var(--border);color:var(--text);font-size:11px;display:inline-flex;align-items:center;justify-content:center;flex:none}
  .step-num.done{background:var(--accent);color:#04231a}
  label{display:block;font-size:13px;color:var(--muted);margin:14px 0 6px}
  input{width:100%;padding:11px 12px;border-radius:9px;border:1px solid var(--border);background:#0f1319;color:var(--text);font-size:15px}
  input:focus{outline:none;border-color:var(--accent)}
  .net-list{border:1px solid var(--border);border-radius:9px;overflow:hidden;margin-top:4px;max-height:220px;overflow-y:auto}
  .net-item{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;font-size:14px;cursor:pointer;border-bottom:1px solid var(--border)}
  .net-item:last-child{border-bottom:none}
  .net-item:hover,.net-item.sel{background:#232b38}
  .net-item .meta{color:var(--muted);font-size:12px}
  .refresh{background:none;border:1px solid var(--border);color:var(--muted);font-size:12px;padding:6px 10px;border-radius:7px;cursor:pointer}
  .pwd-row{position:relative}
  .pwd-row button{position:absolute;right:6px;top:6px;background:none;border:none;color:var(--muted);font-size:12px;cursor:pointer;padding:6px 8px}
  button.primary{width:100%;margin-top:20px;padding:13px;border:none;border-radius:9px;background:var(--accent);color:#04231a;font-weight:600;font-size:15px;cursor:pointer}
  button.primary:disabled{opacity:.6}
  button.link{background:none;border:none;color:var(--accent);font-size:13px;cursor:pointer;padding:6px 0;text-decoration:underline}
  .status{font-size:13px;margin-top:12px;text-align:center;min-height:18px}
  .status.err{color:var(--danger)}
  .status.ok{color:var(--accent)}
  .hint{font-size:12px;color:var(--muted);margin-top:4px}
  .row-head{display:flex;justify-content:space-between;align-items:center}
  .manual-fields{margin-top:6px}
</style>
</head>
<body>
  <div class="card" id="step1">
    <h1>Bambu LED Status</h1>
    <p class="subtitle">Connect the device to your WiFi network.</p>

    <div class="row-head"><h2><span class="step-num">1</span>WiFi network</h2><button class="refresh" id="btnScanWifi" type="button">Refresh</button></div>
    <div class="net-list" id="netList"><div class="net-item"><span>Scanning...</span></div></div>

    <label for="ssid">Network name (SSID)</label>
    <input id="ssid" placeholder="Pick above or type manually" autocomplete="off">

    <label for="pass">Password</label>
    <div class="pwd-row">
      <input id="pass" type="password" placeholder="WiFi password" autocomplete="off">
      <button type="button" id="togglePass">show</button>
    </div>

    <button class="primary" id="btnConnect">Connect</button>
    <p class="status" id="status1"></p>
  </div>

  <div class="card hidden" id="step2">
    <h2><span class="step-num done">✓</span>Connected</h2>
    <p class="hint" id="connectedInfo"></p>

    <h2 style="margin-top:18px"><span class="step-num">2</span>Find your printer</h2>
    <div class="row-head"><span class="hint" id="scanHint">Searching the network for Bambu Lab printers...</span><button class="refresh" id="btnScanPrinter" type="button">Search again</button></div>
    <div class="net-list hidden" id="printerList"></div>
    <button class="link" id="btnManualToggle" type="button">Enter printer info manually</button>

    <div class="manual-fields" id="manualFields">
      <label for="pip">Printer IP</label>
      <input id="pip" placeholder="e.g. 192.168.1.50" autocomplete="off">
      <label for="pserial">Serial number</label>
      <input id="pserial" placeholder="e.g. 01P00A000000000" autocomplete="off">
    </div>

    <label for="pcode">Access Code</label>
    <div class="pwd-row">
      <input id="pcode" type="password" placeholder="LAN Mode access code" autocomplete="off">
      <button type="button" id="toggleCode">show</button>
    </div>
    <p class="hint">On the printer: Settings &gt; WLAN &gt; enable LAN Only Mode &gt; tap the gear/info icon to see the Access Code and Serial Number.</p>

    <button class="primary" id="btnSave">Save &amp; finish</button>
    <p class="status" id="status2"></p>
  </div>

<script>
let networks = [];
let printers = [];
let connectedIp = '';

function escapeHtml(s){ return s.replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }

// ---------- Step 1: WiFi ----------

function renderNetworks(){
  const el = document.getElementById('netList');
  if(!networks.length){ el.innerHTML = '<div class="net-item"><span>No networks found</span></div>'; return; }
  el.innerHTML = networks.map(n => {
    const lock = n.secure ? '🔒' : '';
    return `<div class="net-item" data-ssid="${escapeHtml(n.ssid)}" onclick="selectNetwork(this)">
      <span>${escapeHtml(n.ssid)} ${lock}</span><span class="meta">${n.rssi} dBm</span></div>`;
  }).join('');
}

function selectNetwork(elm){
  document.querySelectorAll('#netList .net-item').forEach(e => e.classList.remove('sel'));
  elm.classList.add('sel');
  document.getElementById('ssid').value = elm.dataset.ssid;
  document.getElementById('pass').focus();
}

async function scanWifi(){
  document.getElementById('netList').innerHTML = '<div class="net-item"><span>Scanning...</span></div>';
  try{
    const r = await fetch('/scan');
    networks = await r.json();
    networks.sort((a,b)=>b.rssi-a.rssi);
    renderNetworks();
  }catch(e){
    document.getElementById('netList').innerHTML = '<div class="net-item"><span>Scan failed</span></div>';
  }
}

document.getElementById('btnScanWifi').addEventListener('click', scanWifi);
document.getElementById('togglePass').addEventListener('click', () => {
  const p = document.getElementById('pass');
  p.type = p.type === 'password' ? 'text' : 'password';
});

document.getElementById('btnConnect').addEventListener('click', async () => {
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  const status = document.getElementById('status1');
  if(!ssid){ status.textContent = 'Select or type a network name.'; status.className = 'status err'; return; }

  document.getElementById('btnConnect').disabled = true;
  status.textContent = 'Connecting... this can take up to 20s.'; status.className = 'status';

  try{
    const r = await fetch('/connect-wifi', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({ssid, pass})
    });
    const d = await r.json();
    if(!d.ok){
      status.textContent = 'Could not connect. Check the password and try again.';
      status.className = 'status err';
      document.getElementById('btnConnect').disabled = false;
      return;
    }
    connectedIp = d.ip;
    status.textContent = ''; status.className = 'status';
    document.getElementById('step1').classList.add('disabled');
    document.getElementById('step2').classList.remove('hidden');
    document.getElementById('connectedInfo').textContent = `Joined "${ssid}" — IP ${d.ip}`;
    scanPrinters();
  }catch(e){
    status.textContent = 'Request failed. Try again.';
    status.className = 'status err';
    document.getElementById('btnConnect').disabled = false;
  }
});

// ---------- Step 2: printer ----------

function renderPrinters(){
  const el = document.getElementById('printerList');
  const hint = document.getElementById('scanHint');
  if(!printers.length){
    el.classList.add('hidden');
    hint.textContent = 'No printers found automatically. Enter the info manually below.';
    setManualMode(true);
    return;
  }
  hint.textContent = `Found ${printers.length} printer(s):`;
  el.classList.remove('hidden');
  el.innerHTML = printers.map(p => {
    const label = p.name || p.model || 'Bambu Lab printer';
    return `<div class="net-item" data-ip="${escapeHtml(p.ip)}" data-serial="${escapeHtml(p.serial)}" onclick="selectPrinter(this)">
      <span>${escapeHtml(label)} (${escapeHtml(p.model)})</span><span class="meta">${escapeHtml(p.ip)}</span></div>`;
  }).join('');
}

function selectPrinter(elm){
  document.querySelectorAll('#printerList .net-item').forEach(e => e.classList.remove('sel'));
  elm.classList.add('sel');
  document.getElementById('pip').value = elm.dataset.ip;
  document.getElementById('pserial').value = elm.dataset.serial;
  setManualMode(false);
  document.getElementById('pcode').focus();
}

function setManualMode(show){
  document.getElementById('manualFields').style.display = show ? 'block' : 'none';
}

async function scanPrinters(){
  document.getElementById('scanHint').textContent = 'Searching the network for Bambu Lab printers...';
  document.getElementById('printerList').classList.add('hidden');
  try{
    const r = await fetch('/scan-printer');
    printers = await r.json();
    renderPrinters();
  }catch(e){
    document.getElementById('scanHint').textContent = 'Search failed. Enter the info manually below.';
    setManualMode(true);
  }
}

document.getElementById('btnScanPrinter').addEventListener('click', scanPrinters);
document.getElementById('btnManualToggle').addEventListener('click', () => {
  document.querySelectorAll('#printerList .net-item').forEach(e => e.classList.remove('sel'));
  document.getElementById('pip').value = '';
  document.getElementById('pserial').value = '';
  setManualMode(true);
  document.getElementById('pip').focus();
});
document.getElementById('toggleCode').addEventListener('click', () => {
  const p = document.getElementById('pcode');
  p.type = p.type === 'password' ? 'text' : 'password';
});

document.getElementById('btnSave').addEventListener('click', async () => {
  const pip = document.getElementById('pip').value.trim();
  const pserial = document.getElementById('pserial').value.trim();
  const pcode = document.getElementById('pcode').value.trim();
  const status = document.getElementById('status2');

  if(!pip || !pserial || !pcode){ status.textContent = 'Fill in IP, serial and access code.'; status.className = 'status err'; return; }

  document.getElementById('btnSave').disabled = true;
  status.textContent = 'Saving...'; status.className = 'status';

  try{
    const r = await fetch('/save-printer', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({printer_ip: pip, printer_serial: pserial, printer_access_code: pcode})
    });
    if(!r.ok) throw new Error('bad response');
    status.className = 'status ok';
    startRedirectCountdown(status);
  }catch(e){
    status.textContent = 'Error saving. Try again.';
    status.className = 'status err';
    document.getElementById('btnSave').disabled = false;
  }
});

// bambuled.local doesn't always resolve (router/OS-dependent mDNS support),
// so fall back to the raw IP we already got back in step 1 — same device,
// same DHCP lease, should still be reachable once it reboots into station mode.
function startRedirectCountdown(status){
  if (!connectedIp) {
    status.textContent = 'Saved! Rebooting — reconnect to your WiFi and visit http://bambuled.local';
    return;
  }
  const url = `http://${connectedIp}`;
  let secs = 20;
  const render = () => {
    status.innerHTML = `Saved! Redirecting to <a href="${url}">${url}</a> in ${secs}s...`;
  };
  render();
  const timer = setInterval(() => {
    secs--;
    if (secs <= 0) { clearInterval(timer); window.location.href = url; return; }
    render();
  }, 1000);
}

setManualMode(false);
scanWifi();
</script>
</body>
</html>
)HTML";

// Served once connected to WiFi (station mode): status + reconfiguration.
static const char STATUS_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bambu LED Status</title>
<style>
  :root{--bg:#12161c;--card:#1b212b;--text:#eef1f5;--muted:#8b93a1;--accent:#00c37a;--border:#2a3240;--danger:#e5484d}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
  .shell{display:flex;max-width:900px;margin:0 auto;min-height:100vh}
  .sidebar{width:190px;flex:none;padding:24px 16px;border-right:1px solid var(--border)}
  .sidebar .brand{font-size:15px;font-weight:700;margin-bottom:20px}
  .sidebar .nav-link{display:block;padding:9px 10px;border-radius:8px;color:var(--muted);text-decoration:none;font-size:14px;margin-bottom:2px}
  .sidebar .nav-link.active{background:var(--card);color:var(--text);font-weight:600}
  .sidebar .nav-sep{height:1px;background:var(--border);margin:14px 0}
  .sidebar .ext-link{display:block;padding:6px 10px;color:var(--muted);text-decoration:none;font-size:13px}
  .sidebar .version{padding:10px 10px 0;font-size:11px;color:var(--muted)}
  .content{flex:1;padding:24px 20px 60px;min-width:0}
  .card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:22px 20px;margin-bottom:16px}
  @media (max-width:680px){
    .shell{flex-direction:column}
    .sidebar{width:100%;border-right:none;border-bottom:1px solid var(--border);display:flex;align-items:center;flex-wrap:wrap;gap:4px 12px;padding:12px 16px}
    .sidebar .brand{width:100%;margin-bottom:4px}
    .sidebar .nav-sep{display:none}
    .sidebar .version{width:100%;padding:6px 0 0}
  }
  h1{font-size:20px;margin:0 0 4px}
  .subtitle{color:var(--muted);font-size:13px;margin:0 0 18px}
  .sech2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:0 0 14px}
  .kv{display:flex;justify-content:space-between;padding:9px 0;border-bottom:1px solid var(--border);font-size:14px}
  .kv:last-child{border-bottom:none}
  .kv span:first-child{color:var(--muted)}
  button{width:100%;margin-top:12px;padding:12px;border-radius:9px;font-size:14px;font-weight:600;cursor:pointer;border:1px solid var(--border);background:#0f1319;color:var(--text)}
  button.danger{color:var(--danger);border-color:var(--danger)}
  .mqtt-row{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--muted);margin-bottom:16px}
  .dot{width:9px;height:9px;border-radius:50%;background:#3a4353;flex:none}
  .dot.on{background:var(--accent)}
  .dot.off{background:var(--danger)}
  .led-bar{display:flex;border-radius:6px;overflow:hidden;margin:4px 0 12px;background:#0f1319;height:30px}
  .led-bar .seg{flex:1;height:100%}
  .state-line{display:flex;justify-content:space-between;align-items:baseline;font-size:13px}
  .state-line .state-name{font-size:16px;font-weight:600;text-transform:capitalize}
  .state-line .state-pct{color:var(--muted)}
  .hint{font-size:12px;color:var(--muted);margin-top:10px}
</style>
</head>
<body>
<div class="shell">
  <nav class="sidebar">
    <div class="brand">Bambu LED Status</div>
    <a href="/" class="nav-link active">Status</a>
    <a href="/settings" class="nav-link">Settings</a>
    <div class="nav-sep"></div>
    <a href="#" id="linkMakerworld" class="ext-link" target="_blank" rel="noopener">MakerWorld</a>
    <a href="#" id="linkGithub" class="ext-link" target="_blank" rel="noopener">GitHub</a>
    <div class="version" id="versionLine">—</div>
  </nav>
  <main class="content">
    <div class="card">
      <h1>Status</h1>
      <p class="subtitle">Device connected.</p>
      <div id="kvs">Loading...</div>
    </div>

    <div class="card">
      <h2 class="sech2">LED preview</h2>
      <div class="mqtt-row"><span class="dot" id="mqttDot"></span><span id="mqttLabel">MQTT: —</span></div>
      <div class="led-bar" id="ledBar"></div>
      <div class="state-line"><span class="state-name" id="stateName">unknown</span><span class="state-pct" id="statePct"></span></div>
      <p class="hint">Live animation, rendered from your saved Settings — this is what the physical strip shows once it's wired up.</p>
    </div>

    <div class="card">
      <button id="btnWifi">Reconfigure WiFi</button>
      <button id="btnFactory" class="danger">Factory reset</button>
    </div>
  </main>
</div>
<script>
const UNKNOWN_CFG = {effect:'Fade', color1:'#505050', color2:'#505050'};
// Mirrors the hardcoded per-state speed/intensity in LedAnimations.cpp —
// these aren't part of the saved config (see Settings page comment).
const SPEED_INTENSITY = {
  idle: {speed:134, intensity:128}, calibrating: {speed:130, intensity:100},
  printing: {speed:128, intensity:128}, paused: {speed:60, intensity:128},
  finished: {speed:60, intensity:128}, error: {speed:255, intensity:128},
  unknown: {speed:60, intensity:128},
};

function hexToRgb(hex){ const v=(hex||'#000000').replace('#',''); return [parseInt(v.substr(0,2),16)||0,parseInt(v.substr(2,2),16)||0,parseInt(v.substr(4,2),16)||0]; }
function lerpRgb(a,b,t){ return [a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t]; }
function cssRgb(rgb,scale){ return `rgb(${Math.round(rgb[0]*scale)},${Math.round(rgb[1]*scale)},${Math.round(rgb[2]*scale)})`; }

// Same rendering logic as the Settings page's live preview, driven by the real
// saved config for the current state (and the printer's real percent, if printing).
function renderFrame(cfg, speed, intensity, brightness, tSec, realPercent){
  const N = 10;
  const c1 = hexToRgb(cfg.color1), c2 = hexToRgb(cfg.color2);
  const bri = 0.35 + (brightness / 255) * 0.65; // floor so dim states are still visible on screen
  const bpm = 4 + (speed / 255) * 46;
  const beats = tSec * (bpm / 60);
  const out = new Array(N).fill(0).map(() => [0, 0, 0]);

  switch (cfg.effect) {
    case 'Off': break;
    case 'Solid': for (let i = 0; i < N; i++) out[i] = c1; break;
    case 'Fade': {
      const level = 0.1 + ((Math.sin(beats * 2 * Math.PI) + 1) / 2) * 0.9;
      for (let i = 0; i < N; i++) out[i] = lerpRgb([0,0,0], c1, level);
      break;
    }
    case 'Loading': {
      const pos = ((Math.sin(beats * 2 * Math.PI) + 1) / 2) * (N - 1);
      const trail = 1 + (255 - intensity) / 255 * 3.5;
      for (let i = 0; i < N; i++) {
        const f = Math.max(0, 1 - Math.abs(i - pos) / trail);
        out[i] = lerpRgb([0,0,0], i === Math.round(pos) ? lerpRgb(c1, c2, 0.5) : c1, f);
      }
      break;
    }
    case 'Percent': {
      const pct = (realPercent !== null && realPercent !== undefined) ? realPercent : ((Math.sin(beats * 0.5 * 2 * Math.PI) + 1) / 2 * 100);
      const lit = Math.floor(pct / 10);
      for (let i = 0; i < N; i++) out[i] = i < lit ? c1 : (i === lit ? c2 : [0, 0, 0]);
      break;
    }
    case 'Plasmoid': {
      for (let i = 0; i < N; i++) {
        const w = (Math.sin((beats + i / N) * 2 * Math.PI) + 1) / 2;
        out[i] = lerpRgb(c1, c2, w);
      }
      break;
    }
    case 'Bounce': {
      const pos = Math.abs(Math.sin(beats * 2 * Math.PI)) * (N - 1);
      for (let i = 0; i < N; i++) {
        const f = Math.max(0, 1 - Math.abs(i - pos) / 1.6);
        out[i] = lerpRgb([0,0,0], lerpRgb(c1, c2, 0.3), f);
      }
      break;
    }
  }
  return out.map(rgb => cssRgb(rgb, bri));
}

const ledBar = document.getElementById('ledBar');
for (let i = 0; i < 10; i++) { const seg = document.createElement('div'); seg.className = 'seg'; ledBar.appendChild(seg); }

let animConfig = null, currentState = 'unknown', currentPercent = null;

function tick(tMs){
  const cfg = (animConfig && animConfig[currentState]) ? animConfig[currentState] : UNKNOWN_CFG;
  const si = SPEED_INTENSITY[currentState] || SPEED_INTENSITY.unknown;
  const brightness = animConfig ? animConfig.brightness : 40;
  const colors = renderFrame(cfg, si.speed, si.intensity, brightness, tMs / 1000, currentState === 'printing' ? currentPercent : null);
  const segs = ledBar.children;
  for (let i = 0; i < segs.length; i++) segs[i].style.background = colors[i];
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

async function load(){
  const [rs, ra] = await Promise.all([fetch('/api/status'), fetch('/api/anim')]);
  const d = await rs.json();
  animConfig = await ra.json();
  document.getElementById('kvs').innerHTML = `
    <div class="kv"><span>Network</span><span>${d.wifi_ssid}</span></div>
    <div class="kv"><span>IP</span><span>${d.ip}</span></div>
    <div class="kv"><span>Hostname</span><span>${d.hostname}</span></div>
    <div class="kv"><span>Printer IP</span><span>${d.printer_ip}</span></div>
    <div class="kv"><span>Serial</span><span>${d.printer_serial}</span></div>
    <div class="kv"><span>Access code</span><span>${d.printer_access_code_masked}</span></div>
    <div class="kv"><span>Uptime</span><span>${d.uptime_s}s</span></div>
  `;
  currentState = d.printer_state || 'unknown';
  currentPercent = (typeof d.printer_percent === 'number' && d.printer_percent >= 0) ? d.printer_percent : null;
  document.getElementById('stateName').textContent = currentState;
  document.getElementById('statePct').textContent = (currentState === 'printing' && currentPercent !== null) ? currentPercent + '%' : '';

  const configured = !!d.printer_ip;
  const dot = document.getElementById('mqttDot');
  const label = document.getElementById('mqttLabel');
  if (!configured) { dot.className = 'dot'; label.textContent = 'MQTT: no printer configured'; }
  else if (d.mqtt_connected) { dot.className = 'dot on'; label.textContent = 'MQTT: connected'; }
  else { dot.className = 'dot off'; label.textContent = 'MQTT: disconnected'; }
}
setInterval(load, 2000);
load();

document.getElementById('btnWifi').addEventListener('click', async () => {
  if(!confirm('Erase WiFi credentials and reboot into setup mode?')) return;
  await fetch('/api/reset-wifi', {method:'POST'});
  alert('Rebooting... connect to the "BambuLED-XXXX" AP.');
});
document.getElementById('btnFactory').addEventListener('click', async () => {
  if(!confirm('This erases ALL settings (WiFi + printer). Continue?')) return;
  await fetch('/api/reset', {method:'POST'});
  alert('Rebooting into setup mode...');
});

async function loadInfo(){
  try{
    const r = await fetch('/api/info');
    const d = await r.json();
    document.getElementById('linkMakerworld').href = d.makerworld_url;
    document.getElementById('linkGithub').href = d.github_url;
    document.getElementById('versionLine').textContent = `${d.project_name} v${d.firmware_version}`;
  }catch(e){}
}
loadInfo();
</script>
</body>
</html>
)HTML";

// Served once connected to WiFi: LED animation settings + OTA firmware upload.
static const char SETTINGS_PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bambu LED Status Settings</title>
<style>
  :root{--bg:#12161c;--card:#1b212b;--text:#eef1f5;--muted:#8b93a1;--accent:#00c37a;--border:#2a3240;--danger:#e5484d}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
  .shell{display:flex;max-width:900px;margin:0 auto;min-height:100vh}
  .sidebar{width:190px;flex:none;padding:24px 16px;border-right:1px solid var(--border)}
  .sidebar .brand{font-size:15px;font-weight:700;margin-bottom:20px}
  .sidebar .nav-link{display:block;padding:9px 10px;border-radius:8px;color:var(--muted);text-decoration:none;font-size:14px;margin-bottom:2px}
  .sidebar .nav-link.active{background:var(--card);color:var(--text);font-weight:600}
  .sidebar .nav-sep{height:1px;background:var(--border);margin:14px 0}
  .sidebar .ext-link{display:block;padding:6px 10px;color:var(--muted);text-decoration:none;font-size:13px}
  .sidebar .version{padding:10px 10px 0;font-size:11px;color:var(--muted)}
  .content{flex:1;padding:24px 20px 60px;min-width:0}
  .card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:22px 20px;margin-bottom:16px}
  @media (max-width:680px){
    .shell{flex-direction:column}
    .sidebar{width:100%;border-right:none;border-bottom:1px solid var(--border);display:flex;align-items:center;flex-wrap:wrap;gap:4px 12px;padding:12px 16px}
    .sidebar .brand{width:100%;margin-bottom:4px}
    .sidebar .nav-sep{display:none}
    .sidebar .version{width:100%;padding:6px 0 0}
  }
  h2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:0 0 14px}
  .kv{display:flex;justify-content:space-between;padding:9px 0;border-bottom:1px solid var(--border);font-size:14px}
  .kv:last-child{border-bottom:none}
  .kv span:first-child{color:var(--muted)}
  .anim-state{border-top:1px solid var(--border);padding-top:16px;margin-top:16px}
  .anim-state:first-of-type{border-top:none;padding-top:0;margin-top:0}
  .anim-state h3{font-size:15px;margin:0 0 10px}
  .row{margin-bottom:10px}
  .row label{display:block;font-size:12px;color:var(--muted);margin-bottom:4px}
  .row.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .row input[type=range]{width:100%}
  .color-field{display:flex;gap:6px}
  .color-native{width:34px;height:34px;padding:2px;border-radius:8px;border:1px solid var(--border);background:#0f1319;flex:none;cursor:pointer}
  .color-hex{flex:1;min-width:0;width:100%;padding:8px 9px;border-radius:8px;border:1px solid var(--border);background:#0f1319;color:var(--text);font-size:13px;font-family:ui-monospace,Consolas,monospace;text-transform:uppercase}
  .led-bar{display:flex;border-radius:6px;overflow:hidden;height:28px;background:#0f1319}
  .led-bar .seg{flex:1;height:100%;background:#232b38}
  button{padding:12px;border-radius:9px;font-size:14px;font-weight:600;cursor:pointer;border:1px solid var(--border);background:#0f1319;color:var(--text)}
  button.primary{background:var(--accent);color:#04231a;border:none}
  button.primary:disabled,button.danger:disabled{opacity:.5;cursor:default}
  button.danger{color:var(--danger);border-color:var(--danger)}
  .actions{display:flex;gap:10px;margin-bottom:16px}
  .actions button{flex:1}
  .status{font-size:13px;margin-top:12px;text-align:center;min-height:18px}
  .status.err{color:var(--danger)}
  .status.ok{color:var(--accent)}
  .hint{font-size:12px;color:var(--muted);margin-top:10px}
  #animStates.loading{opacity:.4;pointer-events:none}
  input[type=file]{width:100%;margin:10px 0;font-size:13px;color:var(--muted)}
</style>
</head>
<body>
<div class="shell">
  <nav class="sidebar">
    <div class="brand">Bambu LED Status</div>
    <a href="/" class="nav-link">Status</a>
    <a href="/settings" class="nav-link active">Settings</a>
    <div class="nav-sep"></div>
    <a href="#" id="linkMakerworld" class="ext-link" target="_blank" rel="noopener">MakerWorld</a>
    <a href="#" id="linkGithub" class="ext-link" target="_blank" rel="noopener">GitHub</a>
    <div class="version" id="versionLine">—</div>
  </nav>
  <main class="content">
    <div class="card">
      <h2>LED Animations</h2>
      <p class="hint" id="animLoadHint" style="margin-top:0;margin-bottom:16px">Loading current settings...</p>
      <div class="row" style="margin-bottom:16px">
        <label>Brightness (whole strip, all states)</label>
        <input type="range" min="0" max="255" id="globalBrightness">
      </div>
      <div id="animStates" class="loading"></div>
    </div>

    <div class="actions">
      <button id="btnRestoreDefaults" class="danger" disabled>Restore Defaults</button>
      <button id="btnSaveAnim" class="primary" disabled>Save</button>
    </div>
    <p class="status" id="animStatus"></p>

    <div class="card">
      <h2>Diagnostics</h2>
      <pre id="logBox" style="max-height:240px;overflow-y:auto;background:var(--bg);padding:10px;border-radius:8px;font-size:12px;line-height:1.5;white-space:pre-wrap;margin:0"></pre>
      <p class="hint">Recent connection/state events — no USB cable needed. Clears on reboot.</p>
    </div>
  </main>
</div>
<script>
// speed/intensity mirror the hardcoded values in LedAnimations.cpp — kept
// here only so the in-browser preview animates at roughly the same pace.
const STATES = [
  {key:'idle',        label:'Idle',        colors:2, c1:'Color A',   c2:'Color B',        speed:134, intensity:128},
  {key:'calibrating',  label:'Calibrating',  colors:1, c1:'Color',                          speed:130, intensity:100},
  {key:'printing',     label:'Printing',     colors:2, c1:'Bar Color', c2:'Leading Pixel',  speed:128, intensity:128},
  {key:'paused',       label:'Paused',       colors:1, c1:'Color',                          speed:60,  intensity:128},
  {key:'finished',     label:'Finished',     colors:1, c1:'Color',                          speed:60,  intensity:128},
  {key:'error',        label:'Error',        colors:1, c1:'Color',                          speed:255, intensity:128},
];
// Plain hex text field (always works, no OS/WebView dependency) + a native
// color-swatch button as a convenience picker where the browser supports it.
function colorFieldHtml(field, label, key){
  return `<div><label>${label}</label><div class="color-field">
    <input type="color" class="color-native" data-pair="${field}-${key}">
    <input type="text" class="f-${field} color-hex" id="${field}-${key}" maxlength="7" placeholder="#000000">
  </div></div>`;
}

// Each state has one fixed, hand-picked animation and speed (not
// user-selectable — see AnimConfigStore::defaults()/LedAnimations.cpp on the
// firmware side); only its 1-2 colors are editable here. Brightness is a
// single shared slider for the whole strip, above this list. The block's
// `data-effect` attribute (set in populateState) carries the loaded effect
// name for the in-browser preview to render with.
function stateBlockHtml(s){
  return `
  <div class="anim-state" data-key="${s.key}">
    <h3>${s.label}</h3>
    <div class="row ${s.colors > 1 ? 'two' : ''}">
      ${colorFieldHtml('color1', s.c1, s.key)}
      ${s.colors > 1 ? colorFieldHtml('color2', s.c2, s.key) : ''}
    </div>
    <div class="row">
      <label>Live preview</label>
      <div class="led-bar" id="prevbar-${s.key}"></div>
    </div>
  </div>`;
}

const container = document.getElementById('animStates');
container.innerHTML = STATES.map(stateBlockHtml).join('');

STATES.forEach(s => {
  const bar = document.getElementById('prevbar-' + s.key);
  for (let i = 0; i < 10; i++) {
    const seg = document.createElement('div');
    seg.className = 'seg';
    bar.appendChild(seg);
  }
  (s.colors > 1 ? ['color1','color2'] : ['color1']).forEach(field => {
    const native = container.querySelector(`.color-native[data-pair="${field}-${s.key}"]`);
    const hex = document.getElementById(`${field}-${s.key}`);
    native.addEventListener('input', () => { hex.value = native.value.toUpperCase(); });
    hex.addEventListener('change', () => {
      let v = hex.value.trim();
      if (v && v[0] !== '#') v = '#' + v;
      if (/^#[0-9A-Fa-f]{6}$/.test(v)) { hex.value = v.toUpperCase(); native.value = v; }
      else { hex.value = native.value; }
    });
  });
});

function blockFor(key){ return container.querySelector(`.anim-state[data-key="${key}"]`); }

function stateInfo(key){ return STATES.find(s => s.key === key); }

function populateState(key, cfg){
  const b = blockFor(key);
  b.dataset.effect = cfg.effect;
  const fields = stateInfo(key).colors > 1 ? ['color1','color2'] : ['color1'];
  fields.forEach(field => {
    const hex = b.querySelector(`.f-${field}`);
    hex.value = cfg[field];
    const native = b.querySelector(`.color-native[data-pair="${field}-${key}"]`);
    if (native) native.value = cfg[field];
  });
}

function collectState(key){
  const b = blockFor(key);
  const twoColors = stateInfo(key).colors > 1;
  const color1 = b.querySelector('.f-color1').value;
  return {
    effect: b.dataset.effect,
    color1,
    // Single-color states just mirror color1 into color2 — matches how the
    // firmware treats them (see AnimConfigStore::defaults()), so effects that
    // blend two colors render as one solid color instead of needing a special case.
    color2: twoColors ? b.querySelector('.f-color2').value : color1,
  };
}

// Guards against interacting (Preview/Save) with fields still at their blank
// browser defaults before the real saved config has finished loading — this is
// what previously let a black "#000000" get saved over a real color.
let animLoaded = false;

async function loadAnim(){
  const r = await fetch('/api/anim');
  const d = await r.json();
  STATES.forEach(s => populateState(s.key, d[s.key]));
  document.getElementById('globalBrightness').value = d.brightness;
  animLoaded = true;
  container.classList.remove('loading');
  document.getElementById('animLoadHint').textContent = 'Pick colors per state — the animation itself is fixed. Brightness above applies to the whole strip. Changes preview live below, then hit Save.';
  document.getElementById('btnSaveAnim').disabled = false;
  document.getElementById('btnRestoreDefaults').disabled = false;
}

// ---- Live preview (rendered entirely in the browser — works with no LED strip attached) ----

function hexToRgb(hex){
  const v = (hex || '#000000').replace('#', '');
  return [parseInt(v.substr(0, 2), 16) || 0, parseInt(v.substr(2, 2), 16) || 0, parseInt(v.substr(4, 2), 16) || 0];
}
function lerpRgb(a, b, t){ return [a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t, a[2]+(b[2]-a[2])*t]; }
function cssRgb(rgb, scale){
  return `rgb(${Math.round(rgb[0]*scale)},${Math.round(rgb[1]*scale)},${Math.round(rgb[2]*scale)})`;
}

// Loose JS approximation of the C++ effects in LedAnimations.cpp — close enough to
// preview colors/motion/speed without needing hardware or a round-trip to the device.
// speed/intensity/brightness are no longer per-state fields — they're passed
// in from STATES' hardcoded values and the shared brightness slider.
function renderFrame(cfg, speed, intensity, brightness, tSec){
  const N = 10;
  const c1 = hexToRgb(cfg.color1), c2 = hexToRgb(cfg.color2);
  const bri = 0.35 + (brightness / 255) * 0.65; // floor so dim states are still visible on screen
  const bpm = 4 + (speed / 255) * 46; // mirrors speedToBpm() in firmware
  const beats = tSec * (bpm / 60);
  const out = new Array(N).fill(0).map(() => [0, 0, 0]);

  switch (cfg.effect) {
    case 'Off':
      break;
    case 'Solid':
      for (let i = 0; i < N; i++) out[i] = c1;
      break;
    case 'Fade': {
      const level = 0.1 + ((Math.sin(beats * 2 * Math.PI) + 1) / 2) * 0.9;
      for (let i = 0; i < N; i++) out[i] = lerpRgb([0,0,0], c1, level);
      break;
    }
    case 'Loading': {
      const pos = ((Math.sin(beats * 2 * Math.PI) + 1) / 2) * (N - 1);
      const trail = 1 + (255 - intensity) / 255 * 3.5;
      for (let i = 0; i < N; i++) {
        const f = Math.max(0, 1 - Math.abs(i - pos) / trail);
        out[i] = lerpRgb([0,0,0], i === Math.round(pos) ? lerpRgb(c1, c2, 0.5) : c1, f);
      }
      break;
    }
    case 'Percent': {
      // Background is always off; c2 is the leading-pixel color (see fxPercent).
      const pct = (Math.sin(beats * 0.5 * 2 * Math.PI) + 1) / 2 * 100; // demo sweep
      const lit = Math.floor(pct / 10);
      for (let i = 0; i < N; i++) {
        out[i] = i < lit ? c1 : (i === lit ? c2 : [0, 0, 0]);
      }
      break;
    }
    case 'Plasmoid': {
      for (let i = 0; i < N; i++) {
        const w = (Math.sin((beats + i / N) * 2 * Math.PI) + 1) / 2;
        out[i] = lerpRgb(c1, c2, w);
      }
      break;
    }
    case 'Bounce': {
      const pos = Math.abs(Math.sin(beats * 2 * Math.PI)) * (N - 1);
      for (let i = 0; i < N; i++) {
        const f = Math.max(0, 1 - Math.abs(i - pos) / 1.6);
        out[i] = lerpRgb([0,0,0], lerpRgb(c1, c2, 0.3), f);
      }
      break;
    }
  }

  return out.map(rgb => cssRgb(rgb, bri));
}

function tickPreview(tMs){
  const tSec = tMs / 1000;
  const brightness = Number(document.getElementById('globalBrightness').value);
  STATES.forEach(s => {
    const bar = document.getElementById('prevbar-' + s.key);
    if (!bar) return;
    const colors = renderFrame(collectState(s.key), s.speed, s.intensity, brightness, tSec);
    const segs = bar.children;
    for (let i = 0; i < segs.length; i++) segs[i].style.background = colors[i];
  });
  requestAnimationFrame(tickPreview);
}
requestAnimationFrame(tickPreview);

document.getElementById('btnSaveAnim').addEventListener('click', async () => {
  if (!animLoaded) return;
  const status = document.getElementById('animStatus');
  const body = { brightness: Number(document.getElementById('globalBrightness').value) };
  STATES.forEach(s => { body[s.key] = collectState(s.key); });
  status.textContent = 'Saving...'; status.className = 'status';
  try{
    const r = await fetch('/api/anim', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)
    });
    if(!r.ok) throw new Error('bad response');
    status.textContent = 'Saved — applied immediately, no reboot needed.';
    status.className = 'status ok';
  }catch(e){
    status.textContent = 'Error saving. Try again.';
    status.className = 'status err';
  }
});

document.getElementById('btnRestoreDefaults').addEventListener('click', async () => {
  if (!animLoaded) return;
  if(!confirm('Reset all animation settings back to the factory defaults?')) return;
  const status = document.getElementById('animStatus');
  status.textContent = 'Restoring...'; status.className = 'status';
  try{
    await fetch('/api/anim/reset', {method:'POST'});
    await loadAnim();
    status.textContent = 'Restored to defaults.';
    status.className = 'status ok';
  }catch(e){
    status.textContent = 'Error restoring defaults.';
    status.className = 'status err';
  }
});

async function loadFwVersion(){
  try{
    const r = await fetch('/api/info');
    const d = await r.json();
    document.getElementById('linkMakerworld').href = d.makerworld_url;
    document.getElementById('linkGithub').href = d.github_url;
    document.getElementById('versionLine').textContent = `${d.project_name} v${d.firmware_version}`;
  }catch(e){}
}

async function loadLogs(){
  try{
    const r = await fetch('/api/logs');
    const t = await r.text();
    const box = document.getElementById('logBox');
    const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 10;
    box.textContent = t;
    if (atBottom) box.scrollTop = box.scrollHeight;
  }catch(e){}
}

loadAnim();
loadFwVersion();
loadLogs();
setInterval(loadLogs, 3000);
</script>
</body>
</html>
)HTML";

#pragma once

// =====================================================================
//  web_page.h -- หน้าเว็บแดชบอร์ด (ดีไซน์: design_handoff_cyd_obd_gauge/WEB_DASHBOARD.md)
//  HTML/CSS/JS ล้วน เก็บใน flash ด้วย PROGMEM เปิดได้ที่ http://<IP ของบอร์ด> หรือ http://cyd-obd.local
//
//  handleIndex() ใน main.cpp ส่งหน้านี้เป็น 3 ส่วน:
//    WEB_PAGE_HEAD  ->  <script> ค่าจากเฟิร์มแวร์ (CFG: เกณฑ์น้ำหล่อเย็น, เวอร์ชัน; DTC_TH: คำอธิบายไทยจาก ui_text_th.h) </script>  ->  WEB_PAGE_TAIL
//  หน้านี้เรียก API เดิมเท่านั้น: /api/status, /api/dtc, /api/dtc/read, /api/dtc/clear, /api/config, /api/backlight
// =====================================================================

const char WEB_PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html>
<html lang="th">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD OBD-II</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<!-- โหลดฟอนต์แบบไม่บล็อกหน้า: โหมด AP ไม่มีอินเทอร์เน็ต จะใช้ฟอนต์ของระบบแทน -->
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@400;500;600;700&display=swap" media="print" onload="this.media='all'">
<style>
:root{--bg:#000;--surface:#14171B;--line:#2A2F36;--muted:#8A9099;--text:#F2F2EE;--accent:#FFB000;--ok:#3DDC97;--bad:#FF4D3D}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font-family:'Chakra Petch',system-ui,sans-serif;-webkit-font-smoothing:antialiased}
button,input{font-family:inherit}
.wrap{max-width:1120px;margin:0 auto;padding:28px 20px 48px;display:flex;flex-direction:column;gap:16px;font-variant-numeric:tabular-nums}
.muted{color:var(--muted)}
.dot{width:7px;height:7px;border-radius:50%;flex:none;background:var(--muted)}
.blink{animation:blink 1s steps(1) infinite}
@keyframes blink{50%{background:var(--line)}}
.badge{font-size:11px;font-weight:700;color:#000;background:var(--accent);padding:2px 7px;border-radius:3px;letter-spacing:.5px}
.hidden{display:none!important}

header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:14px;padding-bottom:18px;border-bottom:1px solid var(--surface)}
.brand{display:flex;align-items:center;gap:10px}
.brand b{font-size:24px;font-weight:700;letter-spacing:.5px}
.chips{display:flex;flex-wrap:wrap;gap:8px;font-size:13px}
.chip{display:flex;align-items:center;gap:7px;padding:6px 12px;border:1px solid var(--line);border-radius:4px}

.row{display:flex;flex-wrap:wrap;gap:12px}
.card{background:var(--surface);border-radius:6px}
.gauge{flex:1 1 300px;padding:16px 16px 18px;display:flex;flex-direction:column;align-items:center;min-height:280px}
.caption{align-self:stretch;display:flex;justify-content:space-between;font-size:12px;color:var(--muted);letter-spacing:1px}
.dial{position:relative;width:240px;height:236px;margin-top:4px}
.dial svg{position:absolute;left:0;top:0}
.speed{position:absolute;left:0;right:0;top:72px;display:flex;flex-direction:column;align-items:center}
.speed b{font-size:72px;font-weight:600;line-height:1;letter-spacing:-2px}
.speed span{font-size:13px;color:var(--muted);letter-spacing:1px;margin-top:4px}
.rpm{position:absolute;left:0;right:0;top:186px;display:flex;justify-content:center;align-items:baseline;gap:5px}
.rpm b{font-size:24px;font-weight:600}
.rpm span{font-size:12px;color:var(--muted);letter-spacing:1px}

.tiles{flex:2 1 480px;display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}
.tile{padding:14px 16px;display:flex;flex-direction:column;justify-content:space-between;gap:14px;min-height:112px}
.tile .top{display:flex;justify-content:space-between;align-items:baseline;gap:8px;color:var(--muted)}
.tile .top span:first-child{font-size:13px}
.tile .top span:last-child{font-size:12px}
.tile .mid{display:flex;justify-content:space-between;align-items:flex-end;gap:8px}
.tile .val{font-size:38px;font-weight:600;line-height:1}
.tile .st{font-size:10px;font-weight:700;letter-spacing:1px}
.bar{height:4px;background:var(--line);border-radius:2px;overflow:hidden}
.bar div{height:4px;width:0;transition:width .3s}

.bottom{align-items:flex-start}
.panel{flex:1 1 420px;padding:18px;display:flex;flex-direction:column}
.dtc{gap:14px}
.cfg{gap:16px}
.title{display:flex;align-items:baseline;gap:10px}
.title b{font-size:17px;font-weight:600}
.title span{font-size:12px;color:var(--muted);letter-spacing:1px}
.dtc-head{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:10px}
.count{display:flex;align-items:center;gap:8px}
.count b{font-size:22px;font-weight:700;line-height:1}
.count span:not(.badge){font-size:12px;color:var(--muted)}
.list{display:flex;flex-direction:column;gap:6px}
.code{display:grid;grid-template-columns:4px 72px minmax(0,1fr);align-items:center;gap:12px;background:#000;border-radius:4px;overflow:hidden;min-height:52px;padding-right:12px}
.code i{align-self:stretch}
.code b{font-size:16px;font-weight:700;letter-spacing:.5px}
.code div{display:flex;flex-direction:column;gap:2px;padding:8px 0;min-width:0}
.code div span:first-child{font-size:14px}
.code div span:last-child{font-size:12px;color:var(--muted)}
.empty{display:flex;align-items:center;gap:10px;background:#000;border-radius:4px;min-height:52px;padding:0 14px;font-size:14px}
.empty .dot{width:8px;height:8px}
.buttons{display:flex;flex-wrap:wrap;gap:8px}
.btn{height:44px;padding:0 20px;display:flex;align-items:center;justify-content:center;gap:8px;border-radius:4px;font-size:14px;font-weight:600;letter-spacing:.5px;cursor:pointer;user-select:none;-webkit-user-select:none;background:none;color:var(--text)}
.btn:disabled{opacity:.5;cursor:default}
.ghost{border:1px solid var(--line)}
.ghost:hover:not(:disabled){border-color:var(--muted)}
.hold{position:relative;flex:1 1 200px;border:1px solid var(--bad);color:var(--bad);overflow:hidden;touch-action:none;-webkit-touch-callout:none}
.hold .fill{position:absolute;left:0;top:0;bottom:0;width:0;background:var(--bad);opacity:.25}
.hold span{position:relative}
.hold .hint{font-size:11px;color:var(--muted);font-weight:500}
.note{font-size:12px;color:var(--muted);min-height:0}
.msg{font-size:13px}

label{display:flex;flex-direction:column;gap:6px}
label>span{font-size:12px;color:var(--muted)}
label>span i{font-style:normal;color:var(--line)}
input[type=text],input[type=password]{height:44px;background:#000;border:1px solid var(--line);border-radius:4px;padding:0 14px;color:var(--text);font-size:16px;font-weight:500}
input[type=password]{letter-spacing:2px}
input:focus{outline:none;border-color:var(--accent)}
.bl{display:flex;flex-direction:column;gap:8px}
.bl div{display:flex;justify-content:space-between;align-items:baseline}
.bl div span{font-size:12px;color:var(--muted)}
.bl div b{font-size:15px;font-weight:600}
input[type=range]{width:100%;margin:0;height:20px;accent-color:var(--accent)}
.save{display:flex;flex-wrap:wrap;align-items:center;gap:12px;padding-top:4px}
.primary{padding:0 22px;background:var(--accent);color:#000;border:0;font-weight:700}
.primary:hover{background:#FFC94D}

footer{display:flex;flex-wrap:wrap;justify-content:space-between;gap:8px;font-size:12px;color:var(--muted);padding-top:8px}
</style>
</head>
<body>
<div class="wrap">

<header>
  <div class="brand"><b>CYD OBD-II</b></div>
  <div class="chips">
    <div class="chip"><span class="dot" id="netDot"></span><span class="muted">เครือข่าย</span><span id="net">--</span></div>
    <div class="chip" id="obdChip"><span class="dot" id="obdDot"></span><span class="muted">OBD</span><span id="obd">--</span></div>
    <div class="chip"><span class="muted">IP</span><span id="ip">--</span></div>
  </div>
</header>

<div class="row">
  <div class="card gauge">
    <div class="caption"><span>ความเร็ว / รอบเครื่อง</span><span>LIVE</span></div>
    <div class="dial">
      <svg width="240" height="236" viewBox="10 22 144 144" id="dial">
        <path d="M36.75 145.25 A64 64 0 1 1 127.25 145.25" fill="none" stroke="#000" stroke-width="9"/>
        <path d="M31.09 150.91 A72 72 0 1 1 132.91 150.91" fill="none" stroke="#FF4D3D" stroke-width="2.5" pathLength="100" stroke-dasharray="0 81.25 18.75 100"/>
        <path id="arc" d="M36.75 145.25 A64 64 0 1 1 127.25 145.25" fill="none" stroke="#FFB000" stroke-width="9" pathLength="100" stroke-dasharray="0 100"/>
        <text x="44" y="162" fill="#8A9099" font-size="7" text-anchor="middle">0</text>
        <text x="120" y="162" fill="#FF4D3D" font-size="7" text-anchor="middle">8k</text>
      </svg>
      <div class="speed"><b id="speed">--</b><span>km/h</span></div>
      <div class="rpm"><b id="rpm">--</b><span>RPM</span></div>
    </div>
  </div>
  <div class="tiles" id="tiles"></div>
</div>

<div class="row bottom">
  <div class="card panel dtc">
    <div class="dtc-head">
      <div class="title"><b>รหัสข้อผิดพลาด</b><span>DTC</span></div>
      <div class="count"><b id="dtcCount">–</b><span>รหัส</span><span class="badge hidden" id="mil">MIL ON</span></div>
    </div>
    <div class="list" id="dtcList"></div>
    <div class="buttons">
      <button class="btn ghost" id="readBtn" type="button">อ่านรหัส</button>
      <button class="btn hold" id="clearBtn" type="button"><span class="fill" id="holdFill"></span><span>ลบรหัส</span><span class="hint">กดค้าง 2 วินาที</span></button>
    </div>
    <div class="msg hidden" id="dtcMsg"></div>
  </div>

  <div class="card panel cfg">
    <div class="title"><b>ตั้งค่าอะแดปเตอร์ OBD</b><span>BLUETOOTH</span></div>
    <label><span>ชื่อ หรือ MAC <i>·</i> AA:BB:CC:DD:EE:FF</span><input type="text" id="cfgName" maxlength="32" autocomplete="off" spellcheck="false"></label>
    <label><span>PIN <i>·</i> เว้นว่างถ้าไม่มี</span><input type="password" id="cfgPin" maxlength="16" placeholder="••••" autocomplete="off"></label>
    <div class="bl">
      <div><span>ความสว่างหน้าจอ</span><b id="blText">--</b></div>
      <input type="range" id="bl" min="10" max="100" value="100">
    </div>
    <div class="save">
      <button class="btn primary" id="saveBtn" type="button">บันทึกและเชื่อมต่อใหม่</button>
      <span class="msg" id="cfgMsg"></span>
    </div>
  </div>
</div>

<footer><span>อัปเดตอัตโนมัติทุก 0.6 วินาที</span><span id="fw"></span></footer>
</div>
)HTML";

const char WEB_PAGE_TAIL[] PROGMEM = R"HTML(<script>
'use strict';
const $ = id => document.getElementById(id);
const COLOR = {ok: '#3DDC97', warn: '#FFB000', bad: '#FF4D3D', text: '#F2F2EE', muted: '#8A9099'};
const WORD = {ok: 'ปกติ', warn: 'เฝ้าระวัง', bad: 'อันตราย'};
const LIMIT = CFG.limit;

// ---- API ----
async function api(path, body) {
  const opt = body === undefined ? {cache: 'no-store'}
    : {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded', 'X-CYD': '1'}, body};  // X-CYD: บอร์ดรับ POST ที่มี header นี้เท่านั้น (กัน CSRF)
  const r = await fetch(path, opt);
  let j = {};
  try { j = await r.json(); } catch (e) {}
  if (!r.ok) { const err = new Error(j.error || ('HTTP ' + r.status)); err.status = r.status; throw err; }
  return j;
}

// ---- ช่องค่า (สร้าง DOM ครั้งเดียว แล้วอัปเดตเฉพาะข้อความ/ความกว้างแถบ) ----
const TILES = [
  {key: 'cool', label: 'น้ำหล่อเย็น', unit: '°C', lo: 60, hi: 120, dec: 0, rule: v => v >= LIMIT ? 'bad' : v >= LIMIT - 8 ? 'warn' : 'ok'},
  {key: 'volt', label: 'แบตเตอรี่', unit: 'V', lo: 11, hi: 15, dec: 1, rule: v => v < 12.4 ? 'bad' : v < 12.8 ? 'warn' : 'ok'},
  {key: 'fuel', label: 'น้ำมัน', unit: '%', lo: 0, hi: 100, dec: 1, rule: v => v < 10 ? 'bad' : v < 20 ? 'warn' : 'ok'},
  {key: 'throttle', label: 'คันเร่ง', unit: '%', lo: 0, hi: 100, dec: 1},
  {key: 'load', label: 'โหลดเครื่อง', unit: '%', lo: 0, hi: 100, dec: 1},
  {key: 'intake', label: 'อากาศเข้า', unit: '°C', lo: 0, hi: 80, dec: 0},
];
function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}
for (const t of TILES) {
  const tile = el('div', 'card tile');
  const top = el('div', 'top');
  top.append(el('span', '', t.label), el('span', '', t.unit));
  const mid = el('div', 'mid');
  t.val = el('span', 'val', '--');
  t.st = el('span', 'st');
  mid.append(t.val, t.st);
  const bar = el('div', 'bar');
  t.fill = el('div');
  bar.append(t.fill);
  tile.append(top, mid, bar);
  $('tiles').append(tile);
}
function renderTile(t, v, na) {
  if (na) {  // รถคันนี้ไม่รองรับค่านี้
    t.val.textContent = 'N/A';
    t.val.style.color = COLOR.muted;
    t.st.textContent = '';
    t.fill.style.width = '0';
    return;
  }
  if (v === null || v === undefined) {
    t.val.textContent = '--';
    t.val.style.color = COLOR.muted;
    t.st.textContent = '';
    t.fill.style.width = '0';
    return;
  }
  const s = t.rule ? t.rule(v) : null;
  const color = s ? COLOR[s] : COLOR.muted;
  t.val.textContent = v.toFixed(t.dec);
  t.val.style.color = s === 'warn' || s === 'bad' ? color : COLOR.text;
  t.st.textContent = s ? WORD[s] : '';
  t.st.style.color = color;
  t.fill.style.width = Math.max(4, Math.min(100, (v - t.lo) / (t.hi - t.lo) * 100)) + '%';
  t.fill.style.background = color;
}

// ---- มาตรวัด: ขีดทุก 1000 รอบ (7k, 8k สีแดง) ----
(() => {
  const svg = $('dial'), cx = 82, cy = 100;
  for (let v = 0; v <= 8; v++) {
    const a = (135 + 270 * v / 8) * Math.PI / 180;
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', cx + 50 * Math.cos(a)); line.setAttribute('y1', cy + 50 * Math.sin(a));
    line.setAttribute('x2', cx + 55 * Math.cos(a)); line.setAttribute('y2', cy + 55 * Math.sin(a));
    line.setAttribute('stroke', v >= 7 ? COLOR.bad : COLOR.muted);
    line.setAttribute('stroke-width', '1.2');
    svg.append(line);
  }
})();

// ---- สถานะสด ----
// เหมือนจอ: ต่อ Bluetooth กับอะแดปเตอร์อยู่ = CONNECTED สีเขียว, นอกนั้น DISCONNECTED สีแดง
const LINK_BUSY = {'BT START': 'CONNECTING', 'CONNECTING': 'CONNECTING', 'INIT ELM': 'CONNECTING', 'SCANNING': 'SCANNING'};
const NET = {WIFI: ['WiFi', 'ok'], AP: ['AP ของบอร์ด', 'ok'], CONNECTING: ['กำลังต่อ WiFi', 'warn'], OFF: ['ปิด', 'bad']};
let blTouchedAt = 0;

function setChip(textId, dotId, text, state, blink) {
  $(textId).textContent = text;
  $(dotId).style.background = COLOR[state];
  $(dotId).classList.toggle('blink', !!blink);
}

function renderStatus(s) {
  const net = NET[s.net] || [s.net, 'warn'];
  setChip('net', 'netDot', net[0], net[1], false);
  // ชื่อสถานะจาก linkName() ในเฟิร์มแวร์: ระหว่างกำลังต่อ/เริ่ม ELM327/ค้นหา = เหลือง จุดกะพริบ
  const link = s.connected ? ['CONNECTED', 'ok', false]
    : LINK_BUSY[s.link] ? [LINK_BUSY[s.link], 'warn', true]
    : ['DISCONNECTED', 'bad', false];
  setChip('obd', 'obdDot', link[0], link[1], link[2]);
  $('obdChip').style.color = COLOR[link[1]];
  $('ip').textContent = s.ip && s.ip !== '0.0.0.0' ? s.ip : '--';

  const speed = s.speed;
  $('speed').textContent = speed === null ? '--' : Math.round(speed);
  const rpm = s.rpm;
  const color = rpm !== null && rpm >= 6500 ? COLOR.bad : COLOR.warn;
  const pct = rpm === null ? 0 : Math.max(0, Math.min(100, rpm / 8000 * 100));
  $('arc').setAttribute('stroke-dasharray', pct.toFixed(2) + ' 100');
  $('arc').setAttribute('stroke', color);
  $('rpm').textContent = rpm === null ? '--' : (Math.round(rpm / 10) * 10).toLocaleString('en-US');
  $('rpm').style.color = rpm === null ? COLOR.muted : color;
  const na = s.na || [];
  for (const t of TILES) renderTile(t, s[t.key], na.includes(t.key));

  if (Date.now() - blTouchedAt > 1500) {  // ไม่ทับค่าที่ผู้ใช้กำลังลาก
    $('bl').value = s.backlight;
    $('blText').textContent = s.backlight + '%';
  }
}

async function pollStatus() {
  try {
    renderStatus(await api('/api/status'));
  } catch (e) {
    setChip('net', 'netDot', 'ติดต่อบอร์ดไม่ได้', 'bad', false);
  }
  setTimeout(pollStatus, 600);
}

// ---- รหัสข้อผิดพลาด ----
const NO_ECU = {NO_LINK: 'ยังไม่ได้เชื่อมต่ออะแดปเตอร์ OBD', NO_ECU: 'ECU ไม่ตอบ', TIMEOUT: 'อะแดปเตอร์ไม่ตอบ (หมดเวลา)', CLEAR_FAILED: 'ECU ไม่ยอมลบรหัส'};
let lastDtc = {state: 'IDLE'};
let dtcKey = '';

function dtcTh(code) {
  return DTC_TH[code] || (code[0] === 'P' && code[1] >= '1' && code[1] <= '3' ? DTC_TH_MFR : DTC_TH_GEN);
}
// แถบสีหน้าแถว: แดง = จุดระเบิดไม่สมบูรณ์ (P03xx) และน้ำหล่อเย็นร้อน (P0118, P0217) เหมือนบนจอ, เหลือง = อื่นๆ
function dtcCritical(code) {
  return code.startsWith('P03') || code === 'P0118' || code === 'P0217';
}
function emptyRow(color, text) {
  const row = el('div', 'empty');
  const dot = el('span', 'dot');
  dot.style.background = color;
  row.append(dot, document.createTextNode(text));
  return row;
}

function renderDtc(d) {
  lastDtc = d;
  const key = JSON.stringify(d);
  if (key === dtcKey) return;  // สร้างรายการใหม่เฉพาะเมื่อข้อมูลเปลี่ยน
  dtcKey = key;

  const busy = d.state === 'BUSY';
  const done = d.state === 'DONE';
  $('readBtn').textContent = busy ? 'กำลังอ่าน…' : 'อ่านรหัส';
  $('readBtn').disabled = busy;
  $('clearBtn').disabled = busy;
  const count = $('dtcCount');
  count.textContent = done ? d.count : '–';
  count.style.color = done ? (d.count > 0 ? COLOR.bad : COLOR.ok) : COLOR.muted;
  $('mil').classList.toggle('hidden', !(done && d.count > 0 && d.mil));

  const list = $('dtcList');
  list.replaceChildren();
  if (done && d.count > 0) {
    for (const c of d.codes) {
      const row = el('div', 'code');
      const strip = el('i');
      strip.style.background = dtcCritical(c.code) ? COLOR.bad : COLOR.warn;
      const text = el('div');
      text.append(el('span', '', c.text), el('span', '', dtcTh(c.code)));
      row.append(strip, el('b', '', c.code), text);
      list.append(row);
    }
  } else if (done) {
    list.append(emptyRow(COLOR.ok, d.msg === 'CLEARED' ? 'ลบรหัสแล้ว · ไม่พบรหัสข้อผิดพลาด' : 'ไม่พบรหัสข้อผิดพลาด'));
  } else if (busy) {
    list.append(emptyRow(COLOR.warn, 'กำลังสื่อสารกับ ECU…'));
  } else if (d.state === 'ERROR') {
    list.append(emptyRow(COLOR.bad, NO_ECU[d.msg] || 'ไม่ได้รับคำตอบจาก ECU'));
  } else {
    list.append(emptyRow(COLOR.muted, 'กด “อ่านรหัส” เพื่อตรวจรหัสข้อผิดพลาด'));
  }
}

let dtcTimer = 0;
async function pollDtc() {
  clearTimeout(dtcTimer);
  try { renderDtc(await api('/api/dtc')); } catch (e) {}
  dtcTimer = setTimeout(pollDtc, lastDtc.state === 'BUSY' ? 400 : 1500);
}

function showDtcMsg(text, color) {
  const m = $('dtcMsg');
  m.textContent = text;
  m.style.color = color;
  m.classList.toggle('hidden', !text);
}

$('readBtn').onclick = async () => {
  showDtcMsg('', '');
  try { await api('/api/dtc/read', ''); } catch (e) { showDtcMsg('อ่านรหัสไม่สำเร็จ: ' + e.message, COLOR.bad); }
  pollDtc();
};

// ---- ลบรหัส: ต้องกดค้าง 2 วินาที ปล่อยก่อน = ยกเลิก ----
const HOLD_MS = 2000;
let holdStart = 0, holdFrame = 0;
const clearBtn = $('clearBtn'), holdFill = $('holdFill');

function stopHold() {
  cancelAnimationFrame(holdFrame);
  holdStart = 0;
  holdFill.style.width = '0';
}
function holdTick() {
  const p = Math.min(100, (performance.now() - holdStart) / HOLD_MS * 100);
  holdFill.style.width = p + '%';
  if (p >= 100) { stopHold(); sendClear(); return; }
  holdFrame = requestAnimationFrame(holdTick);
}
clearBtn.addEventListener('pointerdown', e => {
  if (clearBtn.disabled || e.button > 0) return;
  showDtcMsg('', '');
  holdStart = performance.now();
  holdFrame = requestAnimationFrame(holdTick);
});
for (const ev of ['pointerup', 'pointerleave', 'pointercancel']) clearBtn.addEventListener(ev, () => { if (holdStart) stopHold(); });
clearBtn.addEventListener('contextmenu', e => e.preventDefault());

async function sendClear() {
  try {
    await api('/api/dtc/clear', 'confirm=yes');
    showDtcMsg('กำลังลบรหัส…', COLOR.muted);
  } catch (e) {
    showDtcMsg(e.status === 409 ? 'รถกำลังวิ่งอยู่ ลบรหัสไม่ได้ จอดรถก่อน' : 'ลบรหัสไม่สำเร็จ: ' + e.message, COLOR.bad);
  }
  pollDtc();
}

// ---- ความสว่างจอ: ส่งค่าขณะลาก (หน่วง 150 ms) ----
let blTimer = 0;
$('bl').addEventListener('input', () => {
  blTouchedAt = Date.now();
  $('blText').textContent = $('bl').value + '%';
  clearTimeout(blTimer);
  blTimer = setTimeout(() => api('/api/backlight', 'value=' + $('bl').value).catch(() => {}), 150);
});

// ---- ตั้งค่าอะแดปเตอร์ ----
let pinSet = false;
function setCfgMsg(text, color) {
  $('cfgMsg').textContent = text;
  $('cfgMsg').style.color = color;
}
async function loadConfig() {
  try {
    const c = await api('/api/config');
    $('cfgName').value = c.name;
    pinSet = c.pinSet;
    $('cfgPin').placeholder = pinSet ? '•••• (ตั้งไว้แล้ว)' : '••••';
  } catch (e) {
    setTimeout(loadConfig, 2000);
  }
}
for (const id of ['cfgName', 'cfgPin']) $(id).addEventListener('input', () => setCfgMsg('', ''));
$('saveBtn').onclick = async () => {
  const name = $('cfgName').value.trim();
  const pin = $('cfgPin').value;
  // API ต้องการทั้งชื่อและ PIN: ช่อง PIN ว่าง = ไม่ใช้ PIN จึงถามก่อนถ้าเดิมเคยตั้งไว้
  if (pinSet && pin === '' && !confirm('ไม่ได้ใส่ PIN: PIN เดิมของอะแดปเตอร์จะถูกล้าง ดำเนินการต่อหรือไม่?')) return;
  try {
    await api('/api/config', new URLSearchParams({name, pin}).toString());
    pinSet = pin !== '';
    $('cfgPin').value = '';
    $('cfgPin').placeholder = pinSet ? '•••• (ตั้งไว้แล้ว)' : '••••';
    setCfgMsg('บันทึกแล้ว · กำลังเชื่อมต่อ ' + (name || '(ไม่มีชื่อ)'), COLOR.ok);
  } catch (e) {
    setCfgMsg('บันทึกไม่สำเร็จ: ' + e.message, COLOR.bad);
  }
};

$('fw').textContent = 'FW ' + CFG.fw;
loadConfig();
pollStatus();
pollDtc();
</script>
</body>
</html>
)HTML";

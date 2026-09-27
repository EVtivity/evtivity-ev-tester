// Web UI pages for the EVtivity EV Tester (served by ac_tester.ino).
// PAGE_MAIN: virtual control panel (LCD mirror + animated MODE/STATS buttons) shown in station mode.
// PAGE_FUN: jukebox screen (/fun) that plays full melodies on the buzzer.
// PAGE_CONFIG: WiFi / login / MQTT setup form shown in AP mode (and at /setup).
#pragma once

// ESP32 keeps const char[] in flash (.rodata) and reads it directly, so no PROGMEM/send_P needed.
const char PAGE_MAIN[] = R"HTMLPAGE(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>EVtivity EV Tester</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=VT323&display=swap" rel="stylesheet">
<style>
:root{--green:#22c55e;--bg:#0b1220;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;}
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:
radial-gradient(1200px 600px at 50% -10%,#13203a,var(--bg));color:#e5e7eb;min-height:100vh;
display:flex;flex-direction:column;align-items:center;padding:22px 14px;}
h1{font-size:19px;margin:2px 0;color:var(--green);letter-spacing:.3px;}
.sub{font-size:12px;color:#8ea0bd;margin-bottom:12px;}
/* realistic green 1602: dark bezel, green STN backlight, dot-matrix pixel grid, dark characters */
.lcd{position:relative;container-type:inline-size;background:linear-gradient(160deg,#0362fc,#034fce);
border:7px solid #0a1226;border-radius:7px;padding:14px 10px;width:min(360px,94vw);
box-shadow:inset 0 0 26px rgba(2,20,70,.4),inset 0 0 3px rgba(255,255,255,.3),0 8px 22px rgba(0,0,0,.5);}
.lcd::before{content:"";position:absolute;inset:0;pointer-events:none;z-index:2;
background:repeating-linear-gradient(0deg,rgba(0,0,0,.16) 0 1px,transparent 1px 3px),
repeating-linear-gradient(90deg,rgba(0,0,0,.16) 0 1px,transparent 1px 3px);mix-blend-mode:multiply;opacity:.55;}
.lcd .row{position:relative;z-index:1;display:grid;grid-template-columns:repeat(16,1fr);}
.lcd .cell{font-family:'VT323',ui-monospace,Menlo,Consolas,monospace;color:#f2f6ff;
font-size:10cqw;line-height:1.32;text-align:center;overflow:hidden;
text-shadow:0 0 7px rgba(150,190,255,.6);}
.pad{display:flex;gap:26px;margin-top:20px;}
.btn{position:relative;width:126px;height:126px;border:none;border-radius:50%;color:#dbe4f3;
font-weight:700;font-size:15px;letter-spacing:.5px;cursor:pointer;overflow:hidden;
background:radial-gradient(circle at 34% 28%,#26365a,#131e34);
box-shadow:0 10px 22px rgba(0,0,0,.55),inset 0 2px 5px rgba(255,255,255,.06),inset 0 -6px 12px rgba(0,0,0,.4);
transition:transform .09s ease,box-shadow .14s ease;display:flex;flex-direction:column;
align-items:center;justify-content:center;gap:9px;}
.btn svg{width:36px;height:36px;stroke:var(--green);fill:none;stroke-width:2.3;stroke-linecap:round;stroke-linejoin:round;}
.btn.pressed{transform:scale(.9);
box-shadow:0 4px 12px rgba(0,0,0,.6),0 0 30px rgba(34,197,94,.7),inset 0 2px 8px rgba(0,0,0,.5);}
.btn.pressed svg{stroke:#c8ffd8;}
.ripple{position:absolute;border-radius:50%;background:rgba(34,197,94,.35);
transform:scale(0);animation:rip .55s ease-out forwards;pointer-events:none;}
@keyframes rip{to{transform:scale(3.4);opacity:0;}}
.hint{font-size:11px;color:#64748b;margin-top:10px;}
.links{display:flex;gap:12px;margin-top:16px;}
.wifi{background:none;border:1px solid #334155;color:#8ea0bd;padding:9px 16px;
border-radius:9px;font-size:13px;cursor:pointer;transition:.15s;}
.wifi:hover{border-color:var(--green);color:var(--green);}
.mqttp{width:min(540px,94vw);margin-top:14px;padding:14px 16px;background:#111a2e;border:1px solid #22304d;
border-radius:10px;font-size:13px;color:#cdd7ea;line-height:1.55;text-align:left;}
.mqttp .mrow{margin:2px 0;} .mqttp .mnote{color:#64748b;font-size:11px;margin-top:4px;}
.mqttp code{font-family:ui-monospace,Menlo,Consolas,monospace;background:#0c1424;color:#8dffb0;
padding:1px 6px;border-radius:5px;font-size:12px;overflow-wrap:anywhere;}
.mqttp table{width:100%;border-collapse:collapse;margin:6px 0 12px;font-size:12.5px;}
.mqttp th,.mqttp td{text-align:left;padding:5px 8px;border-bottom:1px solid #22304d;vertical-align:top;}
.mqttp th{color:#8ea0bd;font-size:10.5px;text-transform:uppercase;letter-spacing:.3px;}
.mqttp tr:last-child td{border-bottom:none;}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#f87171;margin-right:6px;vertical-align:middle;}
.dot.ok{background:var(--green);box-shadow:0 0 8px var(--green);}
.tbl{width:min(540px,94vw);margin-top:16px;border-collapse:collapse;font-size:12.5px;
background:#111a2e;border:1px solid #22304d;border-radius:10px;overflow:hidden;}
.tbl caption{caption-side:top;color:#8ea0bd;font-size:11px;text-align:left;padding:0 0 8px 12px;
text-transform:uppercase;letter-spacing:.4px;}
.tbl th,.tbl td{padding:9px 11px;text-align:left;border-bottom:1px solid #22304d;}
.tbl th{background:#0c1424;color:#8ea0bd;font-size:10px;text-transform:uppercase;letter-spacing:.4px;}
.tbl tr:last-child td{border-bottom:none;}
.tbl code{font-family:ui-monospace,Menlo,Consolas,monospace;background:#0c1424;color:#8dffb0;
padding:1px 6px;border-radius:5px;font-size:12px;}
</style></head><body>
<h1>EVtivity EV Tester</h1>
<div class="sub"><span id="dot" class="dot"></span>virtual control panel</div>
<div class="lcd"><div id="l1" class="row">&nbsp;</div><div id="l2" class="row">&nbsp;</div></div>
<div class="pad">
 <button class="btn" id="bMode" aria-label="Mode">
  <svg viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v4h-4"/></svg>MODE</button>
 <button class="btn" id="bStats" aria-label="Stats">
  <svg viewBox="0 0 24 24"><path d="M4 20V11M10 20V5M16 20v-6M22 20H2"/></svg>STATS</button>
</div>
<div class="hint">MODE cycles Ready - Charging - Suspended. STATS toggles the power view. Hold MODE 5 s for WiFi setup.</div>
<table class="tbl"><caption>Mode to connector status</caption>
<thead><tr><th>Tester Mode</th><th>OCPP 1.6</th><th>OCPP 2.1 (connector / charging state)</th></tr></thead>
<tbody>
<tr><td><code>Ready</code></td><td>Available</td><td>Available / Idle</td></tr>
<tr><td><code>Preparing</code></td><td>Preparing</td><td>Occupied / EVConnected</td></tr>
<tr><td><code>Charging</code></td><td>Charging</td><td>Occupied / Charging</td></tr>
<tr><td><code>Suspended</code></td><td>SuspendedEV</td><td>Occupied / SuspendedEV</td></tr>
</tbody></table>
<div class="links">
 <button class="wifi" id="mqtt">MQTT</button>
 <button class="wifi" id="fun">Fun</button>
</div>
<div id="mqttp" class="mqttp" style="display:none"></div>
<script>
function ripple(el){const r=document.createElement('span');r.className='ripple';
 const s=Math.max(el.clientWidth,el.clientHeight);r.style.width=r.style.height=s+'px';
 r.style.left=(el.clientWidth-s)/2+'px';r.style.top=(el.clientHeight-s)/2+'px';
 el.appendChild(r);setTimeout(()=>r.remove(),560);}
function setRow(el,s){s=(s||'').padEnd(16,' ').slice(0,16);el.textContent='';
 for(const c of s){const sp=document.createElement('span');sp.className='cell';
  sp.textContent=(c===' '?' ':c);el.appendChild(sp);}}
function tap(el,btn){el.classList.add('pressed');ripple(el);
 fetch('/press?btn='+btn).then(update).catch(()=>{});
 setTimeout(()=>el.classList.remove('pressed'),150);}
bStats.addEventListener('pointerdown',e=>{e.preventDefault();tap(bStats,'stats');});
/* MODE: quick tap cycles the mode; hold 5s reboots into the setup AP (same as the physical MODE). */
let modeT=null,modeHeld=false;
function modeCancel(){if(modeT){clearTimeout(modeT);modeT=null;}bMode.classList.remove('pressed');}
bMode.addEventListener('pointerdown',e=>{e.preventDefault();modeHeld=false;
 bMode.classList.add('pressed');ripple(bMode);
 modeT=setTimeout(()=>{modeHeld=true;modeT=null;bMode.classList.remove('pressed');clearInterval(pollTimer);
  document.body.innerHTML='<div style="min-height:100vh;display:flex;flex-direction:column;align-items:center;'
   +'justify-content:center;gap:12px;font-family:-apple-system,Segoe UI,Roboto,sans-serif;color:#e5e7eb;'
   +'background:#0b1220;padding:28px;text-align:center">'
   +'<div style="font-size:21px;font-weight:700;color:#22c55e">Opening setup mode</div>'
   +'<div style="color:#8ea0bd;font-size:14px">1. Join the WiFi network</div>'
   +'<div style="font-size:22px;font-weight:700">'+(window.apSsid||'EVtivity-EV-Tester')+'</div>'
   +'<div style="color:#8ea0bd;font-size:14px;margin-top:6px">2. Open in a browser</div>'
   +'<div style="font-size:21px;font-weight:700;color:#22c55e">http://192.168.4.1</div></div>';
  fetch('/enterap',{method:'POST'}).catch(()=>{});},5000);});
bMode.addEventListener('pointerup',e=>{const held=modeHeld;modeCancel();
 if(!held){bMode.classList.add('pressed');fetch('/press?btn=mode').then(update).catch(()=>{});
  setTimeout(()=>bMode.classList.remove('pressed'),150);}});
bMode.addEventListener('pointerleave',modeCancel);
bMode.addEventListener('pointercancel',modeCancel);
fun.addEventListener('click',()=>{location.href='/fun';});
mqtt.addEventListener('click',()=>{const p=document.getElementById('mqttp');
 if(p.style.display!=='none'){p.style.display='none';return;}
 fetch('/mqttinfo').then(r=>r.json()).then(j=>{const t=j.topic||'evtester';const h=j.host||'BROKER';
  p.innerHTML=(j.on?'<div class=mrow>Broker <code>mqtt://'+j.host+':'+j.port+'</code> &middot; '
    +(j.connected?'<span style=color:#22c55e>connected</span>':'<span style=color:#f87171>offline</span>')+'</div>'
    :'<div class=mnote>MQTT is off &mdash; set a broker in setup mode (hold MODE 5s).</div>')
   +'<div class=mrow style=margin-top:6px>Base topic <code>'+t+'</code></div>'
   +'<div class=mrow style=margin-top:8px><b>Mode commands</b> &mdash; publish to <code>'+t+'/mode</code></div>'
   +'<table><tr><th>Value</th><th>Action</th></tr>'
   +'<tr><td><code>ready</code></td><td>Go to Ready</td></tr>'
   +'<tr><td><code>charge</code></td><td>Start Charging (needs a live charger)</td></tr>'
   +'<tr><td><code>suspend</code></td><td>Suspend</td></tr>'
   +'<tr><td><code>fault</code></td><td>Inject a pilot fault (needs a live charger; cleared by any other mode)</td></tr></table>'
   +'<div class=mrow><b>Song commands</b> &mdash; publish to <code>'+t+'/song</code></div>'
   +'<table><tr><th>Value</th><th>Action</th></tr>'
   +'<tr><td><code>hbd</code></td><td>Play Happy Birthday</td></tr>'
   +'<tr><td><code>xmas</code></td><td>Play Merry Christmas</td></tr>'
   +'<tr><td><code>canon</code></td><td>Play Canon in D</td></tr>'
   +'<tr><td><code>stop</code></td><td>Stop the song</td></tr></table>'
   +'<div class=mrow><b>Publishes</b> (retained)</div>'
   +'<table><tr><th>Topic</th><th>Values</th></tr>'
   +'<tr><td><code>'+t+'/state</code></td><td>ready / charging / suspended / faulted</td></tr>'
   +'<tr><td><code>'+t+'/availability</code></td><td>online / offline</td></tr></table>'
   +'<div class=mrow><b>Examples</b> &mdash; topic and payload together (mosquitto CLI)</div>'
   +'<table><tr><th>Goal</th><th>Command</th></tr>'
   +'<tr><td>Start charging</td><td><code>mosquitto_pub -h '+h+' -p '+(j.port||1883)+' -t '+t+'/mode -m charge</code></td></tr>'
   +'<tr><td>Suspend</td><td><code>mosquitto_pub -h '+h+' -p '+(j.port||1883)+' -t '+t+'/mode -m suspend</code></td></tr>'
   +'<tr><td>Play a song</td><td><code>mosquitto_pub -h '+h+' -p '+(j.port||1883)+' -t '+t+'/song -m hbd</code></td></tr>'
   +'<tr><td>Watch state changes</td><td><code>mosquitto_sub -h '+h+' -p '+(j.port||1883)+' -t '+t+'/state</code></td></tr></table>';
  p.style.display='block';
 }).catch(()=>{p.innerHTML='<div class=mnote>Could not load MQTT info.</div>';p.style.display='block';});});
function update(){fetch('/status').then(r=>r.json()).then(j=>{
 setRow(l1,j.l1);setRow(l2,j.l2);if(j.ssid)window.apSsid=j.ssid;dot.classList.add('ok');
}).catch(()=>dot.classList.remove('ok'));}
const pollTimer=setInterval(update,500);update();
</script>
<footer style="margin-top:24px;margin-bottom:6px;color:#64748b;font-size:11px;text-align:center;line-height:1.5">&copy; 2026 EVtivity &middot; EV Tester</footer>
</body></html>
)HTMLPAGE";

const char PAGE_FUN[] = R"HTMLPAGE(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>EVtivity EV Tester - Fun</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=VT323&display=swap" rel="stylesheet">
<style>
:root{--green:#22c55e;--bg:#0b1220;}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;}
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:
radial-gradient(1200px 600px at 50% -10%,#13203a,var(--bg));color:#e5e7eb;min-height:100vh;
display:flex;flex-direction:column;align-items:center;padding:22px 14px;}
h1{font-size:19px;margin:2px 0;color:var(--green);letter-spacing:.3px;}
.sub{font-size:12px;color:#8ea0bd;margin-bottom:12px;}
.lcd{position:relative;container-type:inline-size;background:linear-gradient(160deg,#0362fc,#034fce);
border:7px solid #0a1226;border-radius:7px;padding:14px 10px;width:min(360px,94vw);
box-shadow:inset 0 0 26px rgba(2,20,70,.4),inset 0 0 3px rgba(255,255,255,.3),0 8px 22px rgba(0,0,0,.5);}
.lcd::before{content:"";position:absolute;inset:0;pointer-events:none;z-index:2;
background:repeating-linear-gradient(0deg,rgba(0,0,0,.16) 0 1px,transparent 1px 3px),
repeating-linear-gradient(90deg,rgba(0,0,0,.16) 0 1px,transparent 1px 3px);mix-blend-mode:multiply;opacity:.55;}
.lcd .row{position:relative;z-index:1;display:grid;grid-template-columns:repeat(16,1fr);}
.lcd .cell{font-family:'VT323',ui-monospace,Menlo,Consolas,monospace;color:#f2f6ff;
font-size:10cqw;line-height:1.32;text-align:center;overflow:hidden;
text-shadow:0 0 7px rgba(150,190,255,.6);}
.pad{display:flex;gap:26px;margin-top:20px;flex-wrap:wrap;justify-content:center;}
.btn{position:relative;width:126px;height:126px;border:none;border-radius:50%;color:#dbe4f3;
font-weight:700;font-size:13px;letter-spacing:.5px;cursor:pointer;overflow:hidden;
background:radial-gradient(circle at 34% 28%,#26365a,#131e34);
box-shadow:0 10px 22px rgba(0,0,0,.55),inset 0 2px 5px rgba(255,255,255,.06),inset 0 -6px 12px rgba(0,0,0,.4);
transition:transform .09s ease,box-shadow .14s ease;display:flex;flex-direction:column;
align-items:center;justify-content:center;gap:9px;}
.btn svg{width:36px;height:36px;stroke:var(--green);fill:none;stroke-width:2.3;stroke-linecap:round;stroke-linejoin:round;}
.btn.pressed{transform:scale(.9);
box-shadow:0 4px 12px rgba(0,0,0,.6),0 0 30px rgba(34,197,94,.7),inset 0 2px 8px rgba(0,0,0,.5);}
.btn.pressed svg{stroke:#c8ffd8;}
.ripple{position:absolute;border-radius:50%;background:rgba(34,197,94,.35);
transform:scale(0);animation:rip .55s ease-out forwards;pointer-events:none;}
@keyframes rip{to{transform:scale(3.4);opacity:0;}}
.hint{font-size:11px;color:#64748b;margin-top:10px;}
.links{display:flex;gap:12px;margin-top:16px;}
.wifi{background:none;border:1px solid #334155;color:#8ea0bd;padding:9px 16px;
border-radius:9px;font-size:13px;cursor:pointer;transition:.15s;}
.wifi:hover{border-color:var(--green);color:var(--green);}
.mqttp{width:min(540px,94vw);margin-top:14px;padding:14px 16px;background:#111a2e;border:1px solid #22304d;
border-radius:10px;font-size:13px;color:#cdd7ea;line-height:1.55;text-align:left;}
.mqttp .mrow{margin:2px 0;} .mqttp .mnote{color:#64748b;font-size:11px;margin-top:4px;}
.mqttp code{font-family:ui-monospace,Menlo,Consolas,monospace;background:#0c1424;color:#8dffb0;
padding:1px 6px;border-radius:5px;font-size:12px;overflow-wrap:anywhere;}
.mqttp table{width:100%;border-collapse:collapse;margin:6px 0 12px;font-size:12.5px;}
.mqttp th,.mqttp td{text-align:left;padding:5px 8px;border-bottom:1px solid #22304d;vertical-align:top;}
.mqttp th{color:#8ea0bd;font-size:10.5px;text-transform:uppercase;letter-spacing:.3px;}
.mqttp tr:last-child td{border-bottom:none;}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#f87171;margin-right:6px;vertical-align:middle;}
.dot.ok{background:var(--green);box-shadow:0 0 8px var(--green);}
</style></head><body>
<h1>EVtivity EV Tester</h1>
<div class="sub"><span id="dot" class="dot"></span>jukebox</div>
<div class="lcd"><div id="l1" class="row">&nbsp;</div><div id="l2" class="row">&nbsp;</div></div>
<div class="pad">
 <button class="btn" id="bHbd" aria-label="Play Happy Birthday">
  <svg viewBox="0 0 24 24"><path d="M4 21h16M6 21v-7h12v7M9 14v-3M12 14v-4M15 14v-3M12 7c-1-1 1-2 0-3"/></svg>BIRTHDAY</button>
 <button class="btn" id="bXmas" aria-label="Play Merry Christmas">
  <svg viewBox="0 0 24 24"><path d="M12 2l5 7h-3l4 6h-4l3 5H7l3-5H6l4-6H7z"/><path d="M12 20v2"/></svg>CHRISTMAS</button>
 <button class="btn" id="bCanon" aria-label="Play Canon in D">
  <svg viewBox="0 0 24 24"><path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/></svg>CANON</button>
</div>
<div class="hint">Full songs play on the tester's buzzer. Any panel button stops them.</div>
<div class="links">
 <button class="wifi" id="bStop">Stop</button>
 <button class="wifi" id="bBack">Back to panel</button>
</div>
<script>
function ripple(el){const r=document.createElement('span');r.className='ripple';
 const s=Math.max(el.clientWidth,el.clientHeight);r.style.width=r.style.height=s+'px';
 r.style.left=(el.clientWidth-s)/2+'px';r.style.top=(el.clientHeight-s)/2+'px';
 el.appendChild(r);setTimeout(()=>r.remove(),560);}
function press(el,song){el.classList.add('pressed');ripple(el);
 fetch('/play?song='+song).then(update).catch(()=>{});
 setTimeout(()=>el.classList.remove('pressed'),150);}
bHbd.addEventListener('pointerdown',e=>{e.preventDefault();press(e.currentTarget,'hbd');});
bXmas.addEventListener('pointerdown',e=>{e.preventDefault();press(e.currentTarget,'xmas');});
bCanon.addEventListener('pointerdown',e=>{e.preventDefault();press(e.currentTarget,'canon');});
bStop.addEventListener('click',()=>{fetch('/play?song=stop').then(update).catch(()=>{});});
bBack.addEventListener('click',()=>{fetch('/play?song=stop').finally(()=>{location.href='/';});});
function setRow(el,s){s=(s||'').padEnd(16,' ').slice(0,16);el.textContent='';
 for(const c of s){const sp=document.createElement('span');sp.className='cell';
  sp.textContent=(c===' '?' ':c);el.appendChild(sp);}}
function update(){fetch('/status').then(r=>r.json()).then(j=>{
 setRow(l1,j.l1);setRow(l2,j.l2);if(j.ssid)window.apSsid=j.ssid;dot.classList.add('ok');
}).catch(()=>dot.classList.remove('ok'));}
const pollTimer=setInterval(update,500);update();
</script>
<footer style="margin-top:24px;margin-bottom:6px;color:#64748b;font-size:11px;text-align:center;line-height:1.5">&copy; 2026 EVtivity &middot; EV Tester</footer>
</body></html>
)HTMLPAGE";

const char PAGE_CONFIG[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EVtivity EV Tester - WiFi Setup</title>
<style>
:root{--green:#22c55e;--bg:#0b1220;}
*{box-sizing:border-box;}
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
background:radial-gradient(1200px 600px at 50% -10%,#13203a,var(--bg));color:#e5e7eb;
min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:32px 16px;}
h1{font-size:19px;margin:2px 0;color:var(--green);}
.sub{font-size:12px;color:#8ea0bd;margin-bottom:22px;}
.card{background:#111a2e;border:1px solid #22304d;border-radius:12px;padding:20px;width:min(360px,92vw);
box-shadow:0 10px 30px rgba(0,0,0,.4);}
label{display:block;font-size:12px;color:#8ea0bd;margin:12px 0 5px;}
input{width:100%;padding:11px 12px;border-radius:8px;border:1px solid #2b3b5c;background:#0c1424;
color:#e5e7eb;font-size:15px;}
input:focus{outline:none;border-color:var(--green);}
button{width:100%;margin-top:20px;padding:12px;border:none;border-radius:9px;background:var(--green);
color:#04240f;font-weight:700;font-size:15px;cursor:pointer;}
.pw{position:relative;}
.pw input{padding-right:42px;}
.eye{position:absolute;right:5px;top:50%;transform:translateY(-50%);width:32px;height:32px;margin:0;
padding:0;background:none;border:none;color:#8ea0bd;cursor:pointer;display:flex;align-items:center;
justify-content:center;}
.eye:hover{color:#e5e7eb;}
.rule{height:1px;background:#22304d;margin:20px 0 2px;}
.reset{display:block;text-align:center;color:#64748b;font-size:12px;margin-top:16px;text-decoration:none;}
.reset:hover{color:#f87171;}
</style></head><body>
<h1>EVtivity EV Tester</h1><div class="sub">WiFi, login &amp; MQTT setup</div>
<form class="card" method="POST" action="/save">
 <label>WiFi network</label>
 <input name="ssid" placeholder="WiFi name" autocomplete="off" required>
 <label>WiFi password</label>
 <div class="pw">
  <input id="wpass" name="pass" type="password" placeholder="WiFi password" autocomplete="off">
  <button type="button" class="eye" onclick="togglePw()" aria-label="Show password">
   <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
    stroke-linecap="round" stroke-linejoin="round">
    <path d="M1 12s4-7 11-7 11 7 11 7-4 7-11 7-11-7-11-7z"/><circle cx="12" cy="12" r="3"/>
    <line id="slash" x1="4" y1="4" x2="20" y2="20" visibility="hidden"/>
   </svg>
  </button>
 </div>
 <div class="rule"></div>
 <label>Admin user (web login)</label>
 <input name="auser" placeholder="admin" autocomplete="off">
 <label>Admin password</label>
 <input name="apass" type="password" placeholder="leave blank to keep (default admin)" autocomplete="off">
 <div class="rule"></div>
 <label>MQTT broker (optional, blank = off)</label>
 <input name="mhost" placeholder="broker IP or hostname" autocomplete="off">
 <label>MQTT port</label>
 <input name="mport" placeholder="1883" inputmode="numeric" autocomplete="off">
 <label>MQTT username (blank = none)</label>
 <input name="muser" placeholder="username" autocomplete="off">
 <label>MQTT password</label>
 <input id="mpass" name="mpass" type="password" placeholder="password" autocomplete="off">
 <label>MQTT base topic</label>
 <input name="mtopic" placeholder="evtester" autocomplete="off">
 <button type="submit">Save &amp; Connect</button>
</form>
<button type="button" id="exitb" style="width:100%;margin-top:12px;padding:11px;border:1px solid #2b3b5c;border-radius:9px;background:none;color:#8ea0bd;font-size:14px;cursor:pointer">Exit setup without saving</button>
<a class="reset" href="/reset" onclick="return confirm('Forget WiFi, admin login, and MQTT settings, and restart in setup mode?')">Forget everything (factory reset)</a>
<script>
function togglePw(){const i=document.getElementById('wpass'),s=document.getElementById('slash');
 const show=i.type==='password';i.type=show?'text':'password';
 s.setAttribute('visibility',show?'visible':'hidden');}
fetch('/cfg').then(r=>r.json()).then(c=>{
 const set=(n,v)=>{const e=document.querySelector('[name='+n+']');if(e&&v!=null&&v!=='')e.value=v;};
 set('ssid',c.ssid);set('auser',c.auser);set('mhost',c.mhost);set('mport',c.mport);
 set('muser',c.muser);set('mtopic',c.mtopic);
 if(c.haspw)document.getElementById('wpass').placeholder='leave blank to keep current';
 if(c.mhaspw)document.getElementById('mpass').placeholder='leave blank to keep current';
}).catch(()=>{});
document.getElementById('exitb').addEventListener('click',()=>{
 fetch('/exitap',{method:'POST'}).finally(()=>{document.body.innerHTML='<div style="min-height:100vh;display:flex;align-items:center;justify-content:center;font-family:-apple-system,sans-serif;color:#e5e7eb;background:#0b1220">Leaving setup, rebooting&hellip;</div>';});});
</script>
<footer style="margin-top:24px;margin-bottom:6px;color:#64748b;font-size:11px;text-align:center;line-height:1.5">&copy; 2026 EVtivity &middot; EV Tester</footer>
</body></html>
)HTMLPAGE";

// The remote control page (v3.38), served at /remote by Remote.ino.
// In a header, not the .ino: Arduino's prototype generator (ctags) breaks on a raw string literal.
#pragma once
#include <pgmspace.h>

static const char REM_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Badge remote</title>
<style>
:root{--bg:#0b0d16;--card:#141827;--line:#26304a;--fg:#e8eefc;--dim:#8d9ab8;--acc:#59f08a;--amb:#ffb43c;--red:#ff5a6e}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.35 system-ui,-apple-system,sans-serif;padding:16px;max-width:560px;margin:auto}
h1{font-size:22px;margin:4px 0 2px}.sub{color:var(--dim);font-size:13px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:12px}
.card h2{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:0 0 10px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.grid2{grid-template-columns:repeat(2,1fr)}
button{background:#1d2338;color:var(--fg);border:1px solid var(--line);border-radius:10px;padding:13px 6px;font:600 15px system-ui;width:100%}
button:active{background:#2b3557;transform:scale(.97)}
.row{display:flex;align-items:center;gap:12px}input[type=range]{flex:1;accent-color:var(--acc);height:28px}
.vol{font:700 18px ui-monospace,monospace;width:44px;text-align:right}
.tog{width:auto;padding:10px 14px}.on{border-color:var(--acc);color:var(--acc)}.off{border-color:var(--red);color:var(--red)}
.bar{height:10px;background:#0c0f1b;border-radius:6px;overflow:hidden;margin:4px 0 10px}.bar i{display:block;height:100%;border-radius:6px}
.lbl{display:flex;justify-content:space-between;font-size:13px;color:var(--dim)}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:#222a44;border:1px solid var(--line);padding:9px 16px;border-radius:20px;opacity:0;transition:opacity .2s;font-size:14px}
.dot{display:inline-block;width:8px;height:8px;border-radius:4px;margin-right:6px;background:#445}.dot.up{background:var(--acc)}
</style></head><body>
<h1 id="title">Badge remote</h1><div class="sub" id="sub">connecting...</div>

<div class="card"><h2>Sound</h2>
 <div class="row"><input type="range" id="vol" min="0" max="100"><span class="vol" id="volv">--</span>
 <button class="tog" id="mute" onclick="toggleMute()">--</button></div></div>

<div class="card"><h2>Soundboard</h2><div class="grid grid2">
 <button onclick="go('play=slap')">Slap</button><button onclick="go('play=drum')">Drum roll</button>
 <button onclick="go('play=chime')">Oracle chime</button><button onclick="go('play=rumour')">Random rumour</button></div></div>

<div class="card"><h2>Open on the badge</h2><div class="grid" id="apps"></div></div>

<div class="card"><h2>Oracle</h2><button onclick="go('oracle=1','The oracle is thinking...')">Ask the Oracle</button></div>

<div class="card"><h2>Slap</h2><div class="grid" id="slaps"></div></div>

<div class="card"><h2>Danny-gotchi <span id="gst" style="text-transform:none;letter-spacing:0"></span></h2>
 <div class="lbl"><span>Coffee</span><span id="gc"></span></div><div class="bar"><i id="bc" style="background:#c8823c"></i></div>
 <div class="lbl"><span>Mood</span><span id="gm"></span></div><div class="bar"><i id="bm" style="background:#f05a82"></i></div>
 <div class="lbl"><span>Energy</span><span id="ge"></span></div><div class="bar"><i id="be" style="background:#fadc6e"></i></div>
 <div class="grid"><button onclick="go('gotchi=coffee')">Coffee</button><button id="gthx" onclick="go('gotchi=thanks')">Thanks</button><button id="gslp" onclick="go('gotchi=sleep')">Sleep</button></div></div>

<div id="toast"></div>
<script>
const APPS=["PICTURES","ASK DANNY","RUMOURS","SLAP","RADAR","ORACLE","CLOCK","GOTCHI","MUSIC","SETTINGS"];
let st=null,volT=null;
function toast(t){const e=document.getElementById('toast');e.textContent=t;e.style.opacity=1;clearTimeout(e._t);e._t=setTimeout(()=>e.style.opacity=0,1600)}
async function go(q,msg){try{const r=await fetch('/api/do?'+q);const j=await r.json();toast(j.ok?(msg||'Done'):(j.err||'Not possible'));setTimeout(load,400)}catch(e){toast('Badge not reachable')}}
function toggleMute(){if(st)go('mute='+(st.muted?0:1),st.muted?'Sounds on':'Sounds off')}
document.getElementById('vol').addEventListener('input',e=>{document.getElementById('volv').textContent=e.target.value;clearTimeout(volT);volT=setTimeout(()=>fetch('/api/do?vol='+e.target.value),120)});
document.getElementById('apps').innerHTML=APPS.map(a=>`<button onclick="go('app=${encodeURIComponent(a)}','Opening ${a.toLowerCase()}')">${a.split(' ').map(w=>w[0]+w.slice(1).toLowerCase()).join(' ')}</button>`).join('')+`<button onclick="go('home=1','Home')">Home</button>`;
async function load(){try{const r=await fetch('/api/state');st=await r.json();
 document.getElementById('title').textContent=st.name+"'s badge";
 document.getElementById('sub').textContent='v'+st.ver+'  -  showing '+st.screen.toLowerCase()+'  -  '+(st.link?'badge network up':'badge network down');
 if(document.activeElement.id!='vol'){document.getElementById('vol').value=st.vol;document.getElementById('volv').textContent=st.vol}
 const m=document.getElementById('mute');m.textContent=st.muted?'Sounds OFF':'Sounds ON';m.className='tog '+(st.muted?'off':'on');
 document.getElementById('slaps').innerHTML=st.badges.filter(b=>!b.me).map(b=>`<button onclick="go('slap=${b.name}','Slapped ${b.name}!')"><span class="dot ${b.up?'up':''}"></span>${b.name}</button>`).join('')||'<span style="color:#8d9ab8">set up the badge network first</span>';
 const g=st.gotchi;for(const[k,v]of[['c',g.c],['m',g.m],['e',g.e]]){document.getElementById('g'+k).textContent=v+'%';const b=document.getElementById('b'+k);b.style.width=v+'%';b.style.opacity=v<25?.55:1}
 document.getElementById('gst').textContent=g.dead?'- KERNEL PANIC':g.asleep?'- asleep':'';
 document.getElementById('gthx').textContent=g.dead?'Reboot':'Thanks';document.getElementById('gslp').textContent=g.asleep?'Wake':'Sleep';
}catch(e){document.getElementById('sub').textContent='badge not reachable - same Wi-Fi?'}}
load();setInterval(load,3000);
</script></body></html>)HTML";

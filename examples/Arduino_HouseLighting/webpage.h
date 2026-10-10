#pragma once
// Веб-интерфейс: общая панель всех устройств + настройки этого устройства.
// Отдаётся из флеш-памяти, внешние библиотеки и интернет не нужны.

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Подсветка дома</title>
<style>
:root{--bg:#f3f4f6;--card:#fff;--fg:#111827;--mut:#6b7280;--acc:#f59e0b;--bd:#e5e7eb;--ok:#16a34a;--bad:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#1a1d24;--fg:#e5e7eb;--mut:#9ca3af;--bd:#2b2f38}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
header{padding:12px 16px;background:var(--card);border-bottom:1px solid var(--bd);position:sticky;top:0;z-index:2}
h1{font-size:18px;margin:0}h2{font-size:16px;margin:0}.mut{color:var(--mut);font-size:13px}
main{max-width:1200px;margin:0 auto;padding:12px 16px;display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(320px,1fr))}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:12px}
.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.sp{flex:1}
button{border:1px solid var(--bd);background:var(--bg);color:var(--fg);border-radius:8px;padding:6px 10px;font:inherit;cursor:pointer}
button.on{background:var(--acc);border-color:var(--acc);color:#111}
.ch{display:grid;grid-template-columns:1fr 44px 28px;gap:6px;align-items:center;margin-top:8px}
.ch .n{grid-column:1/-1;font-size:13px;display:flex;justify-content:space-between}
input[type=range]{width:100%;accent-color:var(--acc)}
.pct{text-align:right;font-variant-numeric:tabular-nums}.a{padding:2px 0;font-size:12px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--bad);margin-right:6px}.dot.ok{background:var(--ok)}
.house{grid-column:1/-1}
details{grid-column:1/-1}summary{cursor:pointer;font-weight:600;padding:4px 0}
form{display:grid;gap:8px;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));margin-top:8px}
label{display:flex;flex-direction:column;font-size:13px;color:var(--mut);gap:3px}
input,select{font:inherit;padding:6px 8px;border:1px solid var(--bd);border-radius:6px;background:var(--bg);color:var(--fg);min-width:0}
fieldset{grid-column:1/-1;border:1px solid var(--bd);border-radius:8px;display:grid;gap:8px;grid-template-columns:repeat(auto-fill,minmax(200px,1fr))}
legend{font-size:13px;color:var(--mut)}.full{grid-column:1/-1}
</style></head><body>
<header><div class="row"><h1 id="title">Подсветка дома</h1><span class="sp"></span><span class="mut" id="clock"></span></div>
<div class="mut" id="sun"></div></header>
<main id="main">
<div class="card house"><div class="row"><h2>Весь дом</h2><span class="sp"></span>
<button onclick="house({level:100})">Включить</button><button onclick="house({level:0})">Выключить</button>
<button onclick="house({auto:1})">По расписанию</button></div>
<div class="row" style="margin-top:8px"><span class="mut">Яркость всех каналов:</span>
<input type="range" min="0" max="100" value="100" id="houseLvl" style="flex:1"
 onchange="house({level:this.value})"></div></div>
<details id="settings"><summary>Настройки этого устройства</summary>
<form id="cfg" onsubmit="saveCfg(event)">
<fieldset><legend>Устройство</legend>
<label>Номер (1-4)<input name="deviceId" type="number" min="1" max="9"></label>
<label>Название<input name="deviceName" maxlength="31"></label>
<label>Адрес соседа 1<input name="peer0" placeholder="192.168.1.52"></label>
<label>Адрес соседа 2<input name="peer1" placeholder="houselight-3.local"></label>
<label>Адрес соседа 3<input name="peer2"></label>
<label>Логин:пароль соседей<input id="peerAuth" placeholder="admin:пароль (если задан)"></label>
</fieldset>
<fieldset><legend>Расписание</legend>
<label>Широта<input name="lat" type="number" step="0.00001"></label>
<label>Долгота<input name="lon" type="number" step="0.00001"></label>
<label>Часовой пояс (POSIX TZ)<input name="tz" list="tzs"></label>
<label>Включение: закат + мин<input name="sunsetOffset" type="number" min="-240" max="240"></label>
<label>Выключение: рассвет + мин<input name="sunriseOffset" type="number" min="-240" max="240"></label>
<label>Ночной режим<select name="nightEnabled"><option value="0">выкл</option><option value="1">вкл</option></select></label>
<label>Ночь с<input name="nightStart" type="time"></label>
<label>Яркость ночью, % от вечерней<input name="nightPct" type="number" min="0" max="100"></label>
</fieldset>
<fieldset id="chcfg"><legend>Каналы: название, вечерняя яркость %, участие в расписании</legend></fieldset>
<fieldset><legend>Доступ</legend>
<label>SMS-команды с номера<input name="phone" placeholder="+79991234567"></label>
<label>Логин веб<input name="webUser"></label>
<label>Новый пароль веб<input name="webPass" type="password" placeholder="пусто — не менять, - убрать"></label>
</fieldset>
<div class="row full"><button class="on" type="submit">Сохранить</button>
<button type="button" onclick="syncTime()">Время с этого телефона/ПК</button><span class="mut" id="msg"></span></div>
</form>
<form onsubmit="saveWifi(event)"><fieldset><legend>Wi-Fi (после сохранения устройство перезагрузится)</legend>
<label>Сеть (SSID)<input name="ssid"></label><label>Пароль<input name="pass" type="password"></label>
<div class="row"><button type="submit">Сохранить Wi-Fi</button></div></fieldset></form>
</details>
</main>
<datalist id="tzs"><option value="MSK-3">Москва</option><option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Киев, Рига</option>
<option value="<+03>-3">Минск</option><option value="<+05>-5">Алматы, Астана</option><option value="<+04>-4">Самара, Баку, Тбилиси</option>
<option value="<+05>-5">Екатеринбург, Ташкент</option><option value="<+07>-7">Новосибирск, Красноярск</option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Берлин, Варшава</option></datalist>
<script>
const NCH=8;
const $=s=>document.querySelector(s);
let devs=[{host:''}];          // первое — это устройство, остальные — соседи
let busy={};                   // ползунок, который сейчас двигают, не перерисовываем
let peerAuth=localStorage.getItem('peerAuth')||'';

function url(d,p){return (d.host?'http://'+d.host:'')+p}
function api(d,p,params){
  const o={method:params?'POST':'GET',headers:{}};
  if(params)o.body=new URLSearchParams(params);
  if(d.host&&peerAuth)o.headers.Authorization='Basic '+btoa(unescape(encodeURIComponent(peerAuth)));
  return fetch(url(d,p),o).then(r=>{if(!r.ok)throw r.status;return r.json()});
}
function el(t,a,...c){const e=document.createElement(t);Object.assign(e,a||{});e.append(...c);return e}

function buildCard(d,k){
  const c=el('div',{className:'card'});
  c.innerHTML=`<div class="row"><span class="dot"></span><h2 class="nm">${d.host||'Это устройство'}</h2><span class="sp"></span>
  <button class="mA">Авто</button><button class="mM">Ручной</button></div>
  <div class="mut st">нет связи</div>
  <div class="row" style="margin-top:6px"><button class="aOn">Все вкл</button><button class="aOff">Все выкл</button><button class="aAuto">Все по расписанию</button></div>
  <div class="chs"></div>`;
  c.querySelector('.mA').onclick=()=>api(d,'/api/mode',{mode:'auto'}).then(poll);
  c.querySelector('.mM').onclick=()=>api(d,'/api/mode',{mode:'manual'}).then(poll);
  c.querySelector('.aOn').onclick=()=>api(d,'/api/all',{level:100}).then(poll);
  c.querySelector('.aOff').onclick=()=>api(d,'/api/all',{level:0}).then(poll);
  c.querySelector('.aAuto').onclick=()=>api(d,'/api/all',{auto:1}).then(poll);
  const chs=c.querySelector('.chs');d.rows=[];
  for(let i=0;i<NCH;i++){
    const r=el('div',{className:'ch'});
    r.innerHTML=`<div class="n"><span class="cn">Канал ${i+1}</span><span class="mut ci"></span></div>
    <input type="range" min="0" max="100"><span class="pct">0%</span><button class="a" title="вернуть в расписание">А</button>`;
    const s=r.querySelector('input'),p=r.querySelector('.pct'),key=k+'_'+i;let t;
    s.oninput=()=>{busy[key]=Date.now();p.textContent=s.value+'%';clearTimeout(t);
      t=setTimeout(()=>api(d,'/api/corner',{i,level:s.value}),150)};
    r.querySelector('.a').onclick=()=>api(d,'/api/corner',{i,auto:1}).then(poll);
    chs.append(r);d.rows.push({r,s,p,key});
  }
  d.card=c;$('#settings').before(c);
}

function render(d,j){
  const c=d.card;c.querySelector('.dot').classList.toggle('ok',!!j);
  if(!j){c.querySelector('.st').textContent='нет связи';return}
  c.querySelector('.nm').textContent=j.device+(d.host?'':' (это)');
  c.querySelector('.mA').classList.toggle('on',j.mode=='auto');
  c.querySelector('.mM').classList.toggle('on',j.mode=='manual');
  let st=j.timeValid?(j.dark?(j.night?'ночь':'темно — включено'):'день — выключено'):'нет времени!';
  if(!j.pca)st+=' · PCA9685 не найден!';
  st+=' · время: '+j.timeSrc+(j.gsm?' · GSM '+j.csq:'');
  c.querySelector('.st').textContent=st;
  j.ch.forEach((ch,i)=>{const o=d.rows[i];if(!o)return;
    o.r.querySelector('.cn').textContent=ch.name;
    o.r.querySelector('.ci').textContent=j.mode=='manual'?'ручной':(ch.ovr>=0?'вручную до события':(ch.auto?'по расписанию':'не в расписании'));
    o.r.querySelector('.a').style.visibility=j.mode=='auto'&&ch.ovr>=0?'visible':'hidden';
    if(Date.now()-(busy[o.key]||0)>3000){o.s.value=ch.target;o.p.textContent=ch.target+'%'}
  });
}

function poll(){
  devs.forEach(d=>api(d,'/api/state').then(j=>{render(d,j);if(!d.host)head(j)}).catch(()=>render(d,null)));
}
function head(j){
  $('#title').textContent=j.device;
  $('#clock').textContent=j.timeValid?j.time:'время не установлено';
  $('#sun').textContent=j.polar<0?'полярная ночь':j.polar>0?'полярный день':
    `рассвет ${j.sunrise} · закат ${j.sunset} · вкл ${j.on} · выкл ${j.off} · Wi-Fi ${j.wifi}`;
}
function house(p){Promise.all(devs.map(d=>api(d,'/api/all',p).catch(()=>0))).then(poll)}

function loadCfg(){
  api(devs[0],'/api/config').then(c=>{
    const f=$('#cfg');
    for(const k in c)if(f.elements[k])f.elements[k].value=typeof c[k]=='boolean'?(c[k]?1:0):c[k];
    c.peers.forEach((p,i)=>f.elements['peer'+i].value=p);
    $('#peerAuth').value=peerAuth;
    const fs=$('#chcfg');fs.querySelectorAll('label').forEach(e=>e.remove());
    c.ch.forEach((ch,i)=>{
      fs.append(el('label',{},`Канал ${i+1}`,el('input',{name:'name'+i,value:ch.name,maxLength:31})));
      fs.append(el('label',{},'яркость %',el('input',{name:'level'+i,type:'number',min:0,max:100,value:ch.level})));
      const s=el('select',{name:'auto'+i});s.innerHTML='<option value="1">в расписании</option><option value="0">только вручную</option>';
      s.value=ch.auto?1:0;fs.append(el('label',{},'режим',s));
    });
    const peers=c.peers.filter(p=>p.trim());
    devs.slice(1).forEach(d=>d.card.remove());
    devs=[devs[0],...peers.map(h=>({host:h.trim()}))];
    devs.slice(1).forEach((d,k)=>buildCard(d,k+1));
    poll();
  });
}
function saveCfg(e){
  e.preventDefault();
  peerAuth=$('#peerAuth').value;localStorage.setItem('peerAuth',peerAuth);
  api(devs[0],'/api/config',new FormData(e.target)).then(()=>{$('#msg').textContent='сохранено';loadCfg()})
    .catch(()=>$('#msg').textContent='ошибка');
}
function saveWifi(e){e.preventDefault();api(devs[0],'/api/wifi',new FormData(e.target)).catch(()=>0);
  $('#msg').textContent='перезагрузка…'}
function syncTime(){Promise.all(devs.map(d=>api(d,'/api/time',{epoch:Math.floor(Date.now()/1000)}).catch(()=>0)))
  .then(()=>{$('#msg').textContent='время отправлено';poll()})}

buildCard(devs[0],0);loadCfg();setInterval(poll,2000);
</script></body></html>)rawliteral";

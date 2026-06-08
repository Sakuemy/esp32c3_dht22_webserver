#pragma once
#include <pgmspace.h>

// ════════════════════════════════════════════════════════
//  ▸ HTML главной страницы
// ════════════════════════════════════════════════════════
static const char HTML_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<script>document.documentElement.style.fontSize=(localStorage.getItem('fontSize')||'16')+'px';</script>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-C3 Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{width:100%;height:100%;overflow:hidden;background:#0f172a;color:#e2e8f0;font-family:system-ui,sans-serif}
body{display:flex;flex-direction:column;padding:10px;gap:8px}
.hdr{display:flex;align-items:center;justify-content:space-between;flex-shrink:0}
.logo{font-size:1.1rem;font-weight:700;color:#7dd3fc}
.hdr-r{display:flex;align-items:center;gap:14px;font-size:.82rem;color:#94a3b8}
#clock{font-variant-numeric:tabular-nums}
.dot{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:4px;vertical-align:middle}
.on-dot{background:#22c55e;box-shadow:0 0 6px #22c55e}
.off-dot{background:#ef4444;box-shadow:0 0 6px #ef4444}
#stTxt.on{color:#22c55e} #stTxt.off{color:#ef4444}
a.admin-link{color:#7dd3fc;text-decoration:none;font-size:.78rem;border:1px solid #334155;padding:2px 8px;border-radius:5px}
a.admin-link:hover{background:#1e3a5f}
.cards{display:flex;gap:8px;flex-shrink:0}
.card{flex:1;background:#1e293b;border-radius:10px;padding:10px 14px;border:1px solid #334155}
.card-lbl{font-size:.72rem;color:#64748b;text-transform:uppercase;letter-spacing:.05em}
.card-val{font-size:2rem;font-weight:700;margin-top:2px}
.card-val.temp{color:#fb923c} .card-val.hum{color:#38bdf8}
.chart-box{flex:1;background:#1e293b;border-radius:10px;border:1px solid #334155;padding:10px;min-height:0;display:flex;flex-direction:column;position:relative}
.chart-ttl{font-size:.75rem;color:#64748b;margin-bottom:4px;flex-shrink:0}
canvas{flex:1;width:100%!important;min-height:0;display:block;cursor:crosshair}
#tip{position:absolute;background:#1e3a5f;border:1px solid #38bdf8;border-radius:6px;padding:5px 9px;font-size:.78rem;pointer-events:none;display:none;white-space:nowrap;color:#e2e8f0;z-index:10}
</style>
</head>
<body>
<div class="hdr">
  <span class="logo">ESP32-C3 Monitor</span>
  <div class="hdr-r">
    <span id="clock">--:--:--</span>
    <span id="uptime" style="font-size:.82rem;color:#64748b">Аптайм: --</span>
    <span><span class="dot off-dot" id="stDot"></span><span id="stTxt" class="off">Offline</span></span>
    <a class="admin-link" href="/admin">Кабинет</a>
  </div>
</div>
<div class="cards">
  <div class="card"><div class="card-lbl">Температура</div><div class="card-val temp" id="tV">--.- °C</div></div>
  <div class="card"><div class="card-lbl">Влажность</div><div class="card-val hum" id="hV">--.- %</div></div>
</div>
<div class="chart-box">
  <div class="chart-ttl">Температура и влажность — последние 24 ч (среднее за 10 мин)</div>
  <canvas id="chart"></canvas>
  <div id="tip"></div>
</div>
<script>
(function(){
  const cv=document.getElementById('chart'),ctx=cv.getContext('2d'),tip=document.getElementById('tip');
  const DPR=window.devicePixelRatio||1;
  let data={labels:[],temp:[],hum:[]};
  // Сохраняем параметры последней отрисовки для tooltip
  let lastDraw={PL:0,PR:0,PT:0,PB:0,PW:0,PH:0,N:0,tMn:0,tMx:0,CW:0,CH:0};

  function resize(){
    const r=cv.parentElement.getBoundingClientRect();
    cv.width=Math.floor(r.width*DPR);
    cv.height=Math.floor((r.height-22)*DPR);
    draw();
  }

  function draw(){
    const CW=cv.width/DPR,CH=cv.height/DPR;
    ctx.setTransform(DPR,0,0,DPR,0,0);
    ctx.clearRect(0,0,CW,CH);
    const LBL=data.labels,TMP=data.temp,HUM=data.hum,N=LBL.length;
    if(N<2){
      ctx.fillStyle='#475569';ctx.font='26px system-ui';ctx.textAlign='center';
      ctx.fillText('Накапливаю данные… (нужно минимум 2 точки)',CW/2,CH/2);
      return;
    }
    const PL=64,PR=64,PT=14,PB=50;
    const PW=CW-PL-PR,PH=CH-PT-PB;
    const vT=TMP.filter(v=>v!==null&&!isNaN(v));
    if(!vT.length)return;
    const tMn=Math.min(...vT)-1,tMx=Math.max(...vT)+1;
    lastDraw={PL,PR,PT,PB,PW,PH,N,tMn,tMx,CW,CH};

    const xOf=i=>PL+(i/(N-1))*PW;
    const yT=v=>PT+PH-((v-tMn)/(tMx-tMn))*PH;
    const yH=v=>PT+PH-(v/100)*PH;

    // Сетка
    ctx.strokeStyle='#334155';ctx.lineWidth=0.5;
    for(let g=0;g<=4;g++){const y=PT+(g/4)*PH;ctx.beginPath();ctx.moveTo(PL,y);ctx.lineTo(PL+PW,y);ctx.stroke();}

    // Ось Y слева — температура
    ctx.fillStyle='#fb923c';ctx.font='20px system-ui';ctx.textAlign='right';
    for(let g=0;g<=4;g++){
      const v=tMn+((4-g)/4)*(tMx-tMn);
      ctx.fillText(v.toFixed(1),PL-6,PT+(g/4)*PH+7);
    }
    ctx.save();ctx.fillStyle='#fb923c88';ctx.font='18px system-ui';ctx.textAlign='center';
    ctx.translate(14,PT+PH/2);ctx.rotate(-Math.PI/2);ctx.fillText('°C',0,0);ctx.restore();

    // Ось Y справа — влажность
    ctx.fillStyle='#38bdf8';ctx.textAlign='left';
    for(let g=0;g<=4;g++){
      const v=100-(g/4)*100;
      ctx.fillText(v.toFixed(0)+'%',PL+PW+7,PT+(g/4)*PH+7);
    }

    // Метки X
    ctx.fillStyle='#64748b';ctx.textAlign='center';ctx.font='20px system-ui';
    const st=Math.max(1,Math.floor(N/6));
    const shown=new Set();
    for(let i=0;i<N;i+=st){ctx.fillText(LBL[i],xOf(i),PT+PH+26);shown.add(i);}
    if(!shown.has(N-1))ctx.fillText(LBL[N-1],xOf(N-1),PT+PH+26);

    // Линия температуры
    ctx.strokeStyle='#fb923c';ctx.lineWidth=2.5;ctx.lineJoin='round';
    ctx.beginPath();let pen=false;
    for(let i=0;i<N;i++){
      if(TMP[i]===null||isNaN(TMP[i])){pen=false;continue;}
      pen?ctx.lineTo(xOf(i),yT(TMP[i])):ctx.moveTo(xOf(i),yT(TMP[i]));pen=true;
    }
    ctx.stroke();

    // Линия влажности
    ctx.strokeStyle='#38bdf8';ctx.lineWidth=2.5;
    ctx.beginPath();pen=false;
    for(let i=0;i<N;i++){
      if(HUM[i]===null||isNaN(HUM[i])){pen=false;continue;}
      pen?ctx.lineTo(xOf(i),yH(HUM[i])):ctx.moveTo(xOf(i),yH(HUM[i]));pen=true;
    }
    ctx.stroke();

    // Легенда
    const lx=PL,ly=CH-12;
    ctx.lineWidth=2.5;
    ctx.strokeStyle='#fb923c';ctx.beginPath();ctx.moveTo(lx,ly);ctx.lineTo(lx+20,ly);ctx.stroke();
    ctx.fillStyle='#fb923c';ctx.textAlign='left';ctx.font='18px system-ui';ctx.fillText('Температура',lx+23,ly+6);
    ctx.strokeStyle='#38bdf8';ctx.beginPath();ctx.moveTo(lx+150,ly);ctx.lineTo(lx+170,ly);ctx.stroke();
    ctx.fillStyle='#38bdf8';ctx.fillText('Влажность',lx+173,ly+6);
  }

  // ── Tooltip при наведении ──────────────────────────────
  cv.addEventListener('mousemove',function(e){
    const {PL,PW,PT,PH,N,tMn,tMx}=lastDraw;
    if(N<2){tip.style.display='none';return;}
    const rect=cv.getBoundingClientRect();
    const mx=e.clientX-rect.left;
    const my=e.clientY-rect.top;
    // Определяем ближайший индекс по X
    const frac=(mx-PL)/PW;
    if(frac<0||frac>1){tip.style.display='none';return;}
    const idx=Math.round(frac*(N-1));
    if(idx<0||idx>=N){tip.style.display='none';return;}
    const T=data.temp[idx],H=data.hum[idx];
    const tStr=T!==null&&!isNaN(T)?T.toFixed(1)+' °C':'—';
    const hStr=H!==null&&!isNaN(H)?H.toFixed(1)+' %':'—';
    tip.innerHTML='<b>'+data.labels[idx]+'</b><br>🌡 '+tStr+'<br>💧 '+hStr;
    // Позиционируем tooltip рядом с курсором, не выходя за края
    const bw=tip.offsetWidth||110,bh=tip.offsetHeight||60;
    const cw=rect.width,ch=rect.height;
    let tx=e.clientX-rect.left+14,ty=e.clientY-rect.top-bh-8;
    if(tx+bw>cw)tx=e.clientX-rect.left-bw-14;
    if(ty<0)ty=e.clientY-rect.top+14;
    tip.style.left=tx+'px';tip.style.top=ty+'px';
    tip.style.display='block';
  });
  cv.addEventListener('mouseleave',function(){tip.style.display='none';});

  window._chartUpdate=function(d){data=d;draw();};
  window.addEventListener('resize',resize);
  resize();
})();

// Часы
const p2=n=>String(n).padStart(2,'0');
let tsOff=0;
function tick(){const d=new Date(Date.now()+tsOff);document.getElementById('clock').textContent=p2(d.getHours())+':'+p2(d.getMinutes())+':'+p2(d.getSeconds());}
setInterval(tick,1000);tick();

// Статус
let online=false;
function setSt(on){
  if(online===on)return;online=on;
  document.getElementById('stDot').className='dot '+(on?'on-dot':'off-dot');
  const t=document.getElementById('stTxt');t.className=on?'on':'off';t.textContent=on?'Online':'Offline';
}

// Форматирование аптайма
function formatUptime(sec) {
  const d = Math.floor(sec / (24 * 3600));
  sec %= (24 * 3600);
  const h = Math.floor(sec / 3600);
  sec %= 3600;
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  
  let res = '';
  if (d > 0) res += d + ' д. ';
  if (d > 0 || h > 0) res += h + ' ч. ';
  if (d > 0 || h > 0 || m > 0) res += m + ' мин. ';
  res += s + ' сек.';
  return res;
}

// Polling с таймаутом и 3 попытками
let failedCount = 0;
let pollTmr = null;

function poll() {
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 6000); // 6 секунд таймаут

  fetch('/api/data', { cache: 'no-store', signal: controller.signal })
    .then(r => r.ok ? r.json() : Promise.reject())
    .then(d => {
      clearTimeout(timeoutId);
      failedCount = 0;
      setSt(true);
      tsOff = d.unixSec * 1000 - Date.now();
      document.getElementById('tV').textContent = d.temp != null ? d.temp.toFixed(1) + ' °C' : '--.- °C';
      document.getElementById('hV').textContent = d.hum != null ? d.hum.toFixed(1) + ' %' : '--.- %';
      document.getElementById('uptime').textContent = 'Аптайм: ' + formatUptime(d.uptime);
      window._chartUpdate(d.chart);
      scheduleNextPoll(60000); // Опрос раз в минуту
    })
    .catch(() => {
      clearTimeout(timeoutId);
      failedCount++;
      if (failedCount >= 3) {
        setSt(false);
      }
      if (failedCount < 3) {
        setTimeout(poll, 1500); // Быстрая перепопытка через 1.5 сек
      } else {
        scheduleNextPoll(60000); // Регулярный опрос раз в минуту
      }
    });
}

function scheduleNextPoll(ms) {
  clearTimeout(pollTmr);
  pollTmr = setTimeout(poll, ms);
}

poll();
</script>
</body>
</html>
)rawliteral";

// ════════════════════════════════════════════════════════
//  ▸ HTML страницы входа
// ════════════════════════════════════════════════════════
static const char HTML_LOGIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<script>document.documentElement.style.fontSize=(localStorage.getItem('fontSize')||'16')+'px';</script>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Вход — ESP32-C3</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%;background:#0f172a;color:#e2e8f0;font-family:system-ui,sans-serif;display:flex;align-items:center;justify-content:center}
.box{background:#1e293b;border:1px solid #334155;border-radius:14px;padding:32px 28px;width:320px;text-align:center}
h2{color:#7dd3fc;margin-bottom:24px;font-size:1.2rem}
input{width:100%;background:#0f172a;border:1px solid #334155;border-radius:8px;color:#e2e8f0;padding:10px 12px;font-size:1rem;margin-bottom:14px;outline:none}
input:focus{border-color:#7dd3fc}
button{width:100%;background:#2563eb;color:#fff;border:none;border-radius:8px;padding:11px;font-size:1rem;cursor:pointer}
button:hover{background:#1d4ed8}
.err{color:#ef4444;font-size:.85rem;margin-top:10px;min-height:1.2em}
.back{margin-top:16px;font-size:.8rem;color:#64748b}<br>
.back a{color:#7dd3fc;text-decoration:none}
</style>
</head>
<body>
<div class="box">
  <h2>Личный кабинет</h2>
  <input type="password" id="pw" placeholder="Пароль" onkeydown="if(event.key==='Enter')login()">
  <button onclick="login()">Войти</button>
  <div class="err" id="err"></div>
  <div class="back"><a href="/">← На главную</a></div>
</div>
<script>
function login(){
  fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'pw='+encodeURIComponent(document.getElementById('pw').value)})
  .then(r=>r.json()).then(d=>{
    if(d.ok)window.location.href='/admin';
    else document.getElementById('err').textContent='Неверный пароль';
  }).catch(()=>document.getElementById('err').textContent='Ошибка соединения');
}
</script>
</body>
</html>
)rawliteral";

// ════════════════════════════════════════════════════════
//  ▸ HTML личного кабинета
// ════════════════════════════════════════════════════════
static const char HTML_ADMIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<script>document.documentElement.style.fontSize=(localStorage.getItem('fontSize')||'16')+'px';</script>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Кабинет — ESP32-C3</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0f172a;color:#e2e8f0;font-family:system-ui,sans-serif;padding:16px;min-height:100vh}
.hdr{display:flex;align-items:center;justify-content:space-between;margin-bottom:20px}
.hdr h1{font-size:1.1rem;color:#7dd3fc}
.hdr-r{display:flex;gap:10px}
a.btn,button.btn{display:inline-block;padding:6px 14px;border-radius:7px;border:none;cursor:pointer;font-size:.85rem;text-decoration:none}
.btn-sec{background:#1e293b;color:#94a3b8;border:1px solid #334155}.btn-sec:hover{background:#334155;color:#e2e8f0}
.btn-pri{background:#2563eb;color:#fff}.btn-pri:hover{background:#1d4ed8}
.btn-dan{background:#7f1d1d;color:#fca5a5;border:1px solid #ef4444}.btn-dan:hover{background:#991b1b}
.btn-sm{padding:4px 10px;font-size:.78rem}
table{width:100%;border-collapse:collapse;margin-top:8px}
th{text-align:left;padding:8px 10px;font-size:.75rem;color:#64748b;text-transform:uppercase;border-bottom:1px solid #334155}
td{padding:9px 10px;border-bottom:1px solid #1e293b;font-size:.9rem;vertical-align:middle}
tr:hover td{background:#1e293b}
.badge{display:inline-block;padding:2px 9px;border-radius:99px;font-size:.75rem;font-weight:600}
.badge-on{background:#14532d;color:#4ade80}
.badge-off{background:#3b0764;color:#c084fc}
/* Модалка */
.overlay{position:fixed;inset:0;background:#00000088;display:flex;align-items:center;justify-content:center;z-index:100;display:none}
.overlay.open{display:flex}
.modal{background:#1e293b;border:1px solid #334155;border-radius:14px;padding:28px;width:340px;max-width:95vw}
.modal h3{color:#7dd3fc;margin-bottom:18px;font-size:1rem}
label{display:block;font-size:.8rem;color:#94a3b8;margin-bottom:4px;margin-top:12px}
label:first-of-type{margin-top:0}
input[type=text],input[type=number],select{width:100%;background:#0f172a;border:1px solid #334155;border-radius:7px;color:#e2e8f0;padding:8px 10px;font-size:.9rem;outline:none}
input:focus,select:focus{border-color:#7dd3fc}
select option{background:#1e293b}
.modal-foot{display:flex;gap:8px;margin-top:18px;justify-content:flex-end}
.empty{color:#475569;font-size:.9rem;padding:20px 10px}
</style>
</head>
<body>
<div class="hdr">
  <h1>Управление устройствами</h1>
  <div class="hdr-r">
    <button class="btn btn-pri" onclick="openModal()">+ Добавить</button>
    <a class="btn btn-sec" href="/logout">Выйти</a>
    <a class="btn btn-sec" href="/">← Главная</a>
  </div>
</div>

<table id="tbl">
  <thead><tr><th>Название</th><th>GPIO</th><th>Статус</th><th>Действия</th></tr></thead>
  <tbody id="tbody"></tbody>
</table>

<!-- Раздел настроек -->
<div class="hdr" style="margin-top: 32px;">
  <h1>Настройки системы</h1>
</div>

<div style="background:#1e293b;border:1px solid #334155;border-radius:14px;padding:20px;max-width:450px;">
  <label>Размер шрифта интерфейса</label>
  <select id="setfontSize" onchange="changefontSize(this.value)" style="margin-bottom: 14px;">
    <option value="14">Мелкий (14px)</option>
    <option value="16">Средний (16px)</option>
    <option value="18">Крупный (18px)</option>
    <option value="20">Очень крупный (20px)</option>
  </select>
  
  <label>Коррекция датчика температуры (°C)</label>
  <div style="display:flex;gap:8px;">
    <input type="number" id="setTempOffset" step="0.1" placeholder="0.0" style="flex:1;">
    <button class="btn btn-pri" onclick="saveTempOffset()">Сохранить</button>
  </div>
  <div id="setErr" style="color:#ef4444;font-size:.8rem;margin-top:8px"></div>
  <div id="setOk" style="color:#22c55e;font-size:.8rem;margin-top:8px"></div>
</div>

<!-- Модалка добавления/редактирования -->
<div class="overlay" id="modal">
  <div class="modal">
    <h3 id="mTitle">Добавить устройство</h3>
    <input type="hidden" id="mId" value="-1">
    <label>Название</label>
    <input type="text" id="mName" maxlength="31" placeholder="Реле 1">
    <label>GPIO пин</label>
    <input type="number" id="mPin" min="0" max="21" placeholder="5">
    <label>Состояние</label>
    <select id="mState">
      <option value="0">Выкл</option>
      <option value="1">Вкл</option>
    </select>
    <div class="modal-foot">
      <button class="btn btn-sec" onclick="closeModal()">Отмена</button>
      <button class="btn btn-pri" onclick="saveDevice()">Сохранить</button>
    </div>
    <div id="mErr" style="color:#ef4444;font-size:.8rem;margin-top:8px"></div>
  </div>
</div>

<script>
let devices=[];

function load(){
  fetch('/api/devices').then(r=>r.json()).then(d=>{devices=d;render();})
    .catch(()=>{});
}

function render(){
  const tb=document.getElementById('tbody');
  if(!devices.length){
    tb.innerHTML='<tr><td colspan="4" class="empty">Устройств пока нет. Нажмите «+ Добавить».</td></tr>';
    return;
  }
  tb.innerHTML=devices.map(d=>`
    <tr>
      <td>${esc(d.name)}</td>
      <td>GPIO ${d.pin}</td>
      <td><span class="badge ${d.state?'badge-on':'badge-off'}">${d.state?'Вкл':'Выкл'}</span></td>
      <td style="display:flex;gap:6px;flex-wrap:wrap">
        <button class="btn btn-sec btn-sm" onclick="toggleDev(${d.id})">${d.state?'Выключить':'Включить'}</button>
        <button class="btn btn-sec btn-sm" onclick="editDev(${d.id})">Изменить</button>
        <button class="btn btn-dan btn-sm" onclick="deleteDev(${d.id})">Удалить</button>
      </td>
    </tr>`).join('');
}

function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}

function openModal(id){
  document.getElementById('mErr').textContent='';
  if(id==null){
    document.getElementById('mTitle').textContent='Добавить устройство';
    document.getElementById('mId').value='-1';
    document.getElementById('mName').value='';
    document.getElementById('mPin').value='';
    document.getElementById('mState').value='0';
  } else {
    const d=devices.find(x=>x.id===id);if(!d)return;
    document.getElementById('mTitle').textContent='Изменить устройство';
    document.getElementById('mId').value=d.id;
    document.getElementById('mName').value=d.name;
    document.getElementById('mPin').value=d.pin;
    document.getElementById('mState').value=d.state?'1':'0';
  }
  document.getElementById('modal').classList.add('open');
  setTimeout(()=>document.getElementById('mName').focus(),50);
}
function closeModal(){document.getElementById('modal').classList.remove('open');}

function editDev(id){openModal(id);}

function saveDevice(){
  const id=parseInt(document.getElementById('mId').value);
  const name=document.getElementById('mName').value.trim();
  const pin=parseInt(document.getElementById('mPin').value);
  const state=document.getElementById('mState').value==='1';
  if(!name){document.getElementById('mErr').textContent='Введите название';return;}
  if(isNaN(pin)||pin<0||pin>21){document.getElementById('mErr').textContent='GPIO должен быть 0–21';return;}
  const body=`id=${id}&name=${encodeURIComponent(name)}&pin=${pin}&state=${state?1:0}`;
  fetch('/api/device/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body})
    .then(r=>r.json()).then(d=>{
      if(d.ok){closeModal();load();}
      else document.getElementById('mErr').textContent=d.err||'Ошибка';
    }).catch(()=>document.getElementById('mErr').textContent='Ошибка соединения');
}

function deleteDev(id){
  if(!confirm('Удалить устройство?'))return;
  fetch('/api/device/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'id='+id})
    .then(r=>r.json()).then(d=>{if(d.ok)load();})
    .catch(()=>{});
}

function toggleDev(id){
  fetch('/api/device/toggle',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'id='+id})
    .then(r=>r.json()).then(d=>{if(d.ok)load();})
    .catch(()=>{});
}

// Закрыть по клику на фон
document.getElementById('modal').addEventListener('click',function(e){if(e.target===this)closeModal();});

function loadConfig() {
  const currentSize = localStorage.getItem('fontSize') || '16';
  document.getElementById('setfontSize').value = currentSize;
  
  fetch('/api/settings')
    .then(r => r.json())
    .then(d => {
      document.getElementById('setTempOffset').value = d.tempOffset;
    })
    .catch(() => {});
}

function changefontSize(size) {
  localStorage.setItem('fontSize', size);
  document.documentElement.style.fontSize = size + 'px';
}

function saveTempOffset() {
  const offsetInput = document.getElementById('setTempOffset');
  const offset = parseFloat(offsetInput.value);
  const errDiv = document.getElementById('setErr');
  const okDiv = document.getElementById('setOk');
  
  errDiv.textContent = '';
  okDiv.textContent = '';
  
  if (isNaN(offset)) {
    errDiv.textContent = 'Введите корректное число';
    return;
  }
  
  fetch('/api/settings/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'tempOffset=' + encodeURIComponent(offset)
  })
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        okDiv.textContent = 'Настройки сохранены';
      } else {
        errDiv.textContent = d.err || 'Ошибка при сохранении';
      }
    })
    .catch(() => {
      errDiv.textContent = 'Ошибка соединения';
    });
}

load();
loadConfig();
</script>
</body>
</html>
)rawliteral";

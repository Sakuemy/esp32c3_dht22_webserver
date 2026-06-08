/*
 * ESP32-C3 | DHT22 + Web Server + NTP + LED + Admin Panel
 *
 * Пины по умолчанию:
 *   GPIO4  — DHT22 data
 *   GPIO12 — LED WiFi (горит при подключении)
 *   GPIO13 — LED TX  (моргает при HTTP-запросе)
 *
 * Зависимости (Library Manager):
 *   - DHT sensor library  by Adafruit  (+ Adafruit Unified Sensor)
 *   - ESPAsyncWebServer   by me-no-dev
 *   - AsyncTCP            by me-no-dev
 *   Preferences — встроена в ESP32 Arduino core (NVS)
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DHT.h>
#include <Preferences.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ════════════════════════════════════════════════════════
//  ▸ НАСТРОЙКИ — измените под свою сеть
// ════════════════════════════════════════════════════════
const char* WIFI_SSID           = "*";
const char* WIFI_PASSWORD       = "*";
const char* ADMIN_PASSWORD      = "admin123";   // пароль личного кабинета
const long  GMT_OFFSET_SEC      = 3 * 3600;     // UTC+3 (Москва)
const int   DAYLIGHT_OFFSET_SEC = 0;

// ════════════════════════════════════════════════════════
//  ▸ КАЛИБРОВКА ДАТЧИКА
//    Прибавляется к каждому замеру температуры.
//    Положительное — повысить показания, отрицательное — понизить.
// ════════════════════════════════════════════════════════
const float TEMP_OFFSET = 0.0f;

// ════════════════════════════════════════════════════════
//  ▸ ПИНЫ
// ════════════════════════════════════════════════════════
#define DHT_PIN     4
#define LED_WIFI    12
#define LED_TX      13
#define DHT_TYPE    DHT22

// ════════════════════════════════════════════════════════
//  ▸ ПАРАМЕТРЫ ИСТОРИИ
// ════════════════════════════════════════════════════════
#define HISTORY_POINTS  144                 // 24 ч × 6 точек/ч
#define SLOT_MS         (10UL * 60 * 1000)  // окно 10 мин
#define DHT_READ_MS     (60UL * 1000)       // опрос DHT раз в 1 мин

// ════════════════════════════════════════════════════════
//  ▸ УСТРОЙСТВА (хранятся в NVS через Preferences)
// ════════════════════════════════════════════════════════
#define MAX_DEVICES     16
#define DEV_NAME_LEN    32

struct Device {
  char     name[DEV_NAME_LEN];
  uint8_t  pin;
  bool     state;   // true = Вкл
  bool     used;    // true = слот занят
};

static Device    devices[MAX_DEVICES];
static Preferences prefs;

// Зарезервированные пины — нельзя назначать устройствам
static bool isPinReserved(uint8_t p) {
  return p == DHT_PIN || p == LED_WIFI || p == LED_TX;
}

void loadDevices() {
  prefs.begin("devices", true);  // read-only
  for (int i = 0; i < MAX_DEVICES; i++) {
    char key[12];
    snprintf(key, sizeof(key), "dev%d", i);
    if (prefs.isKey(key)) {
      prefs.getBytes(key, &devices[i], sizeof(Device));
      // Восстанавливаем физическое состояние пина
      if (devices[i].used && !isPinReserved(devices[i].pin)) {
        pinMode(devices[i].pin, OUTPUT);
        digitalWrite(devices[i].pin, devices[i].state ? HIGH : LOW);
      }
    } else {
      memset(&devices[i], 0, sizeof(Device));
      devices[i].used = false;
    }
  }
  prefs.end();
}

void saveDevice(int idx) {
  if (idx < 0 || idx >= MAX_DEVICES) return;
  prefs.begin("devices", false);  // read-write
  char key[12];
  snprintf(key, sizeof(key), "dev%d", idx);
  prefs.putBytes(key, &devices[idx], sizeof(Device));
  prefs.end();
}

void deleteDeviceFromNVS(int idx) {
  if (idx < 0 || idx >= MAX_DEVICES) return;
  prefs.begin("devices", false);
  char key[12];
  snprintf(key, sizeof(key), "dev%d", idx);
  prefs.remove(key);
  prefs.end();
}

// ════════════════════════════════════════════════════════
//  ▸ СЕССИЯ (простой токен в памяти)
//    Один активный токен — один залогиненный клиент.
// ════════════════════════════════════════════════════════
static char sessionToken[33] = {0};  // 32 hex-символа + \0

static void generateToken() {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  uint32_t r4 = esp_random();
  snprintf(sessionToken, sizeof(sessionToken),
           "%08x%08x%08x%08x", r1, r2, r3, r4);
}

static bool isAuthorized(AsyncWebServerRequest* req) {
  if (sessionToken[0] == 0) return false;
  if (!req->hasHeader("Cookie")) return false;
  String cookies = req->header("Cookie");
  String needle  = String("esp_sess=") + sessionToken;
  return cookies.indexOf(needle) >= 0;
}

// ════════════════════════════════════════════════════════
//  ▸ КОЛЬЦЕВОЙ БУФЕР ИСТОРИИ
// ════════════════════════════════════════════════════════
struct DataPoint {
  time_t   timestamp;
  float    tempSum;
  float    humSum;
  uint16_t count;
  float    tempAvg;
  float    humAvg;
  bool     ready;
};

static DataPoint history[HISTORY_POINTS];
static int  historyHead  = 0;
static int  historyCount = 0;
static int  currentSlot  = -1;

static float curTemp = NAN;
static float curHum  = NAN;

// ════════════════════════════════════════════════════════
//  ▸ МЬЮТЕКС (защита history/curTemp/curHum)
// ════════════════════════════════════════════════════════
static SemaphoreHandle_t dataMutex = nullptr;
#define MUTEX_TAKE()  xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50))
#define MUTEX_GIVE()  xSemaphoreGive(dataMutex)

// ════════════════════════════════════════════════════════
//  ▸ ОБЪЕКТЫ
// ════════════════════════════════════════════════════════
static DHT            dht(DHT_PIN, DHT_TYPE);
static AsyncWebServer server(80);

// ════════════════════════════════════════════════════════
//  ▸ LED TX — неблокирующий
// ════════════════════════════════════════════════════════
static volatile unsigned long ledTxOffAt = 0;
static void blinkTx() {
  digitalWrite(LED_TX, HIGH);
  ledTxOffAt = millis() + 80;
}

// ════════════════════════════════════════════════════════
//  ▸ ИСТОРИЯ — функции (вызывать под мьютексом)
// ════════════════════════════════════════════════════════
static void initHistory() {
  memset(history, 0, sizeof(history));
  historyHead = historyCount = 0;
  currentSlot = -1;
}
static void openNewSlot(time_t t) {
  currentSlot = historyHead;
  DataPoint& p = history[currentSlot];
  p.timestamp = t; p.tempSum = 0; p.humSum = 0;
  p.count = 0; p.tempAvg = NAN; p.humAvg = NAN; p.ready = false;
}
static void closeCurrentSlot() {
  if (currentSlot < 0) return;
  DataPoint& p = history[currentSlot];
  if (p.count > 0) { p.tempAvg = p.tempSum / p.count; p.humAvg = p.humSum / p.count; }
  p.ready = true;
  historyHead = (historyHead + 1) % HISTORY_POINTS;
  if (historyCount < HISTORY_POINTS) historyCount++;
  currentSlot = -1;
}
static void addSampleToSlot(float t, float h) {
  if (currentSlot < 0 || isnan(t) || isnan(h)) return;
  history[currentSlot].tempSum += t;
  history[currentSlot].humSum  += h;
  history[currentSlot].count++;
}

// ════════════════════════════════════════════════════════
//  ▸ JSON /api/data  (статический буфер — нет heap alloc)
// ════════════════════════════════════════════════════════
#define JSON_BUF_SIZE 4096
static char jsonBuf[JSON_BUF_SIZE];

static const char* buildApiJson() {
  if (MUTEX_TAKE() != pdTRUE) {
    strncpy(jsonBuf, "{\"error\":\"busy\"}", JSON_BUF_SIZE);
    return jsonBuf;
  }
  float lt = curTemp, lh = curHum;
  time_t nowT; time(&nowT);
  int lHead = historyHead, lCount = historyCount;
  static DataPoint snap[HISTORY_POINTS];
  memcpy(snap, history, sizeof(history));
  MUTEX_GIVE();

  char* p = jsonBuf, *end = jsonBuf + JSON_BUF_SIZE - 1;
  p += snprintf(p, end-p, "{\"unixSec\":%lu,", (unsigned long)nowT);
  if (isnan(lt)) p += snprintf(p, end-p, "\"temp\":null,");
  else           p += snprintf(p, end-p, "\"temp\":%.1f,", lt);
  if (isnan(lh)) p += snprintf(p, end-p, "\"hum\":null,");
  else           p += snprintf(p, end-p, "\"hum\":%.1f,",  lh);

  int oldest = (lHead - lCount + HISTORY_POINTS) % HISTORY_POINTS;
  p += snprintf(p, end-p, "\"chart\":{\"labels\":[");
  bool first = true;
  for (int i = 0; i < lCount && p < end; i++) {
    int idx = (oldest + i) % HISTORY_POINTS;
    if (!snap[idx].ready) continue;
    struct tm* ti = localtime(&snap[idx].timestamp);
    char tb[6]; strftime(tb, sizeof(tb), "%H:%M", ti);
    p += snprintf(p, end-p, "%s\"%s\"", first?"":","  , tb);
    first = false;
  }
  p += snprintf(p, end-p, "],\"temp\":[");
  first = true;
  for (int i = 0; i < lCount && p < end; i++) {
    int idx = (oldest + i) % HISTORY_POINTS;
    if (!snap[idx].ready) continue;
    p += snprintf(p, end-p, isnan(snap[idx].tempAvg) ? "%snull" : "%s%.1f",
                  first?"":"," , snap[idx].tempAvg);
    first = false;
  }
  p += snprintf(p, end-p, "],\"hum\":[");
  first = true;
  for (int i = 0; i < lCount && p < end; i++) {
    int idx = (oldest + i) % HISTORY_POINTS;
    if (!snap[idx].ready) continue;
    p += snprintf(p, end-p, isnan(snap[idx].humAvg) ? "%snull" : "%s%.1f",
                  first?"":"," , snap[idx].humAvg);
    first = false;
  }
  snprintf(p, end-p, "]}}");
  return jsonBuf;
}

// ════════════════════════════════════════════════════════
//  ▸ JSON /api/devices
// ════════════════════════════════════════════════════════
#define DEV_JSON_SIZE 1024
static char devJsonBuf[DEV_JSON_SIZE];

static const char* buildDevicesJson() {
  char* p = devJsonBuf, *end = devJsonBuf + DEV_JSON_SIZE - 1;
  p += snprintf(p, end-p, "[");
  bool first = true;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!devices[i].used) continue;
    // Экранируем имя: заменяем " на \"
    char safeName[DEV_NAME_LEN * 2];
    int si = 0;
    for (int k = 0; devices[i].name[k] && si < (int)sizeof(safeName)-2; k++) {
      if (devices[i].name[k] == '"') safeName[si++] = '\\';
      safeName[si++] = devices[i].name[k];
    }
    safeName[si] = 0;
    p += snprintf(p, end-p, "%s{\"id\":%d,\"name\":\"%s\",\"pin\":%d,\"state\":%s}",
                  first?"":"," , i, safeName, devices[i].pin,
                  devices[i].state ? "true" : "false");
    first = false;
  }
  snprintf(p, end-p, "]");
  return devJsonBuf;
}

// ════════════════════════════════════════════════════════
//  ▸ HTML главной страницы
// ════════════════════════════════════════════════════════
static const char HTML_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
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
let online=false,offTmr=null;
function setSt(on){
  if(online===on)return;online=on;
  document.getElementById('stDot').className='dot '+(on?'on-dot':'off-dot');
  const t=document.getElementById('stTxt');t.className=on?'on':'off';t.textContent=on?'Online':'Offline';
}

// Polling
function poll(){
  fetch('/api/data',{cache:'no-store'})
    .then(r=>r.ok?r.json():Promise.reject())
    .then(d=>{
      tsOff=d.unixSec*1000-Date.now();
      clearTimeout(offTmr);setSt(true);offTmr=setTimeout(()=>setSt(false),9000);
      document.getElementById('tV').textContent=d.temp!=null?d.temp.toFixed(1)+' °C':'--.- °C';
      document.getElementById('hV').textContent=d.hum !=null?d.hum.toFixed(1) +' %' :'--.- %';
      window._chartUpdate(d.chart);
    })
    .catch(()=>{clearTimeout(offTmr);setSt(false);});
}
poll();setInterval(poll,3000);
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

load();
</script>
</body>
</html>
)rawliteral";

// ════════════════════════════════════════════════════════
//  ▸ setup
// ════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[Boot] ESP32-C3 DHT22 Monitor v3");

  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_TX,   OUTPUT);
  digitalWrite(LED_WIFI, LOW);
  digitalWrite(LED_TX,   LOW);

  dataMutex = xSemaphoreCreateMutex();
  configASSERT(dataMutex);

  dht.begin();
  initHistory();
  loadDevices();  // загружаем устройства из NVS и восстанавливаем состояние пинов

  // WiFi
  Serial.printf("[WiFi] Подключение к: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
  digitalWrite(LED_WIFI, HIGH);

  // NTP
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP]  Синхронизация");
  struct tm ti;
  while (!getLocalTime(&ti)) { delay(500); Serial.print("."); }
  Serial.println(" OK");

  // ── Маршруты ──────────────────────────────────────────

  // Главная
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    req->send_P(200, "text/html; charset=utf-8", HTML_MAIN);
  });

  // Данные сенсора + история
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    req->send(200, "application/json", buildApiJson());
  });

  // Страница входа
  server.on("/login", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (isAuthorized(req)) { req->redirect("/admin"); return; }
    req->send_P(200, "text/html; charset=utf-8", HTML_LOGIN);
  });

  // POST /api/login — проверка пароля
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (req->hasParam("pw", true)) {
      String pw = req->getParam("pw", true)->value();
      if (pw == ADMIN_PASSWORD) {
        generateToken();
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
        String cookie = String("esp_sess=") + sessionToken + "; Path=/; HttpOnly";
        resp->addHeader("Set-Cookie", cookie);
        req->send(resp);
        return;
      }
    }
    req->send(200, "application/json", "{\"ok\":false}");
  });

  // Выход
  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    sessionToken[0] = 0;  // инвалидируем токен
    AsyncWebServerResponse* resp = req->beginResponse(302, "text/plain", "");
    resp->addHeader("Location", "/login");
    resp->addHeader("Set-Cookie", "esp_sess=; Path=/; Max-Age=0");
    req->send(resp);
  });

  // Страница кабинета
  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->redirect("/login"); return; }
    req->send_P(200, "text/html; charset=utf-8", HTML_ADMIN);
  });

  // GET /api/devices — список устройств (только авторизованным)
  server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    req->send(200, "application/json", buildDevicesJson());
  });

  // POST /api/device/save — добавить или изменить
  server.on("/api/device/save", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }

    if (!req->hasParam("name",true) || !req->hasParam("pin",true) || !req->hasParam("state",true)) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"missing params\"}");
      return;
    }

    int    id    = req->hasParam("id",true) ? req->getParam("id",true)->value().toInt() : -1;
    String name  = req->getParam("name",true)->value();
    int    pin   = req->getParam("pin",true)->value().toInt();
    bool   state = req->getParam("state",true)->value().toInt() != 0;

    // Валидация
    if (name.length() == 0 || name.length() > 31) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"invalid name\"}"); return;
    }
    if (pin < 0 || pin > 21) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"invalid pin\"}"); return;
    }
    if (isPinReserved((uint8_t)pin)) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"pin reserved\"}"); return;
    }

    // Ищем слот: если id=-1 — новый, иначе редактируем
    int slot = -1;
    if (id >= 0 && id < MAX_DEVICES && devices[id].used) {
      slot = id;
    } else {
      // Найти первый свободный слот
      for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) { slot = i; break; }
      }
    }
    if (slot < 0) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"no slots\"}"); return;
    }

    // Если пин изменился — обнуляем старый
    if (devices[slot].used && devices[slot].pin != (uint8_t)pin) {
      if (!isPinReserved(devices[slot].pin)) {
        pinMode(devices[slot].pin, INPUT);
      }
    }

    strncpy(devices[slot].name, name.c_str(), DEV_NAME_LEN - 1);
    devices[slot].name[DEV_NAME_LEN - 1] = 0;
    devices[slot].pin   = (uint8_t)pin;
    devices[slot].state = state;
    devices[slot].used  = true;

    pinMode(pin, OUTPUT);
    digitalWrite(pin, state ? HIGH : LOW);
    saveDevice(slot);

    req->send(200, "application/json", "{\"ok\":true}");
  });

  // POST /api/device/delete
  server.on("/api/device/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    if (!req->hasParam("id", true)) { req->send(200, "application/json", "{\"ok\":false}"); return; }

    int id = req->getParam("id", true)->value().toInt();
    if (id < 0 || id >= MAX_DEVICES || !devices[id].used) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"not found\"}"); return;
    }
    // Гасим пин перед удалением
    if (!isPinReserved(devices[id].pin)) {
      digitalWrite(devices[id].pin, LOW);
      pinMode(devices[id].pin, INPUT);
    }
    memset(&devices[id], 0, sizeof(Device));
    devices[id].used = false;
    deleteDeviceFromNVS(id);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // POST /api/device/toggle
  server.on("/api/device/toggle", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    if (!req->hasParam("id", true)) { req->send(200, "application/json", "{\"ok\":false}"); return; }

    int id = req->getParam("id", true)->value().toInt();
    if (id < 0 || id >= MAX_DEVICES || !devices[id].used) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"not found\"}"); return;
    }
    devices[id].state = !devices[id].state;
    if (!isPinReserved(devices[id].pin)) {
      digitalWrite(devices[id].pin, devices[id].state ? HIGH : LOW);
    }
    saveDevice(id);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[HTTP] Сервер запущен.");
}

// ════════════════════════════════════════════════════════
//  ▸ loop
// ════════════════════════════════════════════════════════
void loop() {
  const unsigned long now = millis();

  // 1. LED TX
  if (ledTxOffAt > 0 && now >= ledTxOffAt) {
    digitalWrite(LED_TX, LOW);
    ledTxOffAt = 0;
  }

  // 2. Опрос DHT раз в 1 мин; применяем TEMP_OFFSET
  static unsigned long lastDht = 0;
  if (now - lastDht >= DHT_READ_MS) {
    lastDht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      t += TEMP_OFFSET;   // ← калибровка
      if (MUTEX_TAKE() == pdTRUE) {
        curTemp = t;
        curHum  = h;
        addSampleToSlot(t, h);
        MUTEX_GIVE();
      }
    }
  }

  // 3. 10-минутные окна истории
  static unsigned long lastSlotOpen = 0;
  static bool          ntpSynced    = false;
  if (!ntpSynced) {
    time_t t; time(&t);
    if (t > 1704067200UL) {
      ntpSynced = true; lastSlotOpen = now;
      if (MUTEX_TAKE() == pdTRUE) { openNewSlot(t); MUTEX_GIVE(); }
      Serial.println("[History] Первое окно открыто.");
    }
  } else if (now - lastSlotOpen >= SLOT_MS) {
    lastSlotOpen += SLOT_MS;
    time_t t; time(&t);
    if (MUTEX_TAKE() == pdTRUE) { closeCurrentSlot(); openNewSlot(t); MUTEX_GIVE(); }
  }

  // 4. LED WiFi раз в секунду
  static unsigned long lastLed = 0;
  if (now - lastLed >= 1000) {
    lastLed = now;
    digitalWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
  }

  // 5. Уступаем CPU (TWDT)
  delay(1);
}

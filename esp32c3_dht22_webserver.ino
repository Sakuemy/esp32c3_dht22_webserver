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

#include "secrets.h"

// ════════════════════════════════════════════════════════
//  ▸ НАСТРОЙКИ — измените под свою сеть
// ════════════════════════════════════════════════════════
const char* ADMIN_PASSWORD      = "admin123";   // пароль личного кабинета
const long  GMT_OFFSET_SEC      = 3 * 3600;     // UTC+3 (Москва)
const int   DAYLIGHT_OFFSET_SEC = 0;

// ════════════════════════════════════════════════════════
//  ▸ КАЛИБРОВКА ДАТЧИКА
//    Прибавляется к каждому замеру температуры.
//    Положительное — повысить показания, отрицательное — понизить.
// ════════════════════════════════════════════════════════
static float tempOffset = 0.0f;

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

void loadSettings() {
  prefs.begin("settings", true);
  tempOffset = prefs.getFloat("tempOffset", 0.0f);
  prefs.end();
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putFloat("tempOffset", tempOffset);
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
  unsigned long uptime = millis() / 1000;
  p += snprintf(p, end-p, "{\"unixSec\":%lu,\"uptime\":%lu,", (unsigned long)nowT, uptime);
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
//  ▸ HTML / JS ресурсы (вынесены в web_pages.h)
// ════════════════════════════════════════════════════════
#include "web_pages.h"

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
  loadSettings();
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

  // GET /api/settings
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"tempOffset\":%.2f}", tempOffset);
    req->send(200, "application/json", buf);
  });

  // POST /api/settings/save
  server.on("/api/settings/save", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    if (req->hasParam("tempOffset", true)) {
      tempOffset = req->getParam("tempOffset", true)->value().toFloat();
      saveSettings();
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"missing tempOffset\"}");
    }
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
      t += tempOffset;   // ← калибровка
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

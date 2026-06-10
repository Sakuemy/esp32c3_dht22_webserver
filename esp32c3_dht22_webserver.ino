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
#include <driver/gpio.h>
#include <esp_timer.h>
#include <Preferences.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "secrets.h"

// ════════════════════════════════════════════════════════
//  ▸ НАСТРОЙКИ — измените под свою сеть
// ════════════════════════════════════════════════════════
static String adminPassword     = "admin123";   // пароль личного кабинета
const long  GMT_OFFSET_SEC      = 3 * 3600;     // UTC+3 (Москва)
const int   DAYLIGHT_OFFSET_SEC = 0;

static float tempOffset = 0.0f;

// Настройки батареи и индикации
static float batMax   = 4.2f;
static float batMin   = 3.0f;
static float batR1    = 230000.0f;   // верхний резистор делителя (Ом)
static float batR2    = 1033000.0f;  // нижний резистор делителя (Ом)
static float batCalib = 1.0f;        // калибровочный коэффициент (для компенсации нелинейности ADC)
static bool ledWifiEn = true;
static bool ledTxEn = true;

static String wifiSSID = "";
static String wifiPassword = "";

// Переменные подключения WiFi
static int wifiAttemptCount = 0;
static unsigned long lastWifiAttemptTime = 0;
static int wifiReconnectCount = 0;
static bool lastDhtReadFailed = false;
static bool wasConnected = false;

// Переменные актуальности данных DHT и очереди ошибок
static unsigned long lastDhtSuccessTime = 0;
static bool dhtSuccessValid = false;

#define MAX_QUEUED_ERRORS 16
static String errorQueue[MAX_QUEUED_ERRORS];
static int errorQueueCount = 0;

// Состояние настройки через Serial
enum SerialState {
  STATE_IDLE,
  STATE_WIFI_SSID,
  STATE_WIFI_PASS,
  STATE_ADMIN_PASS
};
static SerialState serialState = STATE_IDLE;
static String newSsid = "";
static String newPass = "";
static unsigned long lastSerialTime = 0;
static String serialInputBuffer = "";

// ════════════════════════════════════════════════════════
//  ▸ ПИНЫ
// ════════════════════════════════════════════════════════
#define DHT_PIN     5
#define LED_WIFI    12
#define LED_TX      13
#define BATTERY_PIN 2
#define DHT_TYPE    DHT22

// ════════════════════════════════════════════════════════
//  ▸ ПАРАМЕТРЫ ИСТОРИИ
// ════════════════════════════════════════════════════════
#define HISTORY_POINTS  144                 // 24 ч × 6 точек/ч
#define SLOT_MS         (10UL * 60 * 1000)  // окно 10 мин
#define DHT_READ_MS     (50UL * 1000)       // опрос DHT раз в 50 сек

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
  return p == DHT_PIN || p == LED_WIFI || p == LED_TX || p == BATTERY_PIN;
}

void loadDevices() {
  prefs.begin("devices", true);  // read-only
  for (int i = 0; i < MAX_DEVICES; i++) {
    char key[12];
    snprintf(key, sizeof(key), "dev%d", i);
    if (prefs.isKey(key)) {
      prefs.getBytes(key, &devices[i], sizeof(Device));
      // Восстанавливаем физическое состояние пина с валидацией его номера
      if (devices[i].used && devices[i].pin <= 21 && !isPinReserved(devices[i].pin)) {
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
  adminPassword = prefs.getString("adminPw", "admin123");
  wifiSSID = prefs.getString("wifiSSID", WIFI_SSID);
  wifiPassword = prefs.getString("wifiPW", WIFI_PASSWORD);
  batMax   = prefs.getFloat("batMax",   4.2f);
  batMin   = prefs.getFloat("batMin",   3.0f);
  batR1    = prefs.getFloat("batR1",    230000.0f);
  batR2    = prefs.getFloat("batR2",    1033000.0f);
  batCalib = prefs.getFloat("batCalib", 1.0f);
  ledWifiEn = prefs.getBool("ledWifiEn", true);
  ledTxEn = prefs.getBool("ledTxEn", true);
  prefs.end();
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putFloat("tempOffset", tempOffset);
  prefs.putString("adminPw", adminPassword);
  prefs.putString("wifiSSID", wifiSSID);
  prefs.putString("wifiPW", wifiPassword);
  prefs.putFloat("batMax",   batMax);
  prefs.putFloat("batMin",   batMin);
  prefs.putFloat("batR1",    batR1);
  prefs.putFloat("batR2",    batR2);
  prefs.putFloat("batCalib", batCalib);
  prefs.putBool("ledWifiEn", ledWifiEn);
  prefs.putBool("ledTxEn", ledTxEn);
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

static void pushError(const String& err) {
  if (MUTEX_TAKE() == pdTRUE) {
    if (errorQueueCount < MAX_QUEUED_ERRORS) {
      errorQueue[errorQueueCount++] = err;
    } else {
      // Сдвигаем влево при переполнении
      for (int i = 1; i < MAX_QUEUED_ERRORS; i++) {
        errorQueue[i - 1] = errorQueue[i];
      }
      errorQueue[MAX_QUEUED_ERRORS - 1] = err;
    }
    MUTEX_GIVE();
  }
}

// ════════════════════════════════════════════════════════
//  ▸ ОБЪЕКТЫ
// ════════════════════════════════════════════════════════
// Функция для прямого чтения датчика DHT22 (без digitalRead, который вызывает WDT-панику)
static bool readDHT22(float &temp, float &hum) {
  uint8_t data[5] = {0};
  
  // 1. Посылаем сигнал запуска
  pinMode(DHT_PIN, OUTPUT);
  digitalWrite(DHT_PIN, LOW);
  delay(20); // Держим линию в LOW 20 мс
  
  // Переводим пин на INPUT_PULLUP с гарантированным импульсом HIGH для крутого фронта
  digitalWrite(DHT_PIN, HIGH);
  pinMode(DHT_PIN, OUTPUT);
  delayMicroseconds(40);
  pinMode(DHT_PIN, INPUT_PULLUP);
  delayMicroseconds(10);
  
  // Входим в критическую секцию для точного замера длительности импульсов
  static portMUX_TYPE myMutex = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&myMutex);
  
  // Ожидание ответа датчика (линия должна уйти в LOW, затем в HIGH, затем снова в LOW)
  #define WaitPin(level, timeout_us) { \
    uint32_t start = esp_timer_get_time(); \
    while (gpio_get_level((gpio_num_t)DHT_PIN) == level) { \
      if ((esp_timer_get_time() - start) > timeout_us) { \
        portEXIT_CRITICAL(&myMutex); \
        return false; \
      } \
    } \
  }
  
  // Ожидаем окончания HIGH, датчик притянет линию к LOW
  WaitPin(HIGH, 100);
  // Датчик держит LOW 80 мкс
  WaitPin(LOW, 100);
  // Датчик держит HIGH 80 мкс
  WaitPin(HIGH, 100);
  
  // Считываем 40 бит данных
  for (int i = 0; i < 40; i++) {
    // Ждем окончания низкого уровня (около 50 мкс)
    WaitPin(LOW, 100);
    
    // Линия ушла в HIGH. Засекаем время
    uint32_t startHigh = esp_timer_get_time();
    // Ждем окончания высокого уровня
    WaitPin(HIGH, 100);
    uint32_t duration = esp_timer_get_time() - startHigh;
    
    int byteIdx = i / 8;
    data[byteIdx] <<= 1;
    // Если импульс длиннее 40 мкс, то это "1", иначе "0"
    if (duration > 40) {
      data[byteIdx] |= 1;
    }
  }
  
  portEXIT_CRITICAL(&myMutex);
  
  // Проверка контрольной суммы
  uint8_t checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
  if (data[4] != checksum) {
    return false;
  }
  
  // Декодируем влажность
  float h = ((data[0] << 8) | data[1]) * 0.1f;
  
  // Декодируем температуру (учитывая знаковый бит)
  int16_t rawTemp = ((data[2] & 0x7F) << 8) | data[3];
  if (data[2] & 0x80) {
    rawTemp = -rawTemp;
  }
  float t = rawTemp * 0.1f;
  
  if (h < 0.0f || h > 100.0f || t < -40.0f || t > 80.0f) {
    return false;
  }
  
  temp = t;
  hum = h;
  return true;
}

static AsyncWebServer server(80);

// ════════════════════════════════════════════════════════
//  ▸ LED TX — неблокирующий
// ════════════════════════════════════════════════════════
static volatile unsigned long ledTxOffAt = 0;
static void blinkTx() {
  if (!ledTxEn) return;
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

static float filteredBatVolts = -1.0f;

static float getBatteryVoltage() {
  // Усредняем несколько отсчётов для уменьшения шума ADC
  uint32_t sum = 0;
  const int SAMPLES = 8;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogReadMilliVolts(BATTERY_PIN);
    delayMicroseconds(100);
  }
  float vAdc = (sum / (float)SAMPLES) / 1000.0f;
  // Восстанавливаем напряжение батареи по делителю: Vbat = Vadc * (R1+R2)/R2
  float ratio = (batR2 > 0.0f) ? (batR1 + batR2) / batR2 : 1.0f;
  return vAdc * ratio * batCalib;
}

// ════════════════════════════════════════════════════════
//  ▸ JSON /api/data  (статический буфер — нет heap alloc)
// ════════════════════════════════════════════════════════
#define JSON_BUF_SIZE 6144
static char jsonBuf[JSON_BUF_SIZE];

// Безопасный макрос для добавления в буфер JSON без риска переполнения
#define APPEND_JSON(...) do { \
  int n = snprintf(p, end - p, __VA_ARGS__); \
  if (n > 0 && n < (end - p)) p += n; \
  else p = end; \
} while(0)

static const char* buildApiJson() {
  static String localErrors[MAX_QUEUED_ERRORS];
  int localErrorCount = 0;

  if (MUTEX_TAKE() != pdTRUE) {
    strncpy(jsonBuf, "{\"error\":\"busy\"}", JSON_BUF_SIZE);
    return jsonBuf;
  }
  float lt = curTemp, lh = curHum;
  
  // Проверка актуальности данных за последнюю минуту
  if (!dhtSuccessValid || (millis() - lastDhtSuccessTime > 60000)) {
    lt = NAN;
    lh = NAN;
  }

  time_t nowT; time(&nowT);
  int lHead = historyHead, lCount = historyCount;
  static DataPoint snap[HISTORY_POINTS];
  memcpy(snap, history, sizeof(history));

  // Копируем и очищаем очередь ошибок
  localErrorCount = errorQueueCount;
  for (int i = 0; i < localErrorCount; i++) {
    localErrors[i] = errorQueue[i];
  }
  errorQueueCount = 0;

  MUTEX_GIVE();

  float vBat = filteredBatVolts;
  int batLvl = -1;
  if (vBat >= 0.5f) {
    if (vBat >= batMax) {
      batLvl = 100;
    } else if (vBat <= batMin) {
      batLvl = 0;
    } else {
      batLvl = (int)roundf((vBat - batMin) / (batMax - batMin) * 100.0f);
    }
  }

  char* p = jsonBuf, *end = jsonBuf + JSON_BUF_SIZE - 1;
  unsigned long uptime = millis() / 1000;
  APPEND_JSON("{\"unixSec\":%lu,\"uptime\":%lu,\"batVolts\":%.2f,\"batLevel\":%d,\"dhtError\":%s,\"wifiRecon\":%d,\"errors\":[",
              (unsigned long)nowT, uptime, vBat, batLvl,
              lastDhtReadFailed ? "true" : "false", wifiReconnectCount);

  for (int i = 0; i < localErrorCount; i++) {
    // Внимание: экранируем кавычки в сообщении об ошибке, если они есть
    String safeMsg = localErrors[i];
    safeMsg.replace("\"", "\\\"");
    APPEND_JSON("%s\"%s\"", i == 0 ? "" : ",", safeMsg.c_str());
  }
  APPEND_JSON("],");
  if (isnan(lt)) APPEND_JSON("\"temp\":null,");
  else           APPEND_JSON("\"temp\":%.1f,", lt);
  if (isnan(lh)) APPEND_JSON("\"hum\":null,");
  else           APPEND_JSON("\"hum\":%.1f,",  lh);

  int oldest = (lHead - lCount + HISTORY_POINTS) % HISTORY_POINTS;
  APPEND_JSON("\"chart\":{\"times\":[");
  bool first = true;
  for (int i = 0; i < lCount; i++) {
    int idx = (oldest + i) % HISTORY_POINTS;
    if (!snap[idx].ready) continue;
    APPEND_JSON("%s%lu", first ? "" : ",", (unsigned long)snap[idx].timestamp);
    first = false;
  }
  APPEND_JSON("],\"temp\":[");
  first = true;
  for (int i = 0; i < lCount; i++) {
    int idx = (oldest + i) % HISTORY_POINTS;
    if (!snap[idx].ready) continue;
    if (isnan(snap[idx].tempAvg)) APPEND_JSON("%snull", first ? "" : ",");
    else                          APPEND_JSON("%s%.1f", first ? "" : ",", snap[idx].tempAvg);
    first = false;
  }
  APPEND_JSON("],\"hum\":[");
  first = true;
  for (int i = 0; i < lCount; i++) {
    int idx = (oldest + i) % HISTORY_POINTS;
    if (!snap[idx].ready) continue;
    if (isnan(snap[idx].humAvg)) APPEND_JSON("%snull", first ? "" : ",");
    else                         APPEND_JSON("%s%.1f", first ? "" : ",", snap[idx].humAvg);
    first = false;
  }
  APPEND_JSON("]}}");
  return jsonBuf;
}

// ════════════════════════════════════════════════════════
//  ▸ JSON /api/devices
// ════════════════════════════════════════════════════════
#define DEV_JSON_SIZE 1024
static char devJsonBuf[DEV_JSON_SIZE];

static const char* buildDevicesJson() {
  char* p = devJsonBuf, *end = devJsonBuf + DEV_JSON_SIZE - 1;
  APPEND_JSON("[");
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
    APPEND_JSON("%s{\"id\":%d,\"name\":\"%s\",\"pin\":%d,\"state\":%s}",
                first ? "" : ",", i, safeName, devices[i].pin,
                devices[i].state ? "true" : "false");
    first = false;
  }
  APPEND_JSON("]");
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
  pinMode(BATTERY_PIN, INPUT);
  digitalWrite(LED_WIFI, LOW);
  digitalWrite(LED_TX,   LOW);

  dataMutex = xSemaphoreCreateMutex();
  configASSERT(dataMutex);

  pinMode(DHT_PIN, INPUT_PULLUP);
  initHistory();
  loadSettings();
  loadDevices();  // загружаем устройства из NVS и восстанавливаем состояние пинов

  // WiFi — один WiFi.begin(), ждём до 20 сек
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  Serial.printf("[WiFi] Подключение к: %s\n", wifiSSID.c_str());
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  {
    unsigned long t0 = millis();
    while (millis() - t0 < 20000 && WiFi.status() != WL_CONNECTED) {
      delay(200);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Подключено! IP: %s\n", WiFi.localIP().toString().c_str());
    wifiAttemptCount = 0;
    wasConnected = true;
  } else {
    Serial.println("[WiFi] Не удалось подключиться. Повтор в фоне.");
    wifiAttemptCount = 1;
    wasConnected = false;
  }
  lastWifiAttemptTime = millis();

  // NTP
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  Serial.println("[NTP] Запущена фоновая синхронизация времени.");

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
      if (pw == adminPassword) {
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
    char buf[300];
    snprintf(buf, sizeof(buf),
             "{\"tempOffset\":%.2f,\"batMax\":%.2f,\"batMin\":%.2f,\"batR1\":%.1f,\"batR2\":%.1f,\"batCalib\":%.4f,\"ledWifiEn\":%s,\"ledTxEn\":%s}",
             tempOffset, batMax, batMin, batR1, batR2, batCalib,
             ledWifiEn ? "true" : "false", ledTxEn ? "true" : "false");
    req->send(200, "application/json", buf);
  });

  // POST /api/settings/save
  server.on("/api/settings/save", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    bool changed = false;
    if (req->hasParam("tempOffset", true)) {
      tempOffset = req->getParam("tempOffset", true)->value().toFloat();
      changed = true;
    }
    if (req->hasParam("batMax", true)) {
      batMax = req->getParam("batMax", true)->value().toFloat();
      changed = true;
    }
    if (req->hasParam("batMin", true)) {
      batMin = req->getParam("batMin", true)->value().toFloat();
      changed = true;
    }
    if (req->hasParam("batR1", true)) {
      batR1 = req->getParam("batR1", true)->value().toFloat();
      changed = true;
    }
    if (req->hasParam("batR2", true)) {
      batR2 = req->getParam("batR2", true)->value().toFloat();
      changed = true;
    }
    if (req->hasParam("batCalib", true)) {
      batCalib = req->getParam("batCalib", true)->value().toFloat();
      if (batCalib < 0.5f) batCalib = 0.5f;  // защита от некорректных значений
      if (batCalib > 2.0f) batCalib = 2.0f;
      changed = true;
    }
    if (req->hasParam("ledWifiEn", true)) {
      ledWifiEn = req->getParam("ledWifiEn", true)->value().toInt() != 0;
      changed = true;
    }
    if (req->hasParam("ledTxEn", true)) {
      ledTxEn = req->getParam("ledTxEn", true)->value().toInt() != 0;
      changed = true;
    }
    if (changed) {
      saveSettings();
      if (!ledWifiEn) {
        digitalWrite(LED_WIFI, LOW);
      } else {
        digitalWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
      }
      if (!ledTxEn) {
        digitalWrite(LED_TX, LOW);
      }
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"no settings changed\"}");
    }
  });

  // POST /api/settings/password
  server.on("/api/settings/password", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    if (req->hasParam("newPassword", true)) {
      String newPw = req->getParam("newPassword", true)->value();
      newPw.trim();
      if (newPw.length() < 4 || newPw.length() > 32) {
        req->send(200, "application/json", "{\"ok\":false,\"err\":\"Длина пароля должна быть от 4 до 32 символов\"}");
        return;
      }
      adminPassword = newPw;
      saveSettings();
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"missing newPassword\"}");
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

static void processSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  
  if (serialState == STATE_IDLE) {
    if (cmd.equalsIgnoreCase("help")) {
      Serial.println("\n=== Доступные команды ===");
      Serial.println("  wifi     - Настройка подключения к WiFi");
      Serial.println("  password - Изменение пароля администратора");
      Serial.println("  status   - Текущий статус системы");
      Serial.println("  help     - Показать это сообщение");
    } else if (cmd.equalsIgnoreCase("wifi")) {
      Serial.println("\n[WiFi] Введите SSID сети:");
      serialState = STATE_WIFI_SSID;
    } else if (cmd.equalsIgnoreCase("password")) {
      Serial.println("\n[Admin] Введите новый пароль администратора (от 4 до 32 символов):");
      serialState = STATE_ADMIN_PASS;
    } else if (cmd.equalsIgnoreCase("status")) {
      Serial.println("\n=== Статус системы ===");
      Serial.printf("  WiFi SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("  IP адрес: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("  WiFi статус: %s\n", WiFi.status() == WL_CONNECTED ? "Подключен" : "Отключен");
      Serial.printf("  Температура: %.1f °C\n", curTemp);
      Serial.printf("  Влажность: %.1f %%\n", curHum);
      float vBat = filteredBatVolts;
      if (vBat >= 0.5f) {
        Serial.printf("  Батарея: %.2f В\n", vBat);
      } else {
        Serial.println("  Батарея: нет сигнала");
      }
    } else {
      Serial.println("Неизвестная команда. Введите 'help' для списка команд.");
    }
  } else if (serialState == STATE_WIFI_SSID) {
    newSsid = cmd;
    Serial.println("[WiFi] Введите пароль сети:");
    serialState = STATE_WIFI_PASS;
  } else if (serialState == STATE_WIFI_PASS) {
    newPass = cmd;
    Serial.printf("[WiFi] Подключение к '%s' с паролем '%s'...\n", newSsid.c_str(), newPass.c_str());
    
    wifiSSID = newSsid;
    wifiPassword = newPass;
    saveSettings();
    
    // Переподключаемся
    WiFi.disconnect();
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    
    // Сбрасываем счетчик попыток подключения
    wifiAttemptCount = 1;
    lastWifiAttemptTime = millis();
    
    serialState = STATE_IDLE;
  } else if (serialState == STATE_ADMIN_PASS) {
    if (cmd.length() < 4 || cmd.length() > 32) {
      Serial.println("[Ошибка] Пароль должен быть от 4 до 32 символов. Введите заново:");
    } else {
      adminPassword = cmd;
      saveSettings();
      Serial.println("[Admin] Пароль администратора успешно изменен!");
      serialState = STATE_IDLE;
    }
  }
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

  // 2. Опрос DHT раз в 1 мин с 3 попытками чтения; применяем TEMP_OFFSET
  static unsigned long lastDht = 0;
  if (now - lastDht >= DHT_READ_MS) {
    lastDht = now;
    float t = NAN;
    float h = NAN;
    bool success = false;
    
    for (int attempt = 1; attempt <= 3; attempt++) {
      if (readDHT22(t, h)) {
        success = true;
        break;
      }
      Serial.printf("[Sensor] Попытка %d чтения DHT22 не удалась\n", attempt);
      if (attempt < 3) {
        delay(2000); // DHT22 требует минимум 2 сек между опросами
      }
    }
    
    if (success) {
      t += tempOffset;   // ← калибровка
      lastDhtReadFailed = false;
      lastDhtSuccessTime = millis();
      dhtSuccessValid = true;
      if (MUTEX_TAKE() == pdTRUE) {
        curTemp = t;
        curHum  = h;
        addSampleToSlot(t, h);
        MUTEX_GIVE();
      }
    } else {
      lastDhtReadFailed = true;
      pushError("Ошибка чтения датчика DHT22!");
      Serial.println("[Sensor] Ошибка чтения DHT22: все 3 попытки завершились неудачей");
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

  // 4. Опрос батареи раз в 60 сек (фактические данные, без экспоненциального усреднения)
  static unsigned long lastBatteryRead = 0;
  if (filteredBatVolts < 0.0f || now - lastBatteryRead >= 60000) {
    lastBatteryRead = now;
    filteredBatVolts = getBatteryVoltage();
  }

  // 5. WiFi Reconnection & status LED
  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      wasConnected = true;
      wifiReconnectCount++;
      Serial.printf("[WiFi] Реконнект зафиксирован. Всего реконнектов: %d\n", wifiReconnectCount);
    }
    if (wifiAttemptCount > 0) {
      Serial.println("\n[WiFi] Подключено!");
      Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
      wifiAttemptCount = 0;
      lastWifiAttemptTime = 0;
    }
    
    // LED WiFi (if enabled)
    static unsigned long lastLed = 0;
    if (now - lastLed >= 1000) {
      lastLed = now;
      digitalWrite(LED_WIFI, ledWifiEn ? HIGH : LOW);
    }
  } else {
    if (wasConnected) {
      wasConnected = false;
      pushError("Потеряно соединение с WiFi!");
    }
    unsigned long interval = (wifiAttemptCount < 6) ? 10000UL : 60000UL;
    if (lastWifiAttemptTime == 0 || now - lastWifiAttemptTime >= interval) {
      wifiAttemptCount++;
      lastWifiAttemptTime = now;
      Serial.printf("\n[WiFi] Попытка подключения %d... (SSID: %s)\n", wifiAttemptCount, wifiSSID.c_str());
      WiFi.disconnect();
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    }
    
    static unsigned long lastLed = 0;
    if (now - lastLed >= 1000) {
      lastLed = now;
      digitalWrite(LED_WIFI, LOW);
    }
  }

  // 6. Чтение и парсинг команд из Serial (кабель)
  while (Serial.available() > 0) {
    char c = Serial.read();
    lastSerialTime = now;
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) {
        processSerialCommand(serialInputBuffer);
        serialInputBuffer = "";
      }
    } else {
      if (serialInputBuffer.length() < 128) {
        serialInputBuffer += c;
      }
    }
  }

  // Сброс состояния Serial при неактивности (таймаут 1 минута)
  if (serialState != STATE_IDLE && now - lastSerialTime > 60000) {
    Serial.println("\n[Serial] Тайм-аут настройки. Возврат в обычный режим.");
    serialState = STATE_IDLE;
    serialInputBuffer = "";
  }

  // 7. Уступаем CPU (TWDT)
  delay(1);
}

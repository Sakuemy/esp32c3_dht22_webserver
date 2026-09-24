/*
 * ESP32-C3 | DHT22 + Web Server + NTP + LED + Admin Panel
 *
 * Пины по умолчанию:
 *   GPIO5  — DHT22 data
 *   GPIO12 — LED WiFi (горит при подключении)
 *   GPIO13 — LED TX  (моргает при HTTP-запросе)
 *
 * Зависимости (Library Manager):
 *   - DHT sensor library  by Adafruit  (+ Adafruit Unified Sensor)
 *   - ESPAsyncWebServer   by me-no-dev
 *   - AsyncTCP            by me-no-dev
 *   Preferences, WiFiClientSecure, HTTPClient — встроены в ESP32 Arduino core 3.x
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
#include "notify.h"

// TLS-рукопожатие уведомлений выполняется в loop() — стандартных 8 КБ стека может не хватить
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ════════════════════════════════════════════════════════
//  ▸ НАСТРОЙКИ — измените под свою сеть
// ════════════════════════════════════════════════════════
static char adminPassword[33]   = "admin123";   // пароль личного кабинета
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

static char wifiSSID[65]     = "";
static char wifiPassword[65] = "";

// Переменные подключения WiFi
static int wifiAttemptCount = 0;
static unsigned long lastWifiAttemptTime = 0;
static int wifiReconnectCount = 0;
static bool lastDhtReadFailed = false;
static bool wasConnected = false;

// ── Хардовый реинит WiFi-стека ──────────────────────────
// Если соединение не восстанавливается дольше WIFI_HARD_REINIT_MS,
// выполняется WiFi.disconnect(true) + WiFi.begin() — полный сброс стека.
#define WIFI_HARD_REINIT_MS  (5UL * 60 * 1000)   // 5 минут без связи
#define WIFI_HARD_REINIT_INTERVAL_MS (2UL * 60 * 1000) // не чаще раза в 2 мин

static unsigned long wifiDisconnectedSince  = 0;   // millis() момента потери связи
static unsigned long lastHardReinitTime     = 0;   // millis() последнего хард-реинита
static int           hardReinitCount        = 0;   // статистика
// ────────────────────────────────────────────────────────

// Переменные актуальности данных DHT и очереди ошибок
static unsigned long lastDhtSuccessTime = 0;
static bool dhtSuccessValid = false;

#define MAX_QUEUED_ERRORS 16
#define ERROR_MSG_LEN     128
static char errorQueue[MAX_QUEUED_ERRORS][ERROR_MSG_LEN];
static int errorQueueCount = 0;

// Состояние настройки через Serial
enum SerialState {
  STATE_IDLE,
  STATE_WIFI_SSID,
  STATE_WIFI_PASS,
  STATE_ADMIN_PASS
};
static SerialState serialState = STATE_IDLE;
static char newSsid[65]          = "";
static char newPass[65]          = "";
static unsigned long lastSerialTime = 0;
static char serialInputBuffer[129] = "";

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
#define HISTORY_POINTS  1008                 // 24 ч × 6 точек/ч
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
  // Читаем строки как байты; если ключа нет — оставляем дефолт
  if (prefs.isKey("adminPw")) prefs.getBytes("adminPw", adminPassword, sizeof(adminPassword));
  else strncpy(adminPassword, "admin123", sizeof(adminPassword) - 1);
  if (prefs.isKey("wifiSSID")) prefs.getBytes("wifiSSID", wifiSSID, sizeof(wifiSSID));
  else strncpy(wifiSSID, WIFI_SSID, sizeof(wifiSSID) - 1);
  if (prefs.isKey("wifiPW"))   prefs.getBytes("wifiPW",   wifiPassword, sizeof(wifiPassword));
  else strncpy(wifiPassword, WIFI_PASSWORD, sizeof(wifiPassword) - 1);
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
  prefs.putBytes("adminPw",  adminPassword, strlen(adminPassword) + 1);
  prefs.putBytes("wifiSSID", wifiSSID,      strlen(wifiSSID)      + 1);
  prefs.putBytes("wifiPW",   wifiPassword,  strlen(wifiPassword)  + 1);
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
//  ▸ НАСТРОЙКИ УВЕДОМЛЕНИЙ (NVS namespace "notify")
//    Изменяются из async_tcp, читаются из loop() — доступ под dataMutex
// ════════════════════════════════════════════════════════
static NotifyConfig notifyCfg;

static void loadNotifyStr(const char* key, char* dst, size_t len) {
  if (prefs.isKey(key)) prefs.getString(key, dst, len);
}

void loadNotifySettings() {
  memset(&notifyCfg, 0, sizeof(notifyCfg));
  prefs.begin("notify", true);
  notifyCfg.tempEn    = prefs.getBool("tempEn", false);
  notifyCfg.tempMin   = prefs.getFloat("tempMin", 15.0f);
  notifyCfg.tempMax   = prefs.getFloat("tempMax", 30.0f);
  notifyCfg.humEn     = prefs.getBool("humEn", false);
  notifyCfg.humMin    = prefs.getFloat("humMin", 30.0f);
  notifyCfg.humMax    = prefs.getFloat("humMax", 70.0f);
  notifyCfg.repeatMin = prefs.getUShort("repeatMin", 60);
  notifyCfg.tgEn      = prefs.getBool("tgEn", false);
  loadNotifyStr("tgToken", notifyCfg.tgToken, sizeof(notifyCfg.tgToken));
  loadNotifyStr("tgChat",  notifyCfg.tgChatId, sizeof(notifyCfg.tgChatId));
  notifyCfg.mailEn    = prefs.getBool("mailEn", false);
  loadNotifyStr("smtpHost", notifyCfg.smtpHost, sizeof(notifyCfg.smtpHost));
  notifyCfg.smtpPort  = prefs.getUShort("smtpPort", 465);
  loadNotifyStr("smtpUser", notifyCfg.smtpUser, sizeof(notifyCfg.smtpUser));
  loadNotifyStr("smtpPass", notifyCfg.smtpPass, sizeof(notifyCfg.smtpPass));
  loadNotifyStr("mailTo",   notifyCfg.mailTo,   sizeof(notifyCfg.mailTo));
  prefs.end();
}

void saveNotifySettings(const NotifyConfig& c) {
  prefs.begin("notify", false);
  prefs.putBool("tempEn", c.tempEn);
  prefs.putFloat("tempMin", c.tempMin);
  prefs.putFloat("tempMax", c.tempMax);
  prefs.putBool("humEn", c.humEn);
  prefs.putFloat("humMin", c.humMin);
  prefs.putFloat("humMax", c.humMax);
  prefs.putUShort("repeatMin", c.repeatMin);
  prefs.putBool("tgEn", c.tgEn);
  prefs.putString("tgToken", c.tgToken);
  prefs.putString("tgChat", c.tgChatId);
  prefs.putBool("mailEn", c.mailEn);
  prefs.putString("smtpHost", c.smtpHost);
  prefs.putUShort("smtpPort", c.smtpPort);
  prefs.putString("smtpUser", c.smtpUser);
  prefs.putString("smtpPass", c.smtpPass);
  prefs.putString("mailTo", c.mailTo);
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
  // Сравниваем без heap-аллокаций: ищем подстроку "esp_sess=<token>" в заголовке Cookie
  const String& cookies = req->header("Cookie");
  char needle[64];
  snprintf(needle, sizeof(needle), "esp_sess=%s", sessionToken);
  return strstr(cookies.c_str(), needle) != nullptr;
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
static volatile uint32_t mutexTimeoutCount = 0;

static inline BaseType_t mutexTake() {
  BaseType_t r = xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50));
  if (r != pdTRUE) {
    mutexTimeoutCount++;
    // Serial потокобезопасен на ESP32 — логируем без мьютекса
    Serial.printf("[MUTEX] TIMEOUT #%lu ctx=%s\n",
                  (unsigned long)mutexTimeoutCount,
                  pcTaskGetName(nullptr));
  }
  return r;
}
#define MUTEX_TAKE()  mutexTake()
#define MUTEX_GIVE()  xSemaphoreGive(dataMutex)

static void pushError(const char* err) {
  if (MUTEX_TAKE() == pdTRUE) {
    if (errorQueueCount < MAX_QUEUED_ERRORS) {
      strncpy(errorQueue[errorQueueCount], err, ERROR_MSG_LEN - 1);
      errorQueue[errorQueueCount][ERROR_MSG_LEN - 1] = 0;
      errorQueueCount++;
    } else {
      // Сдвигаем влево при переполнении
      for (int i = 1; i < MAX_QUEUED_ERRORS; i++) {
        memcpy(errorQueue[i - 1], errorQueue[i], ERROR_MSG_LEN);
      }
      strncpy(errorQueue[MAX_QUEUED_ERRORS - 1], err, ERROR_MSG_LEN - 1);
      errorQueue[MAX_QUEUED_ERRORS - 1][ERROR_MSG_LEN - 1] = 0;
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
  
  // Переводим пин на OUTPUT HIGH для крутого фронта, затем INPUT_PULLUP
  pinMode(DHT_PIN, OUTPUT);
  digitalWrite(DHT_PIN, HIGH);
  delayMicroseconds(40);
  pinMode(DHT_PIN, INPUT_PULLUP);
  delayMicroseconds(10);
  
  // Входим в критическую секцию для точного замера длительности импульсов
  static portMUX_TYPE myMutex = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&myMutex);
  
  // Ожидание ответа датчика (линия должна уйти в LOW, затем в HIGH, затем снова в LOW)
  #define WaitPin(level, timeout_us) { \
    int64_t start = esp_timer_get_time(); \
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
    uint64_t startHigh = esp_timer_get_time();
    // Ждем окончания высокого уровня
    WaitPin(HIGH, 100);
    uint64_t duration = esp_timer_get_time() - startHigh;
    
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
  static char localErrors[MAX_QUEUED_ERRORS][ERROR_MSG_LEN];
  int localErrorCount = 0;

  if (MUTEX_TAKE() != pdTRUE) {
    strncpy(jsonBuf, "{\"error\":\"busy\"}", JSON_BUF_SIZE);
    return jsonBuf;
  }
  float lt = curTemp, lh = curHum;
  float vBatSnap = filteredBatVolts;  // снимок под мьютексом — защита от гонки с loop()
  
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
    memcpy(localErrors[i], errorQueue[i], ERROR_MSG_LEN);
  }
  errorQueueCount = 0;

  MUTEX_GIVE();

  float vBat = vBatSnap;
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
  APPEND_JSON("{\"unixSec\":%lu,\"uptime\":%lu,\"batVolts\":%.2f,\"batLevel\":%d,\"dhtError\":%s,\"wifiRecon\":%d,\"mutexTO\":%lu,\"errors\":[",
              (unsigned long)nowT, uptime, vBat, batLvl,
              lastDhtReadFailed ? "true" : "false", wifiReconnectCount,
              (unsigned long)mutexTimeoutCount);

  for (int i = 0; i < localErrorCount; i++) {
    // Экранируем кавычки в сообщении об ошибке без heap-аллокаций
    char escaped[ERROR_MSG_LEN * 2];
    int ei = 0;
    for (int k = 0; localErrors[i][k] && ei < (int)sizeof(escaped) - 2; k++) {
      if (localErrors[i][k] == '"' || localErrors[i][k] == '\\') escaped[ei++] = '\\';
      escaped[ei++] = localErrors[i][k];
    }
    escaped[ei] = 0;
    APPEND_JSON("%s\"%s\"", i == 0 ? "" : ",", escaped);
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
#define DEV_JSON_SIZE 2048
static char devJsonBuf[DEV_JSON_SIZE];

static const char* buildDevicesJson() {
  // Снимок devices[] под мьютексом — защита от гонки с async_tcp задачей
  static Device snapDevices[MAX_DEVICES];
  if (MUTEX_TAKE() == pdTRUE) {
    memcpy(snapDevices, devices, sizeof(devices));
    MUTEX_GIVE();
  } else {
    strncpy(devJsonBuf, "[]", DEV_JSON_SIZE);
    return devJsonBuf;
  }

  char* p = devJsonBuf, *end = devJsonBuf + DEV_JSON_SIZE - 1;
  APPEND_JSON("[");
  bool first = true;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!snapDevices[i].used) continue;
    // Экранируем имя: заменяем " на \"
    char safeName[DEV_NAME_LEN * 2];
    int si = 0;
    for (int k = 0; snapDevices[i].name[k] && si < (int)sizeof(safeName)-2; k++) {
      if (snapDevices[i].name[k] == '"') safeName[si++] = '\\';
      safeName[si++] = snapDevices[i].name[k];
    }
    safeName[si] = 0;
    APPEND_JSON("%s{\"id\":%d,\"name\":\"%s\",\"pin\":%d,\"state\":%s}",
                first ? "" : ",", i, safeName, snapDevices[i].pin,
                snapDevices[i].state ? "true" : "false");
    first = false;
  }
  APPEND_JSON("]");
  return devJsonBuf;
}

// ════════════════════════════════════════════════════════
//  ▸ УВЕДОМЛЕНИЯ — контроль границ и тестовая отправка
// ════════════════════════════════════════════════════════
#define TEMP_HYST 0.5f   // возврат в норму только после отхода от границы на гистерезис,
#define HUM_HYST  2.0f   // чтобы шум датчика у границы не порождал поток сообщений

static int8_t        tempAlertState = 0;   // -1 ниже нормы, 0 норма, 1 выше нормы
static int8_t        humAlertState  = 0;
static unsigned long lastAlertSent  = 0;

// Тестовая отправка: запрос ставит веб-обработчик, выполняет loop()
enum NotifyChannel : uint8_t { NOTIFY_CH_NONE = 0, NOTIFY_CH_TG = 1, NOTIFY_CH_MAIL = 2 };
enum NotifyTestState : uint8_t { TEST_IDLE, TEST_PENDING, TEST_OK, TEST_ERROR };
static volatile uint8_t notifyTestReq   = NOTIFY_CH_NONE;
static volatile uint8_t notifyTestState = TEST_IDLE;
static char             notifyTestMsg[160] = "";

static bool snapshotNotifyCfg(NotifyConfig& out) {
  if (MUTEX_TAKE() != pdTRUE) return false;
  out = notifyCfg;
  MUTEX_GIVE();
  return true;
}

static int8_t evalRange(float v, float lo, float hi, float hyst, int8_t prev) {
  if (v > hi) return 1;
  if (v < lo) return -1;
  if (prev == 1  && v > hi - hyst) return 1;
  if (prev == -1 && v < lo + hyst) return -1;
  return 0;
}

// Отправка во все включённые каналы; ошибки попадают в очередь ошибок веб-интерфейса
// (pushError обрезает сообщение до ERROR_MSG_LEN)
static void notifyAll(const NotifyConfig& c, const char* subject, const char* text) {
  char err[128];
  char msg[sizeof(err) + 64];
  if (c.tgEn && !sendTelegram(c, text, err, sizeof(err))) {
    Serial.printf("[Notify] Telegram: %s\n", err);
    snprintf(msg, sizeof(msg), "Уведомление Telegram не отправлено: %s", err);
    pushError(msg);
  }
  if (c.mailEn && !sendEmail(c, subject, text, err, sizeof(err))) {
    Serial.printf("[Notify] E-mail: %s\n", err);
    snprintf(msg, sizeof(msg), "Уведомление e-mail не отправлено: %s", err);
    pushError(msg);
  }
}

static const char* rangeWord(int8_t s) { return s > 0 ? "выше нормы" : "ниже нормы"; }

// Вызывается после каждого успешного чтения DHT22
static void checkAlerts(float t, float h) {
  NotifyConfig c;
  if (!snapshotNotifyCfg(c)) return;
  if (!c.tgEn && !c.mailEn) { tempAlertState = humAlertState = 0; return; }

  int8_t ts = c.tempEn ? evalRange(t, c.tempMin, c.tempMax, TEMP_HYST, tempAlertState) : 0;
  int8_t hs = c.humEn  ? evalRange(h, c.humMin,  c.humMax,  HUM_HYST,  humAlertState)  : 0;
  bool changed = (ts != tempAlertState) || (hs != humAlertState);
  bool remind  = !changed && (ts || hs) && c.repeatMin > 0 &&
                 millis() - lastAlertSent >= c.repeatMin * 60000UL;
  if (!changed && !remind) return;
  // Без сети состояние не фиксируем — попробуем при следующем опросе датчика
  if (WiFi.status() != WL_CONNECTED) return;

  char text[512];
  char* p = text, *end = text + sizeof(text) - 1;
  bool hasLines = false;
  APPEND_JSON("%s ESP32-C3 Monitor%s\n", (ts || hs) ? "⚠️" : "✅", remind ? " — напоминание" : "");
  if (ts) {
    APPEND_JSON("🌡 Температура %.1f °C — %s (%.1f…%.1f °C)\n", t, rangeWord(ts), c.tempMin, c.tempMax);
    hasLines = true;
  } else if (tempAlertState && c.tempEn) {
    APPEND_JSON("🌡 Температура %.1f °C — снова в норме\n", t);
    hasLines = true;
  }
  if (hs) {
    APPEND_JSON("💧 Влажность %.1f %% — %s (%.0f…%.0f %%)\n", h, rangeWord(hs), c.humMin, c.humMax);
    hasLines = true;
  } else if (humAlertState && c.humEn) {
    APPEND_JSON("💧 Влажность %.1f %% — снова в норме\n", h);
    hasLines = true;
  }
  APPEND_JSON("Сейчас: %.1f °C, %.1f %%", t, h);

  tempAlertState = ts;
  humAlertState  = hs;
  lastAlertSent  = millis();
  if (!hasLines) return;   // контроль параметра выключили — сообщать нечего

  const char* subject = (ts || hs) ? "ESP32-C3: выход за границы" : "ESP32-C3: показания в норме";
  Serial.printf("[Notify] %s\n", subject);
  notifyAll(c, subject, text);
}

static void setTestResult(uint8_t state, const char* msg) {
  if (MUTEX_TAKE() == pdTRUE) {
    strncpy(notifyTestMsg, msg, sizeof(notifyTestMsg) - 1);
    notifyTestMsg[sizeof(notifyTestMsg) - 1] = 0;
    MUTEX_GIVE();
  }
  notifyTestState = state;
}

// Выполнение запрошенной тестовой отправки (из loop)
static void processNotifyTest() {
  uint8_t ch = notifyTestReq;
  if (ch == NOTIFY_CH_NONE) return;
  notifyTestReq = NOTIFY_CH_NONE;

  NotifyConfig c;
  if (!snapshotNotifyCfg(c)) { setTestResult(TEST_ERROR, "Устройство занято, повторите попытку"); return; }
  if (WiFi.status() != WL_CONNECTED) { setTestResult(TEST_ERROR, "Нет подключения к WiFi"); return; }

  float lt = NAN, lh = NAN;
  if (MUTEX_TAKE() == pdTRUE) {
    if (dhtSuccessValid && millis() - lastDhtSuccessTime <= 60000) { lt = curTemp; lh = curHum; }
    MUTEX_GIVE();
  }
  char text[256];
  if (isnan(lt)) {
    snprintf(text, sizeof(text), "🔔 Тестовое уведомление ESP32-C3 Monitor\nДанные датчика пока недоступны.");
  } else {
    snprintf(text, sizeof(text), "🔔 Тестовое уведомление ESP32-C3 Monitor\n🌡 Температура: %.1f °C\n💧 Влажность: %.1f %%", lt, lh);
  }

  char err[128];
  bool ok = (ch == NOTIFY_CH_TG)
          ? sendTelegram(c, text, err, sizeof(err))
          : sendEmail(c, "ESP32-C3: тестовое уведомление", text, err, sizeof(err));
  const char* chName = (ch == NOTIFY_CH_TG) ? "Telegram" : "E-mail";
  char msg[sizeof(notifyTestMsg)];
  if (ok) snprintf(msg, sizeof(msg), "%s: тестовое сообщение отправлено", chName);
  else    snprintf(msg, sizeof(msg), "%s: %s", chName, err);
  Serial.printf("[Notify] Тест — %s\n", msg);
  setTestResult(ok ? TEST_OK : TEST_ERROR, msg);
}

// JSON настроек уведомлений (секреты не отдаются — только признак, что они заданы)
static char notifyJsonBuf[1024];
static const char* buildNotifyJson() {
  NotifyConfig c;
  if (!snapshotNotifyCfg(c)) return "{\"error\":\"busy\"}";
  char chat[sizeof(c.tgChatId) * 2], host[sizeof(c.smtpHost) * 2], user[sizeof(c.smtpUser) * 2], to[sizeof(c.mailTo) * 2];
  jsonEscape(chat, sizeof(chat), c.tgChatId);
  jsonEscape(host, sizeof(host), c.smtpHost);
  jsonEscape(user, sizeof(user), c.smtpUser);
  jsonEscape(to,   sizeof(to),   c.mailTo);
  snprintf(notifyJsonBuf, sizeof(notifyJsonBuf),
           "{\"tempEn\":%s,\"tempMin\":%.1f,\"tempMax\":%.1f,\"humEn\":%s,\"humMin\":%.1f,\"humMax\":%.1f,"
           "\"repeatMin\":%u,\"tgEn\":%s,\"tgTokenSet\":%s,\"tgChatId\":\"%s\","
           "\"mailEn\":%s,\"smtpHost\":\"%s\",\"smtpPort\":%u,\"smtpUser\":\"%s\",\"smtpPassSet\":%s,\"mailTo\":\"%s\"}",
           c.tempEn ? "true" : "false", c.tempMin, c.tempMax,
           c.humEn ? "true" : "false", c.humMin, c.humMax, c.repeatMin,
           c.tgEn ? "true" : "false", c.tgToken[0] ? "true" : "false", chat,
           c.mailEn ? "true" : "false", host, c.smtpPort, user, c.smtpPass[0] ? "true" : "false", to);
  return notifyJsonBuf;
}

// Копирует строковый POST-параметр с обрезкой пробелов; false — если длина превышает буфер
static bool copyParam(AsyncWebServerRequest* req, const char* name, char* dst, size_t len) {
  if (!req->hasParam(name, true)) return true;
  String v = req->getParam(name, true)->value();
  v.trim();
  if (v.length() >= len) return false;
  strncpy(dst, v.c_str(), len - 1);
  dst[len - 1] = 0;
  return true;
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
  loadNotifySettings();
  loadDevices();  // загружаем устройства из NVS и восстанавливаем состояние пинов

  // WiFi — один WiFi.begin(), ждём до 20 сек
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  Serial.printf("[WiFi] Подключение к: %s\n", wifiSSID);
  WiFi.begin(wifiSSID, wifiPassword);
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
      const String& pw = req->getParam("pw", true)->value();
      if (pw == adminPassword) {
        generateToken();
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
        char cookie[80];
        snprintf(cookie, sizeof(cookie), "esp_sess=%s; Path=/; HttpOnly", sessionToken);
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
    const String& nameStr = req->getParam("name",true)->value();
    int    pin   = req->getParam("pin",true)->value().toInt();
    bool   state = req->getParam("state",true)->value().toInt() != 0;

    // Валидация
    if (nameStr.length() == 0 || nameStr.length() > 31) {
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

    strncpy(devices[slot].name, nameStr.c_str(), DEV_NAME_LEN - 1);
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
      const String& newPwStr = req->getParam("newPassword", true)->value();
      // trim: находим начало и конец без пробелов (без heap-аллокации)
      int pStart = 0, pEnd = (int)newPwStr.length() - 1;
      while (pStart <= pEnd && newPwStr[pStart] == ' ') pStart++;
      while (pEnd >= pStart && newPwStr[pEnd] == ' ')  pEnd--;
      int pLen = pEnd - pStart + 1;
      if (pLen < 4 || pLen > 32) {
        req->send(200, "application/json", "{\"ok\":false,\"err\":\"Длина пароля должна быть от 4 до 32 символов\"}");
        return;
      }
      strncpy(adminPassword, newPwStr.c_str() + pStart, pLen);
      adminPassword[pLen] = 0;
      saveSettings();
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"missing newPassword\"}");
    }
  });

  // GET /api/notify/config — настройки уведомлений
  // (не "/api/notify": AsyncWebServer сопоставляет по префиксу и перехватил бы /api/notify/status)
  server.on("/api/notify/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    req->send(200, "application/json", buildNotifyJson());
  });

  // POST /api/notify/save — пустые tgToken/smtpPass оставляют сохранённые значения
  server.on("/api/notify/save", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    NotifyConfig c;
    if (!snapshotNotifyCfg(c)) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"Устройство занято, повторите попытку\"}"); return; }

    auto flag = [req](const char* n, bool& dst) {
      if (req->hasParam(n, true)) dst = req->getParam(n, true)->value().toInt() != 0;
    };
    auto num = [req](const char* n, float& dst) {
      if (req->hasParam(n, true)) dst = req->getParam(n, true)->value().toFloat();
    };
    flag("tempEn", c.tempEn);  num("tempMin", c.tempMin); num("tempMax", c.tempMax);
    flag("humEn",  c.humEn);   num("humMin",  c.humMin);  num("humMax",  c.humMax);
    flag("tgEn",   c.tgEn);
    flag("mailEn", c.mailEn);
    if (req->hasParam("repeatMin", true)) {
      long v = req->getParam("repeatMin", true)->value().toInt();
      if (v < 0 || v > 1440) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"Интервал повтора: 0–1440 мин\"}"); return; }
      c.repeatMin = (uint16_t)v;
    }
    if (req->hasParam("smtpPort", true)) {
      long v = req->getParam("smtpPort", true)->value().toInt();
      if (v < 1 || v > 65535) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"Некорректный порт SMTP\"}"); return; }
      c.smtpPort = (uint16_t)v;
    }

    char tgToken[sizeof(c.tgToken)] = "", smtpPass[sizeof(c.smtpPass)] = "";
    bool lenOk = copyParam(req, "tgToken",  tgToken,    sizeof(tgToken))
              && copyParam(req, "tgChatId", c.tgChatId, sizeof(c.tgChatId))
              && copyParam(req, "smtpHost", c.smtpHost, sizeof(c.smtpHost))
              && copyParam(req, "smtpUser", c.smtpUser, sizeof(c.smtpUser))
              && copyParam(req, "smtpPass", smtpPass,   sizeof(smtpPass))
              && copyParam(req, "mailTo",   c.mailTo,   sizeof(c.mailTo));
    if (!lenOk) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"Слишком длинное значение в настройках уведомлений\"}"); return; }
    if (tgToken[0])  strcpy(c.tgToken,  tgToken);
    if (smtpPass[0]) strcpy(c.smtpPass, smtpPass);

    if (c.tempMin >= c.tempMax || c.tempMin < -40 || c.tempMax > 80) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"Границы температуры: от -40 до 80 °C, минимум меньше максимума\"}"); return;
    }
    if (c.humMin >= c.humMax || c.humMin < 0 || c.humMax > 100) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"Границы влажности: от 0 до 100 %, минимум меньше максимума\"}"); return;
    }

    if (MUTEX_TAKE() != pdTRUE) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"Устройство занято, повторите попытку\"}"); return; }
    notifyCfg = c;
    MUTEX_GIVE();
    saveNotifySettings(c);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // POST /api/notify/test — ставит тестовую отправку в очередь (выполняется в loop)
  server.on("/api/notify/test", HTTP_POST, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    const char* ch = req->hasParam("ch", true) ? req->getParam("ch", true)->value().c_str() : "";
    uint8_t chan = strcmp(ch, "tg") == 0 ? NOTIFY_CH_TG : strcmp(ch, "mail") == 0 ? NOTIFY_CH_MAIL : NOTIFY_CH_NONE;
    if (chan == NOTIFY_CH_NONE) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"unknown channel\"}"); return; }
    if (notifyTestState == TEST_PENDING) { req->send(200, "application/json", "{\"ok\":false,\"err\":\"Предыдущая отправка ещё выполняется\"}"); return; }
    notifyTestState = TEST_PENDING;
    notifyTestReq   = chan;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // GET /api/notify/status — результат тестовой отправки
  server.on("/api/notify/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    blinkTx();
    if (!isAuthorized(req)) { req->send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
    static const char* names[] = {"idle", "pending", "ok", "error"};
    char msg[sizeof(notifyTestMsg) * 2] = "";
    if (MUTEX_TAKE() == pdTRUE) {
      jsonEscape(msg, sizeof(msg), notifyTestMsg);
      MUTEX_GIVE();
    }
    char buf[sizeof(msg) + 48];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"msg\":\"%s\"}", names[notifyTestState], msg);
    req->send(200, "application/json", buf);
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

static void processSerialCommand(char* cmd) {
  // trim in-place
  int len = strlen(cmd);
  while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\r' || cmd[len-1] == '\n')) cmd[--len] = 0;
  char* start = cmd;
  while (*start == ' ') start++;
  if (*start == 0) return;

  if (serialState == STATE_IDLE) {
    if (strcasecmp(start, "help") == 0) {
      Serial.println("\n=== Доступные команды ===");
      Serial.println("  wifi     - Настройка подключения к WiFi");
      Serial.println("  password - Изменение пароля администратора");
      Serial.println("  status   - Текущий статус системы");
      Serial.println("  help     - Показать это сообщение");
    } else if (strcasecmp(start, "wifi") == 0) {
      Serial.println("\n[WiFi] Введите SSID сети:");
      serialState = STATE_WIFI_SSID;
    } else if (strcasecmp(start, "password") == 0) {
      Serial.println("\n[Admin] Введите новый пароль администратора (от 4 до 32 символов):");
      serialState = STATE_ADMIN_PASS;
    } else if (strcasecmp(start, "status") == 0) {
      Serial.println("\n=== Статус системы ===");
      Serial.printf("  WiFi SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("  IP адрес: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("  WiFi статус: %s\n", WiFi.status() == WL_CONNECTED ? "Подключен" : "Отключен");
      Serial.printf("  Реконнектов: %d, хард-реинитов: %d\n", wifiReconnectCount, hardReinitCount);
      Serial.printf("  Таймаутов мьютекса: %lu\n", (unsigned long)mutexTimeoutCount);
      if (WiFi.status() != WL_CONNECTED && wifiDisconnectedSince > 0) {
        Serial.printf("  Нет связи: %lu сек\n", (millis() - wifiDisconnectedSince) / 1000UL);
      }
      Serial.printf("  Температура: %.1f °C\n", curTemp);
      Serial.printf("  Влажность: %.1f %%\n", curHum);
      float vBat = filteredBatVolts;  // читаем из основного потока, гонки нет
      if (vBat >= 0.5f) {
        Serial.printf("  Батарея: %.2f В\n", vBat);
      } else {
        Serial.println("  Батарея: нет сигнала");
      }
    } else {
      Serial.println("Неизвестная команда. Введите 'help' для списка команд.");
    }
  } else if (serialState == STATE_WIFI_SSID) {
    strncpy(newSsid, start, sizeof(newSsid) - 1);
    newSsid[sizeof(newSsid) - 1] = 0;
    Serial.println("[WiFi] Введите пароль сети:");
    serialState = STATE_WIFI_PASS;
  } else if (serialState == STATE_WIFI_PASS) {
    strncpy(newPass, start, sizeof(newPass) - 1);
    newPass[sizeof(newPass) - 1] = 0;
    Serial.printf("[WiFi] Подключение к '%s' с паролем '%s'...\n", newSsid, newPass);

    strncpy(wifiSSID,     newSsid, sizeof(wifiSSID)     - 1); wifiSSID[sizeof(wifiSSID)-1] = 0;
    strncpy(wifiPassword, newPass, sizeof(wifiPassword) - 1); wifiPassword[sizeof(wifiPassword)-1] = 0;
    saveSettings();

    // Переподключаемся
    WiFi.disconnect();
    WiFi.begin(wifiSSID, wifiPassword);

    // Сбрасываем счетчик попыток подключения
    wifiAttemptCount = 1;
    lastWifiAttemptTime = millis();

    serialState = STATE_IDLE;
  } else if (serialState == STATE_ADMIN_PASS) {
    int plen = strlen(start);
    if (plen < 4 || plen > 32) {
      Serial.println("[Ошибка] Пароль должен быть от 4 до 32 символов. Введите заново:");
    } else {
      strncpy(adminPassword, start, sizeof(adminPassword) - 1);
      adminPassword[sizeof(adminPassword) - 1] = 0;
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
  static bool firstDhtRead = true;
  if (firstDhtRead || now - lastDht >= DHT_READ_MS) {
    firstDhtRead = false;
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
        // Неблокирующая пауза 2 с: уступаем CPU, не блокируем TWDT
        unsigned long pauseStart = millis();
        while (millis() - pauseStart < 2000UL) {
          delay(1);
        }
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
      checkAlerts(t, h);
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

  // 4. Опрос батареи раз в 60 сек
  static unsigned long lastBatteryRead = 0;
  if (filteredBatVolts < 0.0f || now - lastBatteryRead >= 60000) {
    lastBatteryRead = now;
    float v = getBatteryVoltage();
    // Атомарно обновляем под мьютексом, чтобы исключить гонку с HTTP-задачей
    if (MUTEX_TAKE() == pdTRUE) {
      filteredBatVolts = v;
      MUTEX_GIVE();
    }
  }

  // 5. WiFi Reconnection & status LED
  if (WiFi.status() == WL_CONNECTED) {
    // Сбрасываем таймер отключения при успешном коннекте
    wifiDisconnectedSince = 0;

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
    // Фиксируем момент начала отключения
    if (wasConnected) {
      wasConnected = false;
      wifiDisconnectedSince = now;
      pushError("Потеряно соединение с WiFi!");
    }
    if (wifiDisconnectedSince == 0) {
      wifiDisconnectedSince = now;   // инициализация при старте без связи
    }

    // ── Хардовый реинит WiFi-стека ──────────────────────
    // Если обычные WiFi.begin() не помогают дольше WIFI_HARD_REINIT_MS,
    // сбрасываем весь стек: disconnect(true) очищает внутренние структуры
    // драйвера, после чего WiFi.begin() стартует с чистого листа.
    unsigned long disconnectedFor = now - wifiDisconnectedSince;
    bool hardReinitDue =
        (disconnectedFor >= WIFI_HARD_REINIT_MS) &&
        (lastHardReinitTime == 0 ||
         now - lastHardReinitTime >= WIFI_HARD_REINIT_INTERVAL_MS);

    if (hardReinitDue) {
      hardReinitCount++;
      lastHardReinitTime    = now;
      lastWifiAttemptTime   = now;   // не даём мягкому реконнекту сразу перебить
      wifiAttemptCount      = 1;

      Serial.printf("\n[WiFi] ХАРД-РЕИНИТ #%d — нет связи %lu сек, сброс стека...\n",
                    hardReinitCount, disconnectedFor / 1000UL);

      // Полный сброс: wifiOff убирает и внутренние структуры драйвера
      WiFi.disconnect(true /*wifioff*/);
      delay(200);                    // даём стеку время освободить ресурсы
      WiFi.mode(WIFI_STA);
      delay(100);
      WiFi.begin(wifiSSID, wifiPassword);

      pushError("WiFi: выполнен хардовый реинит стека.");
      Serial.println("[WiFi] Хард-реинит выполнен, ожидаем подключения...");

    } else {
      // Обычный мягкий реконнект: раз в 10 с (первые 6 попыток), затем раз в 60 с
      unsigned long interval = (wifiAttemptCount < 6) ? 10000UL : 60000UL;
      if (lastWifiAttemptTime == 0 || now - lastWifiAttemptTime >= interval) {
        wifiAttemptCount++;
        lastWifiAttemptTime = now;
        Serial.printf("\n[WiFi] Попытка подключения %d... (SSID: %s, нет связи %lu сек)\n",
                      wifiAttemptCount, wifiSSID, disconnectedFor / 1000UL);
        WiFi.disconnect();
        WiFi.begin(wifiSSID, wifiPassword);
      }
    }
    // ────────────────────────────────────────────────────

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
      if (serialInputBuffer[0] != 0) {
        processSerialCommand(serialInputBuffer);
        serialInputBuffer[0] = 0;
      }
    } else {
      int slen = strlen(serialInputBuffer);
      if (slen < 128) {
        serialInputBuffer[slen]     = c;
        serialInputBuffer[slen + 1] = 0;
      }
    }
  }

  // Сброс состояния Serial при неактивности (таймаут 1 минута)
  if (serialState != STATE_IDLE && now - lastSerialTime > 60000) {
    Serial.println("\n[Serial] Тайм-аут настройки. Возврат в обычный режим.");
    serialState = STATE_IDLE;
    serialInputBuffer[0] = 0;
  }

  // 7. Тестовая отправка уведомления, запрошенная из веб-интерфейса
  processNotifyTest();

  // 8. Уступаем CPU (TWDT)
  delay(1);
}

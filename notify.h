#pragma once
// ════════════════════════════════════════════════════════
//  ▸ УВЕДОМЛЕНИЯ: Telegram (Bot API) и E-mail (SMTP)
//
//  Функции отправки блокирующие (TLS-рукопожатие 1–3 сек),
//  поэтому вызываются только из loop(), не из обработчиков
//  AsyncWebServer.
//
//  Сертификаты серверов не проверяются (setInsecure): в прошивке
//  нет хранилища корневых CA. Трафик шифруется, но подмена
//  сервера в локальной сети теоретически возможна.
// ════════════════════════════════════════════════════════
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/base64.h>
#include <time.h>

#define NOTIFY_TIMEOUT_MS 10000

struct NotifyConfig {
  bool     tempEn;              // контроль температуры
  float    tempMin, tempMax;
  bool     humEn;               // контроль влажности
  float    humMin, humMax;
  uint16_t repeatMin;           // повтор напоминания, мин (0 — не повторять)

  bool     tgEn;
  char     tgToken[64];
  char     tgChatId[40];

  bool     mailEn;
  char     smtpHost[64];
  uint16_t smtpPort;            // 465 — SSL/TLS, иначе STARTTLS (обычно 587)
  char     smtpUser[64];
  char     smtpPass[64];
  char     mailTo[128];         // один или несколько адресов через запятую
};

// Экранирование строки для JSON (кавычки, обратный слеш, управляющие символы)
static void jsonEscape(char* dst, size_t dstLen, const char* src) {
  size_t di = 0;
  for (; *src && di + 7 < dstLen; src++) {
    char c = *src;
    if (c == '"' || c == '\\') { dst[di++] = '\\'; dst[di++] = c; }
    else if (c == '\n')        { dst[di++] = '\\'; dst[di++] = 'n'; }
    else if (c == '\r')        { dst[di++] = '\\'; dst[di++] = 'r'; }
    else if (c == '\t')        { dst[di++] = '\\'; dst[di++] = 't'; }
    else if ((uint8_t)c < 0x20) di += snprintf(dst + di, dstLen - di, "\\u%04x", c);
    else                        dst[di++] = c;
  }
  dst[di] = 0;
}

// ────────────────────────────────────────────────────────
//  Telegram
// ────────────────────────────────────────────────────────
static bool sendTelegram(const NotifyConfig& c, const char* text, char* err, size_t errLen) {
  if (!c.tgToken[0] || !c.tgChatId[0]) {
    snprintf(err, errLen, "не задан токен бота или chat_id");
    return false;
  }

  static char body[1200];
  static char escText[1100];
  char escChat[64];
  jsonEscape(escText, sizeof(escText), text);
  jsonEscape(escChat, sizeof(escChat), c.tgChatId);
  snprintf(body, sizeof(body), "{\"chat_id\":\"%s\",\"text\":\"%s\"}", escChat, escText);

  char url[128];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", c.tgToken);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(NOTIFY_TIMEOUT_MS / 1000);
  HTTPClient http;
  http.setConnectTimeout(NOTIFY_TIMEOUT_MS);
  http.setTimeout(NOTIFY_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    snprintf(err, errLen, "не удалось инициализировать HTTPS");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)body, strlen(body));
  bool ok = (code == 200);
  if (!ok) {
    if (code < 0) {
      snprintf(err, errLen, "ошибка соединения: %s", HTTPClient::errorToString(code).c_str());
    } else {
      // Telegram возвращает {"ok":false,"error_code":...,"description":"..."}
      String resp = http.getString();
      int d = resp.indexOf("\"description\":\"");
      if (d >= 0) {
        d += 15;
        int e = resp.indexOf('"', d);
        snprintf(err, errLen, "HTTP %d: %s", code, resp.substring(d, e < 0 ? resp.length() : e).c_str());
      } else {
        snprintf(err, errLen, "HTTP %d", code);
      }
    }
  }
  http.end();
  return ok;
}

// ────────────────────────────────────────────────────────
//  E-mail (SMTP + AUTH LOGIN)
// ────────────────────────────────────────────────────────

// Читает ответ SMTP (включая многострочный) и проверяет код
static bool smtpExpect(WiFiClientSecure& cl, int expect, char* err, size_t errLen) {
  char line[160];
  for (;;) {
    size_t n = 0;
    unsigned long t0 = millis();
    for (;;) {
      if (cl.available()) {
        char ch = cl.read();
        if (ch == '\n') break;
        if (ch != '\r' && n < sizeof(line) - 1) line[n++] = ch;
      } else {
        if (!cl.connected()) { snprintf(err, errLen, "SMTP: соединение закрыто сервером"); return false; }
        if (millis() - t0 > NOTIFY_TIMEOUT_MS) { snprintf(err, errLen, "SMTP: нет ответа от сервера"); return false; }
        delay(5);
      }
    }
    line[n] = 0;
    if (n >= 4 && line[3] == '-') continue;   // промежуточная строка многострочного ответа
    if (atoi(line) != expect) {
      snprintf(err, errLen, "SMTP: %s", line);
      return false;
    }
    return true;
  }
}

static bool smtpCmd(WiFiClientSecure& cl, const char* cmd, int expect, char* err, size_t errLen) {
  cl.print(cmd);
  cl.print("\r\n");
  return smtpExpect(cl, expect, err, errLen);
}

static void base64Encode(char* dst, size_t dstLen, const char* src, size_t srcLen) {
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char*)dst, dstLen, &olen, (const unsigned char*)src, srcLen) != 0) olen = 0;
  dst[olen] = 0;
}

static bool sendEmail(const NotifyConfig& c, const char* subject, const char* text, char* err, size_t errLen) {
  if (!c.smtpHost[0] || !c.smtpUser[0] || !c.smtpPass[0] || !c.mailTo[0]) {
    snprintf(err, errLen, "заполните сервер, логин, пароль и получателя");
    return false;
  }

  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setHandshakeTimeout(NOTIFY_TIMEOUT_MS / 1000);
  bool implicitTls = (c.smtpPort == 465);
  if (!implicitTls) cl.setPlainStart();
  if (!cl.connect(c.smtpHost, c.smtpPort, NOTIFY_TIMEOUT_MS)) {
    snprintf(err, errLen, "не удалось подключиться к %s:%u", c.smtpHost, c.smtpPort);
    return false;
  }

  char buf[200];
  bool ok = smtpExpect(cl, 220, err, errLen)
         && smtpCmd(cl, "EHLO esp32c3", 250, err, errLen);
  if (ok && !implicitTls) {
    ok = smtpCmd(cl, "STARTTLS", 220, err, errLen);
    if (ok && !cl.startTLS()) {
      snprintf(err, errLen, "SMTP: ошибка TLS-рукопожатия (STARTTLS)");
      ok = false;
    }
    if (ok) ok = smtpCmd(cl, "EHLO esp32c3", 250, err, errLen);
  }
  if (ok) ok = smtpCmd(cl, "AUTH LOGIN", 334, err, errLen);
  if (ok) {
    base64Encode(buf, sizeof(buf), c.smtpUser, strlen(c.smtpUser));
    ok = smtpCmd(cl, buf, 334, err, errLen);
  }
  if (ok) {
    base64Encode(buf, sizeof(buf), c.smtpPass, strlen(c.smtpPass));
    ok = smtpCmd(cl, buf, 235, err, errLen);
    if (!ok) strncat(err, " (проверьте логин/пароль приложения)", errLen - strlen(err) - 1);
  }
  if (ok) {
    snprintf(buf, sizeof(buf), "MAIL FROM:<%s>", c.smtpUser);
    ok = smtpCmd(cl, buf, 250, err, errLen);
  }

  // Получатели через запятую / точку с запятой / пробел
  char to[sizeof(c.mailTo)];
  strncpy(to, c.mailTo, sizeof(to) - 1);
  to[sizeof(to) - 1] = 0;
  char toHeader[sizeof(c.mailTo) + 16] = "";
  char* save = nullptr;
  for (char* addr = strtok_r(to, ",; ", &save); ok && addr; addr = strtok_r(nullptr, ",; ", &save)) {
    snprintf(buf, sizeof(buf), "RCPT TO:<%s>", addr);
    ok = smtpCmd(cl, buf, 250, err, errLen);
    if (toHeader[0]) strncat(toHeader, ", ", sizeof(toHeader) - strlen(toHeader) - 1);
    strncat(toHeader, addr, sizeof(toHeader) - strlen(toHeader) - 1);
  }

  if (ok) ok = smtpCmd(cl, "DATA", 354, err, errLen);
  if (ok) {
    char encSubj[256];
    base64Encode(encSubj, sizeof(encSubj), subject, strlen(subject));
    cl.printf("From: \"ESP32-C3 Monitor\" <%s>\r\n", c.smtpUser);
    cl.printf("To: %s\r\n", toHeader);
    cl.printf("Subject: =?UTF-8?B?%s?=\r\n", encSubj);
    time_t now; time(&now);
    if (now > 1704067200) {
      struct tm tmUtc;
      gmtime_r(&now, &tmUtc);
      strftime(buf, sizeof(buf), "Date: %a, %d %b %Y %H:%M:%S +0000\r\n", &tmUtc);
      cl.print(buf);
    }
    cl.print("MIME-Version: 1.0\r\n"
             "Content-Type: text/plain; charset=UTF-8\r\n"
             "Content-Transfer-Encoding: base64\r\n\r\n");
    // Тело в base64 (строки по 76 символов) — без проблем с кириллицей и точками в начале строки
    static char encBody[1500];
    base64Encode(encBody, sizeof(encBody), text, strlen(text));
    size_t len = strlen(encBody);
    for (size_t i = 0; i < len; i += 76) {
      cl.write((const uint8_t*)encBody + i, (len - i) < 76 ? (len - i) : 76);
      cl.print("\r\n");
    }
    ok = smtpCmd(cl, ".", 250, err, errLen);
  }
  if (ok) smtpCmd(cl, "QUIT", 221, buf, sizeof(buf));  // ответ на QUIT не важен
  cl.stop();
  return ok;
}

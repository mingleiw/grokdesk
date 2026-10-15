// GrokDesk on the Waveshare ESP32-S3-Touch-LCD-4.3B.
// Polls the same status.json contract as the browser simulator.
//
//   state       healthy | failed | resolved | analyzing | anything else
//   service     service name
//   summary     status line, or the Grok Bot explanation when failed
//   fix         suggested fix (may be empty)
//   run_url     CI run URL (serial, and a short line on the failed screen)
//   updated_at  ISO-8601 timestamp
//
// Wi-Fi credentials are stored in NVS by WiFiManager. A 3s touch resets them.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Arduino_GFX_Library.h>
#include <time.h>

#include "board_config.h"
#include "panel.h"

struct DeskStatus {
  String state;
  String service;
  String summary;
  String fix;
  String runUrl;
  String updated;
  bool ok = false;
};

static WiFiManager wm;
static DeskStatus status;
static bool portalUp = false;
static bool wifiUp = false;
static bool stale = true;
static String staleReason = "waiting";
static bool restartPending = false;
static uint32_t lastPoll = 0;
static bool polledOnce = false;
static char clockText[8] = "--:--";
static char clockDrawn[8] = "";

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// Simulator colors: healthy #0d8a3a / #eaffef, failed #c4121c, explain #0b0d12,
// title #ff5a5a, Grok Bot #7fd0ff, suggest #5dff9a, unreachable #243044 / #d5e4ff.
static const uint16_t COL_OK = rgb565(0x0d, 0x8a, 0x3a);
static const uint16_t COL_OK_TEXT = rgb565(0xea, 0xff, 0xef);
static const uint16_t COL_FAIL = rgb565(0xc4, 0x12, 0x1c);
static const uint16_t COL_DARK = rgb565(0x0b, 0x0d, 0x12);
static const uint16_t COL_TITLE_RED = rgb565(0xff, 0x5a, 0x5a);
static const uint16_t COL_GROK = rgb565(0x7f, 0xd0, 0xff);
static const uint16_t COL_FIX = rgb565(0x5d, 0xff, 0x9a);
static const uint16_t COL_ERR = rgb565(0x24, 0x30, 0x44);
static const uint16_t COL_ERR_TEXT = rgb565(0xd5, 0xe4, 0xff);
static const uint16_t COL_AMBER = rgb565(0xff, 0xcc, 0x00);
static const uint16_t COL_WHITE = rgb565(0xff, 0xff, 0xff);
static const uint16_t COL_DIM = rgb565(0x9a, 0xa4, 0xb0);

static uint16_t darken(uint16_t c) {
  const uint8_t r = ((c >> 11) & 0x1F) >> 1;
  const uint8_t g = ((c >> 5) & 0x3F) >> 1;
  const uint8_t b = (c & 0x1F) >> 1;
  return (r << 11) | (g << 5) | b;
}

static int charWidth(int size) { return 6 * size; }

static void drawCentered(const char *text, int y, int size, uint16_t color) {
  Arduino_GFX *gfx = panelGfx();
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setTextWrap(false);
  const int w = static_cast<int>(strlen(text)) * charWidth(size);
  int x = (LCD_W - w) / 2;
  if (x < 12) x = 12;
  gfx->setCursor(x, y);
  gfx->print(text);
}

// Returns the y of the next line. Stops drawing past maxY but still advances.
static int drawWrapped(const String &text, int x, int y, int maxWidth, int size, uint16_t color, int maxY) {
  Arduino_GFX *gfx = panelGfx();
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setTextWrap(false);
  const int maxChars = max(8, maxWidth / charWidth(size));
  const int lineH = 8 * size + 8;
  String line;
  String word;
  auto emit = [&]() {
    if (y <= maxY && line.length()) {
      gfx->setCursor(x, y);
      gfx->print(line);
    }
    if (line.length()) y += lineH;
    line = "";
  };
  for (unsigned i = 0; i <= text.length(); i++) {
    const char c = (i == text.length()) ? ' ' : text[i];
    if (c == ' ' || c == '\n') {
      if (word.length() &&
          static_cast<int>(line.length()) + (line.length() ? 1 : 0) + static_cast<int>(word.length()) > maxChars) {
        emit();
      }
      while (static_cast<int>(word.length()) > maxChars) {
        int room = maxChars - static_cast<int>(line.length());
        if (room <= 1) {
          emit();
          room = maxChars;
        }
        line += word.substring(0, room);
        word.remove(0, room);
        emit();
      }
      if (word.length()) {
        if (line.length()) line += ' ';
        line += word;
        word = "";
      }
      if (c == '\n') emit();
    } else {
      word += c;
    }
  }
  emit();
  return y;
}

static String hhmmFromIso(const String &iso) {
  const int t = iso.indexOf('T');
  if (t < 0 || static_cast<int>(iso.length()) < t + 6) return "--:--";
  return iso.substring(t + 1, t + 6);
}

static void drawChrome(uint16_t bg, uint16_t fg, const char *footRight) {
  Arduino_GFX *gfx = panelGfx();
  const uint16_t bar = darken(bg);
  gfx->fillRect(0, 0, LCD_W, 48, bar);
  gfx->fillRect(0, LCD_H - 44, LCD_W, 44, bar);
  gfx->setTextSize(2);
  gfx->setTextWrap(false);
  gfx->setTextColor(fg);
  gfx->setCursor(16, 16);
  gfx->print(clockText);
  drawCentered("GROKDESK", 16, 2, fg);
  const char *wifi = wifiUp ? "WiFi" : "no WiFi";
  const uint16_t wifiColor = wifiUp ? fg : COL_AMBER;
  const int ww = static_cast<int>(strlen(wifi)) * charWidth(2);
  gfx->setTextColor(wifiColor);
  gfx->setCursor(LCD_W - 16 - ww, 16);
  gfx->print(wifi);

  gfx->setTextColor(fg);
  gfx->setCursor(16, LCD_H - 30);
  gfx->print("grokdesk");
  String right = footRight ? footRight : "";
  if (stale) {
    if (right.length()) right += "  ";
    right += "STALE";
  }
  const int rw = static_cast<int>(right.length()) * charWidth(2);
  gfx->setTextColor(stale ? COL_AMBER : fg);
  gfx->setCursor(LCD_W - 16 - rw, LCD_H - 30);
  gfx->print(right);
}

static void render() {
  Arduino_GFX *gfx = panelGfx();
  if (!gfx) return;
  gfx->setTextWrap(false);

  if (portalUp) {
    gfx->fillScreen(COL_DARK);
    drawChrome(COL_DARK, COL_WHITE, "");
    drawCentered("WI-FI SETUP", 120, 5, COL_WHITE);
    drawCentered("Join this network", 210, 3, COL_DIM);
    drawCentered(AP_NAME, 260, 4, COL_FIX);
    drawCentered("Open the page it offers.", 330, 3, COL_WHITE);
    drawCentered("Hold the screen 3s to reset Wi-Fi.", 390, 2, COL_DIM);
  } else if (!wifiUp && !status.ok) {
    gfx->fillScreen(COL_ERR);
    drawChrome(COL_ERR, COL_ERR_TEXT, "");
    drawCentered("CONNECTING", 170, 5, COL_ERR_TEXT);
    drawCentered("Looking for saved Wi-Fi", 260, 3, COL_WHITE);
  } else if (!status.ok) {
    gfx->fillScreen(COL_ERR);
    drawChrome(COL_ERR, COL_ERR_TEXT, "");
    drawCentered("OFFLINE", 150, 6, COL_ERR_TEXT);
    drawWrapped(staleReason.length() ? staleReason : "status.json unreachable", 40, 250, LCD_W - 80, 3, COL_WHITE,
                400);
  } else if (status.state == "healthy") {
    gfx->fillScreen(COL_OK);
    const String sub = status.summary.length() ? status.summary : status.service;
    const String foot = String("upd ") + hhmmFromIso(status.updated);
    drawChrome(COL_OK, COL_OK_TEXT, foot.c_str());
    drawCentered("DEPLOY HEALTHY", 150, 5, COL_OK_TEXT);
    if (sub.length() * charWidth(3) < LCD_W - 48) drawCentered(sub.c_str(), 250, 3, COL_OK_TEXT);
    else drawWrapped(sub, 32, 240, LCD_W - 64, 3, COL_OK_TEXT, 400);
  } else if (status.state == "resolved") {
    gfx->fillScreen(COL_OK);
    const String sub = status.summary.length() ? status.summary : (status.service.length() ? status.service : "Fixed");
    const String foot = String("upd ") + hhmmFromIso(status.updated);
    drawChrome(COL_OK, COL_OK_TEXT, foot.c_str());
    drawCentered("RESOLVED", 150, 6, COL_OK_TEXT);
    if (sub.length() * charWidth(3) < LCD_W - 48) drawCentered(sub.c_str(), 270, 3, COL_OK_TEXT);
    else drawWrapped(sub, 32, 260, LCD_W - 64, 3, COL_OK_TEXT, 400);
  } else if (status.state == "analyzing") {
    gfx->fillScreen(COL_DARK);
    const String foot = String("upd ") + hhmmFromIso(status.updated);
    drawChrome(COL_DARK, COL_WHITE, foot.c_str());
    drawCentered("Grok Bot analyzing...", 140, 4, COL_WHITE);
    const String sub = status.summary.length() ? status.summary : status.service;
    drawWrapped(sub, 32, 230, LCD_W - 64, 3, COL_GROK, 400);
  } else if (status.state == "failed" && (status.summary.length() || status.fix.length())) {
    gfx->fillScreen(COL_DARK);
    const String foot = String("upd ") + hhmmFromIso(status.updated);
    drawChrome(COL_DARK, COL_WHITE, foot.c_str());
    gfx->setTextSize(5);
    gfx->setTextColor(COL_TITLE_RED);
    gfx->setCursor(28, 70);
    gfx->print("DEPLOY FAILED");
    int y = 140;
    if (status.service.length()) {
      gfx->setTextSize(3);
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(28, y);
      gfx->print(status.service);
      y += 40;
    }
    gfx->setTextSize(3);
    gfx->setTextColor(COL_GROK);
    gfx->setCursor(28, y);
    gfx->print("Grok Bot:");
    y += 36;
    if (status.summary.length()) y = drawWrapped(status.summary, 28, y, LCD_W - 56, 3, COL_WHITE, 360);
    if (status.fix.length()) {
      y += 6;
      gfx->setTextSize(3);
      gfx->setTextColor(COL_FIX);
      gfx->setCursor(28, y);
      gfx->print("Suggest:");
      y += 36;
      y = drawWrapped(status.fix, 28, y, LCD_W - 56, 3, COL_FIX, 400);
    }
    if (status.runUrl.length() && y < 400) {
      String url = status.runUrl;
      if (url.length() > 62) url = url.substring(url.length() - 62);
      gfx->setTextSize(2);
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(28, min(y + 8, 404));
      gfx->print(url);
    }
  } else if (status.state == "failed") {
    gfx->fillScreen(COL_FAIL);
    const String foot = String("upd ") + hhmmFromIso(status.updated);
    drawChrome(COL_FAIL, COL_WHITE, foot.c_str());
    drawCentered("DEPLOY FAILED", 160, 5, COL_WHITE);
    if (status.service.length()) drawCentered(status.service.c_str(), 250, 3, COL_WHITE);
  } else {
    gfx->fillScreen(COL_ERR);
    const String foot = String("upd ") + hhmmFromIso(status.updated);
    drawChrome(COL_ERR, COL_ERR_TEXT, foot.c_str());
    drawCentered("UNKNOWN STATE", 160, 4, COL_ERR_TEXT);
    drawCentered(status.state.length() ? status.state.c_str() : "(missing)", 240, 3, COL_WHITE);
  }
  gfx->flush();
  strncpy(clockDrawn, clockText, sizeof(clockDrawn) - 1);
  clockDrawn[sizeof(clockDrawn) - 1] = '\0';
}

static void drawHold(float fraction) {
  Arduino_GFX *gfx = panelGfx();
  if (!gfx || fraction <= 0) return;
  gfx->fillRect(0, LCD_H - 78, LCD_W, 34, 0x0000);
  gfx->setTextSize(2);
  gfx->setTextColor(COL_AMBER);
  gfx->setCursor(24, LCD_H - 72);
  gfx->print("Hold to reset Wi-Fi");
  const int full = LCD_W - 48;
  gfx->fillRect(24, LCD_H - 22, full, 10, 0x2104);
  gfx->fillRect(24, LCD_H - 22, static_cast<int>(full * fraction), 10, COL_AMBER);
  gfx->flush();
}

static void refreshClock() {
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    strncpy(clockText, "--:--", sizeof(clockText));
    return;
  }
  struct tm t;
  gmtime_r(&now, &t);
  snprintf(clockText, sizeof(clockText), "%02d:%02d", t.tm_hour, t.tm_min);
}

static void markStale(const String &reason) {
  stale = true;
  staleReason = reason;
  Serial.printf("stale: %s\n", reason.c_str());
}

static void pollStatus() {
  lastPoll = millis();
  polledOnce = true;
  if (WiFi.status() != WL_CONNECTED) {
    wifiUp = false;
    markStale("no Wi-Fi");
    render();
    return;
  }
  wifiUp = true;

  String url = STATUS_URL;
  url += (url.indexOf('?') >= 0) ? "&t=" : "?t=";
  url += String(millis());

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  const bool https = url.startsWith("https://");
  bool begun = false;
  if (https) {
    secure.setInsecure();
    secure.setHandshakeTimeout(30);
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(plain, url);
  }
  if (!begun) {
    markStale("HTTP begin failed");
    render();
    return;
  }
  http.setTimeout(12000);
  http.setConnectTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");
  http.addHeader("User-Agent", "GrokDesk/1.0");
  const int code = http.GET();
  Serial.printf("GET %s -> %d\n", url.c_str(), code);
  if (code != 200) {
    markStale(String("HTTP ") + code);
    http.end();
    render();
    return;
  }
  const String body = http.getString();
  http.end();
  if (body.length() == 0 || body.length() > 8192) {
    markStale("empty status");
    render();
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    markStale(String("bad json: ") + err.c_str());
    render();
    return;
  }

  DeskStatus next;
  next.state = String(doc["state"] | "");
  next.service = String(doc["service"] | "");
  next.summary = String(doc["summary"] | "");
  next.fix = String(doc["fix"] | "");
  next.runUrl = String(doc["run_url"] | "");
  next.updated = String(doc["updated_at"] | "");
  next.ok = true;
  next.state.trim();
  next.service.trim();
  next.summary.trim();
  next.fix.trim();

  const bool changed = !status.ok || next.state != status.state || next.summary != status.summary ||
                       next.fix != status.fix || next.updated != status.updated || next.service != status.service ||
                       stale;
  status = next;
  stale = false;
  staleReason = "";
  Serial.printf("state=%s service=%s updated_at=%s\n", status.state.c_str(), status.service.c_str(),
                status.updated.c_str());
  if (status.runUrl.length()) Serial.printf("run_url=%s\n", status.runUrl.c_str());
  if (changed) render();
}

static void resetWifi() {
  Arduino_GFX *gfx = panelGfx();
  if (gfx) {
    gfx->fillScreen(COL_DARK);
    drawCentered("RESETTING WI-FI", 200, 4, COL_WHITE);
    gfx->flush();
  }
  Serial.println("long press: clearing Wi-Fi");
  wm.resetSettings();
  WiFi.disconnect(true, true);
  delay(400);
  ESP.restart();
}

static void serviceTouch() {
  static bool down = false;
  static uint32_t began = 0;
  static bool fired = false;
  static uint32_t lastBar = 0;

  const bool touch = panelTouchDown();
  const uint32_t now = millis();
  if (touch) {
    if (!down) {
      down = true;
      began = now;
      fired = false;
    } else if (!fired && now - began >= LONG_PRESS_MS) {
      fired = true;
      resetWifi();
    } else if (!fired && now - began > 300 && now - lastBar > 150) {
      lastBar = now;
      const float f = static_cast<float>(now - began) / static_cast<float>(LONG_PRESS_MS);
      drawHold(f > 1 ? 1 : f);
    }
  } else if (down) {
    const bool wasShowing = now - began > 300 && !fired;
    down = false;
    fired = false;
    if (wasShowing) render();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("grokdesk 4.3B");
  Serial.printf("status URL %s\n", STATUS_URL);

  if (!panelBegin()) {
    Serial.println("panel init failed");
    while (true) delay(1000);
  }

  wm.setDebugOutput(true, WM_DEBUG_NOTIFY);
  wm.setConfigPortalBlocking(false);
  wm.setCaptivePortalEnable(true);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(0);
  wm.setBreakAfterConfig(true);
  wm.setTitle("GrokDesk");
  wm.setAPCallback([](WiFiManager *) { portalUp = true; });
  wm.setSaveConfigCallback([]() { restartPending = true; });
  WiFi.setHostname("grokdesk");
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);

  render();
  wm.autoConnect(AP_NAME);
  wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp) {
    portalUp = false;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    refreshClock();
    pollStatus();
  } else {
    render();
  }
}

void loop() {
  if (restartPending) {
    delay(800);
    ESP.restart();
  }
  wm.process();
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiUp) {
    wifiUp = true;
    portalUp = false;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    refreshClock();
    pollStatus();
  } else if (!connected && wifiUp) {
    wifiUp = false;
    markStale("no Wi-Fi");
    render();
  }

  serviceTouch();

  if (wifiUp && !portalUp && ( !polledOnce || millis() - lastPoll >= POLL_MS)) {
    pollStatus();
  }

  refreshClock();
  if (strcmp(clockText, clockDrawn) != 0) render();

  delay(20);
}

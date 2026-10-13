// grokdesk — ESP32 + ILI9341 320x240 desk display (TFT_eSPI + ArduinoJson).
// Polls the same status.json contract the browser simulator uses.
//
//   state        healthy | failed | analyzing | resolved
//   service      service name
//   summary      human-readable status, or the Grok Bot explanation when failed
//   fix          suggested fix (may be empty)
//   run_url      CI run URL (shown on Serial; the panel is too narrow)
//   updated_at   ISO-8601 timestamp; redraws when state or updated_at changes
//
// Default URL: https://mingleiw.github.io/grokdesk/status.json
// Override WIFI_SSID, WIFI_PASS, and STATUS_URL in secrets.h (see secrets.h.example)
// or with -D flags. Wokwi's simulated network is SSID "Wokwi-GUEST", empty password.
//
// Pins come from build_opt.h (discrete module / Wokwi). For the ESP32-2432S028
// Cheap Yellow Display, replace build_opt.h with build_opt.cyd.h before compiling.
//
// HTTPS uses WiFiClientSecure::setInsecure() so the same sketch builds on
// Arduino-ESP32 2.x and 3.x (including Wokwi). The certificate is not checked.
// The file is public status. Prefer an http:// URL on your LAN to avoid TLS.

#if !defined(ILI9341_DRIVER) && !defined(ST7789_DRIVER)
#error "TFT_eSPI setup missing. Keep build_opt.h next to grokdesk.ino (or pass the same -D flags)."
#endif

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "Wokwi-GUEST"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef STATUS_URL
#define STATUS_URL "https://mingleiw.github.io/grokdesk/status.json"
#endif
#ifndef POLL_MS
#define POLL_MS 5000
#endif
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 22
#endif

TFT_eSPI tft;
String lastKey;
uint32_t lastPoll = 0;
bool wifiBegun = false;

void drawFrame(uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("GROKDESK", 8, 6, 2);
  tft.setTextDatum(BL_DATUM);
  tft.drawString("grokdesk", 8, 232, 2);
}

void drawCentered(const String& title, const String& sub, uint16_t bg, uint16_t fg) {
  drawFrame(bg, fg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  const uint8_t titleFont = title.length() > 16 ? 2 : 4;
  tft.drawString(title, 160, 96, titleFont);
  tft.drawString(sub, 160, 142, 2);
}

void drawExplain(const String& text) {
  drawFrame(TFT_BLACK, TFT_WHITE);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("DEPLOY FAILED", 8, 28, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // Font 2 is ~10px wide. Stay under ~28 characters so lines fit 320px.
  int y = 56;
  String line, word;
  const String body = text.length() > 220 ? text.substring(0, 220) : text;
  for (size_t i = 0; i <= body.length() && y < 220; i++) {
    char c = i < body.length() ? body[i] : ' ';
    if (c == ' ' || c == '\n') {
      if (line.length() + word.length() + 1 > 28) {
        tft.drawString(line, 8, y, 2);
        y += 18;
        line = "";
      }
      if (word.length()) {
        line += (line.length() ? " " : "") + word;
        word = "";
      }
    } else {
      word += c;
    }
  }
  if (line.length() && y < 220) tft.drawString(line, 8, y, 2);
}

void setLed(bool on) {
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

void renderStatus(JsonDocument& doc) {
  const char* state = doc["state"] | "unknown";
  const char* service = doc["service"] | "";
  const char* summary = doc["summary"] | "";
  const char* fix = doc["fix"] | "";
  const char* runUrl = doc["run_url"] | "";
  const char* updated = doc["updated_at"] | "";

  Serial.printf("state=%s service=%s updated_at=%s\n", state, service, updated);
  if (runUrl[0]) Serial.printf("run_url=%s\n", runUrl);

  if (strcmp(state, "healthy") == 0) {
    drawCentered("DEPLOY HEALTHY", summary[0] ? summary : service, TFT_DARKGREEN, TFT_WHITE);
    setLed(false);
  } else if (strcmp(state, "resolved") == 0) {
    drawCentered("RESOLVED", summary[0] ? summary : "Fixed", TFT_DARKGREEN, TFT_WHITE);
    setLed(false);
  } else if (strcmp(state, "analyzing") == 0) {
    drawCentered("ANALYZING", summary[0] ? summary : service, TFT_BLACK, TFT_CYAN);
    setLed(false);
  } else if (strcmp(state, "failed") == 0) {
    setLed(true);
    if (summary[0] || fix[0]) {
      String text = summary;
      if (fix[0]) {
        if (text.length()) text += " ";
        text += "Suggest: ";
        text += fix;
      }
      drawExplain(text);
    } else {
      drawCentered("DEPLOY FAILED", service, TFT_RED, TFT_WHITE);
    }
  } else {
    drawCentered("UNKNOWN", state, TFT_DARKGREY, TFT_WHITE);
    setLed(false);
  }
}

void poll() {
  if (WiFi.status() != WL_CONNECTED) {
    drawCentered("WIFI...", WIFI_SSID, TFT_NAVY, TFT_WHITE);
    Serial.println("wifi down");
    return;
  }

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  // Pages responds Cache-Control: max-age=600 and ignores a stable URL.
  // A new ?t= each poll plus Cache-Control: no-cache stops the client,
  // and any cache that honors the request, from replaying the old body.
  String url = STATUS_URL;
  url += (url.indexOf('?') >= 0) ? "&t=" : "?t=";
  url += String(millis());
  const bool https = url.startsWith("https://");
  if (https) {
    secure.setInsecure();
    if (!http.begin(secure, url)) {
      drawCentered("HTTP BEGIN", "failed", TFT_DARKGREY, TFT_WHITE);
      return;
    }
  } else if (!http.begin(plain, url)) {
    drawCentered("HTTP BEGIN", "failed", TFT_DARKGREY, TFT_WHITE);
    return;
  }
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int code = http.GET();
  Serial.printf("GET %s -> %d\n", url.c_str(), code);
  if (code == 200) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      String key = String(doc["state"] | "") + "|" + String(doc["updated_at"] | "") + "|" +
                   String(doc["summary"] | "");
      if (key != lastKey) {
        lastKey = key;
        renderStatus(doc);
      }
    } else {
      lastKey = "badjson";
      drawCentered("BAD JSON", err.c_str(), TFT_DARKGREY, TFT_WHITE);
    }
  } else {
    lastKey = String("http") + code;
    drawCentered(String("HTTP ") + code, "status.json", TFT_DARKGREY, TFT_WHITE);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(50);
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
#ifdef TFT_BACKLIGHT_ON
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#else
  digitalWrite(TFT_BL, HIGH);
#endif
#endif
  tft.init();
  tft.setRotation(1);  // landscape 320x240
  drawCentered("grokdesk", "connecting...", TFT_BLACK, TFT_WHITE);
  Serial.printf("grokdesk status URL %s\n", STATUS_URL);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiBegun = true;
}

void loop() {
#if STATUS_LED_PIN >= 0
  if (lastKey.startsWith("failed")) setLed((millis() / 400) % 2);
#endif
  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    poll();
  }
  delay(20);
}

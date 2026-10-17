#include "panel.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "board_config.h"

static Arduino_ESP32RGBPanel *rgbPanel = nullptr;
static Arduino_RGB_Display *gfx = nullptr;
static uint8_t gt911Addr = GT911_ADDR_5D;

static bool i2cWrite(uint8_t addr, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(value);
  const uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("i2c write 0x%02x failed: %u\n", addr, err);
    return false;
  }
  return true;
}

static bool gtWrite(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(gt911Addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool gtRead(uint16_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(gt911Addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(gt911Addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool gtProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Same order as Waveshare's 4.3B board header: enable CH422G outputs, pulse LCD
// reset, then hold TP_INT low across the touch reset so the GT911 takes 0x5D.
static void bringUpExpanderAndTouch() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);
  delay(10);

  i2cWrite(CH422G_WR_SET, CH422G_IO_OUTPUT);
  delay(2);
  // All EXIO high: backlight/DISP on, resets released, SD card deselected.
  uint8_t io = 0xFF;
  i2cWrite(CH422G_WR_IO, io);

  auto setBit = [&](uint8_t bit, bool high) {
    if (high) io |= static_cast<uint8_t>(1u << bit);
    else io &= static_cast<uint8_t>(~(1u << bit));
    i2cWrite(CH422G_WR_IO, io);
  };

  setBit(EXIO_LCD_RST, false);
  delay(10);
  setBit(EXIO_LCD_RST, true);
  delay(100);

  pinMode(TP_INT, OUTPUT);
  digitalWrite(TP_INT, LOW);
  delay(10);
  setBit(EXIO_TP_RST, false);
  delay(100);
  setBit(EXIO_TP_RST, true);
  delay(200);
  pinMode(TP_INT, INPUT);

  // Backlight / DISP stays high (EXIO2). SD CS stays high (inactive).
  setBit(EXIO_LCD_BL, true);
  setBit(EXIO_SD_CS, true);

  if (!gtProbe(GT911_ADDR_5D) && gtProbe(GT911_ADDR_14)) gt911Addr = GT911_ADDR_14;
  uint8_t id[4] = {0};
  if (gtRead(GT911_REG_ID, id, 4)) {
    Serial.printf("GT911 at 0x%02x id %c%c%c%c\n", gt911Addr, id[0], id[1], id[2], id[3]);
  } else {
    Serial.printf("GT911 at 0x%02x did not answer\n", gt911Addr);
  }
}

bool panelBegin() {
  Serial.printf("PSRAM %u bytes\n", static_cast<unsigned>(ESP.getPsramSize()));
  if (ESP.getPsramSize() == 0) {
    Serial.println("PSRAM missing; the RGB framebuffer needs the 8MB OPI PSRAM");
  }
  bringUpExpanderAndTouch();

  rgbPanel = new Arduino_ESP32RGBPanel(
      LCD_PIN_DE, LCD_PIN_VSYNC, LCD_PIN_HSYNC, LCD_PIN_PCLK,
      LCD_PIN_R0, LCD_PIN_R1, LCD_PIN_R2, LCD_PIN_R3, LCD_PIN_R4,
      LCD_PIN_G0, LCD_PIN_G1, LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5,
      LCD_PIN_B0, LCD_PIN_B1, LCD_PIN_B2, LCD_PIN_B3, LCD_PIN_B4,
      0, LCD_HFP, LCD_HPW, LCD_HBP,
      0, LCD_VFP, LCD_VPW, LCD_VBP,
      1, LCD_PCLK_HZ, false,
      0, 0, static_cast<size_t>(LCD_W) * 10);

  gfx = new Arduino_RGB_Display(LCD_W, LCD_H, rgbPanel, 0, false);
  if (!gfx->begin(LCD_PCLK_HZ)) {
    Serial.println("RGB panel begin failed");
    return false;
  }
  gfx->fillScreen(0x0000);
  gfx->flush();
  return true;
}

Arduino_GFX *panelGfx() { return gfx; }

static bool touchDown = false;
static int touchX = 0, touchY = 0;

bool panelTouchDown() {
  uint8_t status = 0;
  if (!gtRead(GT911_REG_STATUS, &status, 1)) return false;
  // Bit 7 marks a new report. Between reports the last one still holds, so a
  // fast loop does not read a held finger as released.
  if ((status & 0x80) == 0) return touchDown;
  const uint8_t points = status & 0x0F;
  uint8_t pt[4];
  if (points > 0 && gtRead(GT911_REG_POINT1, pt, 4)) {
    touchX = pt[0] | (pt[1] << 8);
    touchY = pt[2] | (pt[3] << 8);
  }
  gtWrite(GT911_REG_STATUS, 0);
  touchDown = points > 0;
  return touchDown;
}

void panelTouchPoint(int *x, int *y) {
  *x = touchX;
  *y = touchY;
}

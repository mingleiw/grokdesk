#pragma once

// Waveshare ESP32-S3-Touch-LCD-4.3B (800x480 RGB, GT911, CH422G).
// Pin names follow the wiki table:
//   https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B
// RGB565 bit order and porch timings follow Waveshare's Arduino board header
// BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B (ST7262, 16 MHz PCLK, active-low clock).
// The wiki names panel pins R3..R7 / G2..G7 / B3..B7; those are the RGB565 bits.

#define LCD_W 800
#define LCD_H 480

#define LCD_PIN_DE 5
#define LCD_PIN_VSYNC 3
#define LCD_PIN_HSYNC 46
#define LCD_PIN_PCLK 7

#define LCD_PIN_R0 1
#define LCD_PIN_R1 2
#define LCD_PIN_R2 42
#define LCD_PIN_R3 41
#define LCD_PIN_R4 40

#define LCD_PIN_G0 39
#define LCD_PIN_G1 0
#define LCD_PIN_G2 45
#define LCD_PIN_G3 48
#define LCD_PIN_G4 47
#define LCD_PIN_G5 21

#define LCD_PIN_B0 14
#define LCD_PIN_B1 38
#define LCD_PIN_B2 18
#define LCD_PIN_B3 17
#define LCD_PIN_B4 10

#define I2C_SDA 8
#define I2C_SCL 9
#define TP_INT 4

// CH422G WR_IO bits. EXIO2 is LCD backlight and DISP, active high.
// EXIO1 resets the GT911. EXIO3 resets the panel. EXIO4 is TF-card CS (active low).
#define EXIO_TP_RST 1
#define EXIO_LCD_BL 2
#define EXIO_LCD_RST 3
#define EXIO_SD_CS 4

#define LCD_PCLK_HZ 16000000
#define LCD_HFP 8
#define LCD_HPW 4
#define LCD_HBP 8
#define LCD_VFP 8
#define LCD_VPW 4
#define LCD_VBP 8

// CH422G selects the register by I2C address (not a register pointer).
// 0x24 = WR_SET (0x48 >> 1), 0x38 = WR_IO (0x70 >> 1). Bit 0 of WR_SET is IO_OE.
#define CH422G_WR_SET 0x24
#define CH422G_WR_IO 0x38
#define CH422G_IO_OUTPUT 0x01

// INT held low across GT911 reset selects address 0x5D. 0x14 is the other option.
#define GT911_ADDR_5D 0x5D
#define GT911_ADDR_14 0x14
#define GT911_REG_ID 0x8140
#define GT911_REG_STATUS 0x814E

#ifndef STATUS_URL
#define STATUS_URL "https://raw.githubusercontent.com/mingleiw/grokdesk/main/status.json"
#endif

#define POLL_MS 30000UL
#define LONG_PRESS_MS 3000UL
#define AP_NAME "GrokDesk-Setup"

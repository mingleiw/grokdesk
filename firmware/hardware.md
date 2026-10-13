# grokdesk hardware

The firmware in `grokdesk/grokdesk.ino` polls the same `status.json` document as the browser simulator (see the schema in the repo README). Default URL:

```
https://mingleiw.github.io/grokdesk/status.json
```

That URL works only after GitHub Pages is enabled for this repo. Until then, point the board at a machine on your LAN:

```cpp
#define STATUS_URL "http://192.168.1.20:8765/status.json"
```

Put overrides in `firmware/grokdesk/secrets.h` (copy `secrets.h.example`; the file is gitignored).

## Recommended: ESP32-2432S028 "Cheap Yellow Display" (no soldering)

A CYD is an ESP32 and a 2.8" 320×240 ILI9341 already on one board, so there is no SPI wiring to do. USB-C power is enough.

1. Copy `firmware/grokdesk/build_opt.cyd.h` over `firmware/grokdesk/build_opt.h`.
2. Those flags match the common ESP32-2432S028 ILI9341 pinout: MOSI 13, MISO 12, SCK 14, CS 15, DC 2, backlight 21, HSPI. The onboard status LED is left unused (`STATUS_LED_PIN=-1`) because GPIO 2 is the TFT DC pin.
3. Flash `grokdesk.ino` with the Arduino IDE or `firmware/compile.sh` (after swapping the build flags — `compile.sh` forwards `build_opt.h`).
4. A few board revisions use an ST7789 panel instead of an ILI9341. If the backlight is on and the picture stays blank, change `-DILI9341_DRIVER=1` to `-DST7789_DRIVER=1` in the copied `build_opt.h`.

## Discrete ESP32 + 2.8" ILI9341

This is the wiring the committed `build_opt.h` and the Wokwi diagram use. It also matches TFT_eSPI's usual ESP32 SPI defaults, so a sketch that misses `build_opt.h` still talks to the same pins.

| Part | Notes |
|---|---|
| ESP32 dev board (ESP32-WROOM-32 DevKitC, or the CYD above) | Wi-Fi + SPI |
| 2.8" ILI9341 320×240 SPI TFT | 3.3 V logic. Touch is unused. |
| 3 mm LED + 220 Ω resistor | status LED on GPIO 22 |
| USB cable + 5 V supply | powers the board |

| TFT pin | ESP32 GPIO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | 15 |
| RST | 4 |
| D/C | 2 |
| MOSI (SDI) | 23 |
| SCK | 18 |
| MISO (SDO) | 19 (optional) |
| LED (backlight) | 3V3 |

Status LED: GPIO 22 → 220 Ω → LED anode, cathode to GND. The LED blinks while `state` is `failed`.

Do not edit the TFT_eSPI library's `User_Setup.h`. `build_opt.h` in the sketch folder is how the ESP32 Arduino core passes `-D` flags, and `firmware/compile.sh` passes the same flags to `arduino-cli`.

## What the panel draws

| `state` | Panel |
|---|---|
| `healthy` | Green. Title `DEPLOY HEALTHY`. Subtitle is `summary`, or `service` when summary is empty. |
| `failed` and `summary`/`fix` empty | Red `DEPLOY FAILED`. Status LED blinks. |
| `failed` with `summary` or `fix` | Black screen, red `DEPLOY FAILED`, wrapped Grok Bot summary and `Suggest:` fix. Status LED blinks. |
| `analyzing` | Black / cyan `ANALYZING` plus `summary`. |
| `resolved` | Green `RESOLVED` plus `summary`. |
| anything else | `UNKNOWN`. |
| Wi-Fi down or HTTP error | `WIFI...` or `HTTP <code>`. |

The device refetches every 5 seconds. Each GET is `status.json?t=<millis()>` with `Cache-Control: no-cache`, so a `max-age=600` response is not reused. It redraws when `state`, `updated_at`, or `summary` changes. `run_url` is printed to Serial at 115200.

HTTPS (the GitHub Pages default) uses `WiFiClientSecure::setInsecure()`, which does not check the server certificate. That call exists on Arduino-ESP32 2.x and 3.x, so the sketch builds in the Arduino IDE and on Wokwi. The file is public status and contains no secrets. A plain `http://` URL on the LAN skips TLS entirely.

`firmware/compile.sh` was run with arduino-cli against `esp32:esp32:esp32` (core 3.3.12). The sketch is about 1.0 MB, 80% of the default 1.25 MB app partition. The Cheap Yellow Display flags in `build_opt.cyd.h` compile the same way when copied over `build_opt.h`.

## Wokwi

`firmware/grokdesk/` is a Wokwi project: `diagram.json` (ESP32 DevKitC v4 + ILI9341 + status LED), `wokwi.toml`, `libraries.txt` (`ArduinoJson`, `TFT_eSPI`), and `build_opt.h`.

- **wokwi.com:** create an ESP32 project and replace its diagram and sketch with this folder (keep `libraries.txt` and `build_opt.h`). The simulated Wi-Fi SSID is `Wokwi-GUEST` with an empty password, which is the sketch default. The simulator has internet access, so the default GitHub Pages URL works once Pages is enabled. RST and the backlight pin are wired for documentation; Wokwi ignores both and keeps the backlight on.
- **Wokwi for VS Code / wokwi-cli:** run `firmware/compile.sh`, then start the simulation. `wokwi.toml` points at `build/grokdesk.ino.bin` and `build/grokdesk.ino.elf`.

## Enclosure

A tilted front (~15°) reads well on a desk. Recess the TFT so the bezel hides the panel border, leave a side slot for USB, and a hole for the status LED. A matte face cuts glare.

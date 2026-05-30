# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Arduino firmware for a Seeed Studio XIAO ESP32-C3/S3 driving a 7.5" UC8179 monochrome e-paper display (800×480, TRMNL DIY kit). Displays a large-digit clock, syncs time via NTP, fetches weather from OpenWeatherMap, and uses deep sleep to conserve battery.

## Build & Upload

This is an Arduino IDE project. Use Arduino IDE 2.x or `arduino-cli`:

```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3 XIAO_epaper_clock.ino

# Upload via USB
arduino-cli upload -p /dev/cu.usbmodem* --fqbn esp32:esp32:XIAO_ESP32C3 XIAO_epaper_clock.ino

# Upload via OTA (after triggering OTA mode with a button press)
arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32C3 --port epaper-clock.local XIAO_epaper_clock.ino
```

Required Arduino libraries: **TFT_eSPI** (with the custom e-paper board support from the TRMNL kit).

## Configuration

`config.h` is gitignored and must be created locally — it is not in the repository. Copy this template:

```cpp
#pragma once

const char *WIFI_SSID = "your_ssid";
const char *WIFI_PASS = "your_password";

// POSIX TZ string — e.g. "CST6CDT,M3.2.0,M11.1.0" for US Central
const char *TZ_INFO = "CST6CDT,M3.2.0,M11.1.0";

const char *NTP_SERVER1 = "pool.ntp.org";
const char *NTP_SERVER2 = "time.nist.gov";

// OpenWeatherMap
const char *OWM_API_KEY = "your_key";
const float OWM_LAT = 30.52;
const float OWM_LON = -97.72;
```

## Mac Simulator

A native SDL2 simulator lives in `simulator/`. It compiles the sketch unchanged and renders an 800×480 window showing exactly what the e-paper display would show. Requires SDL2 and SDL2_ttf (`brew install sdl2 sdl2_ttf`).

```bash
cd simulator
make        # build
make run    # build and launch
make clean  # remove binary
```

**How it works:** `simulator/main.cpp` `#include`s the `.ino` directly as a C++ translation unit. `simulator/stubs/` provides thin header replacements for every Arduino/ESP32 API (`Arduino.h`, `WiFi.h`, `HTTPClient.h`, `ArduinoOTA.h`, `esp_sleep.h`, `time_compat.h`, `TFT_eSPI.h`). `EPaperSim.h` implements the `EPaper` class using an SDL2 renderer; `update()` presents the frame and pumps the event loop.

Key simulator behaviors vs. real hardware:
- WiFi is always "connected"; weather returns hardcoded fake data (`Clear sky, 88°/71°F`)
- Time uses the Mac's real system clock (same timezone logic as the device)
- `esp_deep_sleep_start()` signals the main loop to pause (capped at 5 s), then reboots the cycle by calling `setup()` again — `RTC_DATA_ATTR` globals persist as they would across real deep sleep
- `FIRST_BOOT_AWAKE_MS` and `OTA_LISTEN_MS` are overridden to 0 so the 60 s / 90 s wait periods are skipped

## Architecture

### File layout

| File | Role |
|---|---|
| `XIAO_epaper_clock.ino` | Main sketch: `setup()`, `loop()`, all display drawing, WiFi/NTP/weather, sleep logic |
| `config.h` | User secrets & settings (gitignored — create locally) |
| `driver.h` | Board/screen combo constants consumed by TFT_eSPI |
| `BigDigits.h` | PROGMEM bitmap data for large digits 0–9 plus `bigDigitsWidth()` / `drawBigDigits()` helpers |

### Deep sleep & wake cycle

All state that must survive a deep sleep cycle is declared `RTC_DATA_ATTR`. On each wake the device:
1. Reads battery voltage
2. Checks if NTP re-sync is due (every 6 hours) — shows "Syncing…" screen if so
3. Checks if weather re-fetch is due (every 6 hours, daytime only)
4. Draws the clock and calls `epaper.update()`
5. Sleeps until the next minute boundary (day) or next 5-minute boundary (night, 1–6am)

Button press (any of D1/D2/D4) wakes via `ext1` and triggers OTA mode: the device connects to WiFi, starts `ArduinoOTA`, draws an OTA screen, and listens for 90 seconds before falling back to normal clock+sleep.

First boot stays awake for 60 seconds so the Arduino IDE serial monitor / OTA can connect before the first sleep.

### Display

All drawing calls go through the `EPaper` object (wraps TFT_eSPI). The `EPAPER_ENABLE` preprocessor guard wraps every display-related block — remove it or define it `0` to build without the display (useful for compile-testing on a plain ESP32).

The clock face renders hour/minute using bitmap glyphs from `BigDigits.h` (extracted from FreeSerifBold 160pt, digits only, stored in flash via `PROGMEM`). The colon is drawn as two filled circles. Weather and battery status appear in the bottom corners.

### Battery

GPIO1 (A0) is the ADC input; GPIO6 (A5) enables/disables the ADC voltage divider. The voltage is averaged over 30 reads with a calibration factor (`CALIBRATION_FACTOR = 0.968`). Full range is 3.2V–4.1V for a single-cell LiPo.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Arduino-framework firmware, built with PlatformIO, for a Seeed Studio XIAO ESP32S3 Plus driving a 7.5" UC8179 monochrome e-paper display (800×480, TRMNL DIY kit). Displays a large-digit clock, syncs time via NTP, fetches weather from OpenWeatherMap, and uses deep sleep to conserve battery.

## Build & Upload

This is a [PlatformIO](https://platformio.org/) project (`pio` CLI, or the PlatformIO IDE extension for VS Code):

```bash
# Compile
pio run

# Upload via USB
pio run -t upload

# Upload via OTA (after triggering OTA mode with a button press)
pio run -t upload --upload-port epaper-clock.local

# Serial monitor
pio device monitor
```

The board (`seeed_xiao_esp32s3`) and the Seeed_GFX library dependency are declared in `platformio.ini` — `pio run` fetches everything it needs on first build, no manual Arduino Library Manager step required.

## Configuration

`src/config.h` is gitignored and must be created locally — it is not in the repository. Copy this template:

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

// Firmware auto-update (GitHub Releases) — see "Firmware auto-update" below
const bool OTA_UPDATES_ENABLED = true;
const char *OTA_GITHUB_OWNER = "your_github_username";
const char *OTA_GITHUB_REPO = "your_repo_name";
```

## Mac Simulator

A native SDL2 simulator lives in `simulator/`. It compiles the sketch unchanged and renders an 800×480 window showing exactly what the e-paper display would show. Requires SDL2 and SDL2_ttf (`brew install sdl2 sdl2_ttf`).

```bash
cd simulator
make              # build
make run          # build and launch interactive window
./sim clock.jpg   # render one boot cycle, save frames, exit
make clean        # remove binary
```

**JPEG export:** passing a filename saves every `update()` call as a numbered JPEG and exits — `clock.jpg` produces `clock_01.jpg` (the "Syncing…" screen, only on first boot) and `clock_02.jpg` (the clock face). On subsequent boots NTP is already synced so only the clock frame is saved.

**How it works:** `simulator/main.cpp` `#include`s `src/main.cpp` directly as a C++ translation unit (its `config.h`/`version.h`/`BigDigits.h` includes resolve against `src/`, since that's `main.cpp`'s own directory). `simulator/stubs/` provides thin header replacements for every Arduino/ESP32 API (`Arduino.h`, `WiFi.h`, `WiFiClientSecure.h`, `Update.h`, `HTTPClient.h`, `ArduinoOTA.h`, `esp_sleep.h`, `time_compat.h`, `TFT_eSPI.h`). `EPaperSim.h` implements the `EPaper` class using an SDL2 renderer backed by a persistent render-target texture (`SDL_TEXTUREACCESS_TARGET`) so frames are always readable for JPEG export regardless of backbuffer swap behaviour. Text is rendered via SDL_ttf using the system SFNS font. JPEG encoding uses the bundled `stb_image_write.h` (no extra dependency).

The simulator is a standalone `make`-based build, independent of PlatformIO — it doesn't link against Seeed_GFX or the real ESP32 Arduino core at all, only its own stubs.

Key simulator behaviors vs. real hardware:
- WiFi is always "connected"; weather returns hardcoded fake data (`Clear sky, 88°/71°F`)
- The GitHub release check always returns a fake old tag (`v0.0.0`), so the auto-update path is exercised for compilation but never actually fires or flashes anything
- Time uses the Mac's real system clock (same timezone logic as the device)
- `esp_deep_sleep_start()` signals the main loop to pause (capped at 5 s), then reboots the cycle by calling `setup()` again — `RTC_DATA_ATTR` globals persist as they would across real deep sleep
- `FIRST_BOOT_AWAKE_MS` and `OTA_LISTEN_MS` are overridden to 0 so the 60 s / 90 s wait periods are skipped

## Architecture

### File layout

| File | Role |
|---|---|
| `platformio.ini` | Board/framework/library config. `build_flags` set `BOARD_SCREEN_COMBO=502` + `USE_XIAO_EPAPER_DISPLAY_BOARD_EE04`, which is how Seeed_GFX picks the 7.5" UC8179 + EE04 pin mapping (see "Display" below) |
| `src/main.cpp` | Main firmware: `setup()`, `loop()`, all display drawing, WiFi/NTP/weather, sleep logic, firmware auto-update |
| `src/config.h` | User secrets & settings (gitignored — create locally) |
| `src/version.h` | `FIRMWARE_VERSION` — bump and tag a matching GitHub release to ship an update |
| `src/BigDigits.h` | PROGMEM bitmap data for large digits 0–9 plus `bigDigitsWidth()` / `drawBigDigits()` helpers |

### Deep sleep & wake cycle

All state that must survive a deep sleep cycle is declared `RTC_DATA_ATTR`. On each wake the device:
1. Reads battery voltage
2. Checks if NTP re-sync is due (every 6 hours) — shows "Syncing…" screen if so
3. Checks if weather re-fetch is due (every 6 hours, daytime only)
4. Draws the clock and calls `epaper.update()`
5. Sleeps until the next minute boundary (day) or next 5-minute boundary (night, 1–6am)

Button press (any of D1/D2/D4) wakes via `ext1` and triggers OTA mode: the device connects to WiFi, starts `ArduinoOTA`, draws an OTA screen (showing the listen countdown, hostname/IP, and running firmware version), and listens for 90 seconds before falling back to normal clock+sleep. If WiFi fails to connect, it skips the listen window entirely rather than burning battery waiting.

First boot stays awake for 60 seconds so the serial monitor (`pio device monitor`) / OTA can connect before the first sleep.

### Firmware auto-update (GitHub Releases)

On normal (non-button) wakes during the night window (1am-6am), and at most once every 24 hours, the device checks `https://api.github.com/repos/{OTA_GITHUB_OWNER}/{OTA_GITHUB_REPO}/releases/latest`. If the release's tag is a newer semver than `version.h`'s `FIRMWARE_VERSION`, it downloads the first `.bin` asset attached to that release, flashes it via `Update.h`, and reboots. A release with no `.bin` asset is ignored. A failed download/flash is retried on the next nightly check, up to `OTA_MAX_UPDATE_ATTEMPTS`, before that version is skipped until a newer tag appears.

To ship an update this way: bump `FIRMWARE_VERSION` in `src/version.h`, build the `.bin` (`pio run`, output at `.pio/build/seeed_xiao_esp32s3/firmware.bin` — or let the `firmware` CI job produce it as a build artifact), and publish it as a GitHub release with a matching tag (e.g. `v1.0.1`).

Caveat: there's no automatic rollback if a released build boot-loops — that needs the ESP-IDF bootloader's rollback feature enabled via a custom `sdkconfig`, which this project doesn't set up, so a bad release stays flashed (recoverable via USB or a fixed follow-up release) rather than self-healing like `ArduinoOTA`'s partition-swap would with proper rollback support. Set `OTA_UPDATES_ENABLED = false` in `src/config.h` to disable the check entirely.

### Display

All drawing calls go through the `EPaper` object (wraps TFT_eSPI, provided by the [Seeed_GFX](https://github.com/Seeed-Studio/Seeed_GFX) fork declared in `platformio.ini`'s `lib_deps`). The `EPAPER_ENABLE` preprocessor guard wraps every display-related block — it's defined transitively by Seeed_GFX's `Setup502_Seeed_XIAO_EPaper_7inch5.h` (selected via `platformio.ini`'s `BOARD_SCREEN_COMBO=502` build flag), not by the firmware itself; remove that build flag or swap it for a non-epaper combo ID to compile-test without the display.

Getting `BOARD_SCREEN_COMBO`/`USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` into Seeed_GFX has to happen via `build_flags`, not a project header: Arduino IDE normally lets a sketch-root `driver.h` reach the library through `__has_include`, because the IDE adds the whole sketch folder to every compiler invocation's include path. PlatformIO's Library Dependency Finder scopes each library's own include path independently, so a project `include/driver.h` is invisible to `Seeed_GFX/TFT_eSPI.cpp` when it's compiled as a library object — only `build_flags`, which PlatformIO applies globally, reliably reach it.

The clock face renders hour/minute using bitmap glyphs from `BigDigits.h` (extracted from FreeSerifBold 160pt, digits only, stored in flash via `PROGMEM`). The colon is drawn as two filled circles. Weather and battery status appear in the bottom corners.

### Battery

GPIO1 (A0) is the ADC input; GPIO6 (A5) enables/disables the ADC voltage divider. The voltage is averaged over 30 reads with a calibration factor (`CALIBRATION_FACTOR = 0.968`). Full range is 3.2V–4.1V for a single-cell LiPo.

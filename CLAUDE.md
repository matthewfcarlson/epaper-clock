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

# Serial monitor
pio device monitor
```

The board (`seeed_xiao_esp32s3`) and the Seeed_GFX library dependency are declared in `platformio.ini` — `pio run` fetches everything it needs on first build, no manual Arduino Library Manager step required.

## Configuration

`src/config.h` is gitignored and must be created locally — it is not in the repository. Copy this template:

```cpp
#pragma once

// Dev-convenience fallback only — used until WiFi is provisioned at runtime
// over USB serial (see "Location & WiFi provisioning" below), at which point
// the NVS-stored credentials take priority. The public release build ships
// these as empty strings; a real device only ever gets real credentials via
// serial provisioning, never compiled into the binary.
const char *WIFI_SSID = "your_ssid";
const char *WIFI_PASS = "your_password";

// POSIX TZ string — e.g. "CST6CDT,M3.2.0,M11.1.0" for US Central
const char *TZ_INFO = "CST6CDT,M3.2.0,M11.1.0";

const char *NTP_SERVER1 = "pool.ntp.org";
const char *NTP_SERVER2 = "time.nist.gov";

// OpenWeatherMap — lat/lon are set at runtime by the user (see "Location
// provisioning" below), not here
const char *OWM_API_KEY = "your_key";

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

**Forcing a time (e.g. to see night-time behavior):** set `SIM_TIME` to freeze the simulator's clock, either as `HH:MM` (today, in the device's configured `TZ_INFO`) or a raw Unix epoch:

```bash
SIM_TIME=03:30 ./sim night.jpg   # 11pm-6am night window: 15-min wake cadence, weather fetch skipped, nightly GitHub check runs
```

The clock stays frozen at that instant for the run (it doesn't keep ticking) — rerun with a different value to look elsewhere. See `applySimTimeOverride()` in `simulator/main.cpp` and `g_sim_time_override` in `simulator/stubs/time_compat.h`.

**How it works:** `simulator/main.cpp` `#include`s `src/main.cpp` directly as a C++ translation unit (its `config.h`/`version.h`/`BigDigits.h`/`ota_health.h` includes resolve against `src/`, since that's `main.cpp`'s own directory), and the Makefile separately compiles `src/ota_health.cpp` as its own translation unit and links it in. `simulator/stubs/` provides thin header replacements for every Arduino/ESP32 API (`Arduino.h`, `WiFi.h`, `WiFiClientSecure.h`, `Update.h`, `HTTPClient.h`, `esp_sleep.h`, `esp_system.h`, `esp_ota_ops.h`, `time_compat.h`, `TFT_eSPI.h`). `EPaperSim.h` implements the `EPaper` class using an SDL2 renderer backed by a persistent render-target texture (`SDL_TEXTUREACCESS_TARGET`) so frames are always readable for JPEG export regardless of backbuffer swap behaviour. Text is rendered via SDL_ttf using the system SFNS font. JPEG encoding uses the bundled `stb_image_write.h` (no extra dependency).

The simulator is a standalone `make`-based build, independent of PlatformIO — it doesn't link against Seeed_GFX or the real ESP32 Arduino core at all, only its own stubs.

Key simulator behaviors vs. real hardware:
- The weather location is pre-seeded on startup (`simulator/main.cpp` calls `saveLocationConfig()` — the same function the real serial-provisioning protocol calls — with coordinates for zip 78681/Round Rock, TX), so `locationConfigured` is `true` from boot instead of needing an interactive provisioning step. WiFi is always "connected"; weather returns hardcoded fake data (`Clear sky, 88°/71°F`) regardless of the coordinates used
- The GitHub release check always returns a fake old tag (`v0.0.0`), so the auto-update path is exercised for compilation but never actually fires or flashes anything
- `esp_ota_mark_app_valid_cancel_rollback()` / `esp_ota_mark_app_invalid_rollback_and_reboot()` are stubbed no-ops (the latter just ends the current `setup()`/`loop()` cycle like a real reboot would) — there's no dual-partition flash to actually roll back
- Time uses the Mac's real system clock (same timezone logic as the device) unless overridden via `SIM_TIME` — see "Forcing a time" above
- `esp_deep_sleep_start()` signals the main loop to pause (capped at 5 s), then reboots the cycle by calling `setup()` again — `RTC_DATA_ATTR` globals persist as they would across real deep sleep. `esp_reset_reason()` approximates real hardware too: `ESP_RST_POWERON` for the process's first boot, `ESP_RST_DEEPSLEEP` for every iteration after that — enough to exercise the OTA force-check gate's two branches (see "Firmware auto-update" below), though it can't distinguish an actual reset/watchdog/brownout from a fresh flash the way hardware can
- `FIRST_BOOT_AWAKE_MS` and `PROVISION_LISTEN_MS` are overridden to 0 so the 60 s / 90 s wait periods are skipped
- `Preferences` (location config storage, and `OtaHealth`'s pending-update tracking) is stubbed as an in-memory map — it persists for the life of one `./sim` run like the `RTC_DATA_ATTR` globals do, but isn't disk-backed, so it doesn't survive across separate `./sim` invocations the way real NVS survives power loss. `Serial.available()`/`read()` are stubbed to report "no input," so the location-provisioning serial protocol compiles but is never exercised interactively in the simulator.

## Architecture

### File layout

| File | Role |
|---|---|
| `platformio.ini` | Board/framework/library config. `build_flags` set `BOARD_SCREEN_COMBO=502` + `USE_XIAO_EPAPER_DISPLAY_BOARD_EE04`, which is how Seeed_GFX picks the 7.5" UC8179 + EE04 pin mapping (see "Display" below) |
| `src/main.cpp` | Main firmware: `setup()`, `loop()`, all display drawing, WiFi/NTP/weather, sleep logic, firmware auto-update |
| `src/config.h` | User secrets & settings (gitignored — create locally) |
| `src/version.h` | `FIRMWARE_VERSION` — bump and tag a matching GitHub release to ship an update |
| `src/BigDigits.h` | PROGMEM bitmap data for large digits 0–9 plus `bigDigitsWidth()` / `drawBigDigits()` helpers |
| `src/ota_health.h` / `src/ota_health.cpp` | `OtaHealth` — bootloader-rollback safety net for the GitHub-releases update path (see "Firmware auto-update" below) |

### Deep sleep & wake cycle

All state that must survive a deep sleep cycle is declared `RTC_DATA_ATTR`. On each wake the device:
1. Reads battery voltage
2. Checks if NTP re-sync is due (every 6 hours) — shows "Syncing…" screen if so
3. Checks if weather re-fetch is due (every 6 hours, daytime only)
4. Draws the clock and calls `epaper.update()`
5. Sleeps until the next 5-minute boundary (day) or next 15-minute boundary (night, 11pm–6am). The displayed time is always rounded to the nearest 5-minute mark, matching this cadence.

Button press (any of D1/D2/D4) wakes via `ext1` and triggers a maintenance window: the device draws a maintenance screen (showing the listen countdown and running firmware version) and listens on the USB serial connection for 90 seconds for location provisioning (see below) before falling back to normal clock+sleep. This window is serial-only and never touches WiFi — firmware updates are pulled from GitHub on the normal nightly schedule (see "Firmware auto-update" below), not triggered by button press.

First boot stays awake for 60 seconds so the serial monitor (`pio device monitor`) / OTA / location provisioning can connect before the first sleep.

### Firmware auto-update (GitHub Releases)

On normal (non-button) wakes during the night window (11pm-6am), and at most once every 24 hours, the device checks `https://api.github.com/repos/{OTA_GITHUB_OWNER}/{OTA_GITHUB_REPO}/releases/latest`. Any wake that isn't a resume from the device's own deep sleep is an exception: it forces the check regardless of the time of day or the 24-hour throttle, so the device doesn't sit on stale firmware until the next night window after a fresh flash, a reset-button/EN-pin press, `CFG REBOOT`, a watchdog/panic recovery, or a brownout — anything ESP-IDF's `esp_reset_reason()` doesn't report as `ESP_RST_DEEPSLEEP` (see `freshStart` in `setup()`, `src/main.cpp`). If the release's tag is a newer semver than `version.h`'s `FIRMWARE_VERSION`, it downloads the first `.bin` asset attached to that release, flashes it via `Update.h`, and reboots. A release with no `.bin` asset is ignored. A failed download/flash is retried on the next check (nightly, or the next forced check after another such wake), up to `OTA_MAX_UPDATE_ATTEMPTS`, before that version is skipped until a newer tag appears.

To ship an update: bump `FIRMWARE_VERSION` in `src/version.h`, commit, and merge that to `main`. Then create the matching tag either way:
- **`.github/workflows/tag-release.yml`** (recommended): manually trigger it — "Run workflow" in the Actions tab of the web UI or the GitHub mobile app, or `gh workflow run tag-release.yml --ref main` from a desk. It reads `FIRMWARE_VERSION` off the selected branch's `src/version.h`, creates a matching `vX.Y.Z` tag, and pushes it using the workflow's own `GITHUB_TOKEN` — this sidesteps interactive/agent sessions whose git credentials are sometimes scoped to branches only and can't push tags directly. It then explicitly dispatches `release-firmware.yml` for that tag itself (`gh workflow run release-firmware.yml --ref main -f tag=vX.Y.Z`), rather than relying on the tag push to trigger it: a tag pushed with the default `GITHUB_TOKEN` does *not* fire other `on: push`-triggered workflows (GitHub's anti-recursion guard), so `release-firmware.yml` also listens for `workflow_dispatch`.
- Or manually: `git tag v1.0.1 && git push origin v1.0.1` — a normal user-authenticated push *does* trigger `release-firmware.yml`'s `on: push: tags:` directly, no separate dispatch needed.

Either way, `.github/workflows/release-firmware.yml` builds and publishes the release — it rejects the run if the tag doesn't match `FIRMWARE_VERSION`. That workflow assembles `src/config.h` from repo secrets (`TZ_INFO`, `OWM_API_KEY`) rather than the CI job's placeholder values, since this `.bin` is what devices actually self-flash. `WIFI_SSID`/`WIFI_PASS` are compiled in blank (see below) — they are not repo secrets. Set the remaining secrets once via the repo's Settings → Secrets and variables → Actions (or `gh secret set NAME`).

It can also be re-run manually for an existing tag without re-pushing it: "Run workflow" and fill in the required `tag` input (e.g. `v0.9.8`) — deliberately a typed field rather than the Actions UI's "Use workflow from" branch/tag selector, which defaults to `main` and silently builds the wrong (mismatched-version) commit if you forget to change it there instead. The job explicitly checks out whatever `tag` names, regardless of what that selector was left on.

WiFi credentials don't belong in a public release binary (see "Location & WiFi provisioning" below) — they live only in NVS via serial provisioning. That migration is complete: `release-firmware.yml` now compiles `WIFI_SSID`/`WIFI_PASS` blank, and the one-time migration in `setup()` (`src/main.cpp`, right after `loadWifiConfig()`) is a no-op for any release built from this point on, since it only fires when a compiled-in `WIFI_SSID` is present. It stays in place only so a device still running an older, pre-migration release picks up its NVS credentials automatically the first time it updates to a newer build.

Rollback safety net: `src/ota_health.h`/`.cpp` (`OtaHealth`) guards against a bad release. The ESP-IDF bootloader's app-rollback feature and this board's default two-OTA-slot partition table are already enabled by the stock Arduino-ESP32 core for esp32s3, so no custom `sdkconfig` is needed. `checkForFirmwareUpdate()` calls `otaHealth.recordOtaAttempt()` right before rebooting into a freshly-flashed build; `setup()` calls `otaHealth.checkBootHealth()` as early as possible on every boot and `otaHealth.confirmHealthy()` once a wake cycle completes without crashing. Two layers of protection result: a boot-time crash/panic on the new partition is rolled back automatically by the bootloader before any app code runs, and firmware that boots but never manages to complete a wake cycle (e.g. a WiFi regression) is force-rolled-back by `OtaHealth` itself after `OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS` (3) unconfirmed boots. Either way the device reverts to the last-known-good build rather than staying stuck on a bad release. Set `OTA_UPDATES_ENABLED = false` in `src/config.h` to disable the check entirely.

### Location & WiFi provisioning

Weather's lat/lon and the device's WiFi credentials are no longer build-time constants — both are set by the user at runtime and stored in NVS via the ESP32 `Preferences` library (namespace `"clockcfg"`, keys `loc_set`/`lat`/`lon` for location, `wifi_set`/`ssid`/`pass` for WiFi), so one firmware build works for any device/location/network and there's nothing device-specific to bake in or leak via a repo secret or a public release binary. Until location is set, weather fetching is skipped entirely (no bogus `0,0` lookup); until WiFi is set (and no compiled-in `config.h` fallback is present — see below), `connectWiFi()` skips connecting entirely rather than blocking on a doomed attempt. Either way the clock face shows a short "press button + connect USB" hint in the weather corner naming whichever is missing.

`connectWiFi()` prefers the NVS-stored credentials but falls back to the compiled-in `WIFI_SSID`/`WIFI_PASS` (`config.h`) when NVS hasn't been provisioned yet. `config.h` is gitignored and is only a local dev convenience — the public release build compiles those two fields blank, so the distributed `.bin` never contains a real network's credentials. A freshly flashed device has no working fallback and must be provisioned once over serial before it can reach WiFi at all; from then on the NVS-stored credentials persist across every future OTA update (NVS isn't touched by an app-partition flash) independently of what's compiled into any given release. `setup()` also runs a one-time migration (`src/main.cpp`, right after `loadWifiConfig()`): if NVS isn't configured yet but a compiled-in `WIFI_SSID` is present, it's copied into NVS immediately, so a device updating from an older, pre-migration release picks up its working credentials without needing manual serial provisioning.

Setting either: press a button to enter the maintenance window (see above), plug in a USB cable, and open `docs/provision.html` (hosted via GitHub Pages, or served locally with e.g. `python3 -m http.server` from `docs/` — Web Serial requires a secure or localhost context) in Chrome or Edge on a computer. The page uses the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API) to talk to the device over the same native-USB CDC connection already used for flashing/`pio device monitor` — no new USB descriptors, no pairing. It reads the device's current status and lets you enter WiFi SSID/password and lat/lon manually, plus lat/lon via the browser's geolocation.

Wire protocol, plain text lines over the existing `Serial` (commands prefixed `CFG `, replies prefixed `>>` so the page can filter them out of ordinary debug logging on the same stream):
- `CFG GET_STATUS` → `>>STATUS configured=0|1 lat=<f> lon=<f> fw=<version> wifi=0|1`
- `CFG SET_LOCATION <lat> <lon>` → `>>OK SET_LOCATION` or `>>ERR RANGE|PARSE`
- `CFG GET_WIFI_SSID` → `>>WIFI_SSID <ssid>` (blank when unset) — the password is never echoed back
- `CFG SET_WIFI_SSID <ssid>` → `>>OK SET_WIFI_SSID` or `>>ERR RANGE`
- `CFG SET_WIFI_PASS <pass>` → `>>OK SET_WIFI_PASS` or `>>ERR RANGE` (empty is valid, for an open network)

SSID/password are separate commands (rather than sharing one line like `SET_LOCATION`'s two floats) because either can legitimately contain spaces — each takes the rest of its line verbatim.

Web Serial is Chrome/Edge desktop only (no Safari, no iOS on any browser).

### Display

All drawing calls go through the `EPaper` object (wraps TFT_eSPI, provided by the [Seeed_GFX](https://github.com/Seeed-Studio/Seeed_GFX) fork declared in `platformio.ini`'s `lib_deps`). The `EPAPER_ENABLE` preprocessor guard wraps every display-related block — it's defined transitively by Seeed_GFX's `Setup502_Seeed_XIAO_EPaper_7inch5.h` (selected via `platformio.ini`'s `BOARD_SCREEN_COMBO=502` build flag), not by the firmware itself; remove that build flag or swap it for a non-epaper combo ID to compile-test without the display.

Getting `BOARD_SCREEN_COMBO`/`USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` into Seeed_GFX has to happen via `build_flags`, not a project header: Arduino IDE normally lets a sketch-root `driver.h` reach the library through `__has_include`, because the IDE adds the whole sketch folder to every compiler invocation's include path. PlatformIO's Library Dependency Finder scopes each library's own include path independently, so a project `include/driver.h` is invisible to `Seeed_GFX/TFT_eSPI.cpp` when it's compiled as a library object — only `build_flags`, which PlatformIO applies globally, reliably reach it.

The clock face renders hour/minute using bitmap glyphs from `BigDigits.h` (extracted from FreeSerifBold 160pt, digits only, stored in flash via `PROGMEM`). The colon is drawn as two filled circles. Weather and battery status appear in the bottom corners.

### Battery

GPIO1 (A0) is the ADC input; GPIO6 (A5) enables/disables the ADC voltage divider. The voltage is averaged over 30 reads with a calibration factor (`CALIBRATION_FACTOR = 0.968`). Full range is 3.2V–4.1V for a single-cell LiPo.

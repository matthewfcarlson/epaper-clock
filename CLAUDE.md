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

// OTA-failure relay (optional) — see "OTA failure reporting" below and
// cloudflare-worker/. Leave blank to skip auto-reporting (the click-to-
// report flow in docs/provision.html still works without this).
const char *OTA_REPORT_ENDPOINT = "";
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

**JPEG export:** passing a filename saves every refresh as a numbered JPEG and exits after one power-on cycle — `clock.jpg` produces `clock_01.jpg` (the "Syncing…" screen, only on first boot) and `clock_02.jpg` (the clock face). On subsequent boots NTP is already synced so only the clock frame is saved. One cycle never reaches a second wake, though, which is where `refreshClockDisplay()` actually takes the primed-partial-refresh path (see "Partial refresh" under "Display" below) — set `SIM_CYCLES` to run more power-on cycles before exiting, e.g. `SIM_CYCLES=6 ./sim clock.jpg` saves boot + 5 more wakes' worth of frames. Multi-cycle export also advances the simulator's clock by each cycle's actual computed sleep duration (rather than only whatever real wall-clock time elapses), so the saved frames show real time-of-day transitions — including minute/hour rollovers — worth eyeballing for correctness after touching anything in the partial-refresh path.

**Forcing a time (e.g. to see night-time behavior):** set `SIM_TIME` to freeze the simulator's clock, either as `HH:MM` (today, in the device's configured `TZ_INFO`) or a raw Unix epoch:

```bash
SIM_TIME=03:30 ./sim night.jpg   # 11pm-6am night window: 15-min wake cadence, weather fetch skipped, nightly GitHub check runs
```

With `SIM_CYCLES=1` (the default), the clock stays frozen at that instant for the whole run — rerun with a different value to look elsewhere. With `SIM_CYCLES` > 1 it's just the starting point; the clock still advances between cycles as described above. See `applySimTimeOverride()` in `simulator/main.cpp` and `g_sim_time_override` in `simulator/stubs/time_compat.h`.

**How it works:** `simulator/main.cpp` `#include`s `src/main.cpp` directly as a C++ translation unit (its `config.h`/`version.h`/`BigDigits.h`/`ota_health.h` includes resolve against `src/`, since that's `main.cpp`'s own directory), and the Makefile separately compiles `src/ota_health.cpp` as its own translation unit and links it in. `simulator/stubs/` provides thin header replacements for every Arduino/ESP32 API (`Arduino.h`, `WiFi.h`, `WiFiClientSecure.h`, `Update.h`, `HTTPClient.h`, `esp_sleep.h`, `esp_system.h`, `esp_ota_ops.h`, `time_compat.h`, `TFT_eSPI.h`). `EPaperSim.h` implements the `EPaper` class using an SDL2 renderer backed by a persistent render-target texture (`SDL_TEXTUREACCESS_TARGET`) so frames are always readable for JPEG export regardless of backbuffer swap behaviour. Text is rendered via SDL_ttf using the system SFNS font. JPEG encoding uses the bundled `stb_image_write.h` (no extra dependency).

The simulator is a standalone `make`-based build, independent of PlatformIO — it doesn't link against Seeed_GFX or the real ESP32 Arduino core at all, only its own stubs.

**Ghosting simulation.** `simulator/EPaperGhostSim.h` implements the `EPaperPartial` class that `src/epaper_partial.h` selects for `EPAPER_SIM_BUILD` (see "Partial refresh" under "Display" below for the real-hardware side of this). It models the UC8179 controller's old/new diffing closely enough to actually catch a bad priming buffer: it tracks a simulated "physical panel" buffer (bistable — persists across everything) separately from a simulated "controller SRAM old-image belief" buffer (cleared on every simulated sleep, exactly like the real chip), and a refresh only renders each pixel correctly where those two agree; everywhere they don't, it renders a visible dither-noise pattern instead of the real content — so a run only looks clean when `refreshClockDisplay()`'s reconstructed priming buffer genuinely matches what's really on the simulated panel. `pushPrimedPartial()` (the real fix) explicitly sets the controller's belief before diffing, so it's clean whenever the reconstruction is right; `updataPartial()` (kept, unused by `main.cpp`, faithfully reproducing the original bug) never does, so it's a way to reproduce the original ghosting on demand — swap it in for `pushPrimedPartial()`'s call in `refreshClockDisplay()` temporarily to see it. Since the app's actual text/digit rendering goes through `src/sdf_font.h`'s software rasterizer (calls `drawPixel()` directly, not SDL_ttf), this catches real content, not just shapes — run `SIM_CYCLES=6 ./sim clock.jpg` (or more) and look at the saved frames after touching `refreshClockDisplay()`, `ClockSnapshot`, or either `EPaperPartial`.

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
| `src/ota_health.h` / `src/ota_health.cpp` | `OtaHealth` — bootloader-rollback safety net and durable failure record for the GitHub-releases update path (see "Firmware auto-update" and "OTA failure reporting" below) |
| `src/epaper_partial.h` | `EPaperPartial` — adds a real primed partial-refresh path on top of Seeed_GFX's `EPaper` (see "Display" below) |
| `cloudflare-worker/` | Optional Cloudflare Worker that relays OTA-failure reports to a GitHub issue via a GitHub App, so the device itself never holds a GitHub credential — see its own `README.md` and "OTA failure reporting" below |

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

#### OTA failure reporting

A failed update used to be visible only in the Serial log for whoever happened to be watching at the time — nothing survived to the next reboot. `OtaHealth` now keeps a durable (NVS-backed, survives power loss) record of the most recent OTA failure, recorded from three places: a failed download/flash (`downloadAndFlashFirmware()` returning false in `checkForFirmwareUpdate()`, `src/main.cpp` — e.g. the DNS/HTTP failure fetching the release asset, or a `Update.h` write error), a completed rollback detected on the next boot (`OtaHealth::checkBootHealth()`), and a forced rollback after exceeding `OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS`. Each record has a `reason` (`download_flash` | `boot_rollback` | `unconfirmed_boot`), the version that was being attempted, a short machine-readable `detail` token (HTTP status / `Update.errorString()` / reset reason — spaces stripped via `sanitizeToken()` so it fits the wire protocol below unescaped), and a timestamp. A later successful update (`downloadAndFlashFirmware()` succeeding) clears it automatically; nothing else does except an explicit dismissal.

It's surfaced three ways:
- **Clock face**: `drawClock()`/`drawNightClock()` show a small "UPDATE FAILED - PRESS BUTTON" hint (top-right on the day face, below the caption on the night face) whenever `otaHealth.hasFailure()` is true — part of `ClockSnapshot` like the WiFi/location hints, so it round-trips correctly through the partial-refresh priming reconstruction (see "Partial refresh" below).
- **Serial CFG protocol** (see "Location & WiFi provisioning" below for the wire protocol this extends): `CFG GET_OTA_STATUS` → `>>OTA_STATUS failed=0|1 attempted=<ver> reason=<str> detail=<str> at=<epoch> reported=0|1`, and `CFG CLEAR_OTA_STATUS` → `>>OK CLEAR_OTA_STATUS` to dismiss it. `CFG GET_STATUS`'s reply also grew `owner=<OTA_GITHUB_OWNER> repo=<OTA_GITHUB_REPO> report=0|1` fields for the next two bullets.
- **`docs/provision.html`**: on connect, the page reads `CFG GET_OTA_STATUS` and, if a failure is pending, shows it with a "Report on GitHub" button. That button opens `https://github.com/{owner}/{repo}/issues/new?title=...&body=...` (prefilled from the failure record, plus a tail of the log — see below) in a new tab — filed under the user's own GitHub login, in their browser. This deliberately does *not* have the device call the GitHub Issues API directly: that would require embedding an issues-scoped token in the compiled firmware, and — same problem as WiFi credentials (see "Location & WiFi provisioning" below) — the release binary is public, so any such token would be extractable and abusable by anyone who downloaded it. A "Dismiss" button sends `CFG CLEAR_OTA_STATUS`.

**Log ring buffer.** A one-line `reason`/`detail` is a starting point, not the full picture, so `otaLogBuf` (`src/main.cpp`, `RTC_DATA_ATTR`, `OTA_LOG_BUF_SIZE` = 768 bytes) keeps the actual trail of recent OTA-path log lines — the `otaLog()` helper is a drop-in replacement for `Serial.print*()` used only at the handful of call sites in `fetchLatestRelease()`/`downloadAndFlashFirmware()`/`checkForFirmwareUpdate()` that matter for diagnosing a failure (not the whole firmware's chatty output — battery/weather/NTP logging stays plain `Serial.print*()`, both to keep the buffer small enough for RTC slow memory and because that chatter would just evict the OTA trail that actually matters). It's a self-pruning append buffer (drops the oldest whole line(s) once full, via `memmove`), not a true wraparound ring, and — like the rest of this block's `RTC_DATA_ATTR` state — survives deep sleep but not power loss; a power-loss crash has no log to save anyway. `CFG GET_OTA_LOG` → `>>OTA_LOG <escaped text>` exposes it (`escapeOtaLog()` turns `\` and embedded newlines into `\\`/`\n` so multi-line content still fits the CFG protocol's one-reply-per-line shape; `provision.html`'s `unescapeOtaLog()` reverses it). `docs/provision.html` fetches this alongside the failure status and offers a "Download log" button (plain client-side `Blob`/`createObjectURL`, no GitHub call) for the full text, plus folds a tail of it (last 1500 chars — GitHub's issue-creation URL silently truncates long bodies) into the "Report on GitHub" prefill.

**Auto-report via the relay Worker (optional).** `cloudflare-worker/` is a small Cloudflare Worker (see its own `README.md` for full setup) that lets the device auto-file an OTA failure without holding a GitHub-scoped credential at all — the earlier design (the device holding a GitHub PAT directly) was replaced with this once it became clear that's a materially better tradeoff, not just a stylistic preference: NVS isn't encrypted in this project, so *any* long-lived credential resident there is one physical/USB extraction away from being usable indefinitely, from anywhere; a device-held GitHub PAT — even scoped to "Issues: write" on one repo — is still a credential that can act on GitHub on its own, whereas the shared secret described below can only ever ask this one rate-limited Worker endpoint to do something, nothing more, regardless of scope.

The device (`reportOtaFailure()`, `src/main.cpp`) POSTs the failure (reason, attempted version, detail, running firmware, timestamp, and the `otaLogBuf` trail above) as JSON to `OTA_REPORT_ENDPOINT` (`config.h` — blank by default, disabling this path entirely), authenticated with `X-Report-Token: <shared secret>`. That secret is provisioned the same way as WiFi credentials: `CFG SET_REPORT_TOKEN <token>` / `CFG CLEAR_REPORT_TOKEN` (`userReportToken`, `"clockcfg"` NVS namespace, keys `rtok_set`/`rtok`), and the `docs/provision.html` "OTA-failure auto-report" field, wired into the same save/reboot flow as WiFi/location. `reportOtaFailure()` is called once per wake right after `checkForFirmwareUpdate()`, regardless of the night-window/throttle gate that function applies to itself — so a failure recorded earlier in the same boot (e.g. by `checkBootHealth()`, before WiFi is up) still gets reported once WiFi is available.

The Worker itself, not the device, holds the real GitHub credential — a **GitHub App installation** rather than a personal account or a long-lived PAT, so filed issues are attributed to a real bot identity (e.g. `your-app-name[bot]`) and the actual credential GitHub sees is a token that auto-expires in about an hour. It rate-limits (per-source and overall, via Workers KV — see `RATE_LIMIT_PER_IP`/`RATE_LIMIT_GLOBAL` in `cloudflare-worker/src/index.js`) and dedupes server-side: it searches for an open issue already carrying the same failure signature (a `<!-- ota-failure-signature: reason:version -->` marker embedded in the issue body) and adds a comment to it instead of filing a new one, across *all* devices reporting through the same Worker — a more thorough dedup than the device alone can do from its own NVS.

Auto-filing is still deduplicated on the device side too, not per attempt: `OtaHealth` persists a `failureReported` flag (`fail_reported` NVS key) alongside the failure record itself, which `recordFailure()` only resets to `false` when the `(reason, attemptedVersion)` pair actually changes — a retry of the *same* failure (e.g. `download_flash` attempt 2/3 of the same tag, which calls `recordFailure()` again just to refresh the attempt count in `detail`) doesn't get re-sent, and — because this lives in NVS rather than the RTC-memory retry counters (`otaFailedVersion`/`otaFailCount`) — neither does a repeat of the same failure surviving a power cycle that reset those. `reportOtaFailure()` checks `!otaHealth.failureReported()` before doing anything and calls `otaHealth.markFailureReported()` only once the Worker actually returns 200; a failed POST (no WiFi, relay down) leaves it `false` so the next wake retries. The Worker's own server-side dedup is a second, independent layer on top of this — it's what catches the case this device-side flag can't: a power cycle wiping `otaFailedVersion`/`otaFailCount` (RTC memory) triggering a fresh round of download/flash *attempts*, which is a distinct concern from *re-filing a report* for what's still, from GitHub's perspective, the same underlying failure.

This is still opt-in and still not the only path: the click-to-report flow above (no relay, no shared secret, just the user's own browser) keeps working identically regardless of whether `OTA_REPORT_ENDPOINT`/the shared secret are set.

### Location & WiFi provisioning

Weather's lat/lon and the device's WiFi credentials are no longer build-time constants — both are set by the user at runtime and stored in NVS via the ESP32 `Preferences` library (namespace `"clockcfg"`, keys `loc_set`/`lat`/`lon` for location, `wifi_set`/`ssid`/`pass` for WiFi), so one firmware build works for any device/location/network and there's nothing device-specific to bake in or leak via a repo secret or a public release binary. Until location is set, weather fetching is skipped entirely (no bogus `0,0` lookup); until WiFi is set (and no compiled-in `config.h` fallback is present — see below), `connectWiFi()` skips connecting entirely rather than blocking on a doomed attempt. Either way the clock face shows a short "press button + connect USB" hint in the weather corner naming whichever is missing.

`connectWiFi()` prefers the NVS-stored credentials but falls back to the compiled-in `WIFI_SSID`/`WIFI_PASS` (`config.h`) when NVS hasn't been provisioned yet. `config.h` is gitignored and is only a local dev convenience — the public release build compiles those two fields blank, so the distributed `.bin` never contains a real network's credentials. A freshly flashed device has no working fallback and must be provisioned once over serial before it can reach WiFi at all; from then on the NVS-stored credentials persist across every future OTA update (NVS isn't touched by an app-partition flash) independently of what's compiled into any given release. `setup()` also runs a one-time migration (`src/main.cpp`, right after `loadWifiConfig()`): if NVS isn't configured yet but a compiled-in `WIFI_SSID` is present, it's copied into NVS immediately, so a device updating from an older, pre-migration release picks up its working credentials without needing manual serial provisioning.

Setting either: press a button to enter the maintenance window (see above), plug in a USB cable, and open `docs/provision.html` (hosted via GitHub Pages, or served locally with e.g. `python3 -m http.server` from `docs/` — Web Serial requires a secure or localhost context) in Chrome or Edge on a computer. The page uses the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API) to talk to the device over the same native-USB CDC connection already used for flashing/`pio device monitor` — no new USB descriptors, no pairing. It reads the device's current status and lets you enter WiFi SSID/password and lat/lon manually, plus lat/lon via the browser's geolocation.

Wire protocol, plain text lines over the existing `Serial` (commands prefixed `CFG `, replies prefixed `>>` so the page can filter them out of ordinary debug logging on the same stream) — the authoritative copy of this list is the comment above `handleSerialProvisioning()` in `src/main.cpp`:
- `CFG GET_STATUS` → `>>STATUS configured=0|1 lat=<f> lon=<f> fw=<version> wifi=0|1 owner=<gh_owner> repo=<gh_repo> report=0|1`
- `CFG SET_LOCATION <lat> <lon>` → `>>OK SET_LOCATION` or `>>ERR RANGE|PARSE`
- `CFG GET_WIFI_SSID` → `>>WIFI_SSID <ssid>` (blank when unset) — the password is never echoed back
- `CFG SET_WIFI_SSID <ssid>` → `>>OK SET_WIFI_SSID` or `>>ERR RANGE`
- `CFG SET_WIFI_PASS <pass>` → `>>OK SET_WIFI_PASS` or `>>ERR RANGE` (empty is valid, for an open network)
- `CFG GET_OTA_STATUS` / `CFG CLEAR_OTA_STATUS`, `CFG GET_OTA_LOG`, `CFG SET_REPORT_TOKEN <token>` / `CFG CLEAR_REPORT_TOKEN` — see "OTA failure reporting" above
- `CFG REBOOT` → `>>OK REBOOT`, then restarts immediately into a normal (non-button) wake cycle instead of waiting out the rest of the maintenance window

SSID/password/PAT are separate commands (rather than sharing one line like `SET_LOCATION`'s two floats) because SSID and password can legitimately contain spaces — each takes the rest of its line verbatim (the PAT command follows the same shape for consistency, though tokens themselves never contain spaces).

Web Serial is Chrome/Edge desktop only (no Safari, no iOS on any browser).

### Display

All drawing calls go through the `EPaper` object (wraps TFT_eSPI, provided by the [Seeed_GFX](https://github.com/Seeed-Studio/Seeed_GFX) fork declared in `platformio.ini`'s `lib_deps`). The `EPAPER_ENABLE` preprocessor guard wraps every display-related block — it's defined transitively by Seeed_GFX's `Setup502_Seeed_XIAO_EPaper_7inch5.h` (selected via `platformio.ini`'s `BOARD_SCREEN_COMBO=502` build flag), not by the firmware itself; remove that build flag or swap it for a non-epaper combo ID to compile-test without the display.

Getting `BOARD_SCREEN_COMBO`/`USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` into Seeed_GFX has to happen via `build_flags`, not a project header: Arduino IDE normally lets a sketch-root `driver.h` reach the library through `__has_include`, because the IDE adds the whole sketch folder to every compiler invocation's include path. PlatformIO's Library Dependency Finder scopes each library's own include path independently, so a project `include/driver.h` is invisible to `Seeed_GFX/TFT_eSPI.cpp` when it's compiled as a library object — only `build_flags`, which PlatformIO applies globally, reliably reach it.

The clock face renders hour/minute using bitmap glyphs from `BigDigits.h` (extracted from FreeSerifBold 160pt, digits only, stored in flash via `PROGMEM`). The colon is drawn as two filled circles. Weather and battery status appear in the bottom corners.

**Partial refresh.** `refreshClockDisplay()` (`src/main.cpp`) does a real partial refresh whenever it safely can, instead of a full flash on every wake. Stock Seeed_GFX can't do this here: `EPaper::update()` (full refresh) pushes both old and new image data from the same buffer, so it never depends on anything surviving between calls, but `EPaper::updataPartial()` only pushes new data and assumes the UC8179 controller's SRAM already holds an accurate old image — untrue in this app, since the panel is put to sleep (`EPD_SLEEP`, which clears that SRAM) after every display write, and the MCU itself deep-sleeps between every wake. Used as-is, `updataPartial()` diffs the new frame against stale/garbage SRAM and corrupts the display immediately rather than merely ghosting it over time.

`src/epaper_partial.h` adds `EPaperPartial : public EPaper`, a small subclass (not a fork — `_img8`/`_width`/`_height` are `protected` in the base class, and the UC8179 command macros are already globally visible via `TFT_eSPI.h`) with one method, `pushPrimedPartial(oldImg)`, that pushes an explicit old-image buffer before the new one and triggers the refresh — full screen only, since this app never does a sub-rectangle update. `refreshClockDisplay()` supplies that old-image buffer by reconstructing the previous wake's actual frame: it persists a `ClockSnapshot` (everything the draw functions read — time, weather, battery, location/WiFi-configured flags) in `RTC_DATA_ATTR` after every refresh, and on the next wake redraws that snapshot into the live sprite, copies it out via `getPointer()`, then draws the real current frame over it before calling `pushPrimedPartial()`. Falls back to a genuine full `update()` (which needs no history) whenever there's no previous snapshot yet, the firmware that drew it isn't this build (different builds can render fonts/layout differently, so a reconstruction drawn by *this* firmware wouldn't match what's physically on the panel), or a 24-hour safety-net interval is due — regardless of correct priming data, an occasional full flash guards against analog drift the panel accumulates on its own.

The simulator has no controller SRAM or waveform to prime, so `EPAPER_SIM_BUILD` (defined by `simulator/Makefile`) selects a trivial stand-in `EPaperPartial` in the same file that just calls `update()` and ignores the priming buffer.

### Battery

GPIO1 (A0) is the ADC input; GPIO6 (A5) enables/disables the ADC voltage divider. The voltage is averaged over 30 reads with a calibration factor (`CALIBRATION_FACTOR = 0.968`). Full range is 3.2V–4.1V for a single-cell LiPo.

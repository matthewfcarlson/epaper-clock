// Simulator entry point.
// Includes the sketch source directly; stub headers shadow the real Arduino/ESP32 headers.

#include <cctype>
#include <cstdio>
#include <cstdlib>

// Pull in stubs before the sketch so its #include <X.h> resolves to our versions
#include "EPaperSim.h"
#include "stubs/Arduino.h"
#include "stubs/SPI.h"
#include "stubs/TFT_eSPI.h"
#include "stubs/WiFi.h"
#include "stubs/WiFiClientSecure.h"
#include "stubs/Update.h"
#include "stubs/HTTPClient.h"
#include "stubs/esp_sleep.h"
#include "stubs/esp_system.h"
#include "stubs/esp_ota_ops.h"
#include "stubs/time_compat.h"
#include "stubs/Preferences.h"

// Enable the display code path (same flag the sketch uses to guard display functions)
#define EPAPER_ENABLE

// Skip the 60-second first-boot IDE grace period and the maintenance listen window
#undef  FIRST_BOOT_AWAKE_MS
#define FIRST_BOOT_AWAKE_MS 0
#undef  PROVISION_LISTEN_MS
#define PROVISION_LISTEN_MS 0

// Globals declared extern in esp_sleep.h / esp_system.h
bool     g_sleep_requested = false;
uint64_t g_sleep_us        = 0;
bool     g_has_deep_slept  = false;

// Include the sketch as a C++ translation unit.
// Relative #includes inside main.cpp (config.h, version.h, BigDigits.h)
// resolve against its own directory, i.e. ../src.
#include "../src/main.cpp"

// Reads SIM_TIME from the environment and, if set, freezes the simulator's
// notion of "now" (see g_sim_time_override in stubs/time_compat.h) so
// time-of-day-dependent behavior — night wake cadence, NTP/weather sync
// gating, the nightly firmware-check window — can be exercised without
// waiting for real time to pass. The clock stays frozen at that instant for
// the whole run (it doesn't keep ticking), which is enough to see what a
// given moment looks like — rerun with a different value to look elsewhere.
//   SIM_TIME=03:30 ./sim          — freeze at 3:30 AM today, device's own TZ
//   SIM_TIME=1732000000 ./sim     — freeze at a raw Unix epoch
static void applySimTimeOverride() {
    const char *simTime = getenv("SIM_TIME");
    if (!simTime || !*simTime) return;

    bool allDigits = true;
    for (const char *p = simTime; *p; p++) {
        if (!isdigit((unsigned char)*p)) { allDigits = false; break; }
    }
    if (allDigits) {
        g_sim_time_override = (time_t)strtoll(simTime, nullptr, 10);
        printf("SIM_TIME: freezing simulator clock at epoch %lld\n", (long long)g_sim_time_override);
        return;
    }

    int hour = -1, minute = 0;
    if (sscanf(simTime, "%d:%d", &hour, &minute) < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        fprintf(stderr, "SIM_TIME: couldn't parse \"%s\" (expected HH:MM or a Unix epoch) — ignoring\n", simTime);
        return;
    }

    // Set the device's own configured timezone (config.h's TZ_INFO) before
    // computing the target epoch, same as setup() itself does — so "today
    // at HH:MM" lands in the device's TZ rather than the Mac's.
    setenv("TZ", TZ_INFO, 1);
    tzset();

    time_t now = time(nullptr);
    struct tm local = *localtime(&now);
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    g_sim_time_override = mktime(&local);
    printf("SIM_TIME: freezing simulator clock at %02d:%02d today (TZ=%s)\n", hour, minute, TZ_INFO);
}

int main(int argc, char **argv) {
    // Optional positional argument: output JPEG base path.
    //   ./sim              — interactive, live-updating window
    //   ./sim clock.jpg    — saves clock_01.jpg (syncing), clock_02.jpg (clock face), exits
    if (argc >= 2) {
        epaper.setExportPath(argv[1]);
    }

    applySimTimeOverride();

    // Simulator convenience: pre-seed the weather location as though the
    // device had already been provisioned, so the clock face and weather
    // fetch path can be exercised without faking Serial input over the real
    // provisioning protocol (see handleSerialProvisioning() in main.cpp).
    // saveLocationConfig() is the exact function that protocol calls, so
    // this persists into the stubbed Preferences store the same way a real
    // "CFG SET_LOCATION" command would. Default: zip 78681 (Round Rock, TX).
    saveLocationConfig(30.5218f, -97.7193f);

    // Each iteration of this loop simulates one power-on cycle.
    // RTC_DATA_ATTR globals (plain globals here) persist across iterations,
    // just as they would survive deep sleep on real hardware.
    while (true) {
        g_sleep_requested = false;
        g_sleep_us        = 0;

        setup();

        while (!g_sleep_requested) {
            loop();
        }

        // In export mode: all frames have been saved; exit cleanly.
        if (epaper.exportMode()) {
            epaper.cleanup(0);
        }

        // Interactive mode: simulate the sleep with a brief pause (capped at 5 s)
        uint64_t pause_ms = g_sleep_us / 1000;
        if (pause_ms > 5000) pause_ms = 5000;
        if (pause_ms < 200)  pause_ms = 200;

        uint64_t deadline = SDL_GetTicks() + pause_ms;
        while (SDL_GetTicks() < deadline) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) {
                    epaper.cleanup(0);
                }
            }
            SDL_Delay(50);
        }
    }
}

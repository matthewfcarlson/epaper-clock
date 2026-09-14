// Simulator entry point.
// Includes the sketch source directly; stub headers shadow the real Arduino/ESP32 headers.

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

// Globals declared extern in esp_sleep.h
bool     g_sleep_requested = false;
uint64_t g_sleep_us        = 0;

// Include the sketch as a C++ translation unit.
// Relative #includes inside main.cpp (config.h, version.h, BigDigits.h)
// resolve against its own directory, i.e. ../src.
#include "../src/main.cpp"

int main(int argc, char **argv) {
    // Optional positional argument: output JPEG base path.
    //   ./sim              — interactive, live-updating window
    //   ./sim clock.jpg    — saves clock_01.jpg (syncing), clock_02.jpg (clock face), exits
    if (argc >= 2) {
        epaper.setExportPath(argv[1]);
    }

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

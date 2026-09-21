#pragma once

// Minimal stand-in for ESP-IDF's esp_system.h — just enough for ota_health.cpp's
// reset-reason logging and main.cpp's "force an OTA check on any non-deep-sleep
// startup" gate (see freshStart in setup()). The simulator has no real reset
// history, so it approximates: ESP_RST_POWERON for the process's first boot,
// then ESP_RST_DEEPSLEEP for every iteration after its first
// esp_deep_sleep_start() call (see g_has_deep_slept in stubs/esp_sleep.h) —
// enough to exercise both branches of that gate, though it can't distinguish
// a real reset/watchdog/brownout from a fresh flash the way hardware can.

typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
} esp_reset_reason_t;

typedef int esp_err_t;
#define ESP_OK 0

extern bool g_has_deep_slept;
inline esp_reset_reason_t esp_reset_reason() {
    return g_has_deep_slept ? ESP_RST_DEEPSLEEP : ESP_RST_POWERON;
}

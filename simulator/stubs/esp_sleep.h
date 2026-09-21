#pragma once
#include <stdint.h>

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
} esp_sleep_wakeup_cause_t;

#define ESP_EXT1_WAKEUP_ANY_LOW  0
#define ESP_EXT1_WAKEUP_ANY_HIGH 1

// Set by esp_deep_sleep_start(); checked by main.cpp to end the loop() cycle
extern bool g_sleep_requested;
extern uint64_t g_sleep_us;

// Set by esp_deep_sleep_start(), read by stubs/esp_system.h's
// esp_reset_reason() — lets the simulator distinguish "resumed from our own
// deep sleep" (every iteration after the process's first) from any other
// kind of startup, the same way real hardware's reset reason does. See
// esp_system.h for why this matters.
extern bool g_has_deep_slept;

inline void esp_sleep_enable_timer_wakeup(uint64_t us) { g_sleep_us = us; }
inline void esp_sleep_enable_ext1_wakeup(uint64_t, int) {}
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() {
    return ESP_SLEEP_WAKEUP_TIMER;
}
inline void esp_deep_sleep_start() {
    g_sleep_requested = true;
    g_has_deep_slept = true;
    // Returns immediately; main.cpp drives the actual pause
}

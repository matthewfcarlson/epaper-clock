#pragma once
#include "esp_system.h"
#include "esp_sleep.h"

// Real bootloader-rollback semantics don't apply to a single-binary Mac build,
// so these just report success without touching any partition. The forced-
// rollback path in OtaHealth::checkBootHealth() still needs to "not return" —
// simulate that the same way esp_deep_sleep_start() does: flag the run loop
// to end this setup()/loop() cycle and start a fresh one.
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback() { return ESP_OK; }
inline esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
    g_sleep_requested = true;
    g_sleep_us = 0;
    return ESP_OK;
}

#pragma once

#include <Arduino.h>
#include <Preferences.h>

/**
 * OTA rollback safety net for the GitHub-releases auto-update path (see
 * checkForFirmwareUpdate() in main.cpp).
 *
 * The ESP-IDF bootloader's app-rollback feature (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
 * and core-dump-to-flash are already compiled into the stock Arduino-ESP32 core for
 * esp32s3, and this board's own default partition table (default_8MB.csv) already
 * provides two OTA app slots — nothing to change in platformio.ini for this.
 *
 *  - Bootloader rollback only protects against a boot-time crash/panic on the pending
 *    (freshly-flashed) partition: if it resets before the app confirms itself, the
 *    *next* boot's bootloader sees the still-pending state and switches back to the
 *    previous partition on its own, before any app code runs. That's automatic and
 *    needs no help from us except eventually calling esp_ota_mark_app_valid_cancel_rollback()
 *    once we're confident (else the device is stuck unable to accept another OTA).
 *  - It does NOT protect against firmware that boots fine but never manages to prove
 *    itself (e.g. a WiFi regression that leaves every wake cycle failing before it
 *    gets anywhere). That's this class's NVS-tracked boot-attempt counter: after
 *    OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS wake cycles without confirmHealthy() being
 *    called, we force a rollback ourselves via esp_ota_mark_app_invalid_rollback_and_reboot().
 *
 * State is kept in NVS (not RTC_DATA_ATTR) so a boot-looping firmware that brownouts
 * or power-cycles instead of cleanly deep-sleeping doesn't lose count.
 */
#define OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS 3

class OtaHealth {
public:
    // Loads persisted state from NVS. Call once, as early as possible in setup().
    void begin();

    // Call right after a successful OTA flash, before ESP.restart() — records what
    // we're attempting so the next boot(s) can tell whether it stuck.
    void recordOtaAttempt(const String& fromVersion, const String& toVersion);

    // Call as early as possible in setup(), right after begin(). Detects a completed
    // rollback (bootloader- or self-triggered) and forces a rollback if the pending
    // version has failed to confirm itself too many wake cycles in a row. May not
    // return (calls esp_restart() internally) in that last case.
    void checkBootHealth();

    // Call once a wake cycle has proven the running firmware works (reached the end
    // of setup() without crashing/hanging). No-op if no OTA is pending confirmation.
    void confirmHealthy();

    // True for exactly the one checkBootHealth() call where this boot is running
    // different firmware than the last wake cycle did (a fresh OTA flash landing, or
    // a rollback reverting to the previous version). The caller uses this to force a
    // full e-paper refresh, since a partial refresh over a frame drawn by different
    // firmware (different fonts/layout) is what produces visible ghosting artifacts.
    bool versionChangedThisBoot() const { return versionChangedThisBoot_; }

private:
    Preferences prefs_;
    String pendingVersion_;   // version we OTA'd to but haven't confirmed yet ("" = none pending)
    String previousVersion_;  // version we OTA'd from, for detecting a completed rollback
    uint32_t bootAttempts_ = 0;
    bool versionChangedThisBoot_ = false;

    void loadFromNVS();
    void savePendingOta();
    void clearPendingOta();
};

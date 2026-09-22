#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

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

    // Durable (NVS-backed, survives power loss) record of the most recent OTA
    // failure, for surfacing to the user (clock-face hint, serial CFG protocol,
    // docs/provision.html) — see "OTA failure reporting" in CLAUDE.md. Recorded
    // from three places: a failed download/flash (checkForFirmwareUpdate() in
    // main.cpp), a completed rollback detected on boot, and a forced rollback
    // after too many unconfirmed boots (both in checkBootHealth() below).
    // `reason` and `detail` are short, space-free tokens (not free text) so they
    // fit the line-based CFG wire protocol unescaped.
    void recordFailure(const String& reason, const String& attemptedVersion, const String& detail);
    bool hasFailure() const { return failurePresent_; }
    const String& failureReason() const { return failureReason_; }
    const String& failureAttemptedVersion() const { return failureAttempted_; }
    const String& failureDetail() const { return failureDetail_; }
    time_t failureTime() const { return failureTime_; }
    void clearFailure();

private:
    Preferences prefs_;
    String pendingVersion_;   // version we OTA'd to but haven't confirmed yet ("" = none pending)
    String previousVersion_;  // version we OTA'd from, for detecting a completed rollback
    uint32_t bootAttempts_ = 0;

    bool failurePresent_ = false;
    String failureReason_;
    String failureAttempted_;
    String failureDetail_;
    time_t failureTime_ = 0;

    void loadFromNVS();
    void savePendingOta();
    void clearPendingOta();
};

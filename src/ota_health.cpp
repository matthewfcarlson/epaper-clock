#include "ota_health.h"
#include "version.h"
#include <esp_system.h>
#include <esp_ota_ops.h>

static const char* NVS_NAMESPACE = "ota_health";
static const char* KEY_PEND_VER = "pend_ver";
static const char* KEY_PREV_VER = "prev_ver";
static const char* KEY_ATTEMPTS = "attempts";
static const char* KEY_LAST_VER = "last_ver";

static const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                 return "unknown";
    }
}

void OtaHealth::begin() {
    loadFromNVS();
    if (pendingVersion_.length() > 0) {
        Serial.printf("OtaHealth: OTA to %s pending confirmation (from %s, %u attempt(s) so far)\n",
                      pendingVersion_.c_str(), previousVersion_.c_str(), (unsigned)bootAttempts_);
    }
}

void OtaHealth::loadFromNVS() {
    prefs_.begin(NVS_NAMESPACE, true);  // read-only
    pendingVersion_ = prefs_.getString(KEY_PEND_VER, "");
    previousVersion_ = prefs_.getString(KEY_PREV_VER, "");
    bootAttempts_ = prefs_.getUInt(KEY_ATTEMPTS, 0);
    lastRunVersion_ = prefs_.getString(KEY_LAST_VER, "");
    prefs_.end();
}

void OtaHealth::savePendingOta() {
    prefs_.begin(NVS_NAMESPACE, false);
    prefs_.putString(KEY_PEND_VER, pendingVersion_);
    prefs_.putString(KEY_PREV_VER, previousVersion_);
    prefs_.putUInt(KEY_ATTEMPTS, bootAttempts_);
    prefs_.end();
}

void OtaHealth::clearPendingOta() {
    pendingVersion_ = "";
    previousVersion_ = "";
    bootAttempts_ = 0;
    prefs_.begin(NVS_NAMESPACE, false);
    prefs_.remove(KEY_PEND_VER);
    prefs_.remove(KEY_PREV_VER);
    prefs_.remove(KEY_ATTEMPTS);
    prefs_.end();
}

void OtaHealth::recordOtaAttempt(const String& fromVersion, const String& toVersion) {
    pendingVersion_ = toVersion;
    previousVersion_ = fromVersion;
    bootAttempts_ = 0;
    savePendingOta();
}

void OtaHealth::checkBootHealth() {
    // Detected independently of the pending-OTA state below, so it also catches a
    // manual USB reflash (which never goes through recordOtaAttempt()) — see the
    // comment on versionChangedThisBoot() in ota_health.h.
    versionChangedThisBoot_ = (lastRunVersion_ != FIRMWARE_VERSION);
    if (versionChangedThisBoot_) {
        Serial.printf("OtaHealth: firmware version changed since last boot (%s -> %s)\n",
                      lastRunVersion_.c_str(), FIRMWARE_VERSION);
        lastRunVersion_ = FIRMWARE_VERSION;
        prefs_.begin(NVS_NAMESPACE, false);
        prefs_.putString(KEY_LAST_VER, lastRunVersion_);
        prefs_.end();
    }

    bool hasPending = pendingVersion_.length() > 0;
    // We flashed `pendingVersion_` but are now back on `previousVersion_` without ever
    // confirming the new one — either the bootloader auto-rolled-back after a boot-time
    // crash, or a previous cycle force-rolled-back via mark_app_invalid_rollback_and_reboot()
    // below and this is that reboot landing.
    bool rolledBack = hasPending &&
                       strcmp(pendingVersion_.c_str(), FIRMWARE_VERSION) != 0 &&
                       strcmp(previousVersion_.c_str(), FIRMWARE_VERSION) == 0;
    bool isPendingVersion = hasPending && strcmp(pendingVersion_.c_str(), FIRMWARE_VERSION) == 0;

    if (rolledBack) {
        Serial.printf("OtaHealth: %s was rolled back to %s (reset reason: %s)\n",
                      pendingVersion_.c_str(), FIRMWARE_VERSION, resetReasonToString(esp_reset_reason()));
        clearPendingOta();
        return;
    }

    if (isPendingVersion) {
        bootAttempts_++;
        savePendingOta();
        Serial.printf("OtaHealth: unconfirmed OTA boot %u/%u on version %s (reset reason: %s)\n",
                      (unsigned)bootAttempts_, OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS, FIRMWARE_VERSION,
                      resetReasonToString(esp_reset_reason()));

        if (bootAttempts_ > OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS) {
            Serial.println("OtaHealth: exceeded max unconfirmed boots — forcing rollback to previous firmware");
            clearPendingOta();
            Serial.flush();
            esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
            // Only reaches here if the rollback couldn't be triggered (e.g. the previous
            // partition isn't valid either) — fall through and keep running this version.
            Serial.printf("OtaHealth: forced rollback call returned %d (expected not to return)\n", (int)err);
        }
    }
}

void OtaHealth::confirmHealthy() {
    if (pendingVersion_.length() == 0) return;
    if (strcmp(pendingVersion_.c_str(), FIRMWARE_VERSION) != 0) return;  // inconsistent state — let checkBootHealth resolve it next boot

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("OtaHealth: marked %s valid, rollback cancelled (err=%d)\n", FIRMWARE_VERSION, (int)err);
    clearPendingOta();
}

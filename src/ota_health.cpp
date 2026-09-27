#include "ota_health.h"
#include "version.h"
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <time.h>

static const char* NVS_NAMESPACE = "ota_health";
static const char* KEY_PEND_VER = "pend_ver";
static const char* KEY_PREV_VER = "prev_ver";
static const char* KEY_ATTEMPTS = "attempts";
static const char* KEY_FAIL_REASON = "fail_reason";
static const char* KEY_FAIL_ATTEMPTED = "fail_attempted";
static const char* KEY_FAIL_DETAIL = "fail_detail";
static const char* KEY_FAIL_CONTEXT = "fail_ctx";
static const char* KEY_FAIL_TIME = "fail_time";
static const char* KEY_FAIL_REPORTED = "fail_reported";
static const char* KEY_FAIL_REPORTED_AT = "fail_rep_at";

String OtaHealth::resetReasonToString(esp_reset_reason_t reason) {
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

// Diagnostic context every failure record carries regardless of who
// recorded it — cheap, always-available ESP-IDF figures that turn "it
// failed" into something worth debugging from: heap headroom (a low
// heap_min relative to heap_free hints at a leak or fragmentation), how
// long this boot had been running when the failure was recorded, and why
// the chip last reset. Callers (main.cpp) append their own extra context —
// WiFi/battery/checkpoint state — that this class has no visibility into.
static String gatherBaseContext() {
    char buf[96];
    snprintf(buf, sizeof(buf), "heap_free=%u heap_min=%u uptime_s=%lu reset=%s",
              (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
              (unsigned long)(millis() / 1000), OtaHealth::resetReasonToString(esp_reset_reason()).c_str());
    return String(buf);
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
    failureReason_ = prefs_.getString(KEY_FAIL_REASON, "");
    failureAttempted_ = prefs_.getString(KEY_FAIL_ATTEMPTED, "");
    failureDetail_ = prefs_.getString(KEY_FAIL_DETAIL, "");
    failureContext_ = prefs_.getString(KEY_FAIL_CONTEXT, "");
    failureTime_ = (time_t)prefs_.getUInt(KEY_FAIL_TIME, 0);
    failureReported_ = prefs_.getBool(KEY_FAIL_REPORTED, false);
    failureReportedAt_ = (time_t)prefs_.getUInt(KEY_FAIL_REPORTED_AT, 0);
    failurePresent_ = failureReason_.length() > 0;
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

void OtaHealth::recordFailure(const String& reason, const String& attemptedVersion, const String& detail,
                               const String& extraContext) {
    // A retry of the *same* failure (same reason + attempted version, just an
    // updated detail/attempt count) keeps whatever `reported` state it had;
    // a genuinely new one (different reason or version) gets a clean slate
    // so reportOtaFailure() knows to file it.
    bool isNewEpisode = !failurePresent_ ||
                         strcmp(reason.c_str(), failureReason_.c_str()) != 0 ||
                         strcmp(attemptedVersion.c_str(), failureAttempted_.c_str()) != 0;
    failurePresent_ = true;
    failureReason_ = reason;
    failureAttempted_ = attemptedVersion;
    failureDetail_ = detail;
    failureContext_ = gatherBaseContext();
    if (extraContext.length() > 0) failureContext_ = failureContext_ + " " + extraContext;
    failureTime_ = time(nullptr);
    if (isNewEpisode) {
        failureReported_ = false;
        failureReportedAt_ = 0;
    }
    prefs_.begin(NVS_NAMESPACE, false);
    prefs_.putString(KEY_FAIL_REASON, failureReason_);
    prefs_.putString(KEY_FAIL_ATTEMPTED, failureAttempted_);
    prefs_.putString(KEY_FAIL_DETAIL, failureDetail_);
    prefs_.putString(KEY_FAIL_CONTEXT, failureContext_);
    prefs_.putUInt(KEY_FAIL_TIME, (uint32_t)failureTime_);
    prefs_.putBool(KEY_FAIL_REPORTED, failureReported_);
    prefs_.putUInt(KEY_FAIL_REPORTED_AT, (uint32_t)failureReportedAt_);
    prefs_.end();
    Serial.printf("OtaHealth: recorded failure reason=%s attempted=%s detail=%s context=%s\n",
                  failureReason_.c_str(), failureAttempted_.c_str(), failureDetail_.c_str(), failureContext_.c_str());
}

void OtaHealth::markFailureReported() {
    if (!failurePresent_ || failureReported_) return;
    failureReported_ = true;
    failureReportedAt_ = time(nullptr);
    prefs_.begin(NVS_NAMESPACE, false);
    prefs_.putBool(KEY_FAIL_REPORTED, true);
    prefs_.putUInt(KEY_FAIL_REPORTED_AT, (uint32_t)failureReportedAt_);
    prefs_.end();
}

void OtaHealth::clearFailure() {
    if (!failurePresent_) return;
    failurePresent_ = false;
    failureReason_ = "";
    failureAttempted_ = "";
    failureDetail_ = "";
    failureContext_ = "";
    failureTime_ = 0;
    failureReported_ = false;
    failureReportedAt_ = 0;
    prefs_.begin(NVS_NAMESPACE, false);
    prefs_.remove(KEY_FAIL_REASON);
    prefs_.remove(KEY_FAIL_ATTEMPTED);
    prefs_.remove(KEY_FAIL_DETAIL);
    prefs_.remove(KEY_FAIL_CONTEXT);
    prefs_.remove(KEY_FAIL_TIME);
    prefs_.remove(KEY_FAIL_REPORTED);
    prefs_.remove(KEY_FAIL_REPORTED_AT);
    prefs_.end();
}

void OtaHealth::recordOtaAttempt(const String& fromVersion, const String& toVersion) {
    pendingVersion_ = toVersion;
    previousVersion_ = fromVersion;
    bootAttempts_ = 0;
    savePendingOta();
}

void OtaHealth::checkBootHealth() {
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
                      pendingVersion_.c_str(), FIRMWARE_VERSION, resetReasonToString(esp_reset_reason()).c_str());
        String detail = String("boot_reset_") + resetReasonToString(esp_reset_reason());
        recordFailure("boot_rollback", pendingVersion_, detail);
        clearPendingOta();
        return;
    }

    if (isPendingVersion) {
        bootAttempts_++;
        savePendingOta();
        Serial.printf("OtaHealth: unconfirmed OTA boot %u/%u on version %s (reset reason: %s)\n",
                      (unsigned)bootAttempts_, OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS, FIRMWARE_VERSION,
                      resetReasonToString(esp_reset_reason()).c_str());

        if (bootAttempts_ > OTA_MAX_UNCONFIRMED_BOOT_ATTEMPTS) {
            Serial.println("OtaHealth: exceeded max unconfirmed boots — forcing rollback to previous firmware");
            String detail = String("unconfirmed_") + (long)bootAttempts_ + "_boots_reset_" + resetReasonToString(esp_reset_reason());
            recordFailure("unconfirmed_boot", pendingVersion_, detail);
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

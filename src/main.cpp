/*
 * Big friendly digital clock for 7.5" e-paper display (TRMNL DIY kit)
 * Uses XIAO ESP32-C3/S3 with UC8179 800x480 mono ePaper
 *
 * Connects to WiFi, syncs time via NTP, draws the clock,
 * then enters deep sleep. Wakes every 1 min (day) or 5 min (night).
 * The ESP32 RTC keeps time across deep sleep cycles.
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <time.h>
#include "config.h"
#include "version.h"
#include "BigDigits.h"

// Sleep durations
#define SLEEP_DAY_US (1ULL * 60ULL * 1000000)  // 5 minutes
#define SLEEP_NIGHT_US (5ULL * 60 * 1000000)  // 15 minutes
#define NIGHT_START_HOUR 1
#define NIGHT_END_HOUR 6

#define BATTERY_PIN 1  // GPIO1 (A0) - BAT_ADC
#define ADC_EN_PIN 6   // GPIO6 (A5) - ADC_EN

// Define button pins
const int BUTTON_D1 = D1;  // First user button
const int BUTTON_D2 = D2;  // Second user button
const int BUTTON_D4 = D4;  // Third user button

const float CALIBRATION_FACTOR = 0.968;

// Battery voltage range (single-cell LiPo)
#define BATT_V_FULL 4.1
#define BATT_V_EMPTY 3.2

// NTP re-sync interval in seconds (6 hours)
#define NTP_SYNC_INTERVAL_S (6UL * 3600)

// How long to stay awake on first boot for IDE access (120 seconds)
#ifndef FIRST_BOOT_AWAKE_MS
#define FIRST_BOOT_AWAKE_MS 120000
#endif

// How long to listen for OTA after button wake (90 seconds)
#ifndef OTA_LISTEN_MS
#define OTA_LISTEN_MS 90000
#endif

// Weather update interval in seconds (6 hours)
#define WEATHER_SYNC_INTERVAL_S (6UL * 3600)

// How often to check GitHub for a newer firmware release, at night only (24 hours)
#define OTA_CHECK_INTERVAL_S (24UL * 60 * 60)
// Give up retrying a specific release after this many failed download/flash attempts
#define OTA_MAX_UPDATE_ATTEMPTS 3

// Persistent across deep sleep
RTC_DATA_ATTR uint32_t wakeCount = 0;
RTC_DATA_ATTR bool everSynced = false;
RTC_DATA_ATTR bool hasSleptOnce = false;
RTC_DATA_ATTR int weatherHigh = 0;
RTC_DATA_ATTR int weatherLow = 0;
RTC_DATA_ATTR char weatherDesc[24] = "";
RTC_DATA_ATTR bool weatherValid = false;
RTC_DATA_ATTR time_t lastNtpSyncTime = 0;
RTC_DATA_ATTR time_t lastWeatherSyncTime = 0;
RTC_DATA_ATTR uint8_t weatherRetries = 0;
// Epoch when the device last cold-booted (i.e. was last charged/plugged in).
// RTC memory clears on power loss, so this resets whenever the battery is charged.
RTC_DATA_ATTR time_t bootEpoch = 0;
#define WEATHER_MAX_RETRIES 3
// Firmware auto-update state
RTC_DATA_ATTR time_t lastOtaCheckTime = 0;
RTC_DATA_ATTR char otaFailedVersion[16] = "";
RTC_DATA_ATTR uint8_t otaFailCount = 0;

float lastVoltage = 0;
unsigned long setupDoneAt = 0;
bool otaMode = false;
unsigned long otaStartedAt = 0;

#ifdef EPAPER_ENABLE

EPaper epaper = EPaper();

#define SCREEN_W 800
#define SCREEN_H 480

void drawWifiIcon(int x, int y, bool connected) {
  uint16_t color = TFT_BLACK;

  // Outer arc
  for (int i = -40; i <= 40; i++) {
    float rad = (i - 90) * 0.0174532925;
    int px = x + cos(rad) * 20;
    int py = y + sin(rad) * 20;
    epaper.drawPixel(px, py, color);
    px = x + cos(rad) * 19;
    py = y + sin(rad) * 19;
    epaper.drawPixel(px, py, color);
  }

  // Middle arc
  for (int i = -40; i <= 40; i++) {
    float rad = (i - 90) * 0.0174532925;
    int px = x + cos(rad) * 14;
    int py = y + sin(rad) * 14;
    epaper.drawPixel(px, py, color);
    px = x + cos(rad) * 13;
    py = y + sin(rad) * 13;
    epaper.drawPixel(px, py, color);
  }

  // Inner arc
  for (int i = -35; i <= 35; i++) {
    float rad = (i - 90) * 0.0174532925;
    int px = x + cos(rad) * 8;
    int py = y + sin(rad) * 8;
    epaper.drawPixel(px, py, color);
    px = x + cos(rad) * 7;
    py = y + sin(rad) * 7;
    epaper.drawPixel(px, py, color);
  }

  // Center dot
  epaper.fillCircle(x, y, 2, color);

  // X over icon when disconnected
  if (!connected) {
    epaper.drawLine(x - 16, y - 18, x + 16, y + 4, color);
    epaper.drawLine(x - 15, y - 18, x + 17, y + 4, color);
    epaper.drawLine(x + 16, y - 18, x - 16, y + 4, color);
    epaper.drawLine(x + 17, y - 18, x - 15, y + 4, color);
  }
}

// Draw battery outline + fill at (x, y), 40x18px, with percentage text
void drawBatteryIcon(int x, int y, int percent) {
  // Battery body outline (36x18)
  epaper.drawRect(x, y, 36, 18, TFT_BLACK);
  epaper.drawRect(x + 1, y + 1, 34, 16, TFT_BLACK);
  // Battery nub on right
  epaper.fillRect(x + 36, y + 5, 4, 8, TFT_BLACK);

  // Fill proportional to percent (inner area is 30x12)
  int fillW = (30 * percent) / 100;
  if (fillW > 0) {
    epaper.fillRect(x + 3, y + 3, fillW, 12, TFT_BLACK);
  }

  // Percentage text to the left of the icon
  char buf[5];
  sprintf(buf, "%d%%", percent);
  epaper.setTextSize(1);
  // Right-align text just left of the battery icon
  int textW = epaper.textWidth(buf, 2);
  epaper.drawString(buf, x - textW - 4, y + 1, 2);
}

// Draw OTA listening screen — download arrow icon + text.
// wifiConnected controls whether we show the listen countdown + connection
// info, or a "connect failed" message (caller skips the listen window in
// that case, so this just explains why nothing happened).
void drawOtaScreen(bool wifiConnected) {
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2 - 60;

  // Download arrow: vertical bar
  epaper.fillRect(cx - 6, cy - 40, 12, 50, TFT_BLACK);
  // Arrowhead (triangle)
  epaper.fillTriangle(cx - 24, cy + 10, cx + 24, cy + 10, cx, cy + 40, TFT_BLACK);
  // Horizontal line (tray)
  epaper.fillRect(cx - 30, cy + 48, 60, 4, TFT_BLACK);

  epaper.setTextSize(1);
  epaper.drawCentreString("OTA Update Ready", cx, cy + 70, 4);

  char lineBuf[48];
  if (wifiConnected) {
    snprintf(lineBuf, sizeof(lineBuf), "Listening for %lus...", (unsigned long)(OTA_LISTEN_MS / 1000));
    epaper.drawCentreString(lineBuf, cx, cy + 100, 2);
    snprintf(lineBuf, sizeof(lineBuf), "epaper-clock.local  %s", WiFi.localIP().toString().c_str());
    epaper.drawCentreString(lineBuf, cx, cy + 130, 2);
  } else {
    epaper.drawCentreString("WiFi connect failed", cx, cy + 100, 2);
  }

  snprintf(lineBuf, sizeof(lineBuf), "fw %s", FIRMWARE_VERSION);
  epaper.drawCentreString(lineBuf, cx, cy + 160, 2);
}

// Draw "installing firmware update" screen shown during a nightly auto-update
void drawUpdateScreen(const String &newVersion) {
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2 - 20;

  epaper.setTextSize(1);
  epaper.drawCentreString("Installing Update", cx, cy, 4);

  char buf[40];
  snprintf(buf, sizeof(buf), "%s -> %s", FIRMWARE_VERSION, newVersion.c_str());
  epaper.drawCentreString(buf, cx, cy + 40, 2);
  epaper.drawCentreString("Do not unplug...", cx, cy + 70, 2);
}

// Format seconds-since-charge as a compact string into buf, e.g. "3d4h", "5h12m", "8m".
void formatUptime(time_t secs, char *buf, size_t bufLen) {
  if (secs < 0) secs = 0;
  long days = secs / 86400;
  long hours = (secs % 86400) / 3600;
  long mins = (secs % 3600) / 60;
  if (days > 0) {
    snprintf(buf, bufLen, "%ldd%ldh", days, hours);
  } else if (hours > 0) {
    snprintf(buf, bufLen, "%ldh%ldm", hours, mins);
  } else {
    snprintf(buf, bufLen, "%ldm", mins);
  }
}

int getBatteryPercent(float voltage) {
  int percent = (int)((voltage - BATT_V_EMPTY) / (BATT_V_FULL - BATT_V_EMPTY) * 100.0);
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;
  return percent;
}

void drawClock(float batteryVoltage) {
  struct tm timeinfo;
  int hour = 0, min = 0;

  if (getLocalTime(&timeinfo, 100)) {
    hour = timeinfo.tm_hour;
    min = timeinfo.tm_min;
  }

  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  // Convert to 12-hour format
  const char *ampm = (hour < 12) ? "AM" : "PM";
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;

  // Build full time string
  char hourStr[3];
  char minStr[3];
  sprintf(hourStr, "%d", hour12);
  sprintf(minStr, "%02d", min);

  // Measure pieces and center the whole time string
  int hourW = bigDigitsWidth(hourStr);
  int colonW = 50;
  int minW = bigDigitsWidth(minStr);
  int totalW = hourW + colonW + minW;
  int xStart = (SCREEN_W - totalW) / 2;
  int yPos = SCREEN_H / 2 + 80;  // baseline position

  drawBigDigits(epaper, xStart, yPos, hourStr);

  // Draw colon as two filled circles, centered on digit height
  // Digits span from yPos-215 (top) to yPos+5 (bottom), center = yPos-105
  int colonX = xStart + hourW + colonW / 2;
  int dotR = 14;
  int colonCenter = yPos - 105;
  int dotSpacing = 45;
  epaper.fillCircle(colonX, colonCenter - dotSpacing, dotR, TFT_BLACK);
  epaper.fillCircle(colonX, colonCenter + dotSpacing, dotR, TFT_BLACK);

  drawBigDigits(epaper, xStart + hourW + colonW, yPos, minStr);

  // AM/PM below the time
  epaper.setFreeFont(&FreeSans12pt7b);
  epaper.setTextSize(1);
  epaper.drawCentreString(ampm, SCREEN_W / 2, yPos + 30, 1);

  // Weather in bottom-left corner
  if (weatherValid) {
    epaper.setFreeFont(&FreeSans12pt7b);
    epaper.setTextSize(1);
    char weatherLine[48];
    snprintf(weatherLine, sizeof(weatherLine), "%s  %d°/%d°F", weatherDesc, weatherHigh, weatherLow);
    epaper.drawString(weatherLine, 10, SCREEN_H - 30, 1);
  }

  // Battery icon in bottom-right corner
  int battPercent = getBatteryPercent(batteryVoltage);
  drawBatteryIcon(SCREEN_W - 50, SCREEN_H - 28, battPercent);

  // Uptime since last charge, right-aligned just above the battery icon.
  // Clear the free font first: TFT_eSPI keeps the previously-set GFX free font
  // active as "font 1", so without this the uptime would render in 12pt instead
  // of the small GLCD font (the simulator doesn't replicate this quirk).
  if (bootEpoch > 0) {
    char uptimeBuf[16];
    formatUptime(time(nullptr) - bootEpoch, uptimeBuf, sizeof(uptimeBuf));
    epaper.setFreeFont(nullptr);
    epaper.setTextSize(1);
    int uptimeW = epaper.textWidth(uptimeBuf, 1);
    epaper.drawString(uptimeBuf, (SCREEN_W - 10) - uptimeW, SCREEN_H - 46, 1);
  }
}

uint64_t getSleepDuration() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) {
    int h = timeinfo.tm_hour;
    int s = timeinfo.tm_sec;

    // Seconds until the next whole minute
    int secsToNextMin = 60 - s;
    if (secsToNextMin <= 0) secsToNextMin = 60;

    bool isNight = (h >= NIGHT_START_HOUR) && (h < NIGHT_END_HOUR);
    int intervalMins = isNight ? 5 : 1;

    // For night mode, align to the next 5-minute mark
    if (isNight) {
      int m = timeinfo.tm_min;
      int minsToNext5 = intervalMins - (m % intervalMins);
      if (minsToNext5 == intervalMins && s == 0) minsToNext5 = intervalMins;
      uint64_t totalSecs = (uint64_t)(minsToNext5 - 1) * 60 + secsToNextMin;
      Serial.printf("Sleep: hour=%d, %llu seconds until next 5-min mark\n", h, totalSecs);
      return totalSecs * 1000000ULL;
    }

    Serial.printf("Sleep: hour=%d, %d seconds until next minute\n", h, secsToNextMin);
    return (uint64_t)secsToNextMin * 1000000ULL;
  }
  return SLEEP_DAY_US;
}

#endif

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" failed.");
  }
}

bool syncNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi — skipping NTP sync");
    return false;
  }

  Serial.println("Syncing NTP...");
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 500)) {
      Serial.println("NTP sync OK");
      Serial.printf("Time: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      return true;
    }
  }

  Serial.println("NTP sync failed");
  return false;
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi — skipping weather fetch");
    return false;
  }

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "http://api.openweathermap.org/data/2.5/weather?lat=%.2f&lon=%.2f&units=imperial&appid=%s",
    OWM_LAT, OWM_LON, OWM_API_KEY);

  Serial.println("Fetching weather...");
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("Weather HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  // Simple JSON parsing — find temp_min, temp_max, and weather description
  // Look for "temp_min": and "temp_max":
  int idx;

  idx = payload.indexOf("\"temp_min\":");
  if (idx >= 0) {
    weatherLow = (int)payload.substring(idx + 11).toFloat();
  }

  idx = payload.indexOf("\"temp_max\":");
  if (idx >= 0) {
    weatherHigh = (int)payload.substring(idx + 11).toFloat();
  }

  // Weather description: "description":"clear sky"
  idx = payload.indexOf("\"description\":\"");
  if (idx >= 0) {
    int start = idx + 15;
    int end = payload.indexOf("\"", start);
    String desc = payload.substring(start, end);
    // Capitalize first letter
    if (desc.length() > 0) {
      desc[0] = toupper(desc[0]);
    }
    strncpy(weatherDesc, desc.c_str(), sizeof(weatherDesc) - 1);
    weatherDesc[sizeof(weatherDesc) - 1] = '\0';
  }

  weatherValid = true;
  lastWeatherSyncTime = time(nullptr);
  Serial.printf("Weather: %s, High %d°F, Low %d°F\n", weatherDesc, weatherHigh, weatherLow);
  return true;
}

// ---- Firmware auto-update (GitHub Releases) ----

// Compares two "vMAJOR.MINOR.PATCH" (or "MAJOR.MINOR.PATCH") version strings.
// Returns >0 if a > b, <0 if a < b, 0 if equal.
int compareVersions(const String &a, const String &b) {
  auto parse = [](const String &vIn, int out[3]) {
    String v = vIn;
    if (v.length() > 0 && (v[0] == 'v' || v[0] == 'V')) v = v.substring(1);
    out[0] = out[1] = out[2] = 0;
    int part = 0, start = 0;
    for (int i = 0; i <= v.length() && part < 3; i++) {
      if (i == v.length() || v[i] == '.') {
        if (i > start) out[part] = v.substring(start, i).toInt();
        part++;
        start = i + 1;
      }
    }
  };
  int pa[3], pb[3];
  parse(a, pa);
  parse(b, pb);
  for (int i = 0; i < 3; i++) {
    if (pa[i] != pb[i]) return pa[i] - pb[i];
  }
  return 0;
}

// Fetches the latest GitHub release for OTA_GITHUB_OWNER/OTA_GITHUB_REPO.
// On success, fills tagOut with the release tag and assetUrlOut with the
// download URL of the first ".bin" asset attached to the release.
bool fetchLatestRelease(String &tagOut, String &assetUrlOut) {
  WiFiClientSecure client;
  client.setInsecure();  // hobby project: skip cert pinning, matches the plain-HTTP weather API's trust model
  HTTPClient http;
  char url[160];
  snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest",
           OTA_GITHUB_OWNER, OTA_GITHUB_REPO);

  http.begin(client, url);
  http.addHeader("User-Agent", "epaper-clock-ota");
  http.addHeader("Accept", "application/vnd.github+json");
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("GitHub release check HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  int idx = payload.indexOf("\"tag_name\":\"");
  if (idx < 0) return false;
  int start = idx + 12;
  int end = payload.indexOf("\"", start);
  if (end < 0) return false;
  tagOut = payload.substring(start, end);

  idx = payload.indexOf("\"browser_download_url\":\"");
  while (idx >= 0) {
    start = idx + 25;
    end = payload.indexOf("\"", start);
    if (end < 0) break;
    String candidate = payload.substring(start, end);
    if (candidate.length() > 4 && candidate.substring(candidate.length() - 4) == ".bin") {
      assetUrlOut = candidate;
      return true;
    }
    idx = payload.indexOf("\"browser_download_url\":\"", end);
  }

  Serial.println("GitHub release: no .bin asset found");
  return false;
}

// Downloads the firmware binary at `url` and writes it to the inactive OTA
// partition. Returns true if the write succeeded (caller should then
// ESP.restart() to boot into it).
bool downloadAndFlashFirmware(const String &url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url.c_str());
  http.addHeader("User-Agent", "epaper-clock-ota");
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub asset URLs redirect to S3

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("Firmware download HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("Update.begin failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  size_t written = Update.writeStream(*http.getStreamPtr());
  bool ok = (len <= 0 || written == (size_t)len) && Update.end(true);
  http.end();

  if (!ok) {
    Serial.printf("Firmware update failed (%u bytes written): %s\n", (unsigned)written, Update.errorString());
    return false;
  }

  Serial.printf("Firmware update written (%u bytes)\n", (unsigned)written);
  return true;
}

// Checks GitHub for a newer release at most once per OTA_CHECK_INTERVAL_S,
// only during the night window, and only on normal (non-button) wakes so it
// never runs alongside an interactive ArduinoOTA session. On success this
// flashes the new firmware and reboots into it; it does not return in that case.
//
// Note: unlike a full ESP-IDF rollback setup, there's no automatic revert if
// the new firmware boot-loops — Arduino IDE's default board config doesn't
// enable the bootloader's rollback feature. A bad release stays flashed until
// a fixed one is published.
void checkForFirmwareUpdate() {
  if (!OTA_UPDATES_ENABLED) return;

  struct tm nowTm;
  if (!getLocalTime(&nowTm, 100)) return;
  bool isNight = (nowTm.tm_hour >= NIGHT_START_HOUR) && (nowTm.tm_hour < NIGHT_END_HOUR);
  if (!isNight) return;

  time_t nowEpoch = time(nullptr);
  if (lastOtaCheckTime != 0 && (nowEpoch - lastOtaCheckTime) < OTA_CHECK_INTERVAL_S) return;
  lastOtaCheckTime = nowEpoch;

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Checking GitHub for newer firmware...");
  String tag, assetUrl;
  if (!fetchLatestRelease(tag, assetUrl)) return;

  if (compareVersions(tag, FIRMWARE_VERSION) <= 0) {
    Serial.printf("Firmware up to date (current %s, latest %s)\n", FIRMWARE_VERSION, tag.c_str());
    return;
  }

  if (strcmp(otaFailedVersion, tag.c_str()) == 0 && otaFailCount >= OTA_MAX_UPDATE_ATTEMPTS) {
    Serial.printf("Skipping %s — already failed %u time(s)\n", tag.c_str(), otaFailCount);
    return;
  }

  Serial.printf("New firmware available: %s (current %s)\n", tag.c_str(), FIRMWARE_VERSION);

#ifdef EPAPER_ENABLE
  drawUpdateScreen(tag);
  epaper.update();
#endif

  if (downloadAndFlashFirmware(assetUrl)) {
    otaFailCount = 0;
    otaFailedVersion[0] = '\0';
    Serial.println("Update installed — restarting");
    Serial.flush();
    delay(200);
    ESP.restart();
  } else if (strcmp(otaFailedVersion, tag.c_str()) == 0) {
    otaFailCount++;
  } else {
    strncpy(otaFailedVersion, tag.c_str(), sizeof(otaFailedVersion) - 1);
    otaFailedVersion[sizeof(otaFailedVersion) - 1] = '\0';
    otaFailCount = 1;
  }
}

void enterDeepSleep(uint64_t sleepUs) {
  // Turn off WiFi before sleeping
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.printf("Deep sleeping for %llu seconds...\n", sleepUs / 1000000);
  Serial.flush();

  // Wake on timer
  esp_sleep_enable_timer_wakeup(sleepUs);

  // Also wake on any button press (LOW = pressed with pull-up)
  // Use ext1 with multiple pins, wake on any LOW
  uint64_t buttonMask = (1ULL << BUTTON_D1) | (1ULL << BUTTON_D2) | (1ULL << BUTTON_D4);
  esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ANY_LOW);

  esp_deep_sleep_start();
}

float readBatteryVoltage() {
  // Enable ADC
  digitalWrite(ADC_EN_PIN, HIGH);
  delay(10);  // Short delay to stabilize

  // Read 30 times and average for more stable readings
  long sum = 0;
  for (int i = 0; i < 30; i++) {
    sum += analogRead(BATTERY_PIN);
    delayMicroseconds(100);
  }

  // Disable ADC to save power
  digitalWrite(ADC_EN_PIN, LOW);

  // Calculate voltage
  float adc_avg = sum / 30.0;
  float voltage = (adc_avg / 4095.0) * 3.6 * 2.0 * CALIBRATION_FACTOR;

  return voltage;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  wakeCount++;
  Serial.printf("Wake #%u\n", wakeCount);

  // Configure ADC_EN
  pinMode(ADC_EN_PIN, OUTPUT);
  digitalWrite(ADC_EN_PIN, LOW);  // Start with ADC disabled to save power

  // Configure ADC
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

  // Read battery voltage
  float voltage = readBatteryVoltage();

  // Print the results
  Serial.print("Battery Voltage: ");
  Serial.print(voltage, 2);  // Print with 2 decimal places
  Serial.println("V");

  // Configure button pins as inputs with internal pull-up resistors
  pinMode(BUTTON_D1, INPUT_PULLUP);
  pinMode(BUTTON_D2, INPUT_PULLUP);
  pinMode(BUTTON_D4, INPUT_PULLUP);

  // Determine battery level
  String batteryStatus;
  if (voltage >= 4.0) {
    batteryStatus = "Full";
  } else if (voltage >= 3.7) {
    batteryStatus = "Good";
  } else if (voltage >= 3.5) {
    batteryStatus = "Medium";
  } else if (voltage >= 3.2) {
    batteryStatus = "Low";
  } else {
    batteryStatus = "Critical";
  }

  Serial.print("Battery Status: ");
  Serial.println(batteryStatus);
  Serial.println();

#ifdef EPAPER_ENABLE
  epaper.begin();
  epaper.setRotation(0);

  // Always set timezone (ESP32 RTC loses TZ info across deep sleep)
  setenv("TZ", TZ_INFO, 1);
  tzset();

  // Sync NTP on first boot, then every NTP_SYNC_INTERVAL_S seconds
  time_t nowEpoch = time(nullptr);
  bool needSync = !everSynced || (nowEpoch - lastNtpSyncTime >= NTP_SYNC_INTERVAL_S);

  if (needSync) {
    epaper.fillScreen(TFT_WHITE);
    epaper.setTextColor(TFT_BLACK, TFT_WHITE);
    epaper.setTextSize(1);
    epaper.drawCentreString("Syncing...", SCREEN_W / 2, SCREEN_H / 2 - 12, 4);
    epaper.update();

    connectWiFi();
    if (syncNTP()) {
      everSynced = true;
      lastNtpSyncTime = time(nullptr);
      // Record the charge baseline on the first sync after a cold boot.
      // bootEpoch is cleared with RTC memory whenever the device is powered
      // off to charge, so this marks "time since last charged".
      if (bootEpoch == 0) {
        bootEpoch = lastNtpSyncTime;
      }
    }
  }

  // Fetch weather every ~3 hours during daytime (not 1am-6am)
  // Always update lastWeatherSyncTime on attempt to avoid hammering API on failure
  struct tm now;
  bool isDaytime = true;
  if (getLocalTime(&now, 100)) {
    isDaytime = (now.tm_hour < NIGHT_START_HOUR || now.tm_hour >= NIGHT_END_HOUR);
  }
  time_t weatherNow = time(nullptr);
  bool weatherDue = (weatherNow - lastWeatherSyncTime) >= WEATHER_SYNC_INTERVAL_S;
  bool shouldFetchWeather = isDaytime && (weatherDue || (weatherRetries > 0 && weatherRetries <= WEATHER_MAX_RETRIES));
  if (shouldFetchWeather) {
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    if (fetchWeather()) {
      weatherRetries = 0;
      lastWeatherSyncTime = weatherNow;
    } else {
      weatherRetries++;
      if (weatherRetries > WEATHER_MAX_RETRIES) {
        Serial.println("Weather: giving up after max retries");
        lastWeatherSyncTime = weatherNow;
        weatherRetries = 0;
      }
    }
  }

  // Check if we woke from a button press
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  if (wakeReason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Button wake detected — entering OTA mode");

    connectWiFi();
    bool wifiOk = (WiFi.status() == WL_CONNECTED);

    if (wifiOk) {
      otaMode = true;

      ArduinoOTA.setHostname("epaper-clock");
      ArduinoOTA.onStart([]() {
        Serial.println("OTA update starting...");
      });
      ArduinoOTA.onEnd([]() {
        Serial.println("OTA update complete!");
      });
      ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error [%u]\n", error);
      });
      ArduinoOTA.begin();

      drawOtaScreen(true);
      epaper.update();
      otaStartedAt = millis();
    } else {
      // No point holding the listen window open with no WiFi — go straight
      // back to the clock so the button press doesn't just burn battery.
      Serial.println("OTA: WiFi connect failed — skipping listen window");
      drawOtaScreen(false);
      epaper.update();
      delay(3000);
      lastVoltage = voltage;
      drawClock(lastVoltage);
      epaper.update();
    }
  } else {
    checkForFirmwareUpdate();  // may flash new firmware and reboot; does not return in that case
    lastVoltage = voltage;
    drawClock(lastVoltage);
    epaper.update();
  }

if (!hasSleptOnce) {
  Serial.printf("First boot\n");
}
  setupDoneAt = millis();
#endif
}

void loop() {
  // OTA mode: listen for updates, then sleep
  if (otaMode) {
    ArduinoOTA.handle();
    unsigned long elapsed = millis() - otaStartedAt;
    if (elapsed >= OTA_LISTEN_MS) {
      Serial.println("OTA listen window expired, drawing clock and going to sleep");
      otaMode = false;
      lastVoltage = readBatteryVoltage();
      drawClock(lastVoltage);
      epaper.update();
      uint64_t sleepDuration = getSleepDuration();
      enterDeepSleep(sleepDuration);
    }
    delay(10);
    return;
  }

  // First boot: stay awake so IDE can push updates via serial
  if (!hasSleptOnce) {
    unsigned long elapsed = millis() - setupDoneAt;
    
    if (elapsed < FIRST_BOOT_AWAKE_MS) {
      Serial.printf("... %lus until sleep", (FIRST_BOOT_AWAKE_MS - elapsed) / 1000);
      delay(5000);
      return;
    }
    Serial.println("\nFirst boot grace period over, starting sleep cycle");
    hasSleptOnce = true;
  }

  uint64_t sleepDuration = getSleepDuration();
  enterDeepSleep(sleepDuration);
}

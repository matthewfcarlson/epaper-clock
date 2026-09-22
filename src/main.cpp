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
#include <Update.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include "config.h"
#include "version.h"
#include "sdf_font.h"
#include "fonts/Inter_Bold_sdf.h"
#include "fonts/Inter_Regular_sdf.h"
#include "ota_health.h"

// SDF pixel heights approximating the sizes the previous bitmap/GFX fonts
// rendered at. InterBold replaces BigDigits.h + the FreeSansBold* GFX
// fonts; InterRegular replaces the plain GLCD/FreeSans12pt7b text.
static const int FONT_PX_CLOCK_DIGITS = 250;  // was BigDigits (~220px tall)
static const int FONT_PX_AMPM = 42;           // was FreeSansBold24pt7b
static const int FONT_PX_WEEKDAY = 32;        // was FreeSansBold18pt7b
static const int FONT_PX_LABEL = 26;          // was FreeSansBold12pt7b (date/weather/hint)
static const int FONT_PX_NIGHT_CAPTION = 26;  // was FreeSans12pt7b
static const int FONT_PX_BATTERY_PCT = 20;    // was GLCD font 2
static const int FONT_PX_HEADING = 40;        // was font 4 (maintenance/update/syncing titles)
static const int FONT_PX_BODY = 22;           // was font 2 (maintenance/update body lines)

// Wake/update interval, in minutes, aligned to the clock (e.g. :00/:05/:10... for 5)
#define DAY_WAKE_INTERVAL_MIN 5
#define NIGHT_WAKE_INTERVAL_MIN 15
#define NIGHT_START_HOUR 23
#define NIGHT_END_HOUR 6
// Fallback sleep duration if the current time isn't available (e.g. NTP never synced)
#define SLEEP_FALLBACK_US ((uint64_t)DAY_WAKE_INTERVAL_MIN * 60ULL * 1000000ULL)

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

// How long to listen for serial location provisioning after button wake (90 seconds)
#ifndef PROVISION_LISTEN_MS
#define PROVISION_LISTEN_MS 90000
#endif

// Weather update interval in seconds (6 hours)
#define WEATHER_SYNC_INTERVAL_S (6UL * 3600)

// How often to check GitHub for a newer firmware release, at night only (24 hours)
#define OTA_CHECK_INTERVAL_S (24UL * 60 * 60)
// Give up retrying a specific release after this many failed download/flash attempts
#define OTA_MAX_UPDATE_ATTEMPTS 3

// Lower WiFi TX power to reduce peak radio current during WiFi-active wakes.
// Default max is WIFI_POWER_19_5dBm; for a router within typical home range
// this holds a reliable link while drawing meaningfully less current.
#define WIFI_TX_POWER WIFI_POWER_11dBm

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
// Cached AP channel/BSSID from the last successful connect, so the next
// WiFi.begin() can skip the AP scan phase. Invalidated on a failed connect
// so a changed/rebooted AP heals within one wake instead of failing repeatedly.
RTC_DATA_ATTR uint8_t wifiChannel = 0;
RTC_DATA_ATTR uint8_t wifiBssid[6] = {0, 0, 0, 0, 0, 0};
RTC_DATA_ATTR bool wifiBssidValid = false;

// Everything the clock face's drawing functions need — captured once per
// wake so the exact same values can be redrawn later to reconstruct what a
// past wake's frame looked like (see refreshClockDisplay()). Time fields
// are already rounded to the 5-minute display cadence.
struct ClockSnapshot {
  bool haveTime;
  int hour, min, wday, mday, mon;
  bool isNight;
  float batteryVoltage;
  bool weatherValid;
  int weatherHigh, weatherLow;
  bool locationConfigured;
  bool wifiCredsAvailable;
};

// The previous wake's snapshot and whether it's usable to reconstruct a
// priming frame for partial refresh — see refreshClockDisplay().
RTC_DATA_ATTR bool havePreviousFrame = false;
RTC_DATA_ATTR ClockSnapshot previousFrame = {};
// Firmware version that drew previousFrame — a different build can render
// fonts/layout differently, so a reconstruction drawn by *this* firmware
// wouldn't actually match what's physically on the panel if versions differ.
RTC_DATA_ATTR char previousFrameVersion[16] = "";
// Even with correct priming data, force an occasional real full refresh as
// a safety net against analog drift the panel accumulates regardless of
// what data it's fed — standard e-paper practice.
RTC_DATA_ATTR time_t lastFullRefreshTime = 0;
#define FULL_REFRESH_SAFETY_INTERVAL_S (24 * 3600)

// Night window can wrap past midnight (e.g. 23 -> 6), so this can't be a
// simple range comparison.
bool isNightHour(int h) {
  if (NIGHT_START_HOUR <= NIGHT_END_HOUR) {
    return h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR;
  }
  return h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR;
}

float lastVoltage = 0;
unsigned long setupDoneAt = 0;
bool maintenanceMode = false;
unsigned long maintenanceStartedAt = 0;

OtaHealth otaHealth;

// User-provisioned weather location (see "Location provisioning" in
// CLAUDE.md), persisted in NVS via Preferences rather than RTC_DATA_ATTR —
// unlike RTC memory, NVS survives full power loss, which is what a
// user-set preference needs.
Preferences prefs;
float userLat = 0;
float userLon = 0;
bool locationConfigured = false;

void loadLocationConfig() {
  prefs.begin("clockcfg", true);
  locationConfigured = prefs.getBool("loc_set", false);
  userLat = prefs.getFloat("lat", 0);
  userLon = prefs.getFloat("lon", 0);
  prefs.end();
}

void saveLocationConfig(float lat, float lon) {
  prefs.begin("clockcfg", false);
  prefs.putBool("loc_set", true);
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.end();
  userLat = lat;
  userLon = lon;
  locationConfigured = true;
}

// User-provisioned WiFi credentials, same NVS namespace/rationale as
// location above. Takes priority over the compiled-in WIFI_SSID/WIFI_PASS
// (config.h) when set — this is what lets the public release binary ship
// without a real network's credentials baked in (see "Location & WiFi
// provisioning" in CLAUDE.md): a fresh device falls back to config.h just
// long enough to be provisioned once over USB serial, and every release
// after that carries no real credentials at all.
char userWifiSsid[33] = "";
char userWifiPass[65] = "";
bool wifiConfigured = false;

void loadWifiConfig() {
  prefs.begin("clockcfg", true);
  wifiConfigured = prefs.getBool("wifi_set", false);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  strncpy(userWifiSsid, ssid.c_str(), sizeof(userWifiSsid) - 1);
  userWifiSsid[sizeof(userWifiSsid) - 1] = '\0';
  strncpy(userWifiPass, pass.c_str(), sizeof(userWifiPass) - 1);
  userWifiPass[sizeof(userWifiPass) - 1] = '\0';
}

void saveWifiSsid(const char *ssid) {
  prefs.begin("clockcfg", false);
  prefs.putBool("wifi_set", true);
  prefs.putString("ssid", ssid);
  prefs.end();
  strncpy(userWifiSsid, ssid, sizeof(userWifiSsid) - 1);
  userWifiSsid[sizeof(userWifiSsid) - 1] = '\0';
  wifiConfigured = true;
}

void saveWifiPass(const char *pass) {
  prefs.begin("clockcfg", false);
  prefs.putString("pass", pass);
  prefs.end();
  strncpy(userWifiPass, pass, sizeof(userWifiPass) - 1);
  userWifiPass[sizeof(userWifiPass) - 1] = '\0';
}

// The credentials connectWiFi() should actually use: NVS-provisioned ones
// once set, otherwise whatever's compiled into config.h (empty on the
// public release build, possibly real for a local dev build).
const char *activeWifiSsid() { return wifiConfigured ? userWifiSsid : WIFI_SSID; }
const char *activeWifiPass() { return wifiConfigured ? userWifiPass : WIFI_PASS; }
bool wifiCredentialsAvailable() { return activeWifiSsid()[0] != '\0'; }

// Non-blocking line-based provisioning protocol over the same USB serial
// connection used for flashing/monitoring. Commands from the browser are
// prefixed "CFG ", replies are prefixed ">>" so a Web Serial client can
// filter protocol lines out of ordinary Serial.print debug output on the
// same stream. Only active during the button-wake maintenance window and
// the first-boot awake window (see loop()).
//   CFG GET_STATUS              -> >>STATUS configured=0|1 lat=<f> lon=<f> fw=<version> wifi=0|1
//   CFG SET_LOCATION <lat> <lon> -> >>OK SET_LOCATION | >>ERR RANGE|PARSE
//   CFG GET_WIFI_SSID           -> >>WIFI_SSID <ssid>  (blank when unset)
//   CFG SET_WIFI_SSID <ssid>    -> >>OK SET_WIFI_SSID | >>ERR RANGE
//   CFG SET_WIFI_PASS <pass>    -> >>OK SET_WIFI_PASS | >>ERR RANGE
//     SSID/password take the rest of the line verbatim (spaces allowed),
//     which is why they're separate commands rather than sharing one line
//     like SET_LOCATION's two floats.
//   CFG REBOOT                  -> >>OK REBOOT, then restarts immediately into
//                                   a normal (non-button) wake cycle instead of
//                                   waiting out the rest of the maintenance window
#define PROVISION_LINE_MAX 96
void handleSerialProvisioning() {
  static char lineBuf[PROVISION_LINE_MAX];
  static size_t lineLen = 0;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
      continue;
    }

    lineBuf[lineLen] = '\0';
    lineLen = 0;
    if (lineBuf[0] == '\0') continue;

    if (strcmp(lineBuf, "CFG GET_STATUS") == 0) {
      Serial.printf(">>STATUS configured=%d lat=%.4f lon=%.4f fw=%s wifi=%d\n",
                     locationConfigured ? 1 : 0, userLat, userLon, FIRMWARE_VERSION, wifiConfigured ? 1 : 0);
    } else if (strncmp(lineBuf, "CFG SET_LOCATION ", 17) == 0) {
      float lat, lon;
      if (sscanf(lineBuf + 17, "%f %f", &lat, &lon) == 2) {
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
          Serial.println(">>ERR RANGE");
        } else {
          saveLocationConfig(lat, lon);
          Serial.println(">>OK SET_LOCATION");
        }
      } else {
        Serial.println(">>ERR PARSE");
      }
    } else if (strcmp(lineBuf, "CFG GET_WIFI_SSID") == 0) {
      Serial.printf(">>WIFI_SSID %s\n", wifiConfigured ? userWifiSsid : "");
    } else if (strncmp(lineBuf, "CFG SET_WIFI_SSID ", sizeof("CFG SET_WIFI_SSID ") - 1) == 0) {
      const char *ssid = lineBuf + (sizeof("CFG SET_WIFI_SSID ") - 1);
      if (strlen(ssid) == 0 || strlen(ssid) > sizeof(userWifiSsid) - 1) {
        Serial.println(">>ERR RANGE");
      } else {
        saveWifiSsid(ssid);
        Serial.println(">>OK SET_WIFI_SSID");
      }
    } else if (strncmp(lineBuf, "CFG SET_WIFI_PASS ", sizeof("CFG SET_WIFI_PASS ") - 1) == 0) {
      const char *pass = lineBuf + (sizeof("CFG SET_WIFI_PASS ") - 1);
      if (strlen(pass) > sizeof(userWifiPass) - 1) {
        Serial.println(">>ERR RANGE");
      } else {
        saveWifiPass(pass);
        Serial.println(">>OK SET_WIFI_PASS");
      }
    } else if (strcmp(lineBuf, "CFG REBOOT") == 0) {
      Serial.println(">>OK REBOOT");
      Serial.flush();
      delay(100);
      ESP.restart();
    } else {
      Serial.println(">>ERR UNKNOWN_CMD");
    }
  }
}

#ifdef EPAPER_ENABLE

#include "epaper_partial.h"

EPaperPartial epaper = EPaperPartial();

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

// Draw battery outline + fill at (x, y), 36x18px, optionally with percentage
// text to its left.
void drawBatteryIcon(int x, int y, int percent, bool showLabel = true) {
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

  if (showLabel) {
    // Percentage text to the left of the icon
    char buf[5];
    sprintf(buf, "%d%%", percent);
    // Right-align text just left of the battery icon
    int textW = sdfTextWidth(InterRegular, buf, FONT_PX_BATTERY_PCT);
    sdfDrawTextTL(epaper, InterRegular, x - textW - 4, y + 1, buf, FONT_PX_BATTERY_PCT, 1.0f, TFT_BLACK);
  }
}

// Draws one word letter-spaced (tracked-out, all-caps look) starting at x,
// returning the x position immediately after the last letter. xScale
// independently condenses glyph width and letter spacing on top of
// pixelHeight, so long strings can be shrunk to fit an available width.
int drawTrackedText(const char *str, int x, int y, const SdfFont &font, int pixelHeight,
                     int letterSpacing, uint32_t color = TFT_BLACK, float xScale = 1.0f) {
  char glyph[5] = {0};
  bool any = false;
  int scaledSpacing = (int)lroundf(letterSpacing * xScale);
  for (const char *p = str; *p; ) {
    int consumed = 1;
    sdfUtf8Decode(p, &consumed);
    memcpy(glyph, p, consumed);
    glyph[consumed] = '\0';
    p += consumed;
    any = true;
    sdfDrawTextTL(epaper, font, x, y, glyph, pixelHeight, xScale, color);
    x += sdfTextWidth(font, glyph, pixelHeight, xScale) + scaledSpacing;
  }
  return any ? x - scaledSpacing : x;
}

int trackedTextWidth(const char *str, const SdfFont &font, int pixelHeight, int letterSpacing,
                      float xScale = 1.0f) {
  char glyph[5] = {0};
  int w = 0;
  bool any = false;
  int scaledSpacing = (int)lroundf(letterSpacing * xScale);
  for (const char *p = str; *p; ) {
    int consumed = 1;
    sdfUtf8Decode(p, &consumed);
    memcpy(glyph, p, consumed);
    glyph[consumed] = '\0';
    p += consumed;
    any = true;
    w += sdfTextWidth(font, glyph, pixelHeight, xScale) + scaledSpacing;
  }
  return any ? w - scaledSpacing : w;
}

static const char *WEEKDAY_NAMES[7] = {
  "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
};
static const char *MONTH_NAMES[12] = {
  "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
  "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
};

// Draw the maintenance-mode listening screen — download arrow icon + text.
// Maintenance mode is serial-only (location provisioning over USB); firmware
// updates are pulled from GitHub on the normal nightly schedule instead, so
// this screen doesn't touch WiFi at all.
void drawMaintenanceScreen() {
  epaper.fillScreen(TFT_WHITE);

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2 - 60;

  // Download arrow: vertical bar
  epaper.fillRect(cx - 6, cy - 40, 12, 50, TFT_BLACK);
  // Arrowhead (triangle)
  epaper.fillTriangle(cx - 24, cy + 10, cx + 24, cy + 10, cx, cy + 40, TFT_BLACK);
  // Horizontal line (tray)
  epaper.fillRect(cx - 30, cy + 48, 60, 4, TFT_BLACK);

  sdfDrawCentreTextTL(epaper, InterBold, cx, cy + 70, "Maintenance Mode", FONT_PX_HEADING, 1.0f, TFT_BLACK);

  char lineBuf[48];
  snprintf(lineBuf, sizeof(lineBuf), "Listening for %lus...", (unsigned long)(PROVISION_LISTEN_MS / 1000));
  sdfDrawCentreTextTL(epaper, InterRegular, cx, cy + 100, lineBuf, FONT_PX_BODY, 1.0f, TFT_BLACK);
  sdfDrawCentreTextTL(epaper, InterRegular, cx, cy + 130, "Connect USB + open provision.html to configure",
                       FONT_PX_BODY, 1.0f, TFT_BLACK);

  snprintf(lineBuf, sizeof(lineBuf), "fw %s", FIRMWARE_VERSION);
  sdfDrawCentreTextTL(epaper, InterRegular, cx, cy + 160, lineBuf, FONT_PX_BODY, 1.0f, TFT_BLACK);
}

// Draw "installing firmware update" screen shown during a nightly auto-update
void drawUpdateScreen(const String &newVersion) {
  epaper.fillScreen(TFT_WHITE);

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2 - 20;

  sdfDrawCentreTextTL(epaper, InterBold, cx, cy, "Installing Update", FONT_PX_HEADING, 1.0f, TFT_BLACK);

  char buf[40];
  snprintf(buf, sizeof(buf), "%s -> %s", FIRMWARE_VERSION, newVersion.c_str());
  sdfDrawCentreTextTL(epaper, InterRegular, cx, cy + 40, buf, FONT_PX_BODY, 1.0f, TFT_BLACK);
  sdfDrawCentreTextTL(epaper, InterRegular, cx, cy + 70, "Do not unplug...", FONT_PX_BODY, 1.0f, TFT_BLACK);
}

int getBatteryPercent(float voltage) {
  int percent = (int)((voltage - BATT_V_EMPTY) / (BATT_V_FULL - BATT_V_EMPTY) * 100.0);
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;
  return percent;
}

void drawClock(const ClockSnapshot &s) {
  bool haveTime = s.haveTime;
  int hour = s.hour, min = s.min, wday = s.wday, mday = s.mday, mon = s.mon;

  epaper.fillScreen(TFT_WHITE);

  const int marginX = 40;

  // Weekday, top-left, tracked bold caps
  drawTrackedText(haveTime ? WEEKDAY_NAMES[wday] : "", marginX, 30, InterBold, FONT_PX_WEEKDAY, 6);

  // Battery icon, top-right, icon only (no percentage text) — only shown
  // once it's actually low, so it doesn't clutter the face the rest of the
  // time.
  int battPercent = getBatteryPercent(s.batteryVoltage);
  if (battPercent < 20) {
    drawBatteryIcon(SCREEN_W - marginX - 40, 34, battPercent, false);
  }

  // Bottom bar (rule + date/weather row) sits close to the bottom edge.
  const int marginBottom = 26;
  int bottomRowY = SCREEN_H - marginBottom - FONT_PX_LABEL;
  int ruleY = bottomRowY - 25;
  epaper.fillRect(marginX, ruleY, SCREEN_W - marginX * 2, 1, TFT_BLACK);

  // Convert to 12-hour format
  const char *ampm = (hour < 12) ? "AM" : "PM";
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;

  char hourStr[3];
  char minStr[3];
  sprintf(hourStr, "%d", hour12);
  sprintf(minStr, "%02d", min);

  // Stretch the big time to fill the box between the weekday row and the
  // rule — width and height are scaled independently, since that's what an
  // SDF atlas (vs. a fixed-size bitmap) actually buys us. Height picks
  // pixelHeight so digit glyphs fill the available vertical band; width
  // then picks a separate xScale so the whole "H:MM + AM/PM" row fills the
  // available horizontal band. The colon and AM/PM sizes are kept
  // proportional to the digit size (same ratios as the original fixed
  // 250px-tall design) rather than independently stretched.
  const int digitMarginX = 30;
  const int topBound = 85;             // just below the weekday row
  const int bottomBound = ruleY - 20;  // small gap above the rule
  int availH = bottomBound - topBound;

  const SdfGlyph *refDigit = sdfFindGlyph(InterBold, '0');
  float scaleY = (float)availH / (float)refDigit->h;
  int pixelHeight = (int)lroundf(scaleY * InterBold.emPx);
  scaleY = (float)pixelHeight / (float)InterBold.emPx;

  int colonWNatural = (int)lroundf(pixelHeight * (50.0f / 250.0f));
  int fontPxAmpm = (int)lroundf(pixelHeight * (42.0f / 250.0f));
  int dotR = (int)lroundf(pixelHeight * (14.0f / 250.0f));
  int dotSpacing = (int)lroundf(pixelHeight * (45.0f / 250.0f));
  int colonCenterOffset = (int)lroundf(pixelHeight * (105.0f / 250.0f));

  int hourWNatural = sdfTabularDigitsWidth(InterBold, strlen(hourStr), pixelHeight);
  int minWNatural = sdfTabularDigitsWidth(InterBold, strlen(minStr), pixelHeight);
  int naturalTimeW = hourWNatural + colonWNatural + minWNatural;

  const int ampmGap = 18;
  int ampmW = sdfTextWidth(InterBold, ampm, fontPxAmpm);
  int availWForTime = (SCREEN_W - digitMarginX * 2) - ampmGap - ampmW;

  float xScale = (float)availWForTime / (float)naturalTimeW;

  int hourW = (int)lroundf(hourWNatural * xScale);
  int colonW = (int)lroundf(colonWNatural * xScale);
  int minW = (int)lroundf(minWNatural * xScale);
  int totalW = hourW + colonW + minW;

  int xStart = digitMarginX;
  int yPos = topBound - (int)lroundf(refDigit->yoff * scaleY);  // baseline

  sdfDrawTabularDigits(epaper, InterBold, xStart, yPos, hourStr, pixelHeight, xScale, TFT_BLACK);

  // Draw colon as two filled circles, centered on digit height
  int colonX = xStart + hourW + colonW / 2;
  int colonCenter = yPos - colonCenterOffset;
  epaper.fillCircle(colonX, colonCenter - dotSpacing, dotR, TFT_BLACK);
  epaper.fillCircle(colonX, colonCenter + dotSpacing, dotR, TFT_BLACK);

  sdfDrawTabularDigits(epaper, InterBold, xStart + hourW + colonW, yPos, minStr, pixelHeight, xScale, TFT_BLACK);

  // AM/PM to the right of the digits, bottom-aligned near the digit baseline
  sdfDrawTextTL(epaper, InterBold, xStart + totalW + ampmGap, yPos - (int)lroundf(fontPxAmpm * 1.3f),
                ampm, fontPxAmpm, 1.0f, TFT_BLACK);

  // Date, bottom-left, tracked bold caps, e.g. "11 SEPTEMBER"
  char dateStr[24];
  if (haveTime) {
    snprintf(dateStr, sizeof(dateStr), "%d %s", mday, MONTH_NAMES[mon]);
  } else {
    dateStr[0] = '\0';
  }
  drawTrackedText(dateStr, marginX, bottomRowY, InterBold, FONT_PX_LABEL, 3);

  // Weather, bottom-right: "<hi>°/<lo>°F", or a setup hint until a location
  // has been provisioned.
  if (s.weatherValid) {
    char tempStr[16];
    snprintf(tempStr, sizeof(tempStr), "%d\xC2\xB0/%d\xC2\xB0", s.weatherHigh, s.weatherLow);

    int tempW = sdfTextWidth(InterBold, tempStr, FONT_PX_LABEL);
    int x = SCREEN_W - marginX - tempW;
    sdfDrawTextTL(epaper, InterBold, x, bottomRowY, tempStr, FONT_PX_LABEL, 1.0f, TFT_BLACK);
  } else if (!s.locationConfigured || !s.wifiCredsAvailable) {
    // Long hint string can outgrow the space left of the right margin once
    // the (variable-width) date string on the left is accounted for — shrink
    // it to fit rather than letting it run into the date.
    const char *hint = !s.wifiCredsAvailable ? "PRESS BUTTON + CONNECT USB TO SET UP WIFI"
                                              : "PRESS BUTTON + CONNECT USB TO SET LOCATION";
    const int hintLetterSpacing = 3;
    const int gap = 20;
    int dateW = trackedTextWidth(dateStr, InterBold, FONT_PX_LABEL, 3);
    int maxHintW = (SCREEN_W - marginX * 2) - dateW - gap;
    int hintW = trackedTextWidth(hint, InterBold, FONT_PX_LABEL, hintLetterSpacing);
    float hintScale = (maxHintW > 0 && hintW > maxHintW) ? (float)maxHintW / (float)hintW : 1.0f;
    hintW = trackedTextWidth(hint, InterBold, FONT_PX_LABEL, hintLetterSpacing, hintScale);
    drawTrackedText(hint, SCREEN_W - marginX - hintW, bottomRowY, InterBold, FONT_PX_LABEL,
                     hintLetterSpacing, TFT_BLACK, hintScale);
  }
}

// Draws "WEEKDAY · AM" (or PM), tracked-out and centered on cx, with a small
// dot separator — the caption under the night clock face.
void drawNightCaption(int cx, int y, const char *weekdayUpper, const char *ampm) {
  const int letterSpacing = 6;
  const int gap = 20;
  const int dotR = 3;

  int weekdayW = trackedTextWidth(weekdayUpper, InterRegular, FONT_PX_NIGHT_CAPTION, letterSpacing);
  int ampmW = trackedTextWidth(ampm, InterRegular, FONT_PX_NIGHT_CAPTION, letterSpacing);
  int totalW = weekdayW + gap + dotR * 2 + gap + ampmW;

  int x = cx - totalW / 2;
  x = drawTrackedText(weekdayUpper, x, y, InterRegular, FONT_PX_NIGHT_CAPTION, letterSpacing, TFT_WHITE);
  x += gap;
  epaper.fillCircle(x + dotR, y + 10, dotR, TFT_WHITE);
  x += dotR * 2 + gap;
  drawTrackedText(ampm, x, y, InterRegular, FONT_PX_NIGHT_CAPTION, letterSpacing, TFT_WHITE);
}

// Night-only rendering: dark background, white text, no date/weather/battery
// — a separate function from drawClock() so day-mode drawing (due for its
// own refactor) is untouched.
void drawNightClock(const ClockSnapshot &s) {
  bool haveTime = s.haveTime;
  int hour = s.hour, min = s.min, wday = s.wday;

  epaper.fillScreen(TFT_BLACK);

  const char *ampm = (hour < 12) ? "AM" : "PM";
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;

  char hourStr[3];
  char minStr[3];
  sprintf(hourStr, "%d", hour12);
  sprintf(minStr, "%02d", min);

  int hourW = sdfTabularDigitsWidth(InterBold, strlen(hourStr), FONT_PX_CLOCK_DIGITS);
  int colonW = 50;
  int minW = sdfTabularDigitsWidth(InterBold, strlen(minStr), FONT_PX_CLOCK_DIGITS);
  int totalW = hourW + colonW + minW;
  int xStart = (SCREEN_W - totalW) / 2;
  int yPos = SCREEN_H / 2 + 60;  // baseline position

  sdfDrawTabularDigits(epaper, InterBold, xStart, yPos, hourStr, FONT_PX_CLOCK_DIGITS, 1.0f, TFT_WHITE);

  // Colon as two filled circles, centered on digit height
  int colonX = xStart + hourW + colonW / 2;
  int dotR = 14;
  int colonCenter = yPos - 105;
  int dotSpacing = 45;
  epaper.fillCircle(colonX, colonCenter - dotSpacing, dotR, TFT_WHITE);
  epaper.fillCircle(colonX, colonCenter + dotSpacing, dotR, TFT_WHITE);

  sdfDrawTabularDigits(epaper, InterBold, xStart + hourW + colonW, yPos, minStr, FONT_PX_CLOCK_DIGITS, 1.0f, TFT_WHITE);

  drawNightCaption(SCREEN_W / 2, yPos + 45, haveTime ? WEEKDAY_NAMES[wday] : "", ampm);
}

// Captures everything the clock face needs to render "right now," rounded
// to the 5-minute display cadence — see ClockSnapshot.
ClockSnapshot captureCurrentSnapshot(float batteryVoltage) {
  ClockSnapshot s = {};
  s.batteryVoltage = batteryVoltage;

  struct tm timeinfo;
  s.haveTime = getLocalTime(&timeinfo, 100);
  if (s.haveTime) {
    s.hour = timeinfo.tm_hour;
    s.min = timeinfo.tm_min;
    s.wday = timeinfo.tm_wday;
    s.mday = timeinfo.tm_mday;
    s.mon = timeinfo.tm_mon;
    s.isNight = isNightHour(s.hour);
  }

  // Round the displayed time to the nearest 5-minute mark, matching the
  // cadence the device actually wakes/updates at.
  s.min = ((s.min + 2) / 5) * 5;
  if (s.min >= 60) {
    s.min = 0;
    s.hour = (s.hour + 1) % 24;
  }

  s.weatherValid = weatherValid;
  s.weatherHigh = weatherHigh;
  s.weatherLow = weatherLow;
  s.locationConfigured = locationConfigured;
  s.wifiCredsAvailable = wifiCredentialsAvailable();
  return s;
}

// Picks day or night rendering based on the snapshot and draws the clock
// face accordingly.
void drawSnapshot(const ClockSnapshot &s) {
  if (s.isNight) {
    drawNightClock(s);
  } else {
    drawClock(s);
  }
}

// Captures the current state, draws and pushes the clock face to the
// panel, and records what was drawn for next wake's reconstruction.
//
// Does a real partial refresh when it safely can. Stock Seeed_GFX can't do
// this across a deep-sleep boundary — see epaper_partial.h for the full
// story — so instead of relying on anything left in the panel controller's
// SRAM, this reconstructs the exact previous frame from the last wake's
// persisted snapshot, primes the controller with it, then pushes the real
// new frame: the controller always gets an accurate old/new pair to diff
// against, regardless of what happened to it between wakes.
//
// Falls back to a genuine full refresh (which manufactures its own
// old==new baseline via EPaper::update() and needs no history) whenever
// there's no previous frame to reconstruct, the firmware that drew it
// isn't this build, or the periodic safety-net interval is due.
void refreshClockDisplay(float batteryVoltage) {
  ClockSnapshot current = captureCurrentSnapshot(batteryVoltage);

  time_t nowEpoch = time(nullptr);
  bool dueForSafetyRefresh =
      (lastFullRefreshTime == 0) || ((nowEpoch - lastFullRefreshTime) >= FULL_REFRESH_SAFETY_INTERVAL_S);
  bool canPrime = havePreviousFrame && !dueForSafetyRefresh &&
                  strcmp(previousFrameVersion, FIRMWARE_VERSION) == 0;

  if (!canPrime) {
    drawSnapshot(current);
    epaper.update();
    lastFullRefreshTime = nowEpoch;
  } else {
    static uint8_t oldFrameBuf[SCREEN_W * SCREEN_H / 8];
    drawSnapshot(previousFrame);
    memcpy(oldFrameBuf, epaper.getPointer(), sizeof(oldFrameBuf));
    drawSnapshot(current);
    epaper.pushPrimedPartial(oldFrameBuf);
  }

  previousFrame = current;
  havePreviousFrame = true;
  strncpy(previousFrameVersion, FIRMWARE_VERSION, sizeof(previousFrameVersion) - 1);
  previousFrameVersion[sizeof(previousFrameVersion) - 1] = '\0';
}

uint64_t getSleepDuration() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) {
    int h = timeinfo.tm_hour;
    int m = timeinfo.tm_min;
    int s = timeinfo.tm_sec;

    // Seconds until the next whole minute
    int secsToNextMin = 60 - s;
    if (secsToNextMin <= 0) secsToNextMin = 60;

    bool isNight = isNightHour(h);
    int intervalMins = isNight ? NIGHT_WAKE_INTERVAL_MIN : DAY_WAKE_INTERVAL_MIN;

    // Align wake to the next interval-minute mark (e.g. :00/:05/:10... for 5 min)
    int minsToNextMark = intervalMins - (m % intervalMins);
    if (minsToNextMark == intervalMins && s == 0) minsToNextMark = intervalMins;
    uint64_t totalSecs = (uint64_t)(minsToNextMark - 1) * 60 + secsToNextMin;

    Serial.printf("Sleep: hour=%d, %llu seconds until next %d-min mark\n", h, totalSecs, intervalMins);
    return totalSecs * 1000000ULL;
  }
  return SLEEP_FALLBACK_US;
}

#endif

void connectWiFi() {
  if (!wifiCredentialsAvailable()) {
    Serial.println("No WiFi credentials provisioned — skipping connect");
    return;
  }

  const char *ssid = activeWifiSsid();
  const char *pass = activeWifiPass();

  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);

  // Skip the AP scan phase when we already know the channel/BSSID from a
  // prior successful connect this charge cycle.
  if (wifiBssidValid) {
    WiFi.begin(ssid, pass, wifiChannel, wifiBssid);
  } else {
    WiFi.begin(ssid, pass);
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    wifiChannel = WiFi.channel();
    memcpy(wifiBssid, WiFi.BSSID(), 6);
    wifiBssidValid = true;
  } else {
    Serial.println(" failed.");
    // Cached channel/BSSID may be stale (AP moved/rebooted) — drop it so the
    // next attempt falls back to a full scan instead of failing again.
    wifiBssidValid = false;
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

// Fetches today's actual high/low + a representative condition description.
// Uses the 5-day/3-hour forecast endpoint rather than the current-weather
// endpoint: the latter's temp_min/temp_max fields are documented by OWM as
// "minimum/maximum temperature at the moment" (a deviation figure for large,
// geographically-spread cities), not the day's real range — aggregating the
// "temp" field across every 3-hour block that falls on today's local date
// gives the actual high/low instead.
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi — skipping weather fetch");
    return false;
  }

  struct tm todayTm;
  if (!getLocalTime(&todayTm, 100)) {
    Serial.println("Weather: no local time available — skipping");
    return false;
  }

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "http://api.openweathermap.org/data/2.5/forecast?lat=%.2f&lon=%.2f&units=imperial&appid=%s",
    userLat, userLon, OWM_API_KEY);

  Serial.println("Fetching weather forecast...");
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("Weather HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  int high = -1000, low = 1000;
  String desc;

  int idx = payload.indexOf("\"dt\":");
  while (idx >= 0) {
    int numStart = idx + 5;
    int numEnd = payload.indexOf(",", numStart);
    if (numEnd < 0) break;
    time_t dt = (time_t)payload.substring(numStart, numEnd).toInt();

    int nextIdx = payload.indexOf("\"dt\":", numEnd);
    int blockEnd = (nextIdx >= 0) ? nextIdx : payload.length();

    struct tm blockTm;
    localtime_r(&dt, &blockTm);
    bool sameDay = (blockTm.tm_year == todayTm.tm_year) &&
                   (blockTm.tm_mon == todayTm.tm_mon) &&
                   (blockTm.tm_mday == todayTm.tm_mday);

    if (sameDay) {
      int tIdx = payload.indexOf("\"temp\":", numEnd);
      if (tIdx >= 0 && tIdx < blockEnd) {
        int temp = (int)payload.substring(tIdx + 7).toFloat();
        if (temp > high) high = temp;
        if (temp < low) low = temp;
      }
      if (desc.length() == 0) {
        int dIdx = payload.indexOf("\"description\":\"", numEnd);
        if (dIdx >= 0 && dIdx < blockEnd) {
          int start = dIdx + 15;
          int end = payload.indexOf("\"", start);
          desc = payload.substring(start, end);
        }
      }
    }

    idx = nextIdx;
  }

  if (high == -1000 || low == 1000) {
    Serial.println("Weather: no forecast blocks for today — skipping");
    return false;
  }

  weatherHigh = high;
  weatherLow = low;
  if (desc.length() > 0) {
    desc[0] = toupper(desc[0]);
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
// only during the night window, and only on normal (non-button) wakes. On
// success this flashes the new firmware and reboots into it; it does not
// return in that case. otaHealth (see ota_health.h) tracks the pending
// version across the reboot and rolls back automatically if it never
// manages to confirm itself healthy.
// forceCheck bypasses the night-window and check-interval gates (used on any
// boot that isn't a resume from our own deep sleep — see the freshStart
// check at the call site — so a device doesn't have to wait for the next
// night window to pick up a newer release after a flash, a reset, or a
// crash/watchdog recovery) but still honors OTA_UPDATES_ENABLED and the
// already-failed-version skip below.
void checkForFirmwareUpdate(bool forceCheck = false) {
  if (!OTA_UPDATES_ENABLED) return;

  struct tm nowTm;
  if (!getLocalTime(&nowTm, 100)) return;
  bool isNight = isNightHour(nowTm.tm_hour);
  if (!isNight && !forceCheck) return;

  time_t nowEpoch = time(nullptr);
  if (!forceCheck && lastOtaCheckTime != 0 && (nowEpoch - lastOtaCheckTime) < OTA_CHECK_INTERVAL_S) return;
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
    otaHealth.recordOtaAttempt(FIRMWARE_VERSION, tag);
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

  // As early as possible, before anything else has a chance to crash — see
  // ota_health.h for what this detects/enforces.
  otaHealth.begin();
  otaHealth.checkBootHealth();

  loadLocationConfig();
  loadWifiConfig();

  // One-time migration: a device updating from older firmware that had real
  // WiFi credentials compiled in lands here with NVS still unconfigured.
  // Capture the compiled-in fallback into NVS now, while this release still
  // carries it, so a future release can blank out WIFI_SSID/WIFI_PASS
  // without stranding this device (see "TEMPORARY, ONE RELEASE ONLY" in
  // release-firmware.yml and "Location & WiFi provisioning" in CLAUDE.md).
  // Self-limiting: once wifiConfigured is true this never runs again, and a
  // build with a blank compiled-in SSID (the eventual steady state) has
  // nothing to migrate.
  if (!wifiConfigured && WIFI_SSID[0] != '\0') {
    Serial.println("Migrating compiled-in WiFi credentials to NVS");
    saveWifiSsid(WIFI_SSID);
    saveWifiPass(WIFI_PASS);
  }

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
    sdfDrawCentreTextTL(epaper, InterBold, SCREEN_W / 2, SCREEN_H / 2 - 12, "Syncing...", FONT_PX_HEADING, 1.0f, TFT_BLACK);

    char fwStr[24];
    snprintf(fwStr, sizeof(fwStr), "fw %s", FIRMWARE_VERSION);
    int fwW = sdfTextWidth(InterRegular, fwStr, FONT_PX_BODY);
    sdfDrawTextTL(epaper, InterRegular, SCREEN_W - 40 - fwW, SCREEN_H - 26 - FONT_PX_BODY, fwStr, FONT_PX_BODY, 1.0f, TFT_BLACK);

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

  // Fetch weather every ~3 hours during daytime (not 11pm-6am)
  // Always update lastWeatherSyncTime on attempt to avoid hammering API on failure
  struct tm now;
  bool isDaytime = true;
  if (getLocalTime(&now, 100)) {
    isDaytime = !isNightHour(now.tm_hour);
  }
  time_t weatherNow = time(nullptr);
  bool weatherDue = (weatherNow - lastWeatherSyncTime) >= WEATHER_SYNC_INTERVAL_S;
  bool shouldFetchWeather = locationConfigured && isDaytime && (weatherDue || (weatherRetries > 0 && weatherRetries <= WEATHER_MAX_RETRIES));
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
    Serial.println("Button wake detected — entering maintenance mode (serial provisioning)");

    // Maintenance mode is serial-only now — WiFi/location provisioning runs
    // over USB and doesn't need WiFi itself. Firmware updates come from the
    // nightly GitHub-releases check (checkForFirmwareUpdate()), not from here.
    maintenanceMode = true;
    drawMaintenanceScreen();
    epaper.update();
    maintenanceStartedAt = millis();
  } else {
    // Force the check on any boot that didn't resume from our own deep
    // sleep — a fresh flash, a manual reset-button/EN-pin press, ESP.restart()
    // (e.g. the CFG REBOOT provisioning command), a watchdog/panic recovery,
    // a brownout, etc. — rather than only on the device's very first-ever
    // boot. A normal periodic wake reports ESP_RST_DEEPSLEEP here and stays
    // on the usual night-window/24h-throttle gate.
    bool freshStart = (esp_reset_reason() != ESP_RST_DEEPSLEEP);
    checkForFirmwareUpdate(freshStart);  // may flash new firmware and reboot; does not return in that case
    lastVoltage = voltage;
    refreshClockDisplay(lastVoltage);
  }

  // Reaching here means this wake cycle ran to completion without crashing
  // or hanging — good enough proof to cancel any pending OTA rollback watch.
  otaHealth.confirmHealthy();

  if (!hasSleptOnce) {
    Serial.printf("First boot — firmware %s\n", FIRMWARE_VERSION);
  }
  setupDoneAt = millis();
#endif
}

void loop() {
  // Maintenance mode: listen for serial location provisioning, then sleep
  if (maintenanceMode) {
    handleSerialProvisioning();
    unsigned long elapsed = millis() - maintenanceStartedAt;
    if (elapsed >= PROVISION_LISTEN_MS) {
      Serial.println("Maintenance listen window expired, drawing clock and going to sleep");
      maintenanceMode = false;
      lastVoltage = readBatteryVoltage();
      refreshClockDisplay(lastVoltage);
      uint64_t sleepDuration = getSleepDuration();
      enterDeepSleep(sleepDuration);
    }
    delay(10);
    return;
  }

  // First boot: stay awake so IDE can push updates via serial, and so
  // location provisioning can happen before the first sleep
  if (!hasSleptOnce) {
    handleSerialProvisioning();
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

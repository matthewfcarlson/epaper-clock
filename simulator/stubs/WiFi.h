#pragma once
#include "Arduino.h"

#define WL_CONNECTED    3
#define WL_DISCONNECTED 6
#define WIFI_STA        1
#define WIFI_OFF        0

// wifi_power_t stand-ins (real values/enum live in esp_wifi_types.h)
typedef int wifi_power_t;
#define WIFI_POWER_19_5dBm 78
#define WIFI_POWER_11dBm   44

struct WiFiClass {
    void mode(int)                  {}
    void begin(const char *, const char *) {}
    void begin(const char *, const char *, int32_t, const uint8_t *) {}
    void setTxPower(wifi_power_t)   {}
    int  status()                   { return WL_CONNECTED; }
    IPAddress localIP()             { IPAddress ip; ip.bytes[0]=127; ip.bytes[3]=1; return ip; }
    void disconnect(bool = false)   {}
    int32_t channel()                { return 1; }
    uint8_t *BSSID()                 { static uint8_t bssid[6] = {0,0,0,0,0,0}; return bssid; }
};
inline WiFiClass WiFi;

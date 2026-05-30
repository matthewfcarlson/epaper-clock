#pragma once
#include "Arduino.h"

#define WL_CONNECTED    3
#define WL_DISCONNECTED 6
#define WIFI_STA        1
#define WIFI_OFF        0

struct WiFiClass {
    void mode(int)                  {}
    void begin(const char *, const char *) {}
    int  status()                   { return WL_CONNECTED; }
    IPAddress localIP()             { IPAddress ip; ip.bytes[0]=127; ip.bytes[3]=1; return ip; }
    void disconnect(bool = false)   {}
};
inline WiFiClass WiFi;

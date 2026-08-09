#pragma once
#include "Arduino.h"

// Minimal stand-in for ESP32's Update (esp_ota) library, plus a bare-bones
// WiFiClient type for HTTPClient::getStreamPtr()'s return value. The
// simulator never actually flashes anything — the GitHub release check
// always reports the current version as up to date (see HTTPClient.h), so
// this code path is unreachable in practice, but it must still compile.

#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFFUL

struct WiFiClient {
};

struct UpdateClass {
    bool   begin(size_t = UPDATE_SIZE_UNKNOWN) { return false; }
    size_t writeStream(WiFiClient &)            { return 0; }
    bool   end(bool = false)                    { return false; }
    const char *errorString()                   { return "sim: firmware update not supported"; }
};
inline UpdateClass Update;

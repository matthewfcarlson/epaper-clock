#pragma once
#include "Arduino.h"

// Minimal stand-in for ESP32's WiFiClientSecure. No TLS is actually performed
// in the simulator — HTTPClient's stub ignores the client and fakes
// responses locally (see HTTPClient.h).
struct WiFiClientSecure {
    void setInsecure() {}
};

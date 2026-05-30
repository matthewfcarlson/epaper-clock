#pragma once
#include "Arduino.h"

// Fake weather response that the sketch's simple JSON parser can extract
static const char *FAKE_WEATHER_JSON =
    "{\"weather\":[{\"description\":\"clear sky\"}],"
    "\"main\":{\"temp\":82.0,\"temp_min\":71.0,\"temp_max\":88.0}}";

struct HTTPClient {
    void begin(const char *) {}
    int  GET()               { return 200; }
    String getString()       { return String(FAKE_WEATHER_JSON); }
    void end()               {}
};

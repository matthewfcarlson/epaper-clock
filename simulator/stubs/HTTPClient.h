#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#include "Update.h"  // for the WiFiClient stand-in returned by getStreamPtr()

// Fake weather response that the sketch's simple JSON parser can extract
static const char *FAKE_WEATHER_JSON =
    "{\"weather\":[{\"description\":\"clear sky\"}],"
    "\"main\":{\"temp\":82.0,\"temp_min\":71.0,\"temp_max\":88.0}}";

// Fake GitHub release response — tag_name is always below any real
// FIRMWARE_VERSION so the simulator never attempts a self-update.
static const char *FAKE_GITHUB_RELEASE_JSON =
    "{\"tag_name\":\"v0.0.0\",\"assets\":["
    "{\"browser_download_url\":\"https://example.invalid/firmware.bin\"}]}";

enum followRedirects_t { HTTPC_DISABLE_FOLLOW_REDIRECTS, HTTPC_FORCE_FOLLOW_REDIRECTS };

struct HTTPClient {
    String url_;
    void begin(const char *url)                    { url_ = url; }
    void begin(WiFiClientSecure &, const char *url) { url_ = url; }
    void addHeader(const char *, const char *)      {}
    void setFollowRedirects(followRedirects_t)      {}
    int  GET()                                      { return 200; }
    int  getSize()                                  { return -1; }
    WiFiClient *getStreamPtr()                      { static WiFiClient c; return &c; }
    String getString() {
        if (url_.indexOf("api.github.com") >= 0) return String(FAKE_GITHUB_RELEASE_JSON);
        return String(FAKE_WEATHER_JSON);
    }
    void end() {}
};

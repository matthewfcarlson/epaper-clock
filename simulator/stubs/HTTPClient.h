#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#include "Update.h"  // for the WiFiClient stand-in returned by getStreamPtr()

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
    // Auto-reporting (reportOtaFailure() in main.cpp) never actually fires
    // in the simulator — no relay token is ever provisioned, since Serial
    // input is stubbed to report "no input" (see "Key simulator behaviors"
    // in CLAUDE.md) — so this only needs to exist for compilation.
    int  POST(const String &)                       { return 201; }
    int  getSize()                                  { return -1; }
    WiFiClient *getStreamPtr()                      { static WiFiClient c; return &c; }
    String getString() {
        if (url_.indexOf("api.github.com") >= 0) return String(FAKE_GITHUB_RELEASE_JSON);

        // Fake 5-day/3-hour forecast response: a few blocks around "now"
        // with distinct temps, mimicking OWM's shape closely enough for the
        // sketch's dt-based same-day high/low aggregation (see fetchWeather()).
        time_t now = time(nullptr);
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"list\":["
            "{\"dt\":%ld,\"main\":{\"temp\":71.0},\"weather\":[{\"description\":\"clear sky\"}]},"
            "{\"dt\":%ld,\"main\":{\"temp\":88.0},\"weather\":[{\"description\":\"clear sky\"}]},"
            "{\"dt\":%ld,\"main\":{\"temp\":79.0},\"weather\":[{\"description\":\"clear sky\"}]}"
            "]}",
            (long)(now - 3600), (long)now, (long)(now + 3600));
        return String(buf);
    }
    void end() {}
};

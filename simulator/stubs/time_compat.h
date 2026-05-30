#pragma once
#include <time.h>
#include <stdlib.h>

// configTzTime: set timezone and NTP servers (NTP ignored in simulator)
inline void configTzTime(const char *tz, const char *, const char * = nullptr) {
    setenv("TZ", tz, 1);
    tzset();
}

// getLocalTime: fills tm from localtime(); returns true on success
inline bool getLocalTime(struct tm *info, int /*timeoutMs*/ = 5000) {
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    if (!t) return false;
    *info = *t;
    return true;
}

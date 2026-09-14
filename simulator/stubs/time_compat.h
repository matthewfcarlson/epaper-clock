#pragma once
#include <time.h>
#include <stdlib.h>

// Simulator time override: 0 = disabled (use the Mac's real wall-clock time).
// Set by main() from the SIM_TIME env var (see main.cpp) to freeze "now" for
// testing time-of-day-dependent behavior — night wake cadence, NTP/weather
// sync gating, the nightly firmware-check window — without waiting for real
// time to pass. Every time(nullptr)/getLocalTime() call in the sketch
// (src/main.cpp) goes through this, via the #define time below.
inline time_t g_sim_time_override = 0;

inline time_t sim_time(time_t *out) {
    time_t now = g_sim_time_override != 0 ? g_sim_time_override : ::time(nullptr);
    if (out) *out = now;
    return now;
}

// configTzTime: set timezone and NTP servers (NTP ignored in simulator)
inline void configTzTime(const char *tz, const char *, const char * = nullptr) {
    setenv("TZ", tz, 1);
    tzset();
}

// getLocalTime: fills tm from sim_time(); returns true on success
inline bool getLocalTime(struct tm *info, int /*timeoutMs*/ = 5000) {
    time_t now = sim_time(nullptr);
    struct tm *t = localtime(&now);
    if (!t) return false;
    *info = *t;
    return true;
}

// Redirect every remaining time(nullptr)/time(&x) call — throughout
// src/main.cpp, included further down — through the override too. Must come
// after the definitions above, which still need the real ::time().
#define time(x) sim_time(x)

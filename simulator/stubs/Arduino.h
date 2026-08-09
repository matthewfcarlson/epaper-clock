#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <functional>

// --- Types ---
typedef bool     boolean;
typedef uint8_t  byte;

// --- Pin constants ---
#define HIGH 1
#define LOW  0
#define INPUT       0
#define OUTPUT      1
#define INPUT_PULLUP 2
#define LED_BUILTIN 13
#define D1 1
#define D2 2
#define D4 4
#define A0 0

typedef int adc_attenuation_t;
#define ADC_11db 3

// RTC data survives deep sleep on ESP32; in simulator these are plain globals
#define RTC_DATA_ATTR

// Flash storage on ESP32 — regular RAM in simulator
#define PROGMEM
#define pgm_read_byte(addr)  (*(const uint8_t *)(addr))
#define pgm_read_word(addr)  (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))

// --- Timing ---
inline unsigned long millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
inline unsigned long micros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}
inline void delay(unsigned long ms)      { usleep((useconds_t)(ms * 1000)); }
inline void delayMicroseconds(unsigned int us) { usleep(us); }

// --- GPIO stubs ---
inline void pinMode(int, int)            {}
inline void digitalWrite(int, int)       {}
inline int  digitalRead(int)             { return HIGH; }
inline int  analogRead(int)              { return 2048; }
inline void analogReadResolution(int)    {}
inline void analogSetPinAttenuation(int, adc_attenuation_t) {}

// --- String class --- (defined before SerialClass so println can use it)
class String {
public:
    String()                    {}
    String(const char *s)       : s_(s ? s : "") {}
    String(char c)              : s_(1, c) {}
    String(int v)               : s_(std::to_string(v)) {}
    String(long v)              : s_(std::to_string(v)) {}
    String(float v, int d = 2)  { char buf[32]; snprintf(buf,sizeof(buf),"%.*f",d,v); s_=buf; }

    const char *c_str()  const  { return s_.c_str(); }
    int  length()        const  { return (int)s_.size(); }
    bool isEmpty()       const  { return s_.empty(); }

    int indexOf(const char *sub, int from = 0) const {
        auto p = s_.find(sub, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    String substring(int from, int to = -1) const {
        if (to < 0) return String(s_.substr(from).c_str());
        return String(s_.substr(from, to - from).c_str());
    }
    float  toFloat() const { return strtof(s_.c_str(), nullptr); }
    int    toInt()   const { return atoi(s_.c_str()); }

    char &operator[](int i)        { return s_[i]; }
    char  operator[](int i) const  { return s_[i]; }
    String  operator+(const String &o) const { return String((s_ + o.s_).c_str()); }
    String  operator+(const char *o)   const { return String((s_ + o).c_str()); }
    String &operator=(const char *s)   { s_ = s ? s : ""; return *this; }
    String &operator=(const String &o) { s_ = o.s_; return *this; }
    bool operator==(const char *s) const { return s_ == s; }
    bool operator!=(const char *s) const { return s_ != s; }

private:
    std::string s_;
};
inline String operator+(const char *a, const String &b) {
    return String((std::string(a) + b.c_str()).c_str());
}

// --- IPAddress ---
struct IPAddress {
    uint8_t bytes[4] = {};
    String toString() const {
        char buf[16];
        snprintf(buf,sizeof(buf),"%d.%d.%d.%d",bytes[0],bytes[1],bytes[2],bytes[3]);
        return String(buf);
    }
};

// --- Serial stub (defined after String so println(const String&) compiles) ---
struct SerialClass {
    void begin(int)  {}
    void flush()     { fflush(stdout); }

    void print(const char *s)     { fputs(s ? s : "", stdout); }
    void print(const String &s)   { fputs(s.c_str(), stdout); }
    void print(int v)             { printf("%d", v); }
    void print(long v)            { printf("%ld", v); }
    void print(float v, int d=2)  { printf("%.*f", d, v); }

    void println()                { putchar('\n'); }
    void println(const char *s)   { puts(s ? s : ""); }
    void println(const String &s) { puts(s.c_str()); }
    void println(int v)           { printf("%d\n", v); }
    void println(long v)          { printf("%ld\n", v); }
    void println(float v, int d=2){ printf("%.*f\n", d, v); }
    // Catch-all for types with toString() (e.g. IPAddress)
    template<typename T>
    auto println(const T &v) -> decltype(v.toString(), void()) {
        puts(v.toString().c_str());
    }

    // Use a format wrapper to avoid -Wformat-security on the pass-through
    template<typename... Args>
    void printf(const char *fmt, Args... args) { ::printf(fmt, args...); }
};
inline SerialClass Serial;

// --- ESP restart --- (used by the firmware auto-update path; unreachable in
// the simulator since the GitHub release check always reports "up to date",
// see HTTPClient.h). Reuses the same sleep-request flag main.cpp already
// polls to end a loop() cycle and re-enter setup(), the closest analog to a
// real device restart.
extern bool g_sleep_requested;
struct EspClass {
    void restart() { g_sleep_requested = true; }
};
inline EspClass ESP;

// --- Math helpers ---
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#endif
template<typename T> T constrain(T v, T lo, T hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

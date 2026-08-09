#pragma once
#include <map>
#include <string>
#include "Arduino.h"

// ESP32 NVS key-value store — in simulator, an in-memory map per namespace.
// This persists for the life of one `./sim` run, the same way RTC_DATA_ATTR
// globals do here (see esp_sleep.h), but — unlike real NVS — does not
// survive across separate `./sim` invocations the way flash survives a real
// power cycle. That's a deliberate simplification: nothing in the sketch
// depends on provisioned location surviving a fresh process launch.
class Preferences {
public:
    void begin(const char *ns, bool readOnly = false) {
        ns_ = ns ? ns : "";
        readOnly_ = readOnly;
    }
    void end() {}

    bool putBool(const char *key, bool v)   { if (readOnly_) return false; store()[ns_ + "/" + key] = v ? "1" : "0"; return true; }
    bool getBool(const char *key, bool def) const {
        auto it = store().find(ns_ + "/" + key);
        return it == store().end() ? def : (it->second == "1");
    }

    bool putFloat(const char *key, float v) {
        if (readOnly_) return false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", v);
        store()[ns_ + "/" + key] = buf;
        return true;
    }
    float getFloat(const char *key, float def) const {
        auto it = store().find(ns_ + "/" + key);
        return it == store().end() ? def : strtof(it->second.c_str(), nullptr);
    }

private:
    static std::map<std::string, std::string> &store() {
        static std::map<std::string, std::string> s;
        return s;
    }
    std::string ns_;
    bool readOnly_ = false;
};

#pragma once
#include <functional>

struct ArduinoOTAClass {
    void setHostname(const char *) {}
    void onStart(std::function<void()>) {}
    void onEnd(std::function<void()>)   {}
    void onError(std::function<void(int)>) {}
    void begin()  {}
    void handle() {}
};
inline ArduinoOTAClass ArduinoOTA;

typedef int ota_error_t;

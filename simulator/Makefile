CXX      = clang++
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
TTF_INC    := -I/opt/homebrew/include/SDL2
TTF_LIB    := -L/opt/homebrew/lib -lSDL2_ttf

CXXFLAGS = -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable \
           -Wno-deprecated-declarations -Wno-format-security \
           -I stubs -I .. \
           $(SDL_CFLAGS) $(TTF_INC)

LDFLAGS  = $(SDL_LIBS) $(TTF_LIB)

.PHONY: all clean run faces

all: sim

faces: experiments/faces.cpp EPaperSim.h stubs/Arduino.h stubs/TFT_eSPI.h ../src/BigDigits.h
	$(CXX) $(CXXFLAGS) experiments/faces.cpp -o experiments/faces $(LDFLAGS)

sim: main.cpp ../src/ota_health.cpp ../src/ota_health.h EPaperSim.h stubs/Arduino.h stubs/TFT_eSPI.h stubs/WiFi.h \
     stubs/WiFiClientSecure.h stubs/Update.h \
     stubs/HTTPClient.h stubs/esp_sleep.h stubs/esp_system.h stubs/esp_ota_ops.h stubs/time_compat.h \
     stubs/Preferences.h
	$(CXX) $(CXXFLAGS) main.cpp ../src/ota_health.cpp -o sim $(LDFLAGS)

run: sim
	./sim

clean:
	rm -f sim

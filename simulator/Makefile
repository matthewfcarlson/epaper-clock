UNAME_S := $(shell uname -s)

# Make's implicit rules already set CXX=g++ by default (origin "default"),
# so a plain `CXX ?= ...` here would never take effect; only override it
# when the user/environment hasn't chosen a compiler.
ifeq ($(origin CXX),default)
CXX := clang++
endif
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

# SDL2_ttf's pkg-config file is available on Linux distro packages but not
# from Homebrew, so try pkg-config first and fall back to locating Homebrew's
# prefix directly (which differs between Apple Silicon and Intel Macs) before
# falling back further to a bare -lSDL2_ttf for anything else.
HAVE_PKGCONFIG_TTF := $(shell pkg-config --exists SDL2_ttf 2>/dev/null && echo 1)
ifeq ($(HAVE_PKGCONFIG_TTF),1)
  TTF_CFLAGS := $(shell pkg-config --cflags SDL2_ttf)
  TTF_LIBS   := $(shell pkg-config --libs SDL2_ttf)
else ifeq ($(UNAME_S),Darwin)
  BREW_PREFIX := $(shell brew --prefix sdl2_ttf 2>/dev/null)
  ifeq ($(BREW_PREFIX),)
    BREW_PREFIX := $(firstword $(wildcard /opt/homebrew) $(wildcard /usr/local))
  endif
  TTF_CFLAGS := -I$(BREW_PREFIX)/include/SDL2
  TTF_LIBS   := -L$(BREW_PREFIX)/lib -lSDL2_ttf
else
  TTF_CFLAGS :=
  TTF_LIBS   := -lSDL2_ttf
endif

CXXFLAGS = -std=c++17 -Wall -Wno-unused-function -Wno-unused-variable \
           -Wno-deprecated-declarations -Wno-format-security \
           -D EPAPER_SIM_BUILD \
           -I stubs -I .. \
           $(SDL_CFLAGS) $(TTF_CFLAGS)

LDFLAGS  = $(SDL_LIBS) $(TTF_LIBS)

.PHONY: all clean run faces

all: sim

faces: experiments/faces.cpp EPaperSim.h stubs/Arduino.h stubs/TFT_eSPI.h ../src/sdf_font.h ../src/fonts/Inter_Bold_sdf.h
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

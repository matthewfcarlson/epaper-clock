#pragma once

#include <TFT_eSPI.h>

// Adds a partial-refresh path that actually primes the UC8179 controller's
// old-image comparison buffer before pushing the new frame, instead of
// relying on state left over in the controller from a previous SPI session.
//
// Stock Seeed_GFX (Extensions/EPaper.cpp) can't do this: EPaper::update()
// (full refresh) pushes both old and new image data from the same buffer,
// so it never depends on anything surviving between calls, but
// EPaper::updataPartial() only pushes new data and assumes the controller's
// SRAM already holds an accurate old image — which isn't true here, since
// this firmware puts the panel to sleep (EPD_SLEEP, which clears that SRAM
// per the UC8179 datasheet) after every single display write, and the MCU
// itself deep-sleeps between every wake. See the long comment on
// refreshClockDisplay() in main.cpp for the full story and why plain
// updataPartial() corrupts the display instead of merely ghosting it.
//
// EPAPER_SIM_BUILD (defined by simulator/Makefile) selects a trivial stand-in
// instead: the simulator has no controller SRAM or waveform to prime, and
// renders straight to SDL rather than through Seeed_GFX/TFT_eSPI at all, so
// none of the real implementation below is even meaningful there.
#ifdef EPAPER_SIM_BUILD

class EPaperPartial : public EPaper {
public:
    void pushPrimedPartial(const uint8_t* /*oldImg*/) {
        update();
    }
};

#else

// _img8/_width/_height are `protected` in TFT_eSprite (EPaper's base
// class), and the EPD_* macros/TFT_RST used below come from
// TFT_Drivers/UC8179_Defines.h, which TFT_eSPI.h already pulls in globally
// for this board (BOARD_SCREEN_COMBO=502 selects UC8179_DRIVER) — so this
// works as a plain subclass added to this repo, no fork of Seeed_GFX
// needed.
class EPaperPartial : public EPaper {
public:
    // Pushes oldImg as the controller's old-image baseline, the sprite's
    // current buffer as the new image, and triggers a partial-waveform
    // refresh. Full screen only — this app never does a sub-rectangle
    // update, so there's no need to replicate updataPartial()'s
    // rotation/alignment windowing logic. oldImg must be _width*_height/8
    // bytes, packed 1bpp row-major — i.e. a copy of a previous
    // getPointer() on this same object, not arbitrary image data.
    //
    // Deliberately bypasses the inherited wake()/sleep(): those gate on
    // EPaper's private _sleep flag, which we have no access to and which
    // starts true every boot regardless of real hardware state. This app
    // only ever does one display write per wake, so there's no need for
    // that idempotency check — we always do a fresh RESET + partial init
    // here, and always end by putting the panel to sleep via the raw
    // EPD_SLEEP() macro rather than the wrapper, since the wrapper would
    // see _sleep still at its constructed default (true), treat the panel
    // as already asleep, and skip issuing EPD_SLEEP() — leaving the panel
    // powered for the whole time the MCU is deep-sleeping.
    void pushPrimedPartial(const uint8_t* oldImg) {
        digitalWrite(TFT_RST, LOW);
        delay(10);
        digitalWrite(TFT_RST, HIGH);
        delay(10);
        CHECK_BUSY();
        EPD_INIT_PARTIAL();

        EPD_SET_WINDOW(0, 0, (_width - 1), (_height - 1));

        int32_t bytes = _width * _height / 8;

        writecommand(0x10);  // old-image data
        for (int32_t i = 0; i < bytes; i++) {
            writedata(oldImg[i]);
        }

        writecommand(0x13);  // new-image data
        for (int32_t i = 0; i < bytes; i++) {
            writedata(_img8[i]);
        }

        EPD_UPDATE_PARTIAL();

        EPD_SLEEP();
    }
};

#endif

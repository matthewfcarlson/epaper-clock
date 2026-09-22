#pragma once

#include "EPaperSim.h"

// Simulates the UC8179 controller's old/new diffing behavior closely
// enough to catch real bugs in main.cpp's partial-refresh priming logic —
// see src/epaper_partial.h and refreshClockDisplay() in src/main.cpp for
// the real story this mirrors. Two buffers, matching real hardware:
//
//   physicalBuffer_      — ground truth: what's actually "printed" on the
//                          simulated panel right now. E-ink is bistable,
//                          so this persists across everything.
//   controllerOldBuffer_ — what the *controller* currently believes the
//                          old image is, used to pick the refresh
//                          waveform. On real hardware this is controller
//                          SRAM that does not survive EPD_SLEEP() (see
//                          epaper_partial.h); controllerOldValid_ models
//                          that — sleep clears it, and only an explicit
//                          priming push (pushPrimedPartial(), or
//                          update()'s own old==new trick) makes it valid
//                          again.
//
// The rule this enforces: a refresh only renders correctly wherever the
// controller's belief about the old image actually matches the true
// physical state. Wherever it's wrong, the simulated result is a visible
// noise pattern instead of the correct new pixel — modeling a real panel
// selecting the wrong per-pixel waveform when fed the wrong "old" data,
// rather than quietly "working out fine anyway." So a run only ever looks
// clean when main.cpp's reconstructed priming buffer genuinely matches
// what's really on the panel — exactly the property the real firmware's
// ghosting fix depends on, and exactly what this is meant to catch a
// regression in.
class EPaperPartial : public EPaper {
public:
    static constexpr size_t BUF_BYTES = (size_t)SCREEN_W * SCREEN_H / 8;

    // Full refresh manufactures its own old==new baseline (see
    // src/epaper_partial.h's comment on the real EPaper::update()) — always
    // clean, and resyncs both simulated buffers to match.
    void update() {
        uint8_t newBuf[BUF_BYTES];
        captureRenderedBuffer(newBuf);
        memcpy(physicalBuffer_, newBuf, BUF_BYTES);
        memcpy(controllerOldBuffer_, newBuf, BUF_BYTES);
        controllerOldValid_ = true;
        presentAndMaybeExport();
        simulateSleep();
    }

    // Mirrors real EPaper::updataPartial() (Seeed_GFX): never primes the
    // controller's old-image buffer at all — the original bug. Not called
    // anywhere in main.cpp (which always uses pushPrimedPartial() below),
    // but kept and reachable so the simulator can reproduce the original
    // ghosting bug on demand — call it directly in place of
    // pushPrimedPartial() to see what it looked like.
    void updataPartial(int, int, int, int) {
        uint8_t newBuf[BUF_BYTES];
        captureRenderedBuffer(newBuf);
        applyRefresh(newBuf);
        presentAndMaybeExport();
        simulateSleep();
    }

    // The fix: explicitly primes the controller with oldImg before pushing
    // the new frame — mirrors src/epaper_partial.h's real
    // EPaperPartial::pushPrimedPartial() pushing an explicit old-image
    // buffer before the new one.
    void pushPrimedPartial(const uint8_t *oldImg) {
        memcpy(controllerOldBuffer_, oldImg, BUF_BYTES);
        controllerOldValid_ = true;

        uint8_t newBuf[BUF_BYTES];
        captureRenderedBuffer(newBuf);
        applyRefresh(newBuf);
        presentAndMaybeExport();
        simulateSleep();
    }

    // Real EPaper::getPointer() (Seeed_GFX) returns the sprite's packed
    // pixel buffer; refreshClockDisplay() uses it to snapshot a frame for
    // priming the next wake's partial refresh. physicalBuffer_ is exactly
    // that here — what's really shown, which is what a real getPointer()
    // read back after a real refresh would also reflect.
    void *getPointer() { return physicalBuffer_; }

private:
    uint8_t physicalBuffer_[BUF_BYTES] = {};
    uint8_t controllerOldBuffer_[BUF_BYTES] = {};
    bool controllerOldValid_ = false;

    // Computes the visible result of a partial-style refresh from
    // controllerOldBuffer_ (the controller's belief) vs newBuf (what's
    // being pushed), writes it into physicalBuffer_, and blits it back
    // onto the render target so it's what actually gets presented/saved —
    // i.e. a wrong belief produces a visibly wrong saved JPEG, not just a
    // wrong buffer nobody looks at.
    void applyRefresh(const uint8_t *newBuf) {
        uint8_t result[BUF_BYTES];
        for (size_t i = 0; i < BUF_BYTES; i++) {
            uint8_t claimedOld = controllerOldValid_ ? controllerOldBuffer_[i] : unknownSramByte();
            uint8_t truePrev = physicalBuffer_[i];
            uint8_t newVal = newBuf[i];
            // Per pixel (bitwise, 8 pixels/byte): correct wherever the
            // controller's belief matches reality; a fixed dither pattern
            // everywhere it doesn't — an incorrectly-selected waveform,
            // not a silently-fine result.
            uint8_t matchMask = (uint8_t)~(claimedOld ^ truePrev);  // 1 bits = correct belief
            uint8_t noise = corruptionByte(i);
            result[i] = (uint8_t)((matchMask & newVal) | (~matchMask & noise));
        }
        memcpy(physicalBuffer_, result, BUF_BYTES);
        blitPackedBuffer(physicalBuffer_);
    }

    // Stand-in for unknowable controller SRAM (no explicit priming since
    // the last sleep): a fixed "believes everything is white" value.
    // Deliberately NOT the same pattern as corruptionByte() below — using
    // one pattern for both let a corrupted result from one refresh get
    // baked into physicalBuffer_ and then coincidentally match this same
    // "unknown" assumption on the *next* unprimed refresh, masking the bug
    // instead of showing it persistently.
    static uint8_t unknownSramByte() {
        return 0x00;
    }

    // Deterministic per-byte checkerboard: the visible corruption pattern
    // rendered wherever the controller's belief doesn't match reality.
    static uint8_t corruptionByte(size_t i) {
        return (i % 2 == 0) ? 0xAA : 0x55;
    }

    void simulateSleep() {
        // Mirrors real EPD_SLEEP(): the controller's old-image SRAM does
        // not survive — see src/epaper_partial.h.
        controllerOldValid_ = false;
    }
};

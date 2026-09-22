#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Colors as 0xRRGGBB (top byte ignored)
#define TFT_BLACK 0x000000u
#define TFT_WHITE 0xFFFFFFu

// GFX font stub — real struct has glyph data; we only use pointSize/bold
struct GFXfont { int pointSize; bool bold = false; };

// Font instances referenced by the sketch
inline const GFXfont FreeSans12pt7b  = {14};
inline const GFXfont FreeSans18pt7b  = {20};
inline const GFXfont FreeSans24pt7b  = {28};
inline const GFXfont FreeSansBold9pt7b  = {12, true};
inline const GFXfont FreeSansBold12pt7b = {15, true};
inline const GFXfont FreeSansBold18pt7b = {21, true};
inline const GFXfont FreeSansBold24pt7b = {29, true};

#define SCREEN_W 800
#define SCREEN_H 480

// Map TFT_eSPI built-in font numbers to point sizes
static int builtinFontPt(int fontNum) {
    // Sizes track the real TFT_eSPI numbered-font pixel heights so the sim
    // matches hardware (font 1 GLCD = 8px, font 2 = 16px, font 4 ~26px, font 7 = 48px).
    switch (fontNum) {
        case 1:  return 8;
        case 2:  return 16;
        case 4:  return 24;
        case 7:  return 48;
        default: return 14;
    }
}

class EPaper {
public:
    EPaper() {}

    // If set before begin(), each update() saves a numbered JPEG:
    //   "clock.jpg" -> "clock_01.jpg", "clock_02.jpg", ...
    // The main loop exits after the full boot cycle completes.
    void setExportPath(const char *path) {
        exportBase_ = path;
        // Strip extension so we can insert "_NN" before it
        std::string s(path);
        auto dot = s.rfind('.');
        if (dot != std::string::npos) {
            exportStem_ = s.substr(0, dot);
            exportExt_  = s.substr(dot);  // includes the dot
        } else {
            exportStem_ = s;
            exportExt_  = ".jpg";
        }
    }

    bool exportMode() const { return !exportBase_.empty(); }

    void begin() {
        if (window_) return; // idempotent across simulated reboots
        SDL_Init(SDL_INIT_VIDEO);
        TTF_Init();
        window_ = SDL_CreateWindow("e-Paper Clock Simulator",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);

        // Persistent render-target texture: all drawing goes here so the frame
        // survives SDL_RenderPresent (which may invalidate the backbuffer).
        target_ = SDL_CreateTexture(renderer_,
                                    SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_TARGET,
                                    SCREEN_W, SCREEN_H);
        SDL_SetRenderTarget(renderer_, target_);

        // Pre-load font at common sizes
        static const int SIZES[] = {12, 14, 16, 20, 24, 28, 48};
        for (int sz : SIZES) loadFont(sz);
        setColor(TFT_BLACK);
    }

    void setRotation(int) {}

    void fillScreen(uint32_t color) {
        setColor(color);
        SDL_RenderClear(renderer_);
    }

    void setTextColor(uint32_t fg, uint32_t /*bg*/ = TFT_WHITE) {
        textColor_ = fg;
    }
    void setTextSize(int s) { textScale_ = s; }
    void setFreeFont(const GFXfont *f) { freeFont_ = f; }

    // --- Primitives (all draw into target_) ---
    void drawPixel(int x, int y, uint32_t color) {
        setColor(color);
        SDL_RenderDrawPoint(renderer_, x, y);
    }
    void drawLine(int x0, int y0, int x1, int y1, uint32_t color) {
        setColor(color);
        SDL_RenderDrawLine(renderer_, x0, y0, x1, y1);
    }
    void drawRect(int x, int y, int w, int h, uint32_t color) {
        setColor(color);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderDrawRect(renderer_, &r);
    }
    void fillRect(int x, int y, int w, int h, uint32_t color) {
        setColor(color);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(renderer_, &r);
    }
    void fillCircle(int cx, int cy, int r, uint32_t color) {
        setColor(color);
        for (int dy = -r; dy <= r; dy++) {
            int dx = (int)sqrt((double)(r * r - dy * dy));
            SDL_RenderDrawLine(renderer_, cx - dx, cy + dy, cx + dx, cy + dy);
        }
    }
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
        setColor(color);
        if (y0 > y1) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
        if (y0 > y2) { int t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
        if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
        for (int y = y0; y <= y2; y++) {
            int lx = (y < y1) ? interp(x0,y0,x1,y1,y) : interp(x1,y1,x2,y2,y);
            int rx = interp(x0,y0,x2,y2,y);
            if (lx > rx) { int t = lx; lx = rx; rx = t; }
            SDL_RenderDrawLine(renderer_, lx, y, rx, y);
        }
    }

    // --- Text ---
    int textWidth(const char *str, int fontNum) {
        TTF_Font *f = fontAt(ptForContext(fontNum));
        if (!f) return strlen(str) * 8;
        TTF_SetFontStyle(f, isBoldContext(fontNum) ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);
        int w = 0, h = 0;
        TTF_SizeUTF8(f, str, &w, &h);
        return w;
    }
    void drawString(const char *str, int x, int y, int fontNum) {
        renderText(str, x, y, fontNum);
    }
    void drawCentreString(const char *str, int cx, int y, int fontNum) {
        TTF_Font *f = fontAt(ptForContext(fontNum));
        if (!f) return;
        TTF_SetFontStyle(f, isBoldContext(fontNum) ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);
        int w = 0, h = 0;
        TTF_SizeUTF8(f, str, &w, &h);
        renderText(str, cx - w / 2, y, fontNum);
    }

    // Present the current frame. In export mode: also save a numbered JPEG
    // (does NOT exit — main.cpp exits after the boot cycle ends so we
    // capture all frames, not just the first).
    void update() {
        presentAndMaybeExport();
    }

    // Sim has no separate backing buffer / waveform to distinguish partial
    // from full refresh — both just present the current frame. Overridden
    // by EPaperPartial (EPaperGhostSim.h) with a real ghosting simulation;
    // this base version only runs if something bypasses that subclass.
    void updataPartial(int, int, int, int) {
        update();
    }

    void cleanup(int code) {
        TTF_Quit();
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_)   SDL_DestroyWindow(window_);
        SDL_Quit();
        exit(code);
    }

protected:
    // Shared with EPaperPartial (EPaperGhostSim.h), which needs to read
    // back and overwrite rendered pixels to simulate the panel controller's
    // old/new diffing — see that file.
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture  *target_   = nullptr;   // persistent render-target

    // Saves a JPEG if in export mode, presents the frame, and pumps events
    // — the part of a refresh that's identical regardless of how the
    // pixels themselves got decided (plain present, or ghosting-simulated).
    void presentAndMaybeExport() {
        if (exportMode()) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s_%02d%s",
                     exportStem_.c_str(), ++frameCount_, exportExt_.c_str());
            saveJPG(buf);
        }

        // Blit target texture to the screen
        SDL_SetRenderTarget(renderer_, nullptr);
        SDL_RenderCopy(renderer_, target_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
        SDL_SetRenderTarget(renderer_, target_);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) cleanup(0);
        }

        if (!exportMode()) SDL_Delay(500);
    }

    // Reads the current render target and packs it into a 1bpp buffer
    // (bit=1 means black, MSB-first, row-major — SCREEN_W*SCREEN_H/8
    // bytes), the same layout real EPaper::getPointer() (Seeed_GFX) would
    // return. Thresholds at 50% luma, so anti-aliased glyph edges round to
    // whichever side they're closer to — fine for catching real logic bugs
    // (a wrong field, a missing redraw), which show up as large-scale
    // mismatches, not lost in edge noise.
    void captureRenderedBuffer(uint8_t *out) {
        size_t bytes = (size_t)SCREEN_W * SCREEN_H / 8;
        memset(out, 0, bytes);

        SDL_Surface *surf = SDL_CreateRGBSurface(
            0, SCREEN_W, SCREEN_H, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (!surf) return;

        if (SDL_RenderReadPixels(renderer_, nullptr, surf->format->format, surf->pixels, surf->pitch) != 0) {
            SDL_FreeSurface(surf);
            return;
        }

        auto *src = reinterpret_cast<uint32_t *>(surf->pixels);
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                uint32_t px = src[y * SCREEN_W + x];
                int luma = (((px >> 16) & 0xFF) + ((px >> 8) & 0xFF) + (px & 0xFF)) / 3;
                if (luma < 128) {
                    out[y * (SCREEN_W / 8) + (x / 8)] |= (0x80 >> (x % 8));
                }
            }
        }
        SDL_FreeSurface(surf);
    }

    // Draws a packed 1bpp buffer (same layout as captureRenderedBuffer())
    // onto the render target as solid black/white pixels — used to make a
    // ghosting-simulated (possibly corrupted) result the thing that
    // actually gets presented/exported, not what was originally drawn.
    void blitPackedBuffer(const uint8_t *buf) {
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                bool black = buf[y * (SCREEN_W / 8) + (x / 8)] & (0x80 >> (x % 8));
                SDL_SetRenderDrawColor(renderer_, black ? 0 : 255, black ? 0 : 255, black ? 0 : 255, 255);
                SDL_RenderDrawPoint(renderer_, x, y);
            }
        }
    }

private:
    SDL_Window   *window_    = nullptr;
    uint32_t      textColor_ = TFT_BLACK;
    int           textScale_ = 1;
    const GFXfont *freeFont_ = nullptr;

    std::string exportBase_;    // full original path, e.g. "clock.jpg"
    std::string exportStem_;    // without extension, e.g. "clock"
    std::string exportExt_;     // extension with dot, e.g. ".jpg"
    int         frameCount_ = 0;

    // Font cache
    static const int MAX_FONTS = 16;
    struct FontEntry { int pt; TTF_Font *font; };
    FontEntry fontCache_[MAX_FONTS] = {};
    int fontCount_ = 0;

    void loadFont(int pt) {
        if (fontCount_ >= MAX_FONTS) return;
        static const char *PATHS[] = {
            // macOS
            "/System/Library/Fonts/SFNS.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            // Linux (CI) — only the small weather/battery text uses these;
            // the big clock digits are bitmaps from BigDigits.h.
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
        };
        // Allow an explicit override (e.g. in CI) via the SIM_FONT env var.
        if (const char *envFont = getenv("SIM_FONT")) {
            if (TTF_Font *ef = TTF_OpenFont(envFont, pt)) {
                fontCache_[fontCount_++] = {pt, ef};
                return;
            }
        }
        TTF_Font *f = nullptr;
        for (auto &p : PATHS) {
            f = TTF_OpenFont(p, pt);
            if (f) break;
        }
        if (!f) { fprintf(stderr, "Could not load font at %dpt: %s\n", pt, TTF_GetError()); return; }
        fontCache_[fontCount_++] = {pt, f};
    }

    TTF_Font *fontAt(int pt) {
        for (int i = 0; i < fontCount_; i++)
            if (fontCache_[i].pt == pt) return fontCache_[i].font;
        loadFont(pt);
        for (int i = 0; i < fontCount_; i++)
            if (fontCache_[i].pt == pt) return fontCache_[i].font;
        return nullptr;
    }

    int ptForContext(int fontNum) {
        // TFT_eSPI quirk: a GFX free font is registered as "font 1", so it only
        // takes effect when fontNum == 1. Any other numbered font ignores it.
        // Replicating this lets the sim catch bugs where a free font set for an
        // earlier draw bleeds into a later drawString(..., 1) call.
        if (freeFont_ && fontNum == 1) return freeFont_->pointSize * textScale_;
        return builtinFontPt(fontNum) * textScale_;
    }

    bool isBoldContext(int fontNum) {
        return freeFont_ && fontNum == 1 && freeFont_->bold;
    }

    void setColor(uint32_t rgb) {
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >>  8) & 0xFF;
        uint8_t b =  rgb        & 0xFF;
        SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
    }

    void renderText(const char *str, int x, int y, int fontNum) {
        TTF_Font *f = fontAt(ptForContext(fontNum));
        if (!f || !str || !*str) return;
        TTF_SetFontStyle(f, isBoldContext(fontNum) ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);
        uint8_t r = (textColor_ >> 16) & 0xFF;
        uint8_t g = (textColor_ >>  8) & 0xFF;
        uint8_t b =  textColor_        & 0xFF;
        SDL_Color col = {r, g, b, 255};
        SDL_Surface *surf = TTF_RenderUTF8_Blended(f, str, col);
        if (!surf) return;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer_, surf);
        SDL_FreeSurface(surf);
        if (!tex) return;
        int w, h;
        SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
        SDL_Rect dst = {x, y, w, h};
        SDL_RenderCopy(renderer_, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }

    // Reads pixels from the render-target texture (not the screen backbuffer)
    // so this is safe to call before or after SDL_RenderPresent.
    bool saveJPG(const char *path, int quality = 90) {
        // Temporarily point at the target texture to read from it
        SDL_Surface *surf = SDL_CreateRGBSurface(
            0, SCREEN_W, SCREEN_H, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (!surf) return false;

        // SDL_RenderReadPixels reads from the current render target
        if (SDL_RenderReadPixels(renderer_, nullptr,
                surf->format->format, surf->pixels, surf->pitch) != 0) {
            fprintf(stderr, "saveJPG: SDL_RenderReadPixels: %s\n", SDL_GetError());
            SDL_FreeSurface(surf);
            return false;
        }

        // Convert ARGB8888 → RGB24 for stb
        int pixels = SCREEN_W * SCREEN_H;
        std::vector<uint8_t> rgb(pixels * 3);
        auto *src = reinterpret_cast<uint32_t *>(surf->pixels);
        for (int i = 0; i < pixels; i++) {
            uint32_t px = src[i];
            rgb[i*3+0] = (px >> 16) & 0xFF;  // R
            rgb[i*3+1] = (px >>  8) & 0xFF;  // G
            rgb[i*3+2] =  px        & 0xFF;  // B
        }
        SDL_FreeSurface(surf);

        int ok = stbi_write_jpg(path, SCREEN_W, SCREEN_H, 3, rgb.data(), quality);
        if (ok) printf("Saved frame %d → %s\n", frameCount_, path);
        else fprintf(stderr, "saveJPG: stbi_write_jpg failed for '%s'\n", path);
        return ok != 0;
    }

    static int interp(int x0, int y0, int x1, int y1, int y) {
        if (y1 == y0) return x0;
        return x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    }
};

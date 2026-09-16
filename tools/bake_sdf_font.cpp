// Offline SDF font atlas baker — host tool, NOT part of the firmware or
// simulator build. Reads a TTF and emits a C header with an 8-bit signed-
// distance-field atlas (PROGMEM byte array) plus a glyph metrics table,
// covering printable ASCII (0x20-0x7E) and the degree sign (U+00B0), which
// together cover every string src/main.cpp draws.
//
// Build:  clang++ -std=c++17 -O2 tools/bake_sdf_font.cpp -o /tmp/bake_sdf_font
// Run:    /tmp/bake_sdf_font <font.ttf> <BaseName> <output.h> [emPx]
//
// Regenerate whenever the source font changes:
//   /tmp/bake_sdf_font tools/fonts/Inter-Bold.ttf InterBold src/fonts/Inter_Bold_sdf.h
//   /tmp/bake_sdf_font tools/fonts/Inter-Regular.ttf InterRegular src/fonts/Inter_Regular_sdf.h

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

// Codepoints baked into the atlas: printable ASCII, then the degree sign as
// one extra trailing slot.
static const int FIRST_CODEPOINT = 0x20;
static const int LAST_ASCII_CODEPOINT = 0x7E;
static const int DEGREE_CODEPOINT = 0x00B0;
static const int NUM_ASCII = LAST_ASCII_CODEPOINT - FIRST_CODEPOINT + 1;
static const int NUM_GLYPHS = NUM_ASCII + 1;  // + degree sign
static const int DEGREE_GLYPH_INDEX = NUM_ASCII;

struct BakedGlyph {
    int codepoint;
    int atlasX = 0, atlasY = 0, w = 0, h = 0;  // bitmap rect in atlas
    int xoff = 0, yoff = 0;                    // offset from pen origin to bitmap top-left, in px at emPx
    float advance = 0;                         // advance width in px at emPx
};

// Minimal shelf/skyline packer — the glyph set here is small (96 glyphs),
// so this doesn't need to be clever.
struct ShelfPacker {
    int width, height = 0;
    int cursorX = 0, cursorY = 0, shelfH = 0;
    static const int PAD = 1;
    explicit ShelfPacker(int w) : width(w) {}
    bool place(int w, int h, int &outX, int &outY) {
        if (cursorX + w + PAD > width) {
            cursorX = 0;
            cursorY += shelfH + PAD;
            shelfH = 0;
        }
        outX = cursorX;
        outY = cursorY;
        cursorX += w + PAD;
        shelfH = std::max(shelfH, h);
        height = cursorY + shelfH + PAD;
        return true;
    }
};

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <font.ttf> <BaseName> <output.h> [emPx=64]\n", argv[0]);
        return 1;
    }
    const char *fontPath = argv[1];
    const char *baseName = argv[2];
    const char *outPath = argv[3];
    const int emPx = argc > 4 ? atoi(argv[4]) : 64;
    const int padding = 8;  // distance-field padding around each glyph, in px

    FILE *f = fopen(fontPath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", fontPath); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data(sz);
    if (fread(data.data(), 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0))) {
        fprintf(stderr, "stbtt_InitFont failed\n");
        return 1;
    }

    float scale = stbtt_ScaleForPixelHeight(&info, (float)emPx);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);

    const unsigned char onedgeValue = 128;
    const float pixelDistScale = (float)onedgeValue / (float)padding;

    std::vector<BakedGlyph> glyphs(NUM_GLYPHS);
    std::vector<unsigned char *> bitmaps(NUM_GLYPHS, nullptr);

    for (int i = 0; i < NUM_GLYPHS; i++) {
        int cp = (i < NUM_ASCII) ? (FIRST_CODEPOINT + i) : DEGREE_CODEPOINT;
        glyphs[i].codepoint = cp;

        int advanceRaw, lsbRaw;
        stbtt_GetCodepointHMetrics(&info, cp, &advanceRaw, &lsbRaw);
        glyphs[i].advance = advanceRaw * scale;

        int w, h, xoff, yoff;
        unsigned char *bmp = stbtt_GetCodepointSDF(&info, scale, cp, padding, onedgeValue,
                                                     pixelDistScale, &w, &h, &xoff, &yoff);
        bitmaps[i] = bmp;
        glyphs[i].w = bmp ? w : 0;
        glyphs[i].h = bmp ? h : 0;
        glyphs[i].xoff = xoff;
        glyphs[i].yoff = yoff;
    }

    // Pack into a square-ish atlas. Estimate width from total glyph area.
    long totalArea = 0;
    for (auto &g : glyphs) totalArea += (long)(g.w + 1) * (g.h + 1);
    int atlasW = 64;
    while ((long)atlasW * atlasW < totalArea * 3 / 2) atlasW *= 2;
    if (atlasW > 1024) atlasW = 1024;

    ShelfPacker packer(atlasW);
    // Pack tallest-first for a denser shelf layout.
    std::vector<int> order(NUM_GLYPHS);
    for (int i = 0; i < NUM_GLYPHS; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return glyphs[a].h > glyphs[b].h; });
    for (int idx : order) {
        auto &g = glyphs[idx];
        if (g.w == 0 || g.h == 0) continue;
        packer.place(g.w, g.h, g.atlasX, g.atlasY);
    }
    int atlasH = std::max(1, packer.height);

    std::vector<unsigned char> atlas((size_t)atlasW * atlasH, 0);
    for (int i = 0; i < NUM_GLYPHS; i++) {
        auto &g = glyphs[i];
        if (!bitmaps[i]) continue;
        for (int y = 0; y < g.h; y++) {
            for (int x = 0; x < g.w; x++) {
                atlas[(size_t)(g.atlasY + y) * atlasW + (g.atlasX + x)] = bitmaps[i][y * g.w + x];
            }
        }
        stbtt_FreeSDF(bitmaps[i], nullptr);
    }

    FILE *out = fopen(outPath, "w");
    if (!out) { fprintf(stderr, "cannot write %s\n", outPath); return 1; }

    fprintf(out, "#pragma once\n\n");
    fprintf(out, "// Auto-generated by tools/bake_sdf_font.cpp from %s — do not hand-edit.\n", fontPath);
    fprintf(out, "// Regenerate: bake_sdf_font %s %s %s %d\n\n", fontPath, baseName, outPath, emPx);
    fprintf(out, "#include \"../sdf_font.h\"\n\n");

    fprintf(out, "static const uint8_t %s_atlas[] PROGMEM = {\n", baseName);
    for (size_t i = 0; i < atlas.size(); i++) {
        fprintf(out, "%u,%s", atlas[i], ((i + 1) % 20 == 0) ? "\n" : "");
    }
    fprintf(out, "\n};\n\n");

    fprintf(out, "static const SdfGlyph %s_glyphs[%d] = {\n", baseName, NUM_GLYPHS);
    for (auto &g : glyphs) {
        fprintf(out, "  { %d, %d, %d, %d, %d, %d, %.3ff }, // U+%04X\n",
                g.atlasX, g.atlasY, g.w, g.h, g.xoff, g.yoff, g.advance, g.codepoint);
    }
    fprintf(out, "};\n\n");

    float ascentPx = ascent * scale;
    fprintf(out, "static const SdfFont %s = {\n", baseName);
    fprintf(out, "  %s_atlas, %d, %d, %d, %.3ff, %.3ff, %.3ff,\n", baseName, atlasW, atlasH, emPx, pixelDistScale, (float)padding, ascentPx);
    fprintf(out, "  %s_glyphs, %d, 0x%02X, 0x%04X\n", baseName, NUM_GLYPHS, FIRST_CODEPOINT, DEGREE_CODEPOINT);
    fprintf(out, "};\n");

    fclose(out);

    fprintf(stderr, "%s: %d glyphs, atlas %dx%d (%zu bytes), em=%dpx\n",
            baseName, NUM_GLYPHS, atlasW, atlasH, atlas.size(), emPx);
    return 0;
}

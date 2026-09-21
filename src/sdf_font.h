#pragma once

// Shared signed-distance-field text renderer. Header-only (like
// BigDigits.h) rather than a separate translation unit, since it calls
// EPaper::drawPixel() and the *type* named `EPaper` differs between the
// firmware (TFT_eSPI-backed, Extensions/EPaper.h) and the simulator
// (simulator/EPaperSim.h) — being header-only lets each translation unit
// resolve `EPaper` to whichever one is in scope, exactly how BigDigits.h
// already gets shared between both today.
//
// Fonts are baked offline by tools/bake_sdf_font.cpp into headers under
// src/fonts/ (e.g. Inter_Bold_sdf.h) that define a `static const SdfFont`
// using the types below.

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#endif

struct SdfGlyph {
  int16_t atlasX, atlasY, w, h;  // bitmap rect within the atlas
  int16_t xoff, yoff;            // pen-origin -> bitmap top-left offset, px at emPx
  float advance;                 // advance width, px at emPx
};

struct SdfFont {
  const uint8_t *atlas;
  int16_t atlasW, atlasH;
  int16_t emPx;
  float pixelDistScale;   // onedgeValue / padding, as baked
  float padding;          // px at emPx
  float ascent;           // px at emPx, top-of-line -> baseline distance
  const SdfGlyph *glyphs; // [0 .. numGlyphs-1]: firstCodepoint..firstCodepoint+numAscii-1, then degreeCodepoint
  int16_t numGlyphs;
  int16_t firstCodepoint;
  int16_t degreeCodepoint;
};

// Decodes one UTF-8 codepoint starting at s, returns it and advances
// *bytesConsumed. Only needs to handle ASCII plus the 2-byte sequences this
// project actually emits (the degree sign), so unsupported sequences just
// fall back to their raw first byte.
static inline uint32_t sdfUtf8Decode(const char *s, int *bytesConsumed) {
  uint8_t c0 = (uint8_t)s[0];
  if (c0 < 0x80) { *bytesConsumed = 1; return c0; }
  if ((c0 & 0xE0) == 0xC0 && s[1]) {
    *bytesConsumed = 2;
    return ((uint32_t)(c0 & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
  }
  if ((c0 & 0xF0) == 0xE0 && s[1] && s[2]) {
    *bytesConsumed = 3;
    return ((uint32_t)(c0 & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F);
  }
  *bytesConsumed = 1;
  return c0;
}

static inline const SdfGlyph *sdfFindGlyph(const SdfFont &f, uint32_t codepoint) {
  int numAscii = f.numGlyphs - 1;  // last slot is the degree sign
  if ((int)codepoint >= f.firstCodepoint && (int)codepoint < f.firstCodepoint + numAscii) {
    return &f.glyphs[codepoint - f.firstCodepoint];
  }
  if ((int)codepoint == f.degreeCodepoint) {
    return &f.glyphs[f.numGlyphs - 1];
  }
  return nullptr;  // unsupported codepoint (e.g. control chars) — skip, no advance
}

template <typename EPaperT>
static void sdfDrawGlyph(EPaperT &d, const SdfFont &f, const SdfGlyph &g, int penX, int baselineY,
                          float scaleX, float scaleY, uint32_t color) {
  if (g.w <= 0 || g.h <= 0) return;

  int dstX0 = penX + (int)lroundf(g.xoff * scaleX);
  int dstY0 = baselineY + (int)lroundf(g.yoff * scaleY);
  int dstW = (int)lroundf(g.w * scaleX);
  int dstH = (int)lroundf(g.h * scaleY);
  if (dstW <= 0 || dstH <= 0) return;

  for (int dy = 0; dy < dstH; dy++) {
    float srcYf = (dy + 0.5f) / scaleY;
    if (srcYf < 0) srcYf = 0;
    if (srcYf > g.h - 1) srcYf = (float)(g.h - 1);
    int sy0 = (int)srcYf;
    int sy1 = sy0 + 1 < g.h ? sy0 + 1 : sy0;
    float fy = srcYf - sy0;

    for (int dx = 0; dx < dstW; dx++) {
      float srcXf = (dx + 0.5f) / scaleX;
      if (srcXf < 0) srcXf = 0;
      if (srcXf > g.w - 1) srcXf = (float)(g.w - 1);
      int sx0 = (int)srcXf;
      int sx1 = sx0 + 1 < g.w ? sx0 + 1 : sx0;
      float fx = srcXf - sx0;

      int ax0 = g.atlasX + sx0, ax1 = g.atlasX + sx1;
      int ay0 = g.atlasY + sy0, ay1 = g.atlasY + sy1;
      int atlasStride = f.atlasW;
      float v00 = pgm_read_byte(&f.atlas[(size_t)ay0 * atlasStride + ax0]);
      float v10 = pgm_read_byte(&f.atlas[(size_t)ay0 * atlasStride + ax1]);
      float v01 = pgm_read_byte(&f.atlas[(size_t)ay1 * atlasStride + ax0]);
      float v11 = pgm_read_byte(&f.atlas[(size_t)ay1 * atlasStride + ax1]);
      float top = v00 + (v10 - v00) * fx;
      float bot = v01 + (v11 - v01) * fx;
      float sample = top + (bot - top) * fy;

      if (sample <= 128.0f) continue;

      d.drawPixel(dstX0 + dx, dstY0 + dy, color);
    }
  }
}

// Draws str (UTF-8) with its baseline at (x, baselineY). pixelHeight scales
// the font's baked em height to this size; xScale independently stretches
// or condenses width on top of that. Returns the pen x position after the
// last glyph (x + total advance).
template <typename EPaperT>
static int sdfDrawText(EPaperT &d, const SdfFont &f, int x, int baselineY, const char *str,
                        int pixelHeight, float xScale, uint32_t color) {
  float scaleY = (float)pixelHeight / (float)f.emPx;
  float scaleX = scaleY * xScale;
  int penX = x;
  while (*str) {
    int consumed = 1;
    uint32_t cp = sdfUtf8Decode(str, &consumed);
    str += consumed;
    const SdfGlyph *g = sdfFindGlyph(f, cp);
    if (!g) continue;
    sdfDrawGlyph(d, f, *g, penX, baselineY, scaleX, scaleY, color);
    penX += (int)lroundf(g->advance * scaleX);
  }
  return penX;
}

static inline int sdfTextWidth(const SdfFont &f, const char *str, int pixelHeight, float xScale = 1.0f) {
  float scaleY = (float)pixelHeight / (float)f.emPx;
  float scaleX = scaleY * xScale;
  float w = 0;
  while (*str) {
    int consumed = 1;
    uint32_t cp = sdfUtf8Decode(str, &consumed);
    str += consumed;
    const SdfGlyph *g = sdfFindGlyph(f, cp);
    if (!g) continue;
    w += g->advance * scaleX;
  }
  return (int)lroundf(w);
}

// Widest advance among '0'-'9' in this font, at its baked em size (i.e. the
// scale a caller applies separately via pixelHeight/xScale below).
static inline float sdfDigitCellAdvance(const SdfFont &f) {
  float maxAdv = 0;
  for (char c = '0'; c <= '9'; c++) {
    const SdfGlyph *g = sdfFindGlyph(f, (uint32_t)c);
    if (g && g->advance > maxAdv) maxAdv = g->advance;
  }
  return maxAdv;
}

// Like sdfDrawText, but for a string of digits only: every glyph gets the
// same advance (the font's widest digit) instead of its own natural advance,
// and is centered within that fixed-width cell. A proportional font's actual
// per-digit widths (e.g. Inter's '1' vs '8') otherwise make a digit string's
// total width - and therefore any layout computed from it - change with
// whichever digits happen to be shown, shifting/rescaling the whole clock
// face on nearly every wake. On e-paper, where a partial refresh already
// struggles to fully flip every changed pixel, that extra churn (moving
// pixels that didn't need to move) is what shows up as visible grain/
// ghosting, so the clock digits keep a fixed pixel grid across draws instead.
template <typename EPaperT>
static int sdfDrawTabularDigits(EPaperT &d, const SdfFont &f, int x, int baselineY, const char *digits,
                                 int pixelHeight, float xScale, uint32_t color) {
  float scaleY = (float)pixelHeight / (float)f.emPx;
  float scaleX = scaleY * xScale;
  float cellAdvance = sdfDigitCellAdvance(f);
  int penX = x;
  for (const char *p = digits; *p; p++) {
    const SdfGlyph *g = sdfFindGlyph(f, (uint32_t)(uint8_t)*p);
    if (g) {
      int glyphX = penX + (int)lroundf((cellAdvance - g->advance) * 0.5f * scaleX);
      sdfDrawGlyph(d, f, *g, glyphX, baselineY, scaleX, scaleY, color);
    }
    penX += (int)lroundf(cellAdvance * scaleX);
  }
  return penX;
}

static inline int sdfTabularDigitsWidth(const SdfFont &f, int numDigits, int pixelHeight, float xScale = 1.0f) {
  float scaleY = (float)pixelHeight / (float)f.emPx;
  float scaleX = scaleY * xScale;
  return (int)lroundf(sdfDigitCellAdvance(f) * numDigits * scaleX);
}

// Convenience wrappers matching TFT_eSPI's top-left / top-center text
// datum, since that's the convention every call site in main.cpp was
// written against (drawString/drawCentreString default to TL/TC datum).
template <typename EPaperT>
static int sdfDrawTextTL(EPaperT &d, const SdfFont &f, int x, int topY, const char *str,
                          int pixelHeight, float xScale, uint32_t color) {
  float scaleY = (float)pixelHeight / (float)f.emPx;
  int baselineY = topY + (int)lroundf(f.ascent * scaleY);
  return sdfDrawText(d, f, x, baselineY, str, pixelHeight, xScale, color);
}

template <typename EPaperT>
static int sdfDrawCentreTextTL(EPaperT &d, const SdfFont &f, int cx, int topY, const char *str,
                                int pixelHeight, float xScale, uint32_t color) {
  int w = sdfTextWidth(f, str, pixelHeight, xScale);
  return sdfDrawTextTL(d, f, cx - w / 2, topY, str, pixelHeight, xScale, color);
}

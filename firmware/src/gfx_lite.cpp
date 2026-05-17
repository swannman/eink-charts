#include "gfx_lite.h"

#include <Arduino.h>
#include <string.h>

#include "font5x7.h"

void fbClear(uint8_t* fb, bool white) {
  memset(fb, white ? 0xFF : 0x00, FB_BYTES_PER_ROW * FB_HEIGHT);
}

void fbSetPixel(uint8_t* fb, int x, int y, bool black) {
  if ((unsigned)x >= FB_WIDTH || (unsigned)y >= FB_HEIGHT) return;
  const int byteIdx = y * FB_BYTES_PER_ROW + (x >> 3);
  const uint8_t mask = 0x80 >> (x & 7);
  if (black) fb[byteIdx] &= ~mask;
  else       fb[byteIdx] |= mask;
}

void fbFillRect(uint8_t* fb, int x, int y, int w, int h, bool black) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      fbSetPixel(fb, xx, yy, black);
    }
  }
}

// Each glyph in `font[]` is 5 columns × 8 rows, LSB = top pixel.
// We render at integer scale, with 1 blank column between glyphs (also scaled).
int fbCharCellW(int scale) { return (FB_GLYPH_W + 1) * scale; }
int fbCharCellH(int scale) { return (FB_GLYPH_H + 1) * scale; }

int fbStringWidth(const char* s, int scale) {
  int n = 0;
  while (s[n]) n++;
  if (n == 0) return 0;
  // n cells of width cellW, minus the trailing gap.
  return n * fbCharCellW(scale) - scale;
}

void fbDrawChar(uint8_t* fb, int x, int y, int scale, char c, bool black) {
  if ((uint8_t)c > 0x7E) c = '?';
  if ((uint8_t)c < 0x20) c = ' ';
  const uint8_t* glyph = &font[(uint8_t)c * 5];
  for (int col = 0; col < FB_GLYPH_W; col++) {
    const uint8_t line = glyph[col];
    for (int row = 0; row < FB_GLYPH_H; row++) {
      if (!(line & (1u << row))) continue;
      // Paint a scale x scale block per "on" pixel.
      const int px = x + col * scale;
      const int py = y + row * scale;
      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
          fbSetPixel(fb, px + dx, py + dy, black);
        }
      }
    }
  }
}

void fbDrawString(uint8_t* fb, int x, int y, int scale, const char* s, bool black) {
  while (*s) {
    fbDrawChar(fb, x, y, scale, *s, black);
    x += fbCharCellW(scale);
    s++;
  }
}

void fbDrawStringCentered(uint8_t* fb, int y, int scale, const char* s, bool black) {
  int w = fbStringWidth(s, scale);
  int x = (FB_WIDTH - w) / 2;
  if (x < 0) x = 0;
  fbDrawString(fb, x, y, scale, s, black);
}

// ---------------------------------------------------------------------------
// Adafruit GFXfont rendering. The font tables are flat bitmaps packed
// MSB-first row-major; each glyph carries its own width/height/xAdvance and
// xOffset/yOffset relative to the (x, y) baseline. See gfxfont.h.

static const GFXglyph* gfxGlyph(const GFXfont* font, char c) {
  uint8_t code = (uint8_t)c;
  if (code < font->first || code > font->last) code = ' ';
  if (code < font->first || code > font->last) return nullptr;
  return &font->glyph[code - font->first];
}

int fbGfxStringWidth(const GFXfont* font, const char* s) {
  if (!font || !s || !*s) return 0;
  int w = 0;
  while (*s) {
    const GFXglyph* g = gfxGlyph(font, *s);
    if (g) w += g->xAdvance;
    s++;
  }
  return w;
}

int fbGfxLineHeight(const GFXfont* font) {
  return font ? font->yAdvance : 0;
}

void fbDrawStringGfx(uint8_t* fb, int x, int y, const GFXfont* font, const char* s, bool black) {
  fbDrawStringGfxScaled(fb, x, y, font, s, 1, black);
}

int fbGfxStringWidthScaled(const GFXfont* font, const char* s, int scale) {
  return fbGfxStringWidth(font, s) * (scale > 0 ? scale : 1);
}

void fbDrawStringGfxScaled(uint8_t* fb, int x, int y, const GFXfont* font, const char* s, int scale, bool black) {
  if (!font || !s) return;
  if (scale < 1) scale = 1;
  while (*s) {
    const GFXglyph* g = gfxGlyph(font, *s);
    if (g && g->width && g->height) {
      const uint8_t* bitmap = font->bitmap + g->bitmapOffset;
      const int x0 = x + g->xOffset * scale;
      const int y0 = y + g->yOffset * scale;
      uint16_t bits = 0;
      uint8_t bit_count = 0;
      for (uint8_t yy = 0; yy < g->height; yy++) {
        for (uint8_t xx = 0; xx < g->width; xx++) {
          if (bit_count == 0) {
            bits = *bitmap++;
            bit_count = 8;
          }
          if (bits & 0x80) {
            // Splat each source pixel as a scale×scale block.
            const int px = x0 + xx * scale;
            const int py = y0 + yy * scale;
            for (int dy = 0; dy < scale; dy++) {
              for (int dx = 0; dx < scale; dx++) {
                fbSetPixel(fb, px + dx, py + dy, black);
              }
            }
          }
          bits <<= 1;
          bit_count--;
        }
      }
    }
    if (g) x += g->xAdvance * scale;
    s++;
  }
}

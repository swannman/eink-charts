#include "gfxc.h"

#include <Arduino.h>
#include <gfxfont.h>

// Vendored with Adafruit GFX (BSD). Regular for titles/labels, bold for values.
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include "framebuffer.h"

namespace gfxc {

namespace {

// A face = GFX font + integer scale. `px` is the nominal on-screen height used
// for size fitting (roughly cap height + descender at that scale).
struct Face { const GFXfont* font; uint8_t scale; int px; };

const Face BOLD[] = {
    {&FreeSansBold9pt7b, 1, 13},  {&FreeSansBold12pt7b, 1, 17},
    {&FreeSansBold18pt7b, 1, 25}, {&FreeSansBold24pt7b, 1, 34},
    {&FreeSansBold18pt7b, 2, 50}, {&FreeSansBold24pt7b, 2, 68},
    {&FreeSansBold24pt7b, 3, 102},
};
constexpr int NBOLD = sizeof(BOLD) / sizeof(BOLD[0]);

const Face LIGHT[] = {
    {&FreeSans9pt7b, 1, 13},  {&FreeSans12pt7b, 1, 17},
    {&FreeSans18pt7b, 1, 25}, {&FreeSans24pt7b, 1, 34},
};
constexpr int NLIGHT = sizeof(LIGHT) / sizeof(LIGHT[0]);

int faceIndexFor(const Face* faces, int n, int target_px) {
  int idx = 0;
  for (int i = 0; i < n; i++) {
    if (faces[i].px <= target_px) idx = i;
  }
  return idx;
}

const GFXglyph* glyphFor(const GFXfont* f, unsigned c) {
  if (c < f->first || c > f->last) return nullptr;
  return &f->glyph[c - f->first];
}

// Pen-advance width of `s` (screen px, includes trailing advance padding).
int advanceWidth(const Face& face, const char* s) {
  int w = 0;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(face.font, (unsigned char)*p);
    if (g) w += g->xAdvance;
  }
  return w * face.scale;
}

// x of the rightmost ink pixel relative to the pen start (screen px) — for
// right-alignment so integer and decimal labels land on the same column.
int inkRight(const Face& face, const char* s) {
  int penX = 0, inkR = 0;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(face.font, (unsigned char)*p);
    if (!g) continue;
    int r = penX + g->xOffset + g->width;
    if (r > inkR) inkR = r;
    penX += g->xAdvance;
  }
  return inkR * face.scale;
}

// Topmost ink offset of `s` relative to the baseline (negative, screen px).
int inkTop(const Face& face, const char* s) {
  int top = 0;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(face.font, (unsigned char)*p);
    if (g && g->yOffset < top) top = g->yOffset;
  }
  return top * face.scale;
}

int chooseIdx(const Face* faces, int n, const char* s, int max_w, int target_px) {
  int idx = faceIndexFor(faces, n, target_px);
  while (idx > 0 && advanceWidth(faces[idx], s) > max_w) idx--;
  return idx;
}

void blitGlyph(const Face& face, int penX, int baseline, unsigned c, uint8_t color) {
  const GFXglyph* g = glyphFor(face.font, c);
  if (!g) return;
  const uint8_t* bitmap = face.font->bitmap + g->bitmapOffset;
  int bit = 0;
  const int s = face.scale;
  for (int yy = 0; yy < g->height; yy++) {
    for (int xx = 0; xx < g->width; xx++, bit++) {
      if (bitmap[bit >> 3] & (0x80 >> (bit & 7))) {
        int px = penX + (g->xOffset + xx) * s;
        int py = baseline + (g->yOffset + yy) * s;
        if (s == 1) {
          fb::pixel(px, py, color);
        } else {
          fb::fillRect(px, py, s, s, color);
        }
      }
    }
  }
}

int drawWith(const Face* faces, int n, int x, int y, int max_w, int target_px,
             const char* s, uint8_t color, int align, int box_w) {
  int idx = chooseIdx(faces, n, s, max_w, target_px);
  const Face& face = faces[idx];
  int dx = x;
  if (align == 1) {
    dx = x + (box_w - advanceWidth(face, s)) / 2;
  } else if (align == 2) {
    dx = x - inkRight(face, s);
  }
  int baseline = y - inkTop(face, s);   // position by visual top
  int penX = dx;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(face.font, (unsigned char)*p);
    if (!g) continue;
    blitGlyph(face, penX, baseline, (unsigned char)*p, color);
    penX += g->xAdvance * face.scale;
  }
  return face.px;
}

}  // namespace

int textWidth(const char* s, int target_px) {
  return advanceWidth(BOLD[faceIndexFor(BOLD, NBOLD, target_px)], s);
}

int fittedPx(const char* s, int max_w, int target_px) {
  return BOLD[chooseIdx(BOLD, NBOLD, s, max_w, target_px)].px;
}

int drawTextFit(int x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(BOLD, NBOLD, x, y, max_w, target_px, s, color, 0, 0);
}

int drawTextFitLight(int x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(LIGHT, NLIGHT, x, y, max_w, target_px, s, color, 0, 0);
}

int drawTextCenteredFit(int x, int y, int w, int target_px, const char* s, uint8_t color) {
  return drawWith(BOLD, NBOLD, x, y, w, target_px, s, color, 1, w);
}

int drawTextRightFit(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(BOLD, NBOLD, right_x, y, max_w, target_px, s, color, 2, 0);
}

int drawTextRightFitLight(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(LIGHT, NLIGHT, right_x, y, max_w, target_px, s, color, 2, 0);
}

int fittedPxLight(const char* s, int max_w, int target_px) {
  return LIGHT[chooseIdx(LIGHT, NLIGHT, s, max_w, target_px)].px;
}

}  // namespace gfxc

#include "gfx4.h"

#include <Group5.h>          // BB_FONT / BB_GLYPH glyph metrics (for ink-extent align)

#include "config.h"
#include "display_trmnl.h"   // extern FASTEPD epd

// Vendored anti-aliased fonts (Apache-2.0). Added to the include path via
// -Isrc/fonts in platformio.ini.
#include "Roboto_Black_16.h"
#include "Roboto_Black_20.h"
#include "Roboto_Black_24.h"
#include "Roboto_Black_32.h"
#include "Roboto_Black_40.h"
#include "Roboto_Black_50.h"
#include "Roboto_Black_80.h"
#include "Roboto_Black_160.h"
#include "Roboto_Thin_16.h"
#include "Roboto_Thin_24.h"
#include "Roboto_Thin_32.h"
#include "Roboto_Thin_40.h"

namespace gfx4 {

namespace {

struct FontEntry { const uint8_t* font; int px; };

// Bold (Black) family — values, numbers, emphasis.
const FontEntry FONTS[] = {
    {Roboto_Black_16, 16},  {Roboto_Black_20, 20},  {Roboto_Black_24, 24},
    {Roboto_Black_32, 32},  {Roboto_Black_40, 40},  {Roboto_Black_50, 50},
    {Roboto_Black_80, 80},  {Roboto_Black_160, 160},
};
constexpr int NFONTS = sizeof(FONTS) / sizeof(FONTS[0]);

// Light (Thin) family — regular-weight panel titles.
const FontEntry LIGHT_FONTS[] = {
    {Roboto_Thin_16, 16}, {Roboto_Thin_24, 24},
    {Roboto_Thin_32, 32}, {Roboto_Thin_40, 40},
};
constexpr int NLIGHT = sizeof(LIGHT_FONTS) / sizeof(LIGHT_FONTS[0]);

// Index of the largest font whose nominal px <= target (>= 0).
int fontIndexFor(const FontEntry* fonts, int n, int target_px) {
  int idx = 0;
  for (int i = 0; i < n; i++) {
    if (fonts[i].px <= target_px) idx = i;
  }
  return idx;
}

int widthAt(const FontEntry* fonts, int idx, const char* s) {
  epd.setFont(fonts[idx].font, /*bAntiAliased=*/true);
  epd.setCursor(0, 0);
  BB_RECT r;
  epd.getStringBox(s, &r);
  return r.w;
}

// The x of the rightmost ink pixel of `s` relative to the pen start, walking the
// glyph metrics directly. getStringBox() returns the pen-*advance* width, whose
// trailing padding varies with the glyphs (a '.' sits in a wide advance cell),
// so right-aligning by it leaves decimal labels shifted left of integer ones.
// Aligning by the real ink extent instead makes every label's last pixel land
// on the same column. Handles both BB_FONT layouts; -1 if the marker is neither
// (caller falls back to advance width). The vendored Roboto fonts are "small".
int inkRightAt(const FontEntry* fonts, int idx, const char* s) {
  const uint8_t* fd = fonts[idx].font;
  uint16_t marker = (uint16_t)fd[0] | ((uint16_t)fd[1] << 8);
  int penX = 0, inkR = 0;
  if (marker == BB_FONT_MARKER) {
    const BB_FONT* f = (const BB_FONT*)fd;
    for (const char* p = s; *p; ++p) {
      unsigned c = (unsigned char)*p;
      if (c < f->first || c > f->last) continue;
      const BB_GLYPH* g = &f->glyphs[c - f->first];
      int r = penX + g->xOffset + (int)g->width;
      if (r > inkR) inkR = r;
      penX += g->xAdvance;
    }
    return inkR;
  }
  if (marker == BB_FONT_MARKER_SMALL) {
    const BB_FONT_SMALL* f = (const BB_FONT_SMALL*)fd;
    for (const char* p = s; *p; ++p) {
      unsigned c = (unsigned char)*p;
      if (c < f->first || c > f->last) continue;
      const BB_GLYPH_SMALL* g = &f->glyphs[c - f->first];
      int r = penX + g->xOffset + (int)g->width;
      if (r > inkR) inkR = r;
      penX += g->xAdvance;
    }
    return inkR;
  }
  return -1;
}

// Pick the font: start at target, shrink until it fits max_w (or smallest).
int chooseIdx(const FontEntry* fonts, int n, const char* s, int max_w, int target_px) {
  int idx = fontIndexFor(fonts, n, target_px);
  while (idx > 0 && widthAt(fonts, idx, s) > max_w) idx--;
  return idx;
}

// FastEPD anchors drawString() at the text BASELINE (glyphs ascend above y). Our
// layout code positions text by its visual TOP, so convert: getStringBox with
// the cursor at 0 returns rect.y == miny (negative ascent), and top = baseline +
// miny, hence baseline = y_top - miny. Requires the font to be set already.
int baselineFor(int y_top, const char* s) {
  epd.setCursor(0, 0);
  BB_RECT r;
  epd.getStringBox(s, &r);
  return y_top - r.y;
}

int drawWith(const FontEntry* fonts, int n, int x, int y, int max_w,
             int target_px, const char* s, uint8_t color, int align, int box_w) {
  int idx = chooseIdx(fonts, n, s, max_w, target_px);
  epd.setFont(fonts[idx].font, /*bAntiAliased=*/true);
  epd.setTextColor(color, BBEP_TRANSPARENT);
  // FastEPD renders anti-aliased glyphs at HALF the font's design size (it draws
  // into a 2x canvas and downsamples 2->1), but getStringBox()/inkRightAt() report
  // DESIGN units. So the on-screen extent is half the measured value — scale by
  // /2 before positioning, or centered/right-aligned text drifts left by ~half its
  // own width (worse for longer strings, which is why decimals/negatives were the
  // most misaligned).
  int tw = (align != 0) ? widthAt(fonts, idx, s) / 2 : 0;   // screen width
  int dx = x;
  if (align == 1) {
    dx = x + (box_w - tw) / 2;   // centered in [x, x+box_w]
  } else if (align == 2) {       // right-aligned: land the last ink pixel on x
    int ink = inkRightAt(fonts, idx, s);
    dx = x - (ink >= 0 ? ink / 2 : tw);
  }
  epd.drawString(s, dx, baselineFor(y, s));
  return fonts[idx].px;
}

}  // namespace

int textWidth(const char* s, int target_px) {
  return widthAt(FONTS, fontIndexFor(FONTS, NFONTS, target_px), s);
}

int fittedPx(const char* s, int max_w, int target_px) {
  return FONTS[chooseIdx(FONTS, NFONTS, s, max_w, target_px)].px;
}

int drawTextFit(int x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(FONTS, NFONTS, x, y, max_w, target_px, s, color, 0, 0);
}

int drawTextFitLight(int x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(LIGHT_FONTS, NLIGHT, x, y, max_w, target_px, s, color, 0, 0);
}

int drawTextCenteredFit(int x, int y, int w, int target_px, const char* s, uint8_t color) {
  return drawWith(FONTS, NFONTS, x, y, w, target_px, s, color, 1, w);
}

int drawTextRightFit(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(FONTS, NFONTS, right_x, y, max_w, target_px, s, color, 2, 0);
}

int drawTextRightFitLight(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color) {
  return drawWith(LIGHT_FONTS, NLIGHT, right_x, y, max_w, target_px, s, color, 2, 0);
}

int fittedPxLight(const char* s, int max_w, int target_px) {
  return LIGHT_FONTS[chooseIdx(LIGHT_FONTS, NLIGHT, s, max_w, target_px)].px;
}

void thickLine(int x0, int y0, int x1, int y1, uint8_t color, int thickness) {
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x0 = clamp(x0, 0, SCREEN_W - 1); x1 = clamp(x1, 0, SCREEN_W - 1);
  for (int t = 0; t < thickness; t++) {
    int yo = t - thickness / 2;
    int ya = clamp(y0 + yo, 0, SCREEN_H - 1);
    int yb = clamp(y1 + yo, 0, SCREEN_H - 1);
    epd.drawLine(x0, ya, x1, yb, color);
  }
}

void lightenToGray(int x0, int y0, int x1, int y1, uint8_t targetGray) {
  // FastEPD's anti-aliased text renderer ignores setTextColor and always draws
  // black (grayColors[]={15,12,6,0}), so we draw text black then lighten it here.
  // Remap each pixel toward white by (15-targetGray)/15: a black pixel (0)
  // becomes `targetGray`, white (15) stays white, and the AA edge ramp scales
  // proportionally — yielding smooth gray text. Only call over regions that are
  // white apart from the text (titles/labels on the plot's white margins); a
  // band or line inside the rect would be lightened too.
  if (targetGray >= 15 || targetGray == 0) return;  // 0 = leave black, 15 = white: both no-ops
  uint8_t* buf = epd.currentBuffer();
  if (!buf) return;
  const int pitch = SCREEN_W >> 1;            // 4bpp: 2 px/byte, native_width=SCREEN_W
  const int num = 15 - targetGray;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > SCREEN_W) x1 = SCREEN_W;
  if (y1 > SCREEN_H) y1 = SCREEN_H;
  for (int y = y0; y < y1; y++) {
    int row = y * pitch;
    for (int x = x0; x < x1; x++) {
      int i = (x >> 1) + row;
      uint8_t b = buf[i];
      uint8_t p = (x & 1) ? (b & 0x0f) : (b >> 4);
      int np = 15 - (15 - p) * num / 15;      // lighten toward white
      if (np < 0) np = 0; else if (np > 15) np = 15;
      buf[i] = (x & 1) ? ((b & 0xf0) | np) : ((b & 0x0f) | (np << 4));
    }
  }
}

void dottedHLine(int x0, int x1, int y, uint8_t color, int pitch) {
  if (y < 0 || y >= SCREEN_H) return;
  for (int x = x0; x <= x1; x += pitch) {
    if (x >= 0 && x < SCREEN_W) epd.drawPixel(x, y, color);
  }
}

}  // namespace gfx4

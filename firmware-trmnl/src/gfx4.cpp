#include "gfx4.h"

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
  int tw = (align != 0) ? widthAt(fonts, idx, s) : 0;
  int dx = x;
  if (align == 1) dx = x + (box_w - tw) / 2;   // centered in [x, x+box_w]
  else if (align == 2) dx = x - tw;            // right-aligned to x
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

void dottedHLine(int x0, int x1, int y, uint8_t color, int pitch) {
  if (y < 0 || y >= SCREEN_H) return;
  for (int x = x0; x <= x1; x += pitch) {
    if (x >= 0 && x < SCREEN_W) epd.drawPixel(x, y, color);
  }
}

}  // namespace gfx4

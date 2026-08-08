#include "dashboard_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bundle_parser.h"
#include "config.h"
#include "framebuffer.h"
#include "gfxc.h"
#include "log.h"

namespace dashboard_renderer {

namespace {

// Layout constants — MUST mirror tools/preview_e1004.py.
constexpr int PANEL_GUTTER = 6;
// Per-series line colors (primary first). Black reads best on white; status
// hues follow for extra series.
const uint8_t SERIES_COLORS[] = {COL_BLACK, COL_BLUE, COL_RED, COL_GREEN};

// Dither coverages (of 16). Threshold bands use the SAME tint as the stat
// tile backgrounds (one green everywhere — the fridge band matches the soil
// tiles). Mirrors preview alphas.
constexpr uint8_t BAND_COVERAGE = 4;    // 25%, == TILE_COVERAGE
constexpr uint8_t TILE_COVERAGE = 4;    // 25%

constexpr int MAX_POINTS = 4096;
constexpr int MAX_LABELS = 8;
constexpr int MAX_BANDS = 8;

float gPX[MAX_POINTS];
float gPY[MAX_POINTS];
char gTitle[96];
char gUnit[16];
char gValue[32];
char gYL[MAX_LABELS][12];
float gYLPos[MAX_LABELS];
struct Band { float y0, y1; uint8_t color; };
Band gBands[MAX_BANDS];

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int readPoints(BinReader& r, uint32_t count) {
  int stored = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint16_t nx = r.u16();
    uint16_t ny = r.u16();
    if (stored < MAX_POINTS) {
      gPX[stored] = nx / 65535.0f;
      gPY[stored] = ny / 65535.0f;
      stored++;
    }
  }
  return stored;
}

void drawPolyline(int left, int bot, int pw, int ph, int n, uint8_t color,
                  int thickness) {
  int prevx = -1, prevy = -1;
  for (int i = 0; i < n; i++) {
    float nx = std::min(1.0f, std::max(0.0f, gPX[i]));
    float ny = std::min(1.0f, std::max(0.0f, gPY[i]));
    int x = left + (int)(nx * pw);
    int y = bot - (int)(ny * ph);
    if (prevx >= 0) fb::thickLine(prevx, prevy, x, y, color, thickness);
    prevx = x;
    prevy = y;
  }
}

void renderStat(int px, int py, int pw, int ph, uint8_t base_color, int sparkN) {
  int x0 = px + PANEL_GUTTER, y0 = py + PANEL_GUTTER;
  int x1 = px + pw - PANEL_GUTTER, y1 = py + ph - PANEL_GUTTER;
  // Pale dithered card only — the tint alone carries the status (the solid
  // top accent bar was tried and dropped as too heavy). White (no threshold)
  // draws a clean card.
  if (base_color != COL_WHITE) {
    fb::fillRectDither(x0, y0, x1 - x0, y1 - y0, base_color, TILE_COVERAGE);
  }

  int titlePx = clampi((int)(ph * 0.09f), 16, 26);
  gfxc::drawTextFit(px + 16, py + 10, pw - 28, titlePx, gTitle, COL_BLACK);

  // Value + unit as a single string in one face ("34%") so kerning and size
  // stay consistent. Centered in the tile.
  char vu[48];
  const char* value = gValue[0] ? gValue : "-";
  snprintf(vu, sizeof(vu), "%s%s", value, gUnit);
  int vtar = clampi((int)(ph * 0.30f), 24, 102);
  int vpx = gfxc::fittedPx(vu, pw - 24, vtar);
  int vy = py + ph / 2 - vpx / 2;
  gfxc::drawTextCenteredFit(px, vy, pw, vpx, vu, COL_BLACK);

  // Sparkline in the bottom band.
  if (sparkN >= 2) {
    int sl = px + 16, sr = px + pw - 16;
    int sb = py + ph - 14, st = py + (int)(ph * 0.72f);
    int sw = sr - sl, sh = sb - st;
    if (sw > 10 && sh > 6) {
      int prevx = -1, prevy = -1;
      for (int i = 0; i < sparkN; i++) {
        float nx = std::min(1.0f, std::max(0.0f, gPX[i]));
        float ny = std::min(1.0f, std::max(0.0f, gPY[i]));
        int x = sl + (int)(nx * sw);
        int y = sb - (int)(ny * sh);
        if (prevx >= 0) fb::thickLine(prevx, prevy, x, y, COL_BLACK, 2);
        prevx = x;
        prevy = y;
      }
    }
  }
}

void renderTimeseriesFrame(int px, int py, int pw, int ph, int yn,
                           int bn, int& left, int& top, int& right, int& bot) {
  int tb_h = clampi((int)(ph * 0.12f), 22, 40);
  int titlePx = clampi((int)(tb_h * 0.72f), 14, 28);

  left = px + PANEL_GUTTER + (yn ? 44 : 10);
  right = px + pw - PANEL_GUTTER - 6;
  top = py + tb_h + 6;
  bot = py + ph - PANEL_GUTTER - 6;   // no x-axis labels: reclaim the bottom

  gfxc::drawTextFitLight(left, py + 6, (px + pw - PANEL_GUTTER) - left, titlePx,
                         gTitle, COL_BLACK);

  if (right - left < 20 || bot - top < 20) { left = right = top = bot = 0; return; }
  int plotW = right - left, plotH = bot - top;

  // Threshold fill bands (y bottom-up), dithered to a light tint of the real
  // hue — every Grafana zone renders, green included (matching Grafana's
  // "area" thresholdsStyle; the bridge drops only white/transparent).
  for (int i = 0; i < bn; i++) {
    int by0 = bot - (int)(gBands[i].y1 * plotH);
    int by1 = bot - (int)(gBands[i].y0 * plotH);
    if (by1 > by0) {
      fb::fillRectDither(left, by0, plotW, by1 - by0, gBands[i].color, BAND_COVERAGE);
    }
  }

  // Y gridlines + labels. Size every label to the compact face the widest one
  // needs so they stay small and uniform. Solid black, small + light-weight —
  // there is no gray on this palette, so weight and size do the receding.
  int labTar = clampi((int)(ph * 0.055f), 13, 17);
  int labPx = labTar;
  for (int i = 0; i < yn; i++) {
    int fp = gfxc::fittedPxLight(gYL[i], 40, labTar);
    if (fp < labPx) labPx = fp;
  }
  for (int i = 0; i < yn; i++) {
    float pos = std::min(1.0f, std::max(0.0f, gYLPos[i]));
    int gy = bot - (int)(pos * plotH);
    fb::dottedHLine(left + 2, right, gy, COL_BLACK, 6);
    gfxc::drawTextRightFitLight(left - 6, gy - labPx / 2, 42, labPx, gYL[i], COL_BLACK);
  }
}

}  // namespace

bool render(const uint8_t* blob, size_t len, uint32_t index) {
  uint32_t ofs = bundle::dashboardOffset(blob, len, index);
  if (ofs == 0) {
    Log.printf("renderer: bad dashboard index %u\n", (unsigned)index);
    return false;
  }

  BinReader r(blob + ofs, len - ofs);
  r.skipPstr();   // dashboard title (not drawn)
  uint8_t grid_cols = r.u8();
  (void)grid_cols;
  uint8_t grid_rows = r.u8();
  uint8_t panel_count = r.u8();

  // Stretch the grid to fill the safe area — independent x/y scaling, no
  // letterbox (mirrors tools/preview_e1004.py).
  int areaW = SCREEN_W - SAFE_LEFT - SAFE_RIGHT;
  int areaH = SCREEN_H - SAFE_TOP - SAFE_BOTTOM;
  float cellX = (float)areaW / GRID_COLS;
  float cellY = (float)areaH / std::max<uint8_t>(1, grid_rows);

  for (uint8_t pi = 0; pi < panel_count; pi++) {
    uint8_t type = r.u8();
    uint8_t gx = r.u8(), gy = r.u8(), gw = r.u8(), gh = r.u8();
    r.pstr(gTitle, sizeof(gTitle));
    r.pstr(gUnit, sizeof(gUnit));
    uint8_t base_color = r.u8();

    int px = SAFE_LEFT + (int)(gx * cellX);
    int py = SAFE_TOP + (int)(gy * cellY);
    int pw = (int)(gw * cellX);
    int ph = (int)(gh * cellY);

    if (type == PANEL_STAT) {
      r.pstr(gValue, sizeof(gValue));
      uint8_t sc = r.u8();
      int sparkN = readPoints(r, sc);
      if (pw > 20 && ph > 20) renderStat(px, py, pw, ph, base_color, sparkN);
    } else {
      // timeseries: labels, bands, series.
      uint8_t yn = r.u8();
      int yStored = 0;
      for (uint8_t i = 0; i < yn; i++) {
        if (yStored < MAX_LABELS) {
          r.pstr(gYL[yStored], sizeof(gYL[0]));
          gYLPos[yStored] = r.u16() / 65535.0f;
          yStored++;
        } else {
          r.skipPstr();
          r.u16();
        }
      }
      uint8_t xn = r.u8();  // x-axis labels are not drawn; skip any present
      for (uint8_t i = 0; i < xn; i++) r.skipPstr();
      uint8_t bn = r.u8();
      int bStored = 0;
      for (uint8_t i = 0; i < bn; i++) {
        float y0 = r.u16() / 65535.0f;
        float y1 = r.u16() / 65535.0f;
        uint8_t c = r.u8();
        if (bStored < MAX_BANDS) gBands[bStored++] = {y0, y1, c};
      }

      // No frame around timeseries panels — the title + plot content give
      // enough structure, and a border reads as clutter on the big panel.
      int left = 0, top = 0, right = 0, bot = 0;
      if (pw > 20 && ph > 20) {
        renderTimeseriesFrame(px, py, pw, ph, yStored, bStored,
                              left, top, right, bot);
      }

      uint8_t sc = r.u8();
      for (uint8_t s = 0; s < sc; s++) {
        uint16_t pn = r.u16();
        int n = readPoints(r, pn);
        if (left != right && n >= 2) {
          drawPolyline(left, bot, right - left, bot - top, n,
                       SERIES_COLORS[s % (sizeof(SERIES_COLORS))], 3);
        }
      }
    }
  }
  Log.printf("renderer: drew dashboard %u (%u panels, %u grid rows)\n",
             (unsigned)index, (unsigned)panel_count, (unsigned)grid_rows);
  return true;
}

}  // namespace dashboard_renderer

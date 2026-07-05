#include "dashboard_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bundle_parser.h"
#include "config.h"
#include "display_trmnl.h"   // extern FASTEPD epd
#include "gfx4.h"
#include "log.h"

namespace dashboard_renderer {

namespace {

// Layout constants — MUST mirror tools/preview_trmnl.py.
constexpr int PANEL_GUTTER = 6;
// Panel titles + y-axis tick labels: drawn in the light (Thin) font family at a
// light gray so they frame the data without competing with the plotted line.
// The Thin weight is what makes them read light — the old Black font looked
// heavy/dark even at a high gray.
constexpr uint8_t TITLE_GRAY = 7;
constexpr uint8_t YAXIS_GRAY = 7;
const uint8_t SERIES_GRAYS[] = {0, 6, 10, 3};

// Holds a full 4x series (~3200 pts) so a high-res chart isn't truncated. Two
// float arrays => ~33 KB BSS at 4096; comfortable in the S3's 512 KB SRAM.
constexpr int MAX_POINTS = 4096;
constexpr int MAX_LABELS = 8;
constexpr int MAX_BANDS = 8;

float gPX[MAX_POINTS];
float gPY[MAX_POINTS];
char gTitle[96];
char gUnit[16];
char gValue[32];
char gYL[MAX_LABELS][12];
float gYLPos[MAX_LABELS];   // normalized 0=bottom..1=top for each y label (v2)
struct Band { float y0, y1; uint8_t g; };
Band gBands[MAX_BANDS];

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Very light tint of a status gray for tile backgrounds / bands (keeps black
// text legible). Mirrors preview_trmnl.tile_bg().
uint8_t tileBg(uint8_t g) {
  if (g >= 15) return 15;
  return (uint8_t)(g + (15 - g) * 3 / 4);
}

// Decode `count` normalized points from the reader into gPX/gPY, storing at
// most MAX_POINTS (extras are consumed but dropped). Returns stored count.
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

void drawPolyline(int left, int bot, int pw, int ph, int n, uint8_t gray) {
  int prevx = -1, prevy = -1;
  for (int i = 0; i < n; i++) {
    float nx = std::min(1.0f, std::max(0.0f, gPX[i]));
    float ny = std::min(1.0f, std::max(0.0f, gPY[i]));
    int x = left + (int)(nx * pw);
    int y = bot - (int)(ny * ph);
    if (prevx >= 0) gfx4::thickLine(prevx, prevy, x, y, gray, 3);
    prevx = x;
    prevy = y;
  }
}

void renderStat(int px, int py, int pw, int ph, uint8_t base_gray, int sparkN) {
  // Tinted status card (no top accent bar — the tint alone carries the status).
  int x0 = px + PANEL_GUTTER, y0 = py + PANEL_GUTTER;
  int x1 = px + pw - PANEL_GUTTER, y1 = py + ph - PANEL_GUTTER;
  epd.fillRect(x0, y0, x1 - x0, y1 - y0, tileBg(base_gray));

  int titlePx = clampi((int)(ph * 0.09f), 18, 28);
  gfx4::drawTextFitLight(px + 16, py + 10, pw - 28, titlePx, gTitle, TITLE_GRAY);

  // Value + unit as a single string in one font ("34%"), so the font's own
  // metrics handle the size and kerning — drawing them separately made the unit
  // look small and mis-spaced. Centered in the tile.
  char vu[48];
  const char* value = gValue[0] ? gValue : "-";
  snprintf(vu, sizeof(vu), "%s%s", value, gUnit);
  int vtar = clampi((int)(ph * 0.30f), 24, 64);
  int vpx = gfx4::fittedPx(vu, pw - 24, vtar);
  int vy = py + ph / 2 - vpx / 2;
  gfx4::drawTextCenteredFit(px, vy, pw, vpx, vu, GRAY_BLACK);

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
        if (prevx >= 0) gfx4::thickLine(prevx, prevy, x, y, 3, 2);
        prevx = x;
        prevy = y;
      }
    }
  }
}

void renderTimeseriesFrame(int px, int py, int pw, int ph, int yn,
                           int bn, int& left, int& top, int& right, int& bot) {
  int tb_h = clampi((int)(ph * 0.12f), 24, 42);
  int titlePx = clampi((int)(tb_h * 0.72f), 16, 30);

  left = px + PANEL_GUTTER + (yn ? 48 : 10);
  right = px + pw - PANEL_GUTTER - 6;
  top = py + tb_h + 6;
  bot = py + ph - PANEL_GUTTER - 6;   // no x-axis labels: reclaim the bottom

  // Title left-aligned with the plot's left edge (where the x-axis begins).
  gfx4::drawTextFitLight(left, py + 6, (px + pw - PANEL_GUTTER) - left, titlePx, gTitle, TITLE_GRAY);

  if (right - left < 20 || bot - top < 20) { left = right = top = bot = 0; return; }
  int plotW = right - left, plotH = bot - top;

  // Threshold fill bands (y bottom-up). The gray is already a light shade
  // chosen on the bridge (red darker than yellow, in-range drawn as none), so
  // paint it directly rather than lightening again.
  for (int i = 0; i < bn; i++) {
    int by0 = bot - (int)(gBands[i].y1 * plotH);
    int by1 = bot - (int)(gBands[i].y0 * plotH);
    if (by1 > by0) epd.fillRect(left, by0, plotW, by1 - by0, gBands[i].g);
  }

  // Y gridlines + labels (no x-axis line, no x tick labels). Size every label
  // to the compact font the widest one needs (so 3-digit / negative labels set
  // the size and all labels stay small and uniform), drawn in a light gray.
  // Each label sits at its own normalized position (gYLPos) — the bridge (like
  // Grafana/uPlot) places ticks inside a data-hugging range, so they are NOT
  // evenly spaced and don't touch the axis edges.
  int labTar = clampi((int)(ph * 0.06f), 16, 20);
  int labPx = labTar;
  for (int i = 0; i < yn; i++) {
    int fp = gfx4::fittedPxLight(gYL[i], 44, labTar);
    if (fp < labPx) labPx = fp;
  }
  for (int i = 0; i < yn; i++) {
    float pos = std::min(1.0f, std::max(0.0f, gYLPos[i]));
    int gy = bot - (int)(pos * plotH);
    gfx4::dottedHLine(left + 2, right, gy, 12);
    // FastEPD measures the '.' glyph's advance but under-renders it, so a label
    // with a decimal lands ~3/4 em left of an integer one — visibly ragged on a
    // right-aligned axis. Nudge decimal labels back right to match the integers.
    int rx = left - 6;
    if (strchr(gYL[i], '.')) rx += labPx * 3 / 4;
    gfx4::drawTextRightFitLight(rx, gy - labPx / 2, 46, labPx, gYL[i], YAXIS_GRAY);
  }
}

}  // namespace

bool render(const uint8_t* blob, size_t len, uint32_t index) {
  uint32_t ofs = bundle::dashboardOffset(blob, len, index);
  if (ofs == 0) {
    Log.printf("renderer: bad dashboard index %u\n", (unsigned)index);
    return false;
  }
  display_trmnl::clear(GRAY_WHITE);

  BinReader r(blob + ofs, len - ofs);
  r.skipPstr();   // dashboard title (not drawn on screen; available via bundle::dashboardTitle)
  uint8_t grid_cols = r.u8();
  (void)grid_cols;
  uint8_t grid_rows = r.u8();
  uint8_t panel_count = r.u8();

  // Stretch the grid to fill the safe area (screen minus the bezel inset) —
  // independent x/y scaling, no letterbox (mirrors tools/preview_trmnl.py).
  int areaW = SCREEN_W - SAFE_LEFT - SAFE_RIGHT;
  int areaH = SCREEN_H - SAFE_TOP - SAFE_BOTTOM;
  float cellX = (float)areaW / GRID_COLS;
  float cellY = (float)areaH / std::max<uint8_t>(1, grid_rows);

  for (uint8_t pi = 0; pi < panel_count; pi++) {
    uint8_t type = r.u8();
    uint8_t gx = r.u8(), gy = r.u8(), gw = r.u8(), gh = r.u8();
    r.pstr(gTitle, sizeof(gTitle));
    r.pstr(gUnit, sizeof(gUnit));
    uint8_t base_gray = r.u8();

    int px = SAFE_LEFT + (int)(gx * cellX);
    int py = SAFE_TOP + (int)(gy * cellY);
    int pw = (int)(gw * cellX);
    int ph = (int)(gh * cellY);

    if (type == PANEL_STAT) {
      r.pstr(gValue, sizeof(gValue));
      uint8_t sc = r.u8();
      int sparkN = readPoints(r, sc);
      if (pw > 20 && ph > 20) renderStat(px, py, pw, ph, base_gray, sparkN);
    } else {
      // timeseries: labels, bands, series.
      uint8_t yn = r.u8();
      int yStored = 0;
      for (uint8_t i = 0; i < yn; i++) {
        if (yStored < MAX_LABELS) {
          r.pstr(gYL[yStored], sizeof(gYL[0]));
          gYLPos[yStored] = r.u16() / 65535.0f;   // v2: per-label position
          yStored++;
        } else {
          r.skipPstr();
          r.u16();
        }
      }
      uint8_t xn = r.u8();  // x-axis labels are no longer drawn; skip any present
      for (uint8_t i = 0; i < xn; i++) r.skipPstr();
      uint8_t bn = r.u8();
      int bStored = 0;
      for (uint8_t i = 0; i < bn; i++) {
        float y0 = r.u16() / 65535.0f;
        float y1 = r.u16() / 65535.0f;
        uint8_t g = r.u8();
        if (bStored < MAX_BANDS) gBands[bStored++] = {y0, y1, g};
      }

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
                       SERIES_GRAYS[s % (sizeof(SERIES_GRAYS))]);
        }
      }
    }
  }
  Log.printf("renderer: drew dashboard %u (%u panels, %u grid rows)\n",
             (unsigned)index, (unsigned)panel_count, (unsigned)grid_rows);
  return true;
}

}  // namespace dashboard_renderer

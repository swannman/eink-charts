#include "framebuffer.h"

#include <Arduino.h>

#include "config.h"

namespace fb {

static uint8_t* gBuf = nullptr;

bool begin() {
  if (gBuf) return true;
  gBuf = (uint8_t*)ps_malloc((size_t)SCREEN_W * SCREEN_H);
  if (gBuf) clear(COL_WHITE);
  return gBuf != nullptr;
}

uint8_t* buffer() { return gBuf; }

void clear(uint8_t color) {
  if (gBuf) memset(gBuf, color, (size_t)SCREEN_W * SCREEN_H);
}

static inline bool inX(int x) { return x >= 0 && x < SCREEN_W; }
static inline bool inY(int y) { return y >= 0 && y < SCREEN_H; }

void pixel(int x, int y, uint8_t color) {
  if (gBuf && inX(x) && inY(y)) gBuf[(size_t)y * SCREEN_W + x] = color;
}

static void clipRect(int& x, int& y, int& w, int& h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
}

void fillRect(int x, int y, int w, int h, uint8_t color) {
  if (!gBuf) return;
  clipRect(x, y, w, h);
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) {
    memset(gBuf + (size_t)yy * SCREEN_W + x, color, w);
  }
}

// 4x4 ordered (Bayer) threshold matrix — cell k lights up once coverage16 > its
// rank, giving evenly scattered ink at any coverage without visible banding.
static const uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

void fillRectDither(int x, int y, int w, int h, uint8_t color, uint8_t coverage16) {
  if (!gBuf || coverage16 == 0) return;
  if (coverage16 >= 16) { fillRect(x, y, w, h, color); return; }
  clipRect(x, y, w, h);
  if (w <= 0 || h <= 0) return;
  for (int yy = y; yy < y + h; yy++) {
    uint8_t* row = gBuf + (size_t)yy * SCREEN_W;
    const uint8_t* brow = kBayer4[yy & 3];
    for (int xx = x; xx < x + w; xx++) {
      if (brow[xx & 3] < coverage16) row[xx] = color;
    }
  }
}

static void line1(int x0, int y0, int x1, int y1, uint8_t color) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void thickLine(int x0, int y0, int x1, int y1, uint8_t color, int thickness) {
  for (int t = 0; t < thickness; t++) {
    int yo = t - thickness / 2;
    line1(x0, y0 + yo, x1, y1 + yo, color);
  }
}

void dottedHLine(int x0, int x1, int y, uint8_t color, int pitch) {
  if (!inY(y)) return;
  for (int x = x0; x <= x1; x += pitch) pixel(x, y, color);
}

void rect(int x, int y, int w, int h, uint8_t color) {
  fillRect(x, y, w, 1, color);
  fillRect(x, y + h - 1, w, 1, color);
  fillRect(x, y, 1, h, color);
  fillRect(x + w - 1, y, 1, h, color);
}

}  // namespace fb

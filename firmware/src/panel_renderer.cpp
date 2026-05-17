#include "panel_model.h"

#include <Arduino.h>
#include <algorithm>

#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "gfx_lite.h"

namespace {

constexpr int CHART_MARGIN_L = 70;
constexpr int CHART_MARGIN_R = 20;
constexpr int CHART_MARGIN_T = 80;
constexpr int CHART_MARGIN_B = 50;

constexpr int CHART_L = CHART_MARGIN_L;
constexpr int CHART_R = FB_WIDTH - CHART_MARGIN_R;
constexpr int CHART_T = CHART_MARGIN_T;
constexpr int CHART_B = FB_HEIGHT - CHART_MARGIN_B;
constexpr int CHART_W = CHART_R - CHART_L;
constexpr int CHART_H = CHART_B - CHART_T;

void drawAxes(uint8_t* fb, const PanelData& p) {
  // X axis only — no Y axis vertical line. Y values are anchored by their
  // labels alone.
  for (int x = CHART_L; x <= CHART_R; x++) fbSetPixel(fb, x, CHART_B, true);

  // Y labels: render from bottom (low) to top (high); JSON gives them low-first.
  // Labels are right-aligned to sit just left of the chart, and horizontal
  // gridlines are dotted at 4-pixel pitch — denser than the previous 6 so
  // they read clearly without overpowering the line.
  const uint32_t yn = p.y_label_count > 0 ? p.y_label_count : 1;
  for (uint32_t i = 0; i < yn; i++) {
    int y = CHART_B - (int)((int64_t)CHART_H * i / std::max<uint32_t>(1, yn - 1));
    const int w = fbStringWidth(p.y_labels[i], 2);
    fbDrawString(fb, CHART_L - 6 - w, y - 6, 2, p.y_labels[i], /*black=*/true);
    for (int x = CHART_L + 2; x < CHART_R; x += 4) fbSetPixel(fb, x, y, true);
  }

  const uint32_t xn = p.x_label_count > 0 ? p.x_label_count : 1;
  for (uint32_t i = 0; i < xn; i++) {
    int x = CHART_L + (int)((int64_t)CHART_W * i / std::max<uint32_t>(1, xn - 1));
    const int w = fbStringWidth(p.x_labels[i], 2);
    int tx;
    if (i == 0) {
      tx = x;                     // left-align first label to chart-left
    } else if (i == xn - 1) {
      tx = x - w;                 // right-align last label to chart-right
    } else {
      tx = x - w / 2;             // center the rest under their tick
    }
    fbDrawString(fb, tx, CHART_B + 8, 2, p.x_labels[i], true);
  }
}

// Light-gray fill below the line. (x + y) % stride == 0 → black, else white.
// Stride is in pixels — 3 reads as ~33% gray, 4 as ~25% (lighter still).
// Fixed parity (no per-panel offset) so transitions between dashboards leave
// the unchanged-fill pixels alone — differential refresh stays clean.
constexpr int FILL_STRIDE = 5;

void fillUnderCurve(uint8_t* fb, const PanelData& p) {
  if (p.point_count < 2) return;
  for (int x = CHART_L + 1; x < CHART_R; x++) {
    float t = (float)(x - CHART_L) / CHART_W;
    // Locate the segment containing t. Points are uniform from Prometheus
    // query_range, so direct index works.
    int idx = (int)(t * (p.point_count - 1));
    if (idx < 0) idx = 0;
    if (idx >= (int)p.point_count - 1) idx = (int)p.point_count - 2;
    float t0 = p.points[idx * 2], t1 = p.points[(idx + 1) * 2];
    float y0 = p.points[idx * 2 + 1], y1 = p.points[(idx + 1) * 2 + 1];
    float ny;
    if (t1 > t0) {
      float lerp = (t - t0) / (t1 - t0);
      if (lerp < 0) lerp = 0;
      if (lerp > 1) lerp = 1;
      ny = y0 + (y1 - y0) * lerp;
    } else {
      ny = y0;
    }
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    int curve_y = CHART_B - (int)(ny * CHART_H);
    for (int y = curve_y + 1; y < CHART_B; y++) {
      if (((x + y) % FILL_STRIDE) == 0) fbSetPixel(fb, x, y, true);
    }
  }
}

void plotSeries(uint8_t* fb, const PanelData& p, int thickness = 2) {
  if (p.point_count == 0) return;
  int prev_x = -1, prev_y = -1;
  for (uint32_t i = 0; i < p.point_count; i++) {
    float nx = p.points[2 * i];
    float ny = p.points[2 * i + 1];
    if (nx < 0) nx = 0;
    if (nx > 1) nx = 1;
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    int x = CHART_L + (int)(nx * CHART_W);
    int y = CHART_B - (int)(ny * CHART_H);
    if (prev_x >= 0) {
      int dx = x - prev_x;
      int dy = y - prev_y;
      int steps = std::max(std::abs(dx), std::abs(dy));
      for (int s = 0; s <= steps; s++) {
        int px = prev_x + (dx * s) / std::max(1, steps);
        int py = prev_y + (dy * s) / std::max(1, steps);
        for (int t = 0; t < thickness; t++) {
          fbSetPixel(fb, px, py + t, true);
        }
      }
    }
    prev_x = x;
    prev_y = y;
  }
}

}  // namespace

namespace {

// Draws a sparkline inside (x0,y0)-(x0+w,y0+h). No axes, no labels. Fills the
// area under the curve with a dotted pattern at FILL_STRIDE pitch (matches
// the chart-panel fill so the visual language is consistent).
void drawSparkline(uint8_t* fb, int x0, int y0, int w, int h,
                   const float* pts, uint32_t n) {
  if (n < 2) return;
  auto curveYAt = [&](int x) -> int {
    float t = (float)(x - x0) / (float)std::max(1, w - 1);
    int idx = (int)(t * (n - 1));
    if (idx < 0) idx = 0;
    if (idx >= (int)n - 1) idx = (int)n - 2;
    float t0 = pts[idx * 2], t1 = pts[(idx + 1) * 2];
    float y0n = pts[idx * 2 + 1], y1n = pts[(idx + 1) * 2 + 1];
    float ny;
    if (t1 > t0) {
      float lerp = (t - t0) / (t1 - t0);
      if (lerp < 0) lerp = 0;
      if (lerp > 1) lerp = 1;
      ny = y0n + (y1n - y0n) * lerp;
    } else {
      ny = y0n;
    }
    if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    return y0 + h - 1 - (int)(ny * (h - 1));
  };
  // Fill first so the line sits on top of the dots.
  for (int x = x0; x < x0 + w; x++) {
    int cy = curveYAt(x);
    for (int y = cy + 1; y < y0 + h; y++) {
      if (((x + y) % FILL_STRIDE) == 0) fbSetPixel(fb, x, y, true);
    }
  }
  // Line on top.
  int prev_x = -1, prev_y = -1;
  for (uint32_t i = 0; i < n; i++) {
    float nx = pts[2 * i];
    float ny = pts[2 * i + 1];
    if (nx < 0) nx = 0; if (nx > 1) nx = 1;
    if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    int x = x0 + (int)(nx * (w - 1));
    int y = y0 + h - 1 - (int)(ny * (h - 1));
    if (prev_x >= 0) {
      int dx = x - prev_x;
      int dy = y - prev_y;
      int steps = std::max(std::abs(dx), std::abs(dy));
      for (int s = 0; s <= steps; s++) {
        int px = prev_x + (dx * s) / std::max(1, steps);
        int py = prev_y + (dy * s) / std::max(1, steps);
        fbSetPixel(fb, px, py, true);
        fbSetPixel(fb, px, py + 1, true);  // 2px thickness
      }
    }
    prev_x = x;
    prev_y = y;
  }
}

}  // namespace

void drawStatScreen(uint8_t* fb, const StatEntry* entries, uint32_t n, const char* view_label) {
  fbClear(fb, /*white=*/true);
  if (n == 0) return;
  if (n > 3) n = 3;

  const int col_w = FB_WIDTH / (int)n;
  // Baselines for FreeSansBold18pt7b at various scales; tuned so the column
  // reads top-to-bottom: title, big value+unit, sparkline.
  const int title_baseline = 80;
  const int value_baseline = 320;   // big value sits in the visual center
  const int spark_top = 400;
  const int spark_h = 100;

  for (uint32_t i = 0; i < n; i++) {
    const StatEntry& e = entries[i];
    const int cx_left = (int)i * col_w;
    const int cx_center = cx_left + col_w / 2;

    // Title — same face/scale as the chart-screen title.
    if (e.title && e.title[0]) {
      const int tw = fbGfxStringWidth(&FreeSansBold18pt7b, e.title);
      fbDrawStringGfx(fb, cx_center - tw / 2, title_baseline,
                      &FreeSansBold18pt7b, e.title, true);
    }

    // Value + unit, rendered as a single horizontal group so they're
    // visually one number. Value is large (scale ~4); unit is half-scale
    // baseline-aligned so it sits next to the value like "72.3F".
    const char* v = (e.value_str && e.value_str[0]) ? e.value_str : "—";
    const char* u = (e.unit && e.unit[0]) ? e.unit : "";
    // Target ~2× the chart-title size; value and unit share the same scale
    // so the unit reads as part of the number ("72F" rather than "72 small-F").
    // Shrink the whole group if a long value would overflow the column.
    int scale = 2;
    int v_w = fbGfxStringWidthScaled(&FreeSansBold18pt7b, v, scale);
    int u_w = u[0] ? fbGfxStringWidthScaled(&FreeSansBold18pt7b, u, scale) : 0;
    int total_w = v_w + u_w;
    while (total_w > col_w - 16 && scale > 1) {
      scale--;
      v_w = fbGfxStringWidthScaled(&FreeSansBold18pt7b, v, scale);
      u_w = u[0] ? fbGfxStringWidthScaled(&FreeSansBold18pt7b, u, scale) : 0;
      total_w = v_w + u_w;
    }
    const int group_left = cx_center - total_w / 2;
    fbDrawStringGfxScaled(fb, group_left, value_baseline,
                          &FreeSansBold18pt7b, v, scale, true);
    if (u[0]) {
      fbDrawStringGfxScaled(fb, group_left + v_w, value_baseline,
                            &FreeSansBold18pt7b, u, scale, true);
    }

    // Sparkline — bottom band of the column. Inset 16px so adjacent
    // sparklines have visual separation.
    if (e.spark && e.spark_count >= 2) {
      drawSparkline(fb, cx_left + 16, spark_top, col_w - 32, spark_h,
                    e.spark, e.spark_count);
    }

    // Dotted vertical separator between columns.
    if (i + 1 < n) {
      const int sx = cx_left + col_w - 1;
      for (int y = 30; y < FB_HEIGHT - 30; y++) {
        if ((y % 4) == 0) fbSetPixel(fb, sx, y, true);
      }
    }
  }

  if (view_label && view_label[0]) {
    const int w = fbGfxStringWidth(&FreeSans18pt7b, view_label);
    fbDrawStringGfx(fb, FB_WIDTH - 20 - w, 50, &FreeSans18pt7b, view_label, true);
  }
}

void drawListView(uint8_t* fb, const char* const* titles, uint32_t n, uint32_t cursor_idx) {
  fbClear(fb, /*white=*/true);

  // 2 columns; fill column 0 top-to-bottom first, then column 1.
  const int top_margin = 60;
  const int bot_margin = 30;
  const int row_h = 50;                                  // FreeSans18pt7b line height + breathing
  const int rows_per_col = (FB_HEIGHT - top_margin - bot_margin) / row_h;
  const int col_w = FB_WIDTH / 2;
  const int text_pad_x = 16;
  const int text_pad_y = 8;                              // baseline offset within the row
  const int text_max_w = col_w - 2 * text_pad_x;         // truncate beyond this

  char buf[64];
  for (uint32_t i = 0; i < n; i++) {
    const int col = (int)(i / rows_per_col);
    if (col >= 2) break;                                  // truncate any overflow
    const int row = (int)(i % rows_per_col);
    const int rx = col * col_w;
    const int ry = top_margin + row * row_h;
    const bool selected = (i == cursor_idx);

    if (selected) {
      // Black highlight bar, slightly inset so adjacent rows don't touch.
      fbFillRect(fb, rx + 8, ry + 4, col_w - 16, row_h - 8, /*black=*/true);
    }
    const char* src = titles[i] ? titles[i] : "";
    // Copy into a working buffer so we can shorten without mutating caller.
    size_t n_copy = strlen(src);
    if (n_copy >= sizeof(buf)) n_copy = sizeof(buf) - 1;
    memcpy(buf, src, n_copy);
    buf[n_copy] = 0;
    // Shrink the string with a trailing ellipsis if it overflows the column.
    if (fbGfxStringWidth(&FreeSans18pt7b, buf) > text_max_w) {
      while (n_copy > 1) {
        n_copy--;
        buf[n_copy] = 0;
        // Pretend we're appending "…" (rendered as "..") while measuring.
        // We strip one extra char first so the dots replace, not extend.
        char probe[64];
        size_t pn = n_copy > 0 ? n_copy - 1 : 0;
        memcpy(probe, buf, pn);
        probe[pn] = 0;
        strncat(probe, "..", sizeof(probe) - strlen(probe) - 1);
        if (fbGfxStringWidth(&FreeSans18pt7b, probe) <= text_max_w) {
          memcpy(buf, probe, strlen(probe) + 1);
          break;
        }
      }
    }
    fbDrawStringGfx(fb, rx + text_pad_x, ry + row_h - text_pad_y,
                    &FreeSans18pt7b, buf, /*black=*/!selected);
  }
}

void drawPanel(uint8_t* fb, const PanelData& panel) {
  fbClear(fb, /*white=*/true);
  drawAxes(fb, panel);
  fillUnderCurve(fb, panel);
  plotSeries(fb, panel);
  // Title last so its descenders (g, p, y) overlay cleanly on top of anything
  // the chart drew into the top margin.
  fbDrawStringGfx(fb, CHART_L, 50, &FreeSansBold18pt7b, panel.title, /*black=*/true);
  if (panel.view_label && panel.view_label[0]) {
    // Non-bold to distinguish from the title — same visual height, lighter
    // stroke, sits quietly in the corner without competing.
    const int w = fbGfxStringWidth(&FreeSans18pt7b, panel.view_label);
    const int x = FB_WIDTH - 20 - w;  // 20-px right margin
    fbDrawStringGfx(fb, x, 50, &FreeSans18pt7b, panel.view_label, true);
  }
}

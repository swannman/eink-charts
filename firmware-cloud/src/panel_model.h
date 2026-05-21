#pragma once
#include <stdint.h>

// Shape that both demo (hardcoded arrays) and prod (parsed from /data JSON)
// hand to the renderer. Points are pre-normalized to [0,1] on both axes.
struct PanelData {
  const char* title;
  const char* series_name;
  const float* points;          // packed [x0,y0, x1,y1, ...]; length = 2*point_count
  uint32_t point_count;
  const char* const* y_labels;  // ordered low-to-high; printed bottom-up
  uint32_t y_label_count;
  const char* const* x_labels;  // ordered left-to-right
  uint32_t x_label_count;
  // Optional short label drawn right-aligned in the top-right (e.g. "24h",
  // "2h", "7d"). Null/empty → nothing drawn there.
  const char* view_label;
};

// One single-value entry inside a stat-group screen. Up to 3 of these are
// rendered side by side per screen.
struct StatEntry {
  const char* title;
  const char* unit;       // short suffix, e.g. "F", "%", "PSI"
  const char* value_str;  // already formatted (correct decimals etc.)
  const float* spark;     // optional sparkline points, same packed layout as PanelData::points
  uint32_t spark_count;
};

void drawPanel(uint8_t* fb, const PanelData& panel);

// Render 1-3 stat entries side by side, splitting FB_WIDTH into equal columns.
// `view_label` is optional and drawn top-right of the whole screen.
void drawStatScreen(uint8_t* fb, const StatEntry* entries, uint32_t n, const char* view_label);

// Render a 2-column scrollable list of panel titles for the list-mode UI.
// Columns fill top→bottom, left→right (top-left → bottom of col 0 → top of
// col 1 → bottom of col 1). `cursor_idx` is the highlighted row (0-based,
// flattened across both columns).
void drawListView(uint8_t* fb, const char* const* titles, uint32_t n, uint32_t cursor_idx);

// Render the device's recent serial-style logs from the RTC ring buffer.
// Shows the most recent ~63 rows × 132 columns of text. Only accessible via
// long-press list mode — not part of the forward-press rotation.
void drawLogsScreen(uint8_t* fb);

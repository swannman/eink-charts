#pragma once
// Grayscale drawing helpers layered on FastEPD's primitives. All colors are
// 0..15 (0=black, 15=white). Text uses the vendored anti-aliased Roboto Black
// BB_FONTs; in 4bpp mode FastEPD anti-aliases glyph edges with a gray ramp, so
// text stays crisp on the 226-DPI panel.
#include <stdint.h>

namespace gfx4 {

// Measure the pixel width of `s` in the bold font nearest `target_px`.
int textWidth(const char* s, int target_px);

// The nominal px of the bold font drawTextFit would actually use for `s` in
// max_w at target_px (so callers can position by the real, discrete size).
int fittedPx(const char* s, int max_w, int target_px);

// Draw `s` with its visual TOP-left at (x, y) in gray `color`, using the largest
// vendored bold font <= target_px that fits within max_w (shrinking if needed).
// (FastEPD's baseline anchor is handled internally.) Returns the chosen font's
// nominal pixel height (useful for line spacing / centering).
int drawTextFit(int x, int y, int max_w, int target_px, const char* s, uint8_t color);

// Same, but in the light (Thin / regular-weight) family — for panel titles.
int drawTextFitLight(int x, int y, int max_w, int target_px, const char* s, uint8_t color);

// Same as drawTextFit, horizontally centered within [x, x+w].
int drawTextCenteredFit(int x, int y, int w, int target_px, const char* s, uint8_t color);

// Right-align `s` so it ends at right_x.
int drawTextRightFit(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color);

// Same, in the light (Thin) family — for y-axis tick labels.
int drawTextRightFitLight(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color);

// The nominal px of the light font that would be used for `s` (light analogue
// of fittedPx) — so callers can size a row of labels uniformly.
int fittedPxLight(const char* s, int max_w, int target_px);

// A mostly-horizontal chart polyline segment with pixel thickness (drawn as
// stacked 1px lines). Clipped to the screen.
void thickLine(int x0, int y0, int x1, int y1, uint8_t color, int thickness);

// Dotted horizontal gridline at `y` from x0..x1 (every `pitch` px).
void dottedHLine(int x0, int x1, int y, uint8_t color, int pitch = 6);

}  // namespace gfx4

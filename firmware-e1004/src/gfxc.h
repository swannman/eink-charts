#pragma once
// Color text helpers drawn into the fb:: framebuffer. Colors are Spectra 6
// codes (config.h COL_*). Text is solid ink — the 6-color panel has no
// grayscale, so there's no anti-aliasing; the 150-DPI pitch keeps the vendored
// FreeSans/FreeSansBold GFX fonts (with an integer scale for big stat values)
// crisp enough. API mirrors firmware-trmnl's gfx4 so the renderer ports 1:1.
#include <stdint.h>

namespace gfxc {

// Measure the pixel width of `s` in the bold face nearest `target_px`.
int textWidth(const char* s, int target_px);

// The nominal px the bold face drawTextFit would actually use for `s` in
// max_w at target_px (so callers can position by the real, discrete size).
int fittedPx(const char* s, int max_w, int target_px);

// Draw `s` with its visual TOP-left at (x, y) in `color`, using the largest
// bold face <= target_px that fits within max_w (shrinking if needed).
// Returns the chosen face's nominal pixel height.
int drawTextFit(int x, int y, int max_w, int target_px, const char* s, uint8_t color);

// Same, in the regular-weight face — for panel titles / axis labels.
int drawTextFitLight(int x, int y, int max_w, int target_px, const char* s, uint8_t color);

// Same as drawTextFit, horizontally centered within [x, x+w].
int drawTextCenteredFit(int x, int y, int w, int target_px, const char* s, uint8_t color);

// Right-align `s` by its true ink extent so it ends at right_x.
int drawTextRightFit(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color);
int drawTextRightFitLight(int right_x, int y, int max_w, int target_px, const char* s, uint8_t color);

int fittedPxLight(const char* s, int max_w, int target_px);

}  // namespace gfxc

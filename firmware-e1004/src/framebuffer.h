#pragma once
// Landscape 1600x1200 indexed-color framebuffer (one Spectra 6 code per byte,
// ~1.9 MB in PSRAM). All drawing happens here; display_e1004::present() feeds
// it to the panel driver with the landscape->portrait rotation. Owning our own
// buffer keeps the renderer independent of the EPD library's paging scheme.
#include <stdint.h>

namespace fb {

// Allocate the buffer in PSRAM (once). Returns false if PSRAM is absent.
bool begin();

// The raw buffer: SCREEN_W * SCREEN_H bytes, row-major, [y * SCREEN_W + x].
uint8_t* buffer();

void clear(uint8_t color);
void pixel(int x, int y, uint8_t color);
void fillRect(int x, int y, int w, int h, uint8_t color);

// Sparse fill for light "tints": paints `color` only where a 4x4 ordered
// (Bayer) mask admits it, leaving the rest untouched (typically white).
// coverage16 is the number of lit cells per 16-px tile (0..16); 4 == 25%.
void fillRectDither(int x, int y, int w, int h, uint8_t color, uint8_t coverage16);

// A mostly-horizontal chart polyline segment with pixel thickness (drawn as
// stacked 1px Bresenham lines). Clipped to the screen.
void thickLine(int x0, int y0, int x1, int y1, uint8_t color, int thickness);

// Dotted horizontal gridline at `y` from x0..x1 (every `pitch` px).
void dottedHLine(int x0, int x1, int y, uint8_t color, int pitch = 6);

// 1px rectangle outline.
void rect(int x, int y, int w, int h, uint8_t color);

}  // namespace fb

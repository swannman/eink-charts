#pragma once
// Thin ownership + lifecycle wrapper around FastEPD for the TRMNL X panel.
// Exposes the FASTEPD instance so gfx4/renderer can draw with its primitives,
// and centralizes the 4bpp bring-up + the vendor gray matrix.
#include <FastEPD.h>

extern FASTEPD epd;

namespace display_trmnl {

// initPanel(BB_PANEL_TRMNL_X) + setPasses + 4bpp mode. Returns false if the
// framebuffer couldn't be allocated (PSRAM missing/misconfigured).
bool begin();

// Fill the back buffer with a gray level (default white).
void clear(uint8_t gray = 15);

// Push the back buffer to the panel: applies the vendor gray matrix then runs
// a full 4bpp refresh. `keepOn` leaves the panel rails powered for a quick
// follow-up refresh.
void present(bool keepOn = false);

// Power the panel down and release the I/O expander before deep sleep.
void sleep();

}  // namespace display_trmnl

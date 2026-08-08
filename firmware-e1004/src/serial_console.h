#pragma once
// Dev-only interactive console over USB serial (CH340 UART on the E1004). Lets
// the host see what the panel is actually showing (the e-ink can't be
// screenshotted) and drive the slideshow. Entered after each render when
// SERIAL_CONSOLE is set; returns (and the caller deep-sleeps) on 's' or an
// idle timeout.
//
// Commands (single chars):
//   d  dump the framebuffer downscaled 3x as base64 palette indices
//   D  dump downscaled 2x (higher resolution, larger)
//   n  next dashboard (re-render + full refresh)
//   p  previous dashboard
//   r  redraw the current dashboard
//   b  battery reading
//   s  sleep now
//
// Dump wire format (host decodes between the markers; tools/e1004_console.py):
//   <<<FBC <w> <h>>>\n  <base64 of w*h bytes, one Spectra 6 palette index per
//   pixel (dominant color of each box)>  \n<<<ENDFBC>>>
#include <stddef.h>
#include <stdint.h>

namespace serial_console {

// Render dashboard `index` (already reduced mod count) from cache and present
// it. Returns false if that dashboard isn't available. Supplied by main.
using RenderIndexFn = bool (*)(uint32_t index);

// count is the number of cached dashboards (0 if none). dashIndex is the live
// slideshow index (RTC-persisted in main), advanced by n/p. renderIndex paints
// a given dashboard from cache. A green-button press also advances.
void run(uint8_t count, uint32_t& dashIndex, RenderIndexFn renderIndex);

}  // namespace serial_console

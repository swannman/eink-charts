#pragma once
// Lifecycle wrapper around Seeed_GFX's EPaper driver (T133A01, combo 523) for
// the E1004's 13.3" Spectra 6 panel. All drawing happens in the landscape fb::
// framebuffer; present() packs it into the library's portrait 4bpp sprite and
// runs the ~25 s full color refresh.
#include <stdint.h>

namespace display_e1004 {

// Bring up the EPaper driver (allocates its 960 KB sprite in PSRAM). Returns
// false if the sprite buffer failed to allocate.
bool begin();

// Pack fb:: into the panel sprite (landscape -> portrait rotation + palette
// mapping) and full-refresh the panel. Blocks for the whole refresh.
void present();

// Put the panel controller into deep sleep (update() already does this after
// each refresh; safe to call again before ESP deep sleep).
void sleep();

}  // namespace display_e1004

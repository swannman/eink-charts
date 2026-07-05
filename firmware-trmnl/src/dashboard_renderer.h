#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dashboard_renderer {

// Draw dashboard `index` from the cached 0xCFB2 blob into the FastEPD back
// buffer (caller calls display_trmnl::present() afterwards). Lays panels out at
// their gridPos scaled to fill the screen (square cells, letterboxed) — the
// on-device twin of tools/preview_trmnl.py. Returns false if index is invalid.
bool render(const uint8_t* blob, size_t len, uint32_t index);

}  // namespace dashboard_renderer

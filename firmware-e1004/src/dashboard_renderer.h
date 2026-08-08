#pragma once
// Renders one dashboard from a v3 color bundle blob into the fb:: framebuffer
// at its Grafana gridPos, scaled to fill the 1600x1200 landscape screen.
// Mirrors bridge-cloud/tools/preview_e1004.py — keep the layout math in sync.
#include <stddef.h>
#include <stdint.h>

namespace dashboard_renderer {

// Draw dashboard `index` from the bundle into the framebuffer (caller clears +
// presents). Returns false if the blob/index is invalid.
bool render(const uint8_t* blob, size_t len, uint32_t index);

}  // namespace dashboard_renderer

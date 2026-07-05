#pragma once
// Reader for the TRMNL X "dashboard bundle" (magic 0xCFB2) produced by
// bridge-cloud/.../data_trmnl.py. Bounds-checked little-endian cursor plus a
// few bundle-level navigation helpers. The renderer streams panels straight
// out of the cached blob with BinReader, so there's no big intermediate model.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr uint16_t TRMNL_BUNDLE_MAGIC = 0xCFB2;
constexpr uint8_t TRMNL_BUNDLE_VERSION = 2;   // v2: y labels carry a u16 position
constexpr uint8_t PANEL_TIMESERIES = 0;
constexpr uint8_t PANEL_STAT = 1;

struct BinReader {
  const uint8_t* p;
  const uint8_t* end;
  BinReader(const uint8_t* data, size_t len) : p(data), end(data + len) {}
  bool ok(size_t n) const { return p + n <= end; }
  uint8_t u8() { return ok(1) ? *p++ : 0; }
  uint16_t u16() {
    if (!ok(2)) { p = end; return 0; }
    uint16_t v = p[0] | ((uint16_t)p[1] << 8);
    p += 2;
    return v;
  }
  uint32_t u32() {
    if (!ok(4)) { p = end; return 0; }
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    return v;
  }
  // Copy a length-prefixed string into `out` (NUL-terminated, truncated to
  // cap-1), advancing past it.
  const char* pstr(char* out, size_t cap) {
    uint8_t n = u8();
    if (!ok(n)) { p = end; if (cap) out[0] = 0; return out; }
    size_t take = (cap && n < cap - 1) ? n : (cap ? cap - 1 : 0);
    if (cap) { memcpy(out, p, take); out[take] = 0; }
    p += n;
    return out;
  }
  void skipPstr() { uint8_t n = u8(); p = ok(n) ? p + n : end; }
};

namespace bundle {

// Validate header (magic/version/count>0).
bool valid(const uint8_t* blob, size_t len);

// Number of dashboards (0 if invalid).
uint8_t dashboardCount(const uint8_t* blob, size_t len);

// next_poll seconds from the header.
uint32_t nextPoll(const uint8_t* blob, size_t len);

// Byte offset of dashboard `index`'s block, or 0 if out of range.
uint32_t dashboardOffset(const uint8_t* blob, size_t len, uint32_t index);

// Copy dashboard `index`'s title into `out`. Returns out.
const char* dashboardTitle(const uint8_t* blob, size_t len, uint32_t index,
                           char* out, size_t cap);

}  // namespace bundle

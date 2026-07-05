#include "bundle_parser.h"

namespace bundle {

static uint32_t readU32(const uint8_t* b, size_t len, size_t pos) {
  if (pos + 4 > len) return 0;
  return (uint32_t)b[pos] | ((uint32_t)b[pos + 1] << 8) |
         ((uint32_t)b[pos + 2] << 16) | ((uint32_t)b[pos + 3] << 24);
}

bool valid(const uint8_t* blob, size_t len) {
  if (len < 8) return false;
  uint16_t magic = blob[0] | ((uint16_t)blob[1] << 8);
  return magic == TRMNL_BUNDLE_MAGIC && blob[2] == TRMNL_BUNDLE_VERSION && blob[3] > 0;
}

uint8_t dashboardCount(const uint8_t* blob, size_t len) {
  return valid(blob, len) ? blob[3] : 0;
}

uint32_t nextPoll(const uint8_t* blob, size_t len) {
  return valid(blob, len) ? readU32(blob, len, 4) : 0;
}

uint32_t dashboardOffset(const uint8_t* blob, size_t len, uint32_t index) {
  if (!valid(blob, len) || index >= blob[3]) return 0;
  uint32_t ofs = readU32(blob, len, 8 + 4 * index);
  return (ofs < len) ? ofs : 0;
}

const char* dashboardTitle(const uint8_t* blob, size_t len, uint32_t index,
                           char* out, size_t cap) {
  if (cap) out[0] = 0;
  uint32_t ofs = dashboardOffset(blob, len, index);
  if (ofs == 0) return out;
  BinReader r(blob + ofs, len - ofs);
  r.pstr(out, cap);
  return out;
}

}  // namespace bundle

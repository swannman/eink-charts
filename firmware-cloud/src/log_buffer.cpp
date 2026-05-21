#include "log_buffer.h"

// Ring buffer lives in RTC slow memory so it survives deep sleep. ~64 of
// the C3's 8 KB RTC SLOW are used by existing RTC_DATA_ATTR vars; 4 KB is
// comfortably under budget.
RTC_DATA_ATTR static uint8_t rtcLogBuf[log_buffer::SIZE];
RTC_DATA_ATTR static size_t rtcLogHead = 0;   // next write index
RTC_DATA_ATTR static size_t rtcLogLen = 0;    // bytes valid (caps at SIZE)

namespace log_buffer {

static inline void appendByte(uint8_t c) {
  rtcLogBuf[rtcLogHead] = c;
  rtcLogHead = (rtcLogHead + 1) % SIZE;
  if (rtcLogLen < SIZE) rtcLogLen++;
}

size_t snapshot(uint8_t* out) {
  size_t n = rtcLogLen;
  // When the buffer hasn't wrapped, oldest byte is at index 0.
  // When it has, oldest is at rtcLogHead (next-to-be-overwritten slot).
  size_t start = (rtcLogLen < SIZE) ? 0 : rtcLogHead;
  for (size_t i = 0; i < n; i++) {
    out[i] = rtcLogBuf[(start + i) % SIZE];
  }
  return n;
}

size_t length() { return rtcLogLen; }

}  // namespace log_buffer

size_t MirrorPrint::write(uint8_t c) {
  Serial.write(c);
  log_buffer::appendByte(c);
  return 1;
}

size_t MirrorPrint::write(const uint8_t* buf, size_t len) {
  Serial.write(buf, len);
  for (size_t i = 0; i < len; i++) log_buffer::appendByte(buf[i]);
  return len;
}

MirrorPrint Log;

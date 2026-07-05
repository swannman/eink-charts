#include "serial_console.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "dashboard_renderer.h"
#include "display_trmnl.h"  // extern FASTEPD epd
#include "log.h"

namespace serial_console {

namespace {

const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Read one 4bpp pixel (0..15) from the rotation-0 framebuffer: byte index is
// (x>>1) + y*pitch, even x in the low nibble, odd x in the high nibble.
inline uint8_t readGray(const uint8_t* buf, int pitch, int x, int y) {
  uint8_t b = buf[(x >> 1) + y * pitch];
  return (x & 1) ? (b >> 4) : (b & 0x0F);
}

// Dump the current framebuffer, box-averaged down by `scale`, as base64 8-bit
// grayscale between <<<FB w h>>> / <<<ENDFB>>> markers.
void dump(int scale) {
  if (scale < 1) scale = 1;
  const uint8_t* buf = epd.currentBuffer();
  if (!buf) { Serial.println("dump: no framebuffer"); return; }
  const int pitch = SCREEN_W >> 1;  // native_width / 2
  const int outW = SCREEN_W / scale;
  const int outH = SCREEN_H / scale;
  const int inv = scale * scale;

  Serial.printf("<<<FB %d %d>>>\n", outW, outH);
  uint8_t tri[3];
  int tn = 0, col = 0;
  char q[4];
  for (int oy = 0; oy < outH; oy++) {
    const int by = oy * scale;
    for (int ox = 0; ox < outW; ox++) {
      const int bx = ox * scale;
      int sum = 0;
      for (int dy = 0; dy < scale; dy++)
        for (int dx = 0; dx < scale; dx++)
          sum += readGray(buf, pitch, bx + dx, by + dy);
      tri[tn++] = (uint8_t)((sum / inv) * 17);  // 0..15 -> 0..255
      if (tn == 3) {
        uint32_t v = ((uint32_t)tri[0] << 16) | (tri[1] << 8) | tri[2];
        q[0] = B64[(v >> 18) & 63]; q[1] = B64[(v >> 12) & 63];
        q[2] = B64[(v >> 6) & 63];  q[3] = B64[v & 63];
        Serial.write((const uint8_t*)q, 4);
        tn = 0;
        if ((col += 4) >= 76) { Serial.write('\n'); col = 0; }
      }
    }
  }
  if (tn > 0) {
    uint32_t v = (uint32_t)tri[0] << 16;
    if (tn == 2) v |= tri[1] << 8;
    q[0] = B64[(v >> 18) & 63]; q[1] = B64[(v >> 12) & 63];
    q[2] = tn == 2 ? B64[(v >> 6) & 63] : '='; q[3] = '=';
    Serial.write((const uint8_t*)q, 4);
  }
  Serial.write('\n');
  Serial.println("<<<ENDFB>>>");
  Serial.flush();
}

}  // namespace

void run(uint8_t count, uint32_t& dashIndex, RenderIndexFn renderIndex) {
  Serial.printf("\n== console: d/D=dump n=next p=prev r=redraw s=sleep (idx=%u/%u) ==\n",
                (unsigned)(count ? dashIndex % count : 0), (unsigned)count);
  uint32_t idle = millis();
  uint32_t limit = CONSOLE_IDLE_MS;  // short until the host first responds
  while (millis() - idle < limit) {
#if ENABLE_WATCHDOG
    esp_task_wdt_reset();  // the dev console can idle far longer than the WDT
#endif
    if (Serial.available()) {
      char c = Serial.read();
      idle = millis();
      limit = CONSOLE_TIMEOUT_MS;  // host is here — stay responsive
      switch (c) {
        // A dump streams for a few seconds while we can't read; the host may have
        // queued extra 'd's to catch the console window. Drain them so they don't
        // each trigger another dump and bury the next command.
        case 'd': dump(3); while (Serial.available()) Serial.read(); break;
        case 'D': dump(2); while (Serial.available()) Serial.read(); break;
        case 'n':
          if (count) { dashIndex++; renderIndex(dashIndex % count); }
          Serial.printf("idx=%u\n", (unsigned)(count ? dashIndex % count : 0));
          break;
        case 'p':
          if (count) { dashIndex += count - 1; renderIndex(dashIndex % count); }
          Serial.printf("idx=%u\n", (unsigned)(count ? dashIndex % count : 0));
          break;
        case 'r':
          if (count) renderIndex(dashIndex % count);
          Serial.println("redrawn");
          break;
        case 's': Serial.println("sleeping"); return;
        default: break;
      }
    }
    delay(5);
  }
}

}  // namespace serial_console

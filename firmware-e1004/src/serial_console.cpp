#include "serial_console.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "battery_e1004.h"
#include "config.h"
#include "framebuffer.h"
#include "log.h"

namespace serial_console {

namespace {

const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Dump the framebuffer, downscaled by `scale`, as base64 palette indices
// between <<<FBC w h>>> / <<<ENDFBC>>> markers. Each output pixel is the
// DOMINANT palette index of its scale x scale box — averaging color indices
// would be meaningless.
void dump(int scale) {
  if (scale < 1) scale = 1;
  const uint8_t* buf = fb::buffer();
  if (!buf) { Serial.println("dump: no framebuffer"); return; }
  const int outW = SCREEN_W / scale;
  const int outH = SCREEN_H / scale;

  Serial.printf("<<<FBC %d %d>>>\n", outW, outH);
  uint8_t tri[3];
  int tn = 0, col = 0;
  char q[4];
  for (int oy = 0; oy < outH; oy++) {
    const int by = oy * scale;
    for (int ox = 0; ox < outW; ox++) {
      const int bx = ox * scale;
      uint8_t counts[COL_COUNT] = {0};
      for (int dy = 0; dy < scale; dy++) {
        const uint8_t* row = buf + (size_t)(by + dy) * SCREEN_W + bx;
        for (int dx = 0; dx < scale; dx++) {
          uint8_t c = row[dx];
          if (c < COL_COUNT) counts[c]++;
        }
      }
      // Chromatic ink wins over white/black even when sparse — dithered tints
      // would otherwise vanish under a dominant-color reduction. Among colors,
      // most-common wins; else black if present; else white.
      uint8_t best = COL_WHITE;
      uint8_t bestN = 0;
      for (uint8_t c = COL_RED; c < COL_COUNT; c++) {
        if (counts[c] > bestN) { best = c; bestN = counts[c]; }
      }
      if (bestN == 0) best = counts[COL_BLACK] ? COL_BLACK : COL_WHITE;
      tri[tn++] = best;
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
  Serial.println("<<<ENDFBC>>>");
  Serial.flush();
}

}  // namespace

void run(uint8_t count, uint32_t& dashIndex, RenderIndexFn renderIndex) {
  Serial.printf("\n== console: d/D=dump n=next p=prev r=redraw b=battery s=sleep (idx=%u/%u) ==\n",
                (unsigned)(count ? dashIndex % count : 0), (unsigned)count);
  uint32_t idle = millis();
  uint32_t limit = CONSOLE_IDLE_MS;  // short until the host first responds
  uint32_t lastBtnAdv = 0;           // debounce: one advance per press window
  while (millis() - idle < limit) {
#if ENABLE_WATCHDOG
    esp_task_wdt_reset();  // the dev console can idle far longer than the WDT
#endif
    // While plugged in we sit here instead of deep-sleeping, so the ext0 button
    // wake never fires — poll the green button so a press still advances.
    if (BTN_GREEN_GPIO >= 0 && digitalRead(BTN_GREEN_GPIO) == LOW && count &&
        (lastBtnAdv == 0 || millis() - lastBtnAdv >= 700)) {
      dashIndex++;
      renderIndex(dashIndex % count);
      Serial.printf("button -> idx=%u\n", (unsigned)(dashIndex % count));
      lastBtnAdv = millis();
      idle = millis();
      limit = CONSOLE_TIMEOUT_MS;
    }
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
        case 'b': {
          battery::Status s = battery::read();
          Serial.printf("battery: mv=%u soc=%d charging=%d\n",
                        (unsigned)s.mv, s.soc == 0xFF ? -1 : (int)s.soc,
                        (int)s.charging);
          break;
        }
        default: break;
      }
    }
    delay(5);
  }
}

}  // namespace serial_console

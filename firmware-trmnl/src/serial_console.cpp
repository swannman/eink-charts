#include "serial_console.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "dashboard_renderer.h"
#include "display_trmnl.h"  // extern FASTEPD epd
#include "log.h"
#include "touch_iqs323.h"

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
  Serial.printf("\n== console: d/D=dump n=next p=prev r=redraw T=touch-reset s=sleep (idx=%u/%u) ==\n",
                (unsigned)(count ? dashIndex % count : 0), (unsigned)count);
  uint32_t idle = millis();
  uint32_t limit = CONSOLE_IDLE_MS;  // short until the host first responds
  uint32_t lastTouchAdv = 0;         // debounce: one advance per press window
#if ENABLE_TOUCH && TOUCH_DEBUG
  int lastRdy = digitalRead(TOUCH_RDY_GPIO);
  uint32_t lastBeat = 0;
  uint32_t lastRecover = 0;  // rate-limit reset/garbage reconfigure attempts
  Serial.printf("== TOUCH DEBUG: streaming RDY edges + status @%lums; device will NOT sleep ==\n",
                (unsigned long)TOUCH_DEBUG_PERIOD_MS);
  Serial.printf("[t=%lu] RDY start = %s\n", (unsigned long)millis(),
                lastRdy ? "HIGH (idle)" : "LOW (asserted)");
#endif
  while (millis() - idle < limit) {
#if ENABLE_WATCHDOG
    esp_task_wdt_reset();  // the dev console can idle far longer than the WDT
#endif
#if ENABLE_TOUCH
#if TOUCH_DEBUG
    // Verbose streaming: keep the session alive forever and print two views of
    // the touch state so we can see exactly where a tap does (or doesn't) land.
    idle = millis();  // never time out into deep sleep while debugging
    limit = CONSOLE_TIMEOUT_MS;
    {
      uint32_t nowMs = millis();
      int rdy = digitalRead(TOUCH_RDY_GPIO);
      // (1) Log every RDY transition the instant it happens — this is the raw
      //     interrupt path. A tap SHOULD drive RDY low in event mode.
      if (rdy != lastRdy) {
        Serial.printf("[t=%lu] RDY -> %s\n", (unsigned long)nowMs,
                      rdy ? "HIGH" : "LOW (asserted!)");
        lastRdy = rdy;
        if (rdy == LOW) {  // read immediately to catch short taps via the interrupt
          touch::Event ev = touch::readEvent();
          Serial.printf("[t=%lu]   edge read ok=%d raw=0x%04X touch=%d rst=%d\n",
                        (unsigned long)nowMs, (int)ev.ok, ev.raw,
                        (int)ev.touched, (int)ev.reset);
        }
      }
      // (2) Force-read SYSTEM_STATUS at a fixed cadence regardless of RDY:
      //     readEvent() opens its own comms window, so the channel/touch bits
      //     show up here even if the interrupt never fires — this isolates a dead
      //     interrupt path from a sensor that just doesn't feel the finger.
      if (nowMs - lastBeat >= TOUCH_DEBUG_PERIOD_MS) {
        lastBeat = nowMs;
        // Read the full report: counts move as a finger approaches even if the
        // touch bit never trips, and the delta from LTA is the clearest signal.
        touch::Report rp = touch::readReport();
        // readReport() returns ok=false on an all-0xEE (chip-not-ready) word.
        bool garbage = !rp.ok;
        bool resetFlag = !garbage && (rp.status & 0x0080);
        bool touched = !garbage && !resetFlag && (rp.status & 0x2A00);
        Serial.printf("[t=%lu] poll RDY=%d ok=%d st=0x%04X touch=%d rst=%d sld=%u "
                      "CH0=%u(d%+d) CH1=%u(d%+d) CH2=%u(d%+d)\n",
                      (unsigned long)nowMs, rdy, (int)rp.ok, rp.status,
                      (int)touched, (int)resetFlag, (unsigned)rp.slider,
                      (unsigned)rp.ch[0], (int)rp.ch[0] - (int)rp.lta[0],
                      (unsigned)rp.ch[1], (int)rp.ch[1] - (int)rp.lta[1],
                      (unsigned)rp.ch[2], (int)rp.ch[2] - (int)rp.lta[2]);
        if (garbage || resetFlag) {
          // The full-screen EPD refresh that an advance triggers disrupts the
          // IQS323 on the shared rail: it comes back flagging SHOW_RESET (or
          // wedged returning 0xEE), which must NOT be read as a touch (that's the
          // runaway-advance bug). Re-stream config to bring it back into event
          // mode, rate-limited so a persistent fault doesn't spin.
          if (lastRecover == 0 || nowMs - lastRecover >= 3000) {
            Serial.printf("[t=%lu] touch: reset/garbage (st=0x%04X) -> reconfigure\n",
                          (unsigned long)nowMs, rp.status);
            touch::configure();
            lastRecover = millis();
            lastRdy = digitalRead(TOUCH_RDY_GPIO);
          }
        } else if (touched && count &&
                   (lastTouchAdv == 0 || nowMs - lastTouchAdv >= 700)) {
          dashIndex++;
          renderIndex(dashIndex % count);
          // The refresh above likely just reset the chip; recover it right away
          // so the next real tap is seen instead of a garbage-driven loop.
          touch::configure();
          lastRdy = digitalRead(TOUCH_RDY_GPIO);
          Serial.printf("[t=%lu] >>> ADVANCE -> idx=%u (touch reconfigured)\n",
                        (unsigned long)nowMs, (unsigned)(dashIndex % count));
          lastTouchAdv = millis();
          lastBeat = millis();
        }
      }
    }
#else
    // While plugged in we sit here instead of deep-sleeping, so the ext0 touch
    // wake never fires — poll the bar directly so a tap still advances. RDY is
    // active-LOW and only pulses on a touch event in event mode, so gate the
    // (windowed) I2C read on it to stay non-blocking when nothing is touched.
    if (digitalRead(TOUCH_RDY_GPIO) == LOW) {
      touch::Event ev = touch::readEvent();
      if (ev.ok && ev.reset) {
        // The chip is flagging a power-on reset (SHOW_RESET): its channel bits
        // are garbage (they read as always-touched) until reconfigured. Don't
        // reconfigure from here — poking the IQS323 mid-console desyncs its comms
        // windows and the config won't land; recovery happens cleanly at the next
        // boot (see main.cpp). Just ignore it so we don't false-advance in a loop.
      } else if (ev.ok && ev.touched && count &&
                 (lastTouchAdv == 0 || millis() - lastTouchAdv >= 700)) {
        // Debounce (not edge-detect): a held finger keeps reporting touched and
        // the chip may not emit a release event, so advance at most once per
        // window rather than relying on seeing not-touched to re-arm.
        dashIndex++;
        renderIndex(dashIndex % count);
        Serial.printf("touch -> idx=%u\n", (unsigned)(dashIndex % count));
        lastTouchAdv = millis();
        idle = millis();                 // treat a tap as activity
        limit = CONSOLE_TIMEOUT_MS;
      }
    }
#endif  // TOUCH_DEBUG
#endif  // ENABLE_TOUCH
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
#if ENABLE_TOUCH
        case 'T': {
          // Recover a wedged touch chip: configure() now self-resets (SW reset ->
          // wait for clean state -> stream config -> ATI -> event mode).
          bool cfg = touch::configure();
          touch::Event ev = touch::readEvent();
          Serial.printf("touch: configure=%d status=0x%04X touched=%d reset=%d\n",
                        (int)cfg, ev.raw, (int)ev.touched, (int)ev.reset);
          break;
        }
#endif
        default: break;
      }
    }
    delay(5);
  }
}

}  // namespace serial_console

#include "battery_e1004.h"

#include <Arduino.h>

#include "config.h"

namespace battery {

void begin() {
  if (BAT_ADC_GPIO >= 0) {
    pinMode(BAT_ADC_GPIO, INPUT);
    analogReadResolution(12);
  }
  if (BAT_EN_GPIO >= 0) {
    // The divider is switched: GPIO21 HIGH connects the pack to the ADC (so it
    // doesn't drain the battery between reads).
    pinMode(BAT_EN_GPIO, OUTPUT);
    digitalWrite(BAT_EN_GPIO, HIGH);
  }
}

// Piecewise-linear single-cell LiPo voltage -> % (resting curve). Coarse by
// design — there's no coulomb counter, and the divider + load add error.
static uint8_t socFromMv(uint16_t mv) {
  struct P { uint16_t mv; uint8_t pct; };
  static const P curve[] = {
      {3300, 0}, {3500, 5}, {3600, 10}, {3700, 25}, {3750, 40},
      {3800, 55}, {3900, 70}, {4000, 85}, {4100, 95}, {4200, 100},
  };
  if (mv <= curve[0].mv) return 0;
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[n - 1].mv) return 100;
  for (int i = 1; i < n; i++) {
    if (mv < curve[i].mv) {
      const P& a = curve[i - 1];
      const P& b = curve[i];
      return a.pct + (uint32_t)(mv - a.mv) * (b.pct - a.pct) / (b.mv - a.mv);
    }
  }
  return 100;
}

Status read() {
  Status s;
  if (BAT_ADC_GPIO < 0) return s;
  if (BAT_EN_GPIO >= 0) {
    digitalWrite(BAT_EN_GPIO, HIGH);
    delay(10);  // let the divider settle after enabling
  }
  // Average several millivolt readings (analogReadMilliVolts applies the
  // factory ADC calibration, far better than raw counts).
  uint32_t sum = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) sum += analogReadMilliVolts(BAT_ADC_GPIO);
  uint32_t pin_mv = sum / N;
  if (BAT_EN_GPIO >= 0) digitalWrite(BAT_EN_GPIO, LOW);  // don't drain between reads
  uint32_t pack_mv = (uint32_t)(pin_mv * BAT_DIVIDER);
  if (pack_mv < 2500 || pack_mv > 4600) return s;  // divider open / not wired
  s.mv = (uint16_t)pack_mv;
  s.soc = socFromMv(s.mv);
  // Above the practical resting ceiling → almost certainly on USB power.
  s.charging = pack_mv > 4250;
  return s;
}

}  // namespace battery

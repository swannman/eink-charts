#include "battery_bq27427.h"

#include <Arduino.h>
#include <limits.h>

#include "BQ27427.h"          // vendored MIT lib — provides the global `lipo`
#include "BQ27427_Definitions.h"
#include "config.h"
#include "display_trmnl.h"    // epd (FastEPD io expander) for detect + charging
#include "log.h"

namespace battery {

// Last detected cell count, kept for read() (begin() updates it).
static uint8_t sCells = CELLS_UNKNOWN;
// True once the gauge is configured and out of INITIALIZATION (SOC trustworthy).
static bool sReady = false;

// ---- Board-specific bits the fuel-gauge library doesn't cover ----------------

// One RC-discharge measurement on the cell-sense pin (clean-room from the
// hardware behaviour: a resistor/cap network on TCA9535 P0_7 discharges at a
// rate set by how many cells load it). Drive it HIGH to charge the cap, release
// to an input, and time how long it takes to fall back LOW. A longer discharge
// means one cell; a shorter one means two; never falling means no battery.
static uint8_t measureCellsOnce() {
  epd.ioPinMode(BAT_DET_IO_PIN, OUTPUT);
  epd.ioWrite(BAT_DET_IO_PIN, HIGH);
  delay(BAT_DET_CHARGE_MS);
  epd.ioPinMode(BAT_DET_IO_PIN, INPUT);  // release; start timing the discharge

  uint32_t start = micros();
  uint32_t now = start;
  bool timeout = false;
  while (true) {
    bool stillHigh = (epd.ioRead(BAT_DET_IO_PIN) != 0);
    now = micros();
    if (!stillHigh) break;  // discharged
    if (now - start >= BAT_DET_TIMEOUT_US) {
      timeout = true;
      break;
    }
  }
  if (timeout) return CELLS_NONE;
  return (now - start > BAT_DET_THRESH_US) ? CELLS_ONE : CELLS_TWO;
}

// The single-shot reading jitters near the threshold, so take it best-of-20:
// return as soon as two consecutive measurements agree.
static uint8_t detectCells() {
  uint8_t last = 0xEE;
  for (int i = 0; i < 20; i++) {
    uint8_t r = measureCellsOnce();
    if (r == last) return r;
    last = r;
    delay(10);
  }
  return last;  // never settled — best effort
}

// ---- Public API --------------------------------------------------------------

bool present() { return lipo.deviceType() == BQ27427_DEVICE_ID; }

uint16_t voltageMv() {
  uint16_t mv = lipo.voltage();
  if (mv < 2000 || mv > 13000) return 0;  // spans 1- and 2-cell packs
  return mv;
}

void begin(uint8_t* cachedCells) {
  sReady = false;
  sCells = cachedCells ? *cachedCells : CELLS_UNKNOWN;

  if (!present()) {
    Log.println("battery: BQ27427 not present");
    sCells = CELLS_NONE;
    if (cachedCells) *cachedCells = CELLS_NONE;
    return;
  }

  uint16_t fl = lipo.flags();
  bool itpor = (fl & BQ27427_FLAG_ITPOR) != 0;

  // Detect the cell count when we don't know it yet, or when the gauge reset
  // (its data flash — and thus our profile — may be gone). Otherwise trust the
  // cached value so a normal wake skips the ~hundreds-of-ms detection.
  if (itpor || sCells == CELLS_UNKNOWN || sCells == CELLS_NONE) {
    sCells = detectCells();
    if (cachedCells) *cachedCells = sCells;
  }

  if (sCells == CELLS_NONE) {
    Log.println("battery: no cells detected");
    return;
  }

  if (itpor) {
    // Gauge reset → reload the design capacity + chemistry (golden file) so the
    // Impedance-Track algorithm produces a real SOC, then wait for it to leave
    // INITIALIZATION. Under load (Wi-Fi/display up) it may not clear until the
    // pack rests; if so, SOC is stale this wake and settles on a later one.
    Log.printf("battery: ITPOR set — loading %d-cell golden file\n", (int)sCells);
    bool cfg = (sCells == CELLS_TWO) ? lipo.configureTwoCell() : lipo.configureOneCell();
    Log.printf("battery: golden file %s\n", cfg ? "OK" : "FAILED");
    uint32_t t0 = millis();
    while ((lipo.flags() & BQ27427_FLAG_ITPOR) && (millis() - t0 < BAT_ITPOR_WAIT_MS))
      delay(100);
  }

  sReady = (lipo.flags() & BQ27427_FLAG_ITPOR) == 0;
  Log.printf("battery: cells=%d ready=%d v=%umV soc=%d%%\n", (int)sCells, (int)sReady,
             (unsigned)voltageMv(), sReady ? (int)lipo.soc() : -1);
}

Status read() {
  Status s = {};
  s.cells = sCells;
  s.tempCx10 = INT16_MIN;
  s.soc = 0xFF;
  s.present = present();
  if (!s.present) return s;

  s.flags = lipo.flags();
  s.ready = sReady && ((s.flags & BQ27427_FLAG_ITPOR) == 0);
  s.mv = voltageMv();
  s.charging = (epd.ioRead(BQ25616_STAT_IO) == 0);  // needs display/TCA9535 up

  if (s.ready) {
    uint16_t soc = lipo.soc();
    if (soc <= 100) s.soc = (uint8_t)soc;
  }
  uint16_t k10 = lipo.temperature(BATTERY);  // 0.1 K
  if (k10 > 0) s.tempCx10 = (int16_t)((int)k10 - 2732);
  return s;
}

}  // namespace battery

#pragma once
// TI BQ27427 fuel-gauge front-end for the TRMNL X.
//
// The heavy lifting lives in the vendored MIT BQ27427 library (lib/BQ27427),
// which owns the I2C command set, the Impedance-Track state, and the "golden
// file" data-flash profiles. This module is a thin facade over that library's
// global `lipo` object plus the two board-specific bits the library doesn't
// cover: detecting how many cells are wired (an RC-discharge timing trick on a
// TCA9535 pin) and reading the charger's STAT line. Both of those go through
// FastEPD's io expander, so begin() must run AFTER display_trmnl::begin().
//
// Why this matters: the BQ27427 only reports a trustworthy state-of-charge once
// it has been told the battery's design capacity + chemistry. It flags ITPOR
// after any reset; on that flag we load the matching golden file (1- or 2-cell)
// and wait for the IT algorithm to settle (OCV at rest). Until then only the
// raw voltage is meaningful. This mirrors the official firmware's behaviour.
#include <stdint.h>

namespace battery {

enum Cells : uint8_t {
  CELLS_NONE = 0,     // detect pin stayed HIGH — no battery
  CELLS_ONE = 1,      // one cell
  CELLS_TWO = 2,      // two cells
  CELLS_UNKNOWN = 0xFF  // not yet measured (force a detection)
};

// A full snapshot for telemetry / on-screen / serial. soc is 0xFF and tempCx10
// is INT16_MIN when unavailable; soc is only trustworthy when `ready`.
struct Status {
  bool present;     // gauge ACKs (deviceType == 0x0427)
  bool ready;       // configured + ITPOR clear → SOC is trustworthy
  uint8_t cells;    // Cells enum
  uint16_t mv;      // battery voltage (mV), 0 if unavailable
  uint8_t soc;      // state of charge %, 0..100, 0xFF if unavailable
  bool charging;    // charger STAT asserted (LOW)
  int16_t tempCx10; // battery temperature in 0.1 °C, INT16_MIN if unavailable
  uint16_t flags;   // raw flags() word
};

// Bring the gauge up: verify it's present, detect the cell count (only when
// `*cachedCells` is UNKNOWN or the gauge reset), reload the golden-file profile
// if ITPOR is set, and poll for the IT algorithm to leave INITIALIZATION.
// `*cachedCells` is read as the last-known count (persist it in RTC across
// wakes) and written back with the current one. Cheap on a normal wake — it
// only does the expensive detect+configure when the gauge actually reset.
// Requires the display (TCA9535) already initialised.
void begin(uint8_t* cachedCells);

// Snapshot everything at once. Safe to call after begin(); voltage works even
// without begin() as long as Wire is up.
Status read();

// True if the gauge ACKs on the bus (deviceType == 0x0427). No begin() needed.
bool present();

// Battery voltage in millivolts (0 if unavailable). No begin() needed.
uint16_t voltageMv();

}  // namespace battery

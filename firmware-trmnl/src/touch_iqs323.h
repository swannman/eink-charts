#pragma once
// Azoteq IQS323 touch-bar driver for "tap to advance". Clean-room logic written
// from the IQS323 datasheet register map (register addresses/bit meanings are
// facts); the board-specific tuning values live in iqs323_config.h.
//
// How it works:
//   - configure() streams the config block, runs ATI (auto-tune), then puts the
//     chip in EVENT MODE with touch events enabled. In event mode the RDY line
//     (GPIO3, active-LOW) asserts ONLY when a finger touches the bar — not every
//     measurement cycle — so the S3 can deep-sleep with ext0 on RDY and wake
//     only on a real tap. (Stock power-on config streams RDY every cycle, which
//     is why touch-wake had to be disabled until this was implemented.)
//   - readEvent() runs on each wake: it opens a comms window, reads SYSTEM_STATUS
//     (which channels are touched + whether the chip reset), and closes the
//     window with a STOP (releasing RDY so the next tap can re-trigger).
//
// The IQS323 stays powered across the S3's deep-sleep cycles, so configure()
// only needs to run once (first boot) — or again if the chip reports a reset.
#include <stdint.h>

namespace touch {

// Result of servicing the touch chip on a wake.
struct Event {
  bool ok;       // I2C read succeeded
  bool touched;  // at least one slider channel is being touched
  bool reset;    // SHOW_RESET flag set — chip lost its config, reconfigure
  uint16_t raw;  // raw SYSTEM_STATUS word (for logging)
};

// Probe the IQS323 on the shared sensor bus (Wire must be begun already).
bool present();

// Stream the config, run ATI, and enable event mode with touch events. Returns
// true if ATI converged and event mode was set. Runs while the device is awake
// (first boot / after a chip reset), so it may block ~a few hundred ms.
bool configure();

// Open a comms window, read SYSTEM_STATUS, and close it (STOP releases RDY).
// Call once per wake to decide whether to advance and to clear the RDY line.
Event readEvent();

// Software-reset the chip (reload defaults, come back streaming). Recovery for a
// chip wedged in a bad state when its power can't be cycled. Follow with
// configure() to re-stream config and return to event mode.
bool swReset();

// Fast-path re-arm check: true if the chip is already sitting in clean event mode
// with our config intact (0x62 == streamed threshold, no SHOW_RESET, RDY idling HIGH),
// so the caller can arm ext0 directly without a costly hwReset + reconfigure + ATI.
// A display refresh leaves the chip this way, so this is the normal path.
bool eventModeReady();

// Hardware-reset the chip by pulsing its master-clear line (tied to RDY/GPIO3).
// Needs no working I2C, so it recovers a chip whose comms have wedged (returning
// 0xEE / RDY stuck low) — the case swReset() can't reach. Chip comes back streaming
// with SHOW_RESET set; follow with configure().
void hwReset();

// True if the RDY line is idling HIGH (event mode armed, no tap pending). Pure
// GPIO read — no I2C — so it reflects the chip's resting state. Used before sleep
// to confirm the chip won't immediately assert ext0 (which would spin-wake us).
bool rdyIdleHigh();

// Debug: read the full 18-byte report (0x10..0x18) and parse out the live
// per-channel raw counts + slider coordinate. Unlike readEvent()'s touch bit,
// the counts move as a finger merely APPROACHES the bar (sub-threshold), so this
// tells us whether the sensor feels the finger at all — independent of ATI
// thresholds and the RDY interrupt. Returns false on I2C failure.
struct Report {
  bool ok;
  uint16_t status;     // 0x10 SYSTEM_STATUS word
  uint16_t slider;     // 0x12 resolved slider coordinate
  uint16_t ch[3];      // CH0/1/2 filtered counts (0x13/0x15/0x17)
  uint16_t lta[3];     // CH0/1/2 long-term averages (baseline)
};
Report readReport();

// Passive variant of readReport(): reads WITHOUT forcing a comms window, so it's
// safe to call at a high rate. Only meaningful when RDY is already LOW (the chip
// has opened its own window). Forcing a window every tick wedges the chip after
// ~30 reads (returns 0xEE), so the interactive tap-poll loop must use this.
Report readReportPassive();

}  // namespace touch

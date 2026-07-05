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

}  // namespace touch

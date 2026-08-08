#pragma once
// Battery status for the E1004: an ADC divider on the 5000 mAh pack (no fuel
// gauge on this board — SOC is estimated from voltage, so it's coarse).
#include <stdint.h>

namespace battery {

struct Status {
  uint16_t mv = 0;        // pack voltage, millivolts (0 = read failed)
  uint8_t soc = 0xFF;     // rough % from the LiPo discharge curve; 0xFF unknown
  bool charging = false;  // true when USB power is present
};

// Configure the ADC pin. Safe to call every wake.
void begin();

// Read a fresh, averaged sample.
Status read();

}  // namespace battery

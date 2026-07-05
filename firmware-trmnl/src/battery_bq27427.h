#pragma once
// Minimal TI BQ27427 fuel-gauge reader (I2C @0x55 on the shared sensor bus).
// Clean-room from the BQ27427 command register map — NOT derived from the GPL
// vendor driver. Assumes Wire has already been begun on SENSOR_SDA/SCL.
#include <stdint.h>

namespace battery {

// True if the gauge ACKs on the bus.
bool present();

// Battery voltage in millivolts (0 if unavailable).
uint16_t voltageMv();

// State of charge %, 0..100 (0xFF if unavailable).
uint8_t soc();

}  // namespace battery

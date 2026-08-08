#pragma once
// Minimal SHT4x reader for the E1004's onboard temp/humidity sensor
// (I2C addr 0x44, SDA 19 / SCL 20). Its one job here: give the EPD controller
// the real ambient temperature so it picks the right (faster, at room temp)
// Spectra 6 waveform instead of the library's conservative 16 C default.
#include <stdint.h>

namespace sht4x {

// Bring up the sensor's I2C bus and take one high-precision measurement.
// Returns true and fills tempC/rhPct on success.
bool read(float* tempC, float* rhPct);

}  // namespace sht4x

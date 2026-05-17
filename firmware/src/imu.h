#pragma once
#include <stdint.h>

// Probe for QMI8658 on the shared I2C bus. Returns true if WHO_AM_I matches.
// Also configures the accelerometer (±8g) and enables it so subsequent
// imuReadAccel() calls return sensible data.
bool imuInit();

// Raw 16-bit signed accel readings. Returns false if the chip wasn't found.
// One LSB ≈ 1g / 4096 at ±8g full-scale.
bool imuReadAccel(int16_t& ax, int16_t& ay, int16_t& az);

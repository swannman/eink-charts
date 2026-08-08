#include "sht4x_e1004.h"

#include <Arduino.h>
#include <Wire.h>

namespace sht4x {

constexpr uint8_t ADDR = 0x44;
constexpr int8_t SDA_PIN = 19;
constexpr int8_t SCL_PIN = 20;
constexpr uint8_t CMD_MEASURE_HIGH = 0xFD;

static uint8_t crc8(const uint8_t* d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

bool read(float* tempC, float* rhPct) {
  // The sensor has its own bus (separate from the EPD SPI); begin() is
  // idempotent for a fixed pin pair.
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.beginTransmission(ADDR);
  Wire.write(CMD_MEASURE_HIGH);
  if (Wire.endTransmission() != 0) return false;
  delay(10);  // high-precision conversion time (~8.3 ms max)
  if (Wire.requestFrom((int)ADDR, 6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) return false;
  uint16_t rt = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rh = ((uint16_t)d[3] << 8) | d[4];
  if (tempC) *tempC = -45.0f + 175.0f * rt / 65535.0f;
  if (rhPct) {
    float h = -6.0f + 125.0f * rh / 65535.0f;
    *rhPct = h < 0 ? 0 : (h > 100 ? 100 : h);
  }
  return true;
}

}  // namespace sht4x

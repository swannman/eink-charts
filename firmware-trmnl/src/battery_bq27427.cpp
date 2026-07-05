#include "battery_bq27427.h"

#include <Wire.h>

#include "config.h"

namespace battery {

static uint16_t readReg(uint8_t reg) {
  Wire.beginTransmission(BQ27427_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFFFF;
  if (Wire.requestFrom((uint8_t)BQ27427_ADDR, (uint8_t)2) != 2) return 0xFFFF;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  return (uint16_t)(lo | ((uint16_t)hi << 8));
}

bool present() {
  Wire.beginTransmission(BQ27427_ADDR);
  return Wire.endTransmission() == 0;
}

uint16_t voltageMv() {
  uint16_t mv = readReg(BQ_REG_VOLTAGE);
  if (mv == 0xFFFF || mv < 2000 || mv > 5000) return 0;
  return mv;
}

uint8_t soc() {
  uint16_t v = readReg(BQ_REG_SOC);
  if (v == 0xFFFF || v > 100) return 0xFF;
  return (uint8_t)v;
}

}  // namespace battery

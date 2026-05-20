#include "imu.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace {

uint8_t g_addr = 0;

bool readReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool imuInit() {
  Wire.begin(BQ_SDA_GPIO, BQ_SCL_GPIO);
  Wire.setClock(400000);
  for (uint8_t a : {QMI_ADDR_PRIMARY, QMI_ADDR_SECONDARY}) {
    uint8_t who = 0;
    if (readReg(a, QMI_REG_WHO_AM_I, &who, 1) && who == 0x05) {
      g_addr = a;
      Serial.printf("imu: QMI8658 found at 0x%02x (WHO_AM_I=0x%02x)\n", a, who);
      // CTRL1 = 0x40: ADDR_AI=1 (auto-increment on multi-byte reads). Without
      // this, a 6-byte burst from AX_L just returns AX_L six times.
      writeReg(a, 0x02, 0x40);
      // CTRL2: aFS[6:4]=010 = ±8g, aODR[3:0]=0011 = 1000 Hz → 0x23.
      writeReg(a, QMI_REG_CTRL2, 0x23);
      // CTRL7 bit 0 = aEN — enable accelerometer.
      writeReg(a, QMI_REG_CTRL7, 0x01);
      // First sample needs ~1ms to land in the output regs.
      delay(5);
      return true;
    }
    Serial.printf("imu: no QMI8658 at 0x%02x (read=0x%02x)\n", a, who);
  }
  return false;
}

bool imuReadAccel(int16_t& ax, int16_t& ay, int16_t& az) {
  if (g_addr == 0) return false;
  uint8_t b[6];
  if (!readReg(g_addr, QMI_REG_AX_L, b, 6)) return false;
  ax = (int16_t)(b[0] | (b[1] << 8));
  ay = (int16_t)(b[2] | (b[3] << 8));
  az = (int16_t)(b[4] | (b[5] << 8));
  return true;
}

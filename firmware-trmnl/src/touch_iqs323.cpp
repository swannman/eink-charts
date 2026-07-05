#include "touch_iqs323.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "iqs323_config.h"
#include "log.h"

namespace touch {

namespace {

// IQS323 memory-map registers (from lib/IQS323/inc/IQS323_addresses.h).
constexpr uint8_t MM_SYSTEM_STATUS = 0x10;  // [b0]=system flags, [b1]=channel flags
constexpr uint8_t MM_SYSTEM_CONTROL = 0xC0;  // [b0]=control bits
constexpr uint8_t MM_EVENT_ENABLE = 0xD3;    // [b0]=event mask, [b1]=activation threshold
constexpr uint8_t MM_CH0_TOUCH = 0x62;       // read-back check: config's touch threshold

// SYSTEM_STATUS byte0 flags.
constexpr uint8_t ST0_ATI_ACTIVE = 0x20;  // bit5
constexpr uint8_t ST0_ATI_ERROR = 0x40;   // bit6
constexpr uint8_t ST0_SHOW_RESET = 0x80;  // bit7
// SYSTEM_STATUS byte1: per-channel touch bits (CH0=1, CH1=3, CH2=5).
constexpr uint8_t ST1_TOUCH_MASK = 0x2A;

// SYSTEM_CONTROL byte0 control bits.
constexpr uint8_t CTRL_ACK_RESET = 0x01;   // bit0
constexpr uint8_t CTRL_RE_ATI = 0x04;      // bit2
constexpr uint8_t CTRL_RESEED = 0x08;      // bit3
constexpr uint8_t CTRL_EVENT_MODE = 0x80;  // bit7

// EVENT_ENABLE byte0: touch events only (bit1). Activation threshold (byte1)
// preserved from the streamed config (0x18).
constexpr uint8_t EVT_TOUCH_ONLY = 0x02;
constexpr uint8_t EVT_ACTIVATION_THRESHOLD = 0x18;

// The RDY line is level, active-LOW, and held until a comms window closes on a
// STOP. We never force-comms: during config the chip streams (RDY pulses low
// every cycle) and on a touch wake the event has already pulled RDY low and the
// widened I2C window (see iqs323_config.h) holds it there through boot — so we
// just wait for the natural window to be open.
bool waitReadyLow(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (digitalRead(TOUCH_RDY_GPIO) != LOW) {
    if (millis() - start > timeout_ms) return false;
    delay(1);
  }
  return true;
}

bool openWindow() { return waitReadyLow(300); }

// Read `len` bytes from `reg`. Uses a repeated-START between the address write
// and the read so the window stays open, then a STOP that releases RDY.
bool readReg(uint8_t reg, uint8_t* buf, uint8_t len) {
  if (!openWindow()) return false;
  Wire.beginTransmission(IQS323_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  uint8_t got = Wire.requestFrom((int)IQS323_ADDR, (int)len, (int)true);  // STOP
  for (uint8_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
  return got == len;
}

// Write `len` bytes starting at `reg`; STOP closes the window.
bool writeReg(uint8_t reg, const uint8_t* buf, uint8_t len) {
  if (!openWindow()) return false;
  Wire.beginTransmission(IQS323_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
  return Wire.endTransmission(true) == 0;
}

// Read-modify-write the SYSTEM_CONTROL low byte (set `setBits`).
bool ctrlSetBits(uint8_t setBits) {
  uint8_t c[2] = {0, 0};
  if (!readReg(MM_SYSTEM_CONTROL, c, 2)) return false;
  c[0] |= setBits;
  return writeReg(MM_SYSTEM_CONTROL, c, 2);
}

}  // namespace

bool present() {
  Wire.beginTransmission(IQS323_ADDR);
  return Wire.endTransmission() == 0;
}

// Poll SYSTEM_STATUS until ATI settles. Returns true on convergence.
bool waitAti(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    uint8_t s[2] = {0, 0};
    if (readReg(MM_SYSTEM_STATUS, s, 2)) {
      if (s[0] & ST0_ATI_ERROR) return false;
      if (!(s[0] & ST0_ATI_ACTIVE)) return true;
    }
    delay(10);
  }
  return false;
}

bool configure() {
  pinMode(TOUCH_RDY_GPIO, INPUT);
  Wire.setTimeOut(300);  // tolerate the IQS323's clock stretching

  // The IQS323 sits on the always-on sensor rail, so it keeps its config across
  // the S3's resets (including a firmware reflash, which clears our RTC flag).
  // An un-configured chip STREAMS — RDY pulses low every measurement cycle; a
  // configured chip is in event mode and idles RDY high until a touch. So if we
  // don't see a streaming window, the chip is already set up: nothing to do
  // (and we couldn't open a comms window to re-stream anyway without a touch).
  if (!waitReadyLow(500)) {
    Log.println("touch: already configured (event mode, RDY idle)");
    return true;
  }

  // 1. Stream the full config block (electrode routing, thresholds, slider,
  //    report rates). Control byte is written as 0x10 here — still streaming.
  for (int i = 0; i < iqs323_cfg::STREAM_COUNT; i++) {
    const auto& c = iqs323_cfg::STREAM[i];
    if (!writeReg(c.reg, c.data, c.len)) {
      Log.printf("touch: config write @0x%02X failed\n", c.reg);
      return false;
    }
  }

  // Verify the block landed (CH0 touch threshold should read back 0x1B). A bad
  // read here means the windowed comms aren't working — bail early with a clear
  // signal rather than chasing a phantom ATI error.
  uint8_t chk[2] = {0, 0};
  if (!readReg(MM_CH0_TOUCH, chk, 2) || chk[0] != 0x1B) {
    Log.printf("touch: config readback bad (0x62=0x%02X, want 0x1B)\n", chk[0]);
    return false;
  }

  // 2. Acknowledge the power-on reset (clears SHOW_RESET so it stops re-flagging).
  if (!ctrlSetBits(CTRL_ACK_RESET)) {
    Log.println("touch: ACK reset failed");
    return false;
  }

  // 3. Trigger ATI and wait for it to converge; retry once with a reseed.
  bool atiOk = false;
  for (int attempt = 0; attempt < 2 && !atiOk; attempt++) {
    if (!ctrlSetBits(attempt == 0 ? CTRL_RE_ATI : (CTRL_RE_ATI | CTRL_RESEED))) {
      Log.println("touch: RE_ATI write failed");
      return false;
    }
    delay(20);  // let ATI_ACTIVE assert before we poll for it to clear
    atiOk = waitAti(3000);
    if (!atiOk) Log.printf("touch: ATI attempt %d failed\n", attempt);
  }
  if (!atiOk) return false;

  // 4. Enable touch events only (RDY will assert on a touch, not every cycle).
  uint8_t ev[2] = {EVT_TOUCH_ONLY, EVT_ACTIVATION_THRESHOLD};
  if (!writeReg(MM_EVENT_ENABLE, ev, 2)) {
    Log.println("touch: event-enable write failed");
    return false;
  }

  // 5. Switch to event mode (SYSTEM_CONTROL b0 |= EVENT_MODE). After this RDY
  //    only pulses on a real touch event.
  if (!ctrlSetBits(CTRL_EVENT_MODE)) {
    Log.println("touch: event-mode write failed");
    return false;
  }

  Log.println("touch: configured (event mode, touch events)");
  return true;
}

Event readEvent() {
  Wire.setTimeOut(300);  // tolerate the IQS323's clock stretching
  Event e = {false, false, false, 0};
  uint8_t s[2] = {0, 0};
  if (!readReg(MM_SYSTEM_STATUS, s, 2)) return e;  // STOP inside readReg releases RDY
  e.ok = true;
  e.raw = (uint16_t)s[0] | ((uint16_t)s[1] << 8);
  e.touched = (s[1] & ST1_TOUCH_MASK) != 0;
  e.reset = (s[0] & ST0_SHOW_RESET) != 0;
  return e;
}

}  // namespace touch

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
constexpr uint8_t CTRL_SW_RESET = 0x02;    // bit1: software reset (reload defaults)
constexpr uint8_t CTRL_RE_ATI = 0x04;      // bit2
constexpr uint8_t CTRL_RESEED = 0x08;      // bit3
constexpr uint8_t CTRL_EVENT_MODE = 0x80;  // bit7

// EVENT_ENABLE byte0: touch events only (bit1). Activation threshold (byte1)
// preserved from the streamed config (0x18).
constexpr uint8_t EVT_TOUCH_ONLY = 0x02;
constexpr uint8_t EVT_ACTIVATION_THRESHOLD = 0x18;

// The RDY line is level, active-LOW, and held until a comms window closes on a
// STOP. In event mode the chip idles RDY HIGH until a touch, so we can't just
// wait for a window — we FORCE one (the official Azoteq method): write 0xFF with
// a STOP, which prompts the IQS323 to open a comms window, then wait for RDY to
// drop. Without this, a chip idling in event mode (or wedged not asserting on
// touch) is completely unreachable — we can't read status, reconfigure, or reset
// it. If RDY is already low we're inside a window already and skip the poke.
void forceComm() {
  // Always request a fresh random-access window: after a reset the chip is
  // streaming (RDY pulses low), and a streaming window expects a READ — writing
  // config into it gets dropped. Poking 0xFF+STOP makes the chip open a clean
  // window addressed for our next transaction, regardless of current RDY state.
  Wire.beginTransmission(IQS323_ADDR);
  Wire.write(0xFF);              // request a communication window
  Wire.endTransmission(true);    // STOP prompts the chip to open it
  for (int i = 0; i < 100; i++) {
    if (digitalRead(TOUCH_RDY_GPIO) == LOW) break;
    delay(1);
  }
}

bool openWindow() {
  forceComm();
  return digitalRead(TOUCH_RDY_GPIO) == LOW;
}

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

// Passive read: read `len` bytes from `reg` WITHOUT forcing a comms window. Only
// valid when the chip has already opened a window on its own (RDY is LOW). The
// official firmware warns that forcing a window (our 0xFF poke) on every poll tick
// wedges the IQS323 after ~30 reads — it stops ACKing and returns 0xEE fill. So
// for high-rate polling (the interactive tap window) we must read passively.
bool readRegPassive(uint8_t reg, uint8_t* buf, uint8_t len) {
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

// Software-reset the chip (SYSTEM_CONTROL bit1). It reloads defaults and comes
// back up STREAMING — the clean state configure() expects. Use this to recover a
// chip wedged in a bad state (e.g. stuck SHOW_RESET, or not asserting on touch)
// when its power can't be cycled (it sits on the always-on sensor rail).
bool swReset() {
  Wire.setTimeOut(300);
  // openWindow() forces a comms window, so this reaches the chip even when it's
  // idle in event mode. Read-modify-write SYSTEM_CONTROL to set the SW_RESET bit.
  uint8_t c[2] = {0, 0};
  if (!readReg(MM_SYSTEM_CONTROL, c, 2)) return false;
  c[0] |= CTRL_SW_RESET;
  bool ok = writeReg(MM_SYSTEM_CONTROL, c, 2);
  delay(200);  // chip reboots into streaming mode before we reconfigure
  return ok;
}

// Fast-path health check for the sleep re-arm. A display refresh leaves the chip in
// clean event mode with config intact (SHOW_RESET clear, 0x62 still holds our streamed
// threshold), so the full hwReset + reconfigure + ATI we used to do on
// EVERY sleep was unnecessary — and its ATI frequently failed on the just-powered
// rail, which was the real source of the flakiness. Returns true only if the chip is
// genuinely ready to arm ext0: config intact (0x62 == the streamed CH0 threshold), no
// SHOW_RESET, and RDY idling HIGH continuously (event mode, not streaming). The RDY
// sweep spans more than one scan period so a streaming chip (RDY pulsing low ~every
// 40ms) can't be mistaken for armed — that would spin-wake ext0 and drain the battery.
bool eventModeReady() {
  Wire.setTimeOut(300);
  uint8_t s[2] = {0, 0}, th[2] = {0, 0};
  if (!readReg(MM_SYSTEM_STATUS, s, 2)) return false;
  if (s[0] & ST0_SHOW_RESET) return false;              // chip reset -> reconfigure
  if (!readReg(MM_CH0_TOUCH, th, 2)) return false;
  if (th[0] != iqs323_cfg::CH0[4]) return false;        // config wiped -> reconfigure
  for (int i = 0; i < 25; i++) {                        // ~75ms of continuous HIGH
    if (digitalRead(TOUCH_RDY_GPIO) == LOW) return false;  // streaming / event pending
    delay(3);
  }
  return true;
}

// Hardware-reset the IQS323 by pulsing its master-clear line. On this board MCLR is
// tied to the RDY pin (GPIO3), so we briefly drive it LOW as an output, then return
// it to a high-impedance input (the board pull-up restores RDY). Unlike swReset()
// this needs NO working I2C, so it recovers a chip whose comms have wedged (stuck
// returning 0xEE fill / RDY stuck low) — the case a software reset can't reach. The
// chip reboots into streaming mode flagging SHOW_RESET; follow with configure().
// Only ever drives the pin LOW (never HIGH), so it's electrically safe against the
// chip's open-drain RDY regardless.
void hwReset() {
  // MCLR pulse timings mirror the official TRMNL iqs323_task: 150us RDY->MCLR
  // switchover, 500us pulse (min 250ns), 150ms recovery + settle before I2C.
  delayMicroseconds(150);          // RDY -> MCLR switchover
  pinMode(TOUCH_RDY_GPIO, OUTPUT);
  digitalWrite(TOUCH_RDY_GPIO, LOW);
  delayMicroseconds(500);          // hold master-clear low
  pinMode(TOUCH_RDY_GPIO, INPUT);  // release — pull-up brings RDY back high
  delay(150);                      // reset recovery + settle before we touch I2C
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

  // Mirror the official IQS323 init: only (re)stream settings when the chip is
  // freshly reset (SHOW_RESET set) AND responsive. Streaming into a running/idle
  // chip drops writes. If it isn't already showing a reset, software-reset it and
  // wait for it to come back up flagging SHOW_RESET before we stream.
  uint8_t s[2] = {0, 0};
  bool freshReset = readReg(MM_SYSTEM_STATUS, s, 2) && (s[0] & ST0_SHOW_RESET);
  if (!freshReset) {
    Log.println("touch: not showing reset — issuing SW reset");
    swReset();  // sets SW_RESET bit, waits ~200ms for the chip to reboot
  }
  // Poll until the chip is responsive and flags SHOW_RESET (clean, ready state).
  uint32_t rstart = millis();
  while (!(readReg(MM_SYSTEM_STATUS, s, 2) && (s[0] & ST0_SHOW_RESET))) {
    if (millis() - rstart > 1000) {
      Log.println("touch: chip never flagged SHOW_RESET after reset — continuing");
      break;
    }
    delay(10);
  }

  // Give the chip a moment to settle after the reset before streaming settings
  // (writes during the initial power-on ATI can be dropped). We don't hard-wait
  // for ATI to clear — with default config it may not converge on this PCB — a
  // brief settle is enough now that every write forces a fresh comms window.
  delay(200);

  // 1. Stream the full config block (electrode routing, thresholds, slider,
  //    report rates), each block written in its own forced-comms window.
  for (int i = 0; i < iqs323_cfg::STREAM_COUNT; i++) {
    const auto& c = iqs323_cfg::STREAM[i];
    if (!writeReg(c.reg, c.data, c.len)) {
      Log.printf("touch: config write @0x%02X failed\n", c.reg);
      return false;
    }
  }

  // Sanity: CH0 touch threshold (0x62 byte0) should read back 0x1B. Non-fatal —
  // ATI convergence below is the real success signal — but log a mismatch.
  uint8_t rb[2] = {0};
  if (readReg(MM_CH0_TOUCH, rb, 2) && rb[0] != 0x1B) {
    Log.printf("touch: config readback 0x62=0x%02X (want 0x1B)\n", rb[0]);
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

  // 4. Event enable is left as streamed by the config block (0xD3 = 0x04, the
  //    official value that makes RDY assert on touch/slider events). We used to
  //    override it to 0x02 here, which enabled the wrong event bit and meant a
  //    touch never pulsed RDY — so we no longer touch it.

  // 5. READ_DATA: the official init reads the full 18-byte report (0x10-0x18)
  //    once before enabling event mode — this drains the pending measurement and
  //    lets the chip settle into normal operation. We skipped it before.
  uint8_t report[18] = {0};
  readReg(MM_SYSTEM_STATUS, report, 18);

  // 6. Switch to event mode (SYSTEM_CONTROL b0 |= EVENT_MODE), then verify the
  //    bit actually read back set (the official warns if it didn't).
  if (!ctrlSetBits(CTRL_EVENT_MODE)) {
    Log.println("touch: event-mode write failed");
    return false;
  }
  delay(10);
  uint8_t ctrl[2] = {0, 0};
  if (readReg(MM_SYSTEM_CONTROL, ctrl, 2) && !(ctrl[0] & CTRL_EVENT_MODE)) {
    Log.printf("touch: WARNING event-mode bit not set (ctrl0=0x%02X)\n", ctrl[0]);
  }

  // Decisive check: in event mode RDY must idle HIGH between taps. Wait one+
  // measurement cycle WITHOUT poking the chip (a forced-comms read would pull RDY
  // low itself and mask a streaming chip), then sample. If it's still LOW the chip
  // is streaming (event mode didn't hold) — the caller must not arm ext0 on it.
  delay(80);
  bool idleHigh = digitalRead(TOUCH_RDY_GPIO) == HIGH;
  Log.printf("touch: configured; post-config RDY idle=%s (event mode %s)\n",
             idleHigh ? "HIGH" : "LOW", idleHigh ? "armed" : "NOT holding");
  return true;
}

// True if RDY is idling HIGH right now (event mode armed, no tap pending). Sampled
// without any I2C so it reflects the chip's resting state, not a forced window.
bool rdyIdleHigh() { return digitalRead(TOUCH_RDY_GPIO) == HIGH; }

// Fill a Report from an 18-byte 0x10 memory-map read (shared by the forced and
// passive variants). Rejects the all-0xEE not-ready fill.
static Report parseReport(const uint8_t* b, bool readOk) {
  Report r = {};
  if (!readOk) return r;
  if (b[0] == 0xEE && b[1] == 0xEE) return r;  // chip not ready / wedged
  r.ok = true;
  auto u16 = [&](int i) -> uint16_t { return (uint16_t)b[i] | ((uint16_t)b[i + 1] << 8); };
  r.status = u16(0);
  r.slider = u16(4);
  r.ch[0] = u16(6);   r.lta[0] = u16(8);
  r.ch[1] = u16(10);  r.lta[1] = u16(12);
  r.ch[2] = u16(14);  r.lta[2] = u16(16);
  return r;
}

// Passive report read — NO forced comms window. Call only when RDY is already LOW
// (the chip opened its own window). Safe for high-rate polling; forcing would
// wedge the chip (see readRegPassive).
Report readReportPassive() {
  Wire.setTimeOut(300);
  uint8_t b[18] = {0};
  return parseReport(b, readRegPassive(MM_SYSTEM_STATUS, b, 18));
}

Report readReport() {
  Wire.setTimeOut(300);
  Report r = {};
  uint8_t b[18] = {0};
  // 0x10..0x18: STATUS[2] GESTURES[2] SLIDER[2] CH0 cnt/lta[4] CH1[4] CH2[4].
  if (!readReg(MM_SYSTEM_STATUS, b, 18)) return r;
  // The IQS323 returns 0xEE fill bytes when addressed while it isn't ready (e.g.
  // mid-reset). Those bytes decode into bogus "touched/reset" flags, so treat an
  // all-0xEE status word as a failed read rather than real data.
  if (b[0] == 0xEE && b[1] == 0xEE) return r;
  r.ok = true;
  auto u16 = [&](int i) -> uint16_t { return (uint16_t)b[i] | ((uint16_t)b[i + 1] << 8); };
  r.status = u16(0);
  r.slider = u16(4);
  r.ch[0] = u16(6);   r.lta[0] = u16(8);
  r.ch[1] = u16(10);  r.lta[1] = u16(12);
  r.ch[2] = u16(14);  r.lta[2] = u16(16);
  return r;
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

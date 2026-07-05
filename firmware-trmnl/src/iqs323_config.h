#pragma once
#include <stdint.h>

// Azoteq IQS323 register configuration for the TRMNL X capacitive touch bar.
//
// These byte values are hardware-calibration DATA exported from Azoteq's config
// tool (reproduced from usetrmnl/trmnl-firmware lib/IQS323/IQS323_config_LP.h):
// the electrode TX/RX routing, ATI targets, and per-channel thresholds that are
// specific to this exact PCB. Like the panel gray matrix in gray_table.h, this
// is functional hardware-tuning data, not driver logic — the driver in
// touch_iqs323.cpp (windowed I2C, event-mode setup, wake handling) is our own,
// written from the datasheet register map (inc/IQS323_addresses.h).
//
// Layout: the IQS323 memory map addresses (0x30, 0x31, ...) each hold a 2-byte
// little-endian word; a burst write auto-increments the register pointer. Each
// chunk below is {start register, byte count, bytes} — byte0 is the low byte of
// the start register, byte1 the high byte, then the next register, and so on.

namespace iqs323_cfg {

// --- Sensor setups (electrode routing + ATI) --------------------------------
static const uint8_t S0[] = {  // 0x30-0x39
    0x01, 0x04, 0x7F, 0x0C, 0x90, 0x13, 0xCF, 0x04, 0x0A, 0x03,
    0x00, 0x00, 0x74, 0x17, 0x64, 0x00, 0x44, 0x64, 0xD9, 0x53};
static const uint8_t S1[] = {  // 0x40-0x49
    0x01, 0x01, 0x7F, 0x0C, 0x90, 0x13, 0xCF, 0x01, 0x0A, 0x03,
    0x00, 0x00, 0xC4, 0x12, 0x64, 0x00, 0x47, 0x5E, 0xD6, 0x53};
static const uint8_t S2[] = {  // 0x50-0x59
    0x01, 0x02, 0x7F, 0x0C, 0x90, 0x13, 0xCF, 0x02, 0x0A, 0x03,
    0x00, 0x00, 0x74, 0x17, 0x64, 0x00, 0x44, 0x5C, 0xA2, 0x4B};

// --- Per-channel setups (touch threshold in the 3rd word, at reg 0x_2) -------
// Touch threshold lowered from the vendor 0x1B/0x1A (~27) to 0x10 (16): waking from
// sleep is the chip's own touch event (ext0 on RDY), and an off-center / light tap
// on the bar wasn't dropping any single channel's count past ~27, so it demanded a
// firm press. Event-mode baseline noise is only a few counts, so 16 is still well
// clear of false wakes. Lower further (e.g. 0x0C) if it still wants a hard tap;
// raise if idle false-wakes appear.
static const uint8_t CH0[] = {0x00, 0x00, 0x14, 0x44, 0x10, 0x00, 0xC8, 0x00};  // 0x60-0x63
static const uint8_t CH1[] = {0x00, 0x00, 0x14, 0x44, 0x10, 0x00, 0xC8, 0x00};  // 0x70-0x73
static const uint8_t CH2[] = {0x00, 0x00, 0x14, 0x44, 0x10, 0x00, 0xC8, 0x00};  // 0x80-0x83

// --- Slider (3-channel enable mask 0x07 at word 0x94) -----------------------
static const uint8_t SLIDER[] = {  // 0x90-0x98
    0x0B, 0x00, 0x00, 0x14, 0xC8, 0x00, 0x00, 0x08, 0x07, 0x00,
    0x52, 0x05, 0x30, 0x04, 0x72, 0x04, 0xB4, 0x04};

// --- Gesture params (tap/swipe/flick/hold timing) ---------------------------
static const uint8_t GESTURE[] = {  // 0xA0-0xA6
    0x0B, 0x00, 0x32, 0x00, 0x5E, 0x01, 0xF4, 0x01,
    0x58, 0x02, 0x90, 0x01, 0xF4, 0x01};

// --- Filter betas -----------------------------------------------------------
static const uint8_t FILTER[] = {  // 0xB0-0xB4
    0x02, 0x01, 0x0C, 0x0C, 0x00, 0x00, 0x02, 0x01, 0x14, 0x00};

// --- System control + report rates (control byte streamed as 0x10; we flip to
//     event mode after ATI, see touch_iqs323.cpp) ---------------------------
// Report rates per power mode (ms): 0xC1=Normal, 0xC2=Low-Power, 0xC3=Ultra-LP;
// 0xC4/0xC5 = NP/LP mode timeouts. The device deep-sleeps ~45s between wakes, so
// the touch chip sits in ULP scanning only every 100ms (vendor value) — a quick
// tap falls between samples and is missed, forcing a touch-and-hold. We speed the
// idle scan up (LP 60->40ms, ULP 100->40ms) so a brief tap lands on a sample. The
// touch threshold is unchanged (a firm tap deflects 200-600 counts vs a threshold
// of ~27, so timing — not sensitivity — was the limiter). Cost is a small bump in
// always-on sensor current, negligible beside the S3 wake/refresh energy.
static const uint8_t SYSTEM[] = {  // 0xC0-0xC5
    0x10, 0x00, 0x10, 0x00, 0x28, 0x00, 0x28, 0x00, 0xB8, 0x0B, 0xD0, 0x07};

// --- General: I2C timeout, event timeouts, EVENTS_ENABLE (streamed 0x04; we
//     override to touch-only 0x02 after ATI) ---------------------------------
// NB: I2C_TRANS_TIMEOUT (word 0xD1) widened from the vendor's 200ms (0x00C8) to
// 1500ms (0x05DC). The IQS323 holds RDY LOW until the comms window is serviced
// or this timeout elapses; on a touch wake the S3 needs ~0.5s to boot before it
// can read, so the window must outlast that or the touch event is missed.
static const uint8_t GENERAL[] = {  // 0xD0-0xD4
    0x00, 0x00, 0xDC, 0x05, 0x75, 0x75, 0x04, 0x18, 0x08, 0x0A};

struct Chunk {
  uint8_t reg;
  uint8_t len;
  const uint8_t* data;
};

// Full config stream, in memory-map order. Written once at first boot before
// ATI; the IQS323 retains it as long as it stays powered (it is on the always-on
// sensor rail, so it survives the S3's deep-sleep cycles).
static const Chunk STREAM[] = {
    {0x30, sizeof(S0), S0},         {0x40, sizeof(S1), S1},
    {0x50, sizeof(S2), S2},         {0x60, sizeof(CH0), CH0},
    {0x70, sizeof(CH1), CH1},       {0x80, sizeof(CH2), CH2},
    {0x90, sizeof(SLIDER), SLIDER}, {0xA0, sizeof(GESTURE), GESTURE},
    {0xB0, sizeof(FILTER), FILTER}, {0xC0, sizeof(SYSTEM), SYSTEM},
    {0xD0, sizeof(GENERAL), GENERAL},
};
static const int STREAM_COUNT = sizeof(STREAM) / sizeof(STREAM[0]);

}  // namespace iqs323_cfg

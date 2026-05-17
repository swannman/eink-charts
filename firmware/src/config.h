#pragma once
#include <stdint.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// X3 EPD pins (verified against the SSD1677 driver gist and community-sdk README)
constexpr int8_t EPD_SCLK = 8;
constexpr int8_t EPD_MOSI = 10;
constexpr int8_t EPD_CS = 21;
constexpr int8_t EPD_DC = 4;
constexpr int8_t EPD_RST = 5;
constexpr int8_t EPD_BUSY = 6;

// Power button — digital INPUT_PULLUP, active LOW. Per SDK's InputManager.
// GPIO0..GPIO5 are RTC-capable on the C3 so this works as a deep-sleep wake.
constexpr int8_t POWER_BUTTON_GPIO = 3;

// X3 battery-power MOSFET enable. Per the gist + TRMNL X4 firmware, this pin
// gates the battery rail. If it's not held HIGH through deep sleep, the chip
// browns out and reboots — looks like a spurious GPIO wake. CRITICAL.
constexpr int8_t BATTERY_MOSFET_GPIO = 13;

// X3 battery: BQ27220 I2C fuel gauge (not an analog divider like the X4).
//   Address 0x55, SCL=GPIO0, SDA=GPIO20, 400 kHz.
//   Voltage register: 0x08 (units: mV).
//   State-of-charge: 0x2C (units: %).
constexpr int8_t BQ_SCL_GPIO = 0;
constexpr int8_t BQ_SDA_GPIO = 20;
constexpr uint8_t BQ_ADDR = 0x55;
constexpr uint8_t BQ_REG_VOLTAGE = 0x08;
constexpr uint8_t BQ_REG_SOC = 0x2C;

// QMI8658 6-axis IMU. Same I2C bus as the BQ27220 (X3 has one bus).
// Address 0x6B (primary) or 0x6A (fallback). WHO_AM_I at reg 0x00 → 0x05.
// INT1 may be wired to GPIO3 (shared with the power button) — needs HW verify.
constexpr uint8_t QMI_ADDR_PRIMARY = 0x6B;
constexpr uint8_t QMI_ADDR_SECONDARY = 0x6A;
constexpr uint8_t QMI_REG_WHO_AM_I = 0x00;
constexpr uint8_t QMI_REG_CTRL2 = 0x03;
constexpr uint8_t QMI_REG_CTRL7 = 0x08;
constexpr uint8_t QMI_REG_AX_L = 0x35;

// Power-button press patterns.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 200;  // gap between presses
constexpr uint32_t LONG_PRESS_MS = 800;           // held > this → long press
constexpr uint32_t MAX_PRESS_HOLD_MS = 3000;      // give up reading past this

// ADC-decoded D-pad buttons on GPIO 1 (the X3's main button cluster reads as
// an analog resistor ladder). Idle voltage sits above the top threshold;
// each press drops the ADC reading into one of the bucketed ranges. Values
// from the CrazyCoder reverse-engineering gist; may need per-device cal.
constexpr int8_t BTN_ADC_PIN = 1;
constexpr int ADC_NO_BUTTON = 3999;     // > this = idle (no press)
constexpr int ADC_UP_MAX    = 3999;     // 3260..3999 → UP
constexpr int ADC_UP_MIN    = 3260;
constexpr int ADC_DOWN_MAX  = 3260;     // 2408..3260 → DOWN
constexpr int ADC_DOWN_MIN  = 2408;
constexpr int ADC_BACK_MAX  = 2408;     // 1194..2408 → BACK
constexpr int ADC_BACK_MIN  = 1194;
constexpr int ADC_OK_MAX    = 1194;     // 0..1194 → OK
constexpr int ADC_OK_MIN    = 0;

constexpr uint32_t LIST_MODE_TIMEOUT_MS = 30000;
constexpr uint32_t LIST_MODE_POLL_MS    = 50;       // ~20 Hz button polling

// Compile-time defaults. These can be overridden by NVS values at runtime;
// they're the fallback when the device hasn't been provisioned yet.
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASSWORD
#define DEFAULT_WIFI_PASSWORD ""
#endif
#ifndef DEFAULT_BRIDGE_URL
#define DEFAULT_BRIDGE_URL "http://192.168.1.100:8080/frame"
#endif

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 30000;
constexpr uint64_t DEFAULT_POLL_SECONDS = 3600;
constexpr uint64_t WIFI_FAIL_RETRY_SECONDS = 60;

// Full refresh every Nth cycle to clear ghosting.
constexpr uint32_t FULL_REFRESH_EVERY = 12;

// Demo mode: skip WiFi + HTTP, cycle through built-in stub dashboards. Set
// via secrets.h or build flags so it can be turned on without committing.
#ifndef DEMO_MODE
#define DEMO_MODE 0
#endif

constexpr uint64_t DEMO_POLL_SECONDS = 60;

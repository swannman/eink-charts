#pragma once
#include <stdint.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// -----------------------------------------------------------------------------
// reTerminal E1004 hardware (ESP32-S3, 32 MB quad flash, 8 MB OPI PSRAM).
// 13.3" E Ink Spectra 6 panel, 1200x1600 portrait native — we render landscape
// 1600x1200 and let the driver rotate. USB is a CH340 UART bridge (no native
// CDC), so serial logs survive deep sleep entry unlike the TRMNL X.
// Pin values come from Seeed's Seeed_GxEPD2/Seeed_GFX board defs and the OSHW
// schematics (github.com/Seeed-Projects/OSHW-reTerminal-Series-E-D).
// -----------------------------------------------------------------------------

// EPD SPI bus (driven by the display library; listed for reference/sleep hold).
constexpr int8_t EPD_SCK_PIN = 7;
constexpr int8_t EPD_MOSI_PIN = 9;
constexpr int8_t EPD_CS_PIN = 10;
constexpr int8_t EPD_DC_PIN = 11;
constexpr int8_t EPD_RES_PIN = 12;
constexpr int8_t EPD_BUSY_PIN = 13;

// Front buttons (active LOW, INPUT_PULLUP; per Seeed's ESPHome config). Any of
// them wakes the device from deep sleep (ext1 ANY_LOW) and advances.
constexpr int8_t BTN_GREEN_GPIO = 3;        // green button
constexpr int8_t BTN_WHITE_RIGHT_GPIO = 4;  // right white button
constexpr int8_t BTN_WHITE_LEFT_GPIO = 5;   // left white button

// Battery voltage sense: GPIO21 HIGH connects the divider to the ADC on
// GPIO1; the divider halves the pack voltage (ESPHome uses multiply: 2.0).
constexpr int8_t BAT_EN_GPIO = 21;
constexpr int8_t BAT_ADC_GPIO = 1;
constexpr float BAT_DIVIDER = 2.0f;

// Buzzer (PWM) + green LED (active LOW). Unused by this firmware so far.
constexpr int8_t BUZZER_GPIO = 45;
constexpr int8_t LED_GPIO = 48;

// -----------------------------------------------------------------------------
// Display geometry (landscape after rotation). Mirrors tools/preview_e1004.py.
// -----------------------------------------------------------------------------
constexpr int SCREEN_W = 1600;
constexpr int SCREEN_H = 1200;
constexpr int GRID_COLS = 24;

// Safe-area insets (px) so nothing important renders under the bezel.
constexpr int SAFE_TOP = 40;
constexpr int SAFE_BOTTOM = 24;
constexpr int SAFE_LEFT = 24;
constexpr int SAFE_RIGHT = 24;

// Spectra 6 color codes — MUST match data_trmnl.py SPECTRA_* (v3 bundle).
constexpr uint8_t COL_BLACK = 0;
constexpr uint8_t COL_WHITE = 1;
constexpr uint8_t COL_RED = 2;
constexpr uint8_t COL_YELLOW = 3;
constexpr uint8_t COL_GREEN = 4;
constexpr uint8_t COL_BLUE = 5;
constexpr uint8_t COL_COUNT = 6;

// -----------------------------------------------------------------------------
// Networking / bundle source. Wi-Fi only (S3 2.4 GHz radio).
// -----------------------------------------------------------------------------
#ifndef DEFAULT_WORKER_URL
#define DEFAULT_WORKER_URL "https://dashboard.contexa.net/bundle-e1004"
#endif
#ifndef DEFAULT_WORKER_BEARER
#define DEFAULT_WORKER_BEARER ""
#endif
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASSWORD
#define DEFAULT_WIFI_PASSWORD ""
#endif

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000;
constexpr uint32_t HTTP_TIMEOUT_MS = 30000;

// -----------------------------------------------------------------------------
// Slideshow cadence. A Spectra 6 full refresh takes ~20-30 s, so the dwell is
// long (like the TRMNL X's 45 min) and every advance pulls fresh data. A green
// button press advances immediately from cache.
// -----------------------------------------------------------------------------
constexpr uint64_t DWELL_SECONDS = 45 * 60;
constexpr uint64_t REFRESH_INTERVAL_SECONDS = 45 * 60;

// Quiet hours: skip the timed advance + refresh (and its Wi-Fi fetch)
// overnight — at 32 cycles/day the ~30 s color refresh IS the battery, so
// sleeping 22:00-06:00 cuts a third of them. A button press still wakes and
// advances from cache. The wall clock is set from the Worker's HTTP Date
// header on each fetch (no NTP roundtrip) and kept by the RTC across deep
// sleep; without a valid clock (fresh flash, no fetch yet) quiet hours are
// simply skipped. The first wake after 06:00 fetches fresh data.
constexpr int QUIET_HOURS_START = 22;  // inclusive
constexpr int QUIET_HOURS_END = 6;     // exclusive — wake-up time
// POSIX TZ for Pacific, DST rules included (mirrors firmware-cloud).
constexpr const char* LOCAL_TZ = "PST8PDT,M3.2.0/2,M11.1.0/2";

// Demo mode: skip Wi-Fi + fetch, render a built-in stub bundle.
#ifndef DEMO_MODE
#define DEMO_MODE 0
#endif

// Dev serial console (see serial_console.h). Set to 0 for battery deployments.
#ifndef SERIAL_CONSOLE
#define SERIAL_CONSOLE 1
#endif
constexpr uint32_t CONSOLE_IDLE_MS = 4000;
constexpr uint32_t CONSOLE_TIMEOUT_MS = 180000;

// -----------------------------------------------------------------------------
// Task watchdog (same rationale as the TRMNL X: a wedged Wi-Fi/HTTP/SPI call
// must panic-reset into a fresh boot instead of burning battery). The timeout
// must cover one full panel refresh (~30 s) — it's fed around the refresh.
// -----------------------------------------------------------------------------
#ifndef ENABLE_WATCHDOG
#define ENABLE_WATCHDOG 1
#endif
constexpr uint32_t WATCHDOG_TIMEOUT_S = 90;

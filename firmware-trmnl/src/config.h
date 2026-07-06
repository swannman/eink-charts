#pragma once
#include <stdint.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// -----------------------------------------------------------------------------
// TRMNL X hardware (ESP32-S3). Pin map + addresses from the official
// usetrmnl/trmnl-firmware BOARD_TRMNL_X build. The parallel EPD bus itself is
// owned by FastEPD's BB_PANEL_TRMNL_X profile — we never touch those pins.
// -----------------------------------------------------------------------------

// Shared sensor I2C bus (touch bar, fuel gauge, accelerometer live here).
// NOTE: FastEPD/its TCA9535 expander is on a SEPARATE bus; don't call
// Wire.begin() again after the touch driver brings this bus up.
constexpr int8_t SENSOR_SDA_PIN = 39;
constexpr int8_t SENSOR_SCL_PIN = 40;

// Azoteq IQS323 capacitive touch bar. RDY/INT line doubles as the deep-sleep
// wake source (active LOW).
constexpr uint8_t IQS323_ADDR = 0x44;
constexpr int8_t  TOUCH_RDY_GPIO = 3;   // == official PIN_INTERRUPT

// TI BQ27427 fuel gauge (fixed I2C @0x55). The register command set + golden
// files live in the vendored MIT lib/BQ27427; battery_bq27427.cpp adds cell
// detection + charge status via the TCA9535/charger I/O below (FastEPD io pins).
constexpr uint8_t  BAT_DET_IO_PIN     = 7;      // TCA9535 P0_7: RC-discharge cell sense
constexpr uint8_t  BQ25616_STAT_IO    = 24;     // charger STAT (LOW = charging)
constexpr uint32_t BAT_DET_CHARGE_MS  = 2;      // drive the sense pin HIGH to charge RC
constexpr uint32_t BAT_DET_TIMEOUT_US = 6000;   // still HIGH after this → no battery
constexpr uint32_t BAT_DET_THRESH_US  = 750;    // discharge >thr → 1 cell, ≤thr → 2 cells
constexpr uint32_t BAT_ITPOR_WAIT_MS  = 5000;   // poll for IT algorithm to leave INIT

// -----------------------------------------------------------------------------
// Display geometry (native landscape). Mirrors tools/preview_trmnl.py.
// -----------------------------------------------------------------------------
constexpr int SCREEN_W = 1872;
constexpr int SCREEN_H = 1404;
constexpr int GRID_COLS = 24;

// Safe-area insets (px): the physical bezel overlaps the glass edges and was
// clipping panel titles along the top. The dashboard grid is laid out inside
// this inset so nothing important renders under the frame. Top gets extra room
// for the titles. Tune if the frame still clips content.
constexpr int SAFE_TOP = 48;
constexpr int SAFE_BOTTOM = 28;
constexpr int SAFE_LEFT = 28;
constexpr int SAFE_RIGHT = 28;

// Grayscale convention (matches FastEPD): 0 = black, 15 = white.
constexpr uint8_t GRAY_BLACK = 0;
constexpr uint8_t GRAY_WHITE = 15;

// -----------------------------------------------------------------------------
// Networking / bundle source. Wi-Fi only (native S3 2.4 GHz radio).
// -----------------------------------------------------------------------------
#ifndef DEFAULT_WORKER_URL
#define DEFAULT_WORKER_URL "https://dashboard.contexa.net/bundle-trmnl"
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
// Slideshow cadence. The device caches the whole multi-dashboard bundle and
// advances through it locally on each timer wake; it only re-fetches over
// Wi-Fi every REFRESH_INTERVAL. A touch tap advances immediately (from cache,
// never over Wi-Fi). With a 45-minute dwell the refresh floor is <= the dwell,
// so every timed advance pulls fresh data (each shown dashboard is <=45 min old).
// -----------------------------------------------------------------------------
constexpr uint64_t DWELL_SECONDS = 45 * 60;         // per-dashboard display time (45 min)
constexpr uint64_t REFRESH_INTERVAL_SECONDS = 45 * 60;  // Wi-Fi re-fetch floor (each advance)

// Wake on a touch-bar tap (ext0 on the IQS323 RDY line) to advance early.
// touch::configure() puts the IQS323 in EVENT MODE so RDY only asserts on a real
// touch (not every measurement cycle), and readEvent() gates advancing on an
// actual channel-touch flag — so a tap advances, and press-release/spurious
// wakes don't. Set to 0 to fall back to pure timed rotation (DWELL_SECONDS).
#ifndef ENABLE_TOUCH
#define ENABLE_TOUCH 1
#endif

// Demo mode: skip Wi-Fi + fetch, render a built-in stub bundle. Set via
// secrets.h or build flags.
#ifndef DEMO_MODE
#define DEMO_MODE 0
#endif

// Dev serial console: after each render, wait briefly for a USB command. If one
// arrives the device stays awake and interactive — dump the framebuffer as a
// base64 grayscale image and step through dashboards from the host — so layout
// can be verified against the real panel. No command within the initial window
// → normal deep sleep. Set to 0 for battery deployments.
#ifndef SERIAL_CONSOLE
#define SERIAL_CONSOLE 1
#endif
constexpr uint32_t CONSOLE_IDLE_MS = 4000;        // wait for the first command
constexpr uint32_t CONSOLE_TIMEOUT_MS = 180000;   // idle limit once interacting

// Touch debug streaming. When set, the serial console never times out into deep
// sleep and continuously streams the touch state: every RDY edge is logged the
// instant it happens (the interrupt path), and the IQS323 SYSTEM_STATUS is
// force-read at TOUCH_DEBUG_PERIOD_MS regardless of RDY (readEvent() opens its
// own comms window). Watching both lines apart tells us whether the sensor even
// feels the finger vs. whether the RDY interrupt is firing. Set to 0 for any
// real (battery) deployment — this keeps the S3 awake forever and spams serial.
#ifndef TOUCH_DEBUG
#define TOUCH_DEBUG 0
#endif
constexpr uint32_t TOUCH_DEBUG_PERIOD_MS = 500;   // forced status poll cadence

// -----------------------------------------------------------------------------
// Task watchdog. Deep sleep already self-heals most failures (every wake is a
// fresh boot), but a hang inside the wake path — a WiFi/TLS handshake that never
// returns, a wedged I2C bus, a stalled HTTP read — would sit burning battery and
// never reach deep sleep. The watchdog panic-resets out of that into a fresh
// boot. It's fed between blocking phases, so the timeout only has to cover a
// single operation (our worst case is one HTTP op: 30s timeout + 5s read-stall),
// not the whole multi-dashboard prefetch — no false resets on a slow network.
#ifndef ENABLE_WATCHDOG
#define ENABLE_WATCHDOG 1
#endif
constexpr uint32_t WATCHDOG_TIMEOUT_S = 60;

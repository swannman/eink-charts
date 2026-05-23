#include <Arduino.h>
#include <ArduinoJson.h>
#include <EInkDisplay.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <time.h>

#include <vector>

#include <WiFiClientSecure.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "ble_bundle_receiver.h"
#include "bundle_seal.h"
#include "config.h"
#include "demo_panel_data.h"
#include "enroll_screen.h"
#include "gfx_lite.h"
#include "imu.h"
#include "panel_model.h"
#include "wifi_config.h"
#include "x25519_keystore.h"
#include "log_buffer.h"

EInkDisplay display(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);

RTC_DATA_ATTR static uint32_t rtcCycleCount = 0;
RTC_DATA_ATTR static uint32_t rtcDemoIdx = 0;
// Magic value: if RTC memory still has it on boot, this isn't a fresh flash —
// it's a wake-from-something (deep-sleep wake, EN-pin reset, watchdog, etc.),
// so we should advance instead of resetting to dashboard A.
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
constexpr uint32_t RTC_MAGIC_VALUE = 0xDEC0DE42u;

// FNV-1a of __DATE__ __TIME__ — unique per build. Mismatch on boot means
// new firmware; trigger one full refresh to scrub ghosts from the prior
// build's pixels.
constexpr uint32_t fnv1a_str(const char* s, uint32_t h = 2166136261u) {
  return *s ? fnv1a_str(s + 1, (h ^ (uint8_t)*s) * 16777619u) : h;
}
constexpr uint32_t kFirmwareTag = fnv1a_str(__DATE__ __TIME__);
RTC_DATA_ATTR static uint32_t rtcFirmwareTag = 0;
// True if we've completed at least one displayBuffer() since the last full
// boot — so the controller's RED RAM (0x10) holds a valid previous frame
// suitable for differential refresh on the next wake.
RTC_DATA_ATTR static bool rtcEpdRamValid = false;
// Time-window the user is viewing. Long-press cycles 24h → 2h → 7d → 24h.
enum ViewMode : uint8_t { VIEW_24H = 0, VIEW_2H = 1, VIEW_7D = 2 };
RTC_DATA_ATTR static uint8_t rtcViewMode = VIEW_24H;
static const char* viewLabel(uint8_t m) {
  switch (m) {
    case VIEW_2H: return "2h";
    case VIEW_7D: return "7d";
    default:      return "24h";
  }
}
// Persisted across deep sleep via the RTC clock. After the first NTP sync we
// can compute "seconds since last successful WiFi fetch" without reconnecting.
RTC_DATA_ATTR static time_t rtcLastFetchEpoch = 0;
// Current panel index — wraps modulo rtcPanelTotal for forward-press
// navigation. The sentinel LOGS_PANEL_IDX means "show the device-logs
// screen instead of a bundle panel"; only reachable via the long-press
// list, never via forward-press cycling.
RTC_DATA_ATTR static uint32_t rtcPanelIdx = 0;
RTC_DATA_ATTR static uint32_t rtcPanelTotal = 0;
constexpr uint32_t LOGS_PANEL_IDX = 0xFFFFFFFFu;
// Lines to skip from the most recent when rendering the logs screen.
// 0 = show the tail (newest). Bumped by UP-button page presses.
RTC_DATA_ATTR static int32_t rtcLogsScrollOffset = 0;
// Refresh cadence — finer time windows want fresher data. 7d barely moves
// hour-to-hour; 2h zoom is for actively watching a trend.
constexpr uint64_t REFRESH_INTERVAL_2H_SECONDS  =  5 * 60;
constexpr uint64_t REFRESH_INTERVAL_24H_SECONDS = 15 * 60;
constexpr uint64_t REFRESH_INTERVAL_7D_SECONDS  = 60 * 60;
static inline uint64_t currentRefreshInterval() {
  switch (rtcViewMode) {
    case VIEW_2H:  return REFRESH_INTERVAL_2H_SECONDS;
    case VIEW_7D:  return REFRESH_INTERVAL_7D_SECONDS;
    default:       return REFRESH_INTERVAL_24H_SECONDS;
  }
}

// Quiet hours: no background WiFi refreshes between 22:00 and 06:00 local.
// Button presses still work (they use cache only); on the first wake after
// 06:00 we do a full fetch to catch up.
constexpr int QUIET_HOURS_START = 22;  // inclusive
constexpr int QUIET_HOURS_END = 6;     // exclusive — wake-up time

// IANA POSIX TZ string for Pacific. Compiled in here so we don't need a
// runtime tzdata; M3.2.0 = 2nd Sunday in March (DST start), M11.1.0 = 1st
// Sunday in November (DST end).
constexpr const char* LOCAL_TZ = "PST8PDT,M3.2.0/2,M11.1.0/2";

static bool getLocalNow(struct tm& out) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &out);
  return true;
}

static bool inQuietHours() {
  struct tm lt;
  if (!getLocalNow(lt)) return false;
  return lt.tm_hour >= QUIET_HOURS_START || lt.tm_hour < QUIET_HOURS_END;
}

// Seconds from `now` until the next QUIET_HOURS_END boundary (06:00 local).
static uint64_t secondsUntilQuietEnd() {
  struct tm lt;
  if (!getLocalNow(lt)) return 0;
  struct tm target = lt;
  target.tm_hour = QUIET_HOURS_END;
  target.tm_min = 0;
  target.tm_sec = 0;
  time_t now = time(nullptr);
  time_t end = mktime(&target);
  if (end <= now) end += 24 * 3600;  // already past 6am today → tomorrow
  return (uint64_t)(end - now);
}

// On-screen "I'm alive" overlay. Minimal: 8 byte-wide cells × 2 px tall at
// top-right. No border. Cells encode 6 bits of cycle counter + wifi + fetch.
static constexpr int BYTES_PER_ROW = 99;        // 792 / 8
static constexpr int STATUS_X_BYTE = 91;         // pixel x = 728 (8 bytes * 8 = 64 px wide)
static constexpr int STATUS_Y = 4;
static constexpr int STATUS_INNER_BYTES = 8;
static constexpr int STATUS_BORDER = 0;
static constexpr int STATUS_W_BYTES = STATUS_INNER_BYTES;
static constexpr int STATUS_H = 2;
static constexpr int CELL_TOP = STATUS_Y;
static constexpr int CELL_BOTTOM = STATUS_Y + STATUS_H;

static void fillBytes(int xByte, int y, int wBytes, int h, uint8_t fill) {
  uint8_t* fb = display.getFrameBuffer();
  for (int row = 0; row < h; row++) {
    memset(fb + (y + row) * BYTES_PER_ROW + xByte, fill, wBytes);
  }
}

// Status cells: bit i of cycle for i in 0..5, then wifi (idx 6), then fetch (idx 7).
static void drawStatusOverlay(uint32_t cycle, bool wifiOk, bool fetchOk) {
  // Black border, white interior.
  fillBytes(STATUS_X_BYTE, STATUS_Y, STATUS_W_BYTES, STATUS_H, 0x00);
  fillBytes(STATUS_X_BYTE + STATUS_BORDER, STATUS_Y + STATUS_BORDER,
            STATUS_INNER_BYTES, STATUS_H - 2 * STATUS_BORDER, 0xFF);

  uint8_t* fb = display.getFrameBuffer();
  for (int i = 0; i < 8; i++) {
    bool on;
    if (i < 6)       on = (cycle >> i) & 1;
    else if (i == 6) on = wifiOk;
    else             on = fetchOk;
    uint8_t color = on ? 0x00 : 0xFF;
    for (int row = CELL_TOP; row < CELL_BOTTOM; row++) {
      fb[row * BYTES_PER_ROW + STATUS_X_BYTE + STATUS_BORDER + i] = color;
    }
  }
}

static void refreshStatusOverlay() {
  display.displayWindow(STATUS_X_BYTE * 8, STATUS_Y, STATUS_W_BYTES * 8, STATUS_H, false);
}

// Solid-black status box, painted as soon as we wake — gives instant visual
// confirmation before WiFi + fetch run (which can take 5-50s).
static void paintWakeMarker() {
  fillBytes(STATUS_X_BYTE, STATUS_Y, STATUS_W_BYTES, STATUS_H, 0x00);
  refreshStatusOverlay();
}

// Esptool flash-recovery window. The C3 uses HWCDC (USB Serial JTAG) — the
// chip's silicon-level reset-to-bootloader gesture works *if* the firmware
// isn't actively reconfiguring the controller during the gesture. The
// reliable workaround is a small sit-and-wait at the top of setup() so
// esptool's DTR/RTS sequence lands while we're idle and USB-CDC is open.
constexpr uint32_t FLASH_RECOVERY_WINDOW_MS = 3000;

// Full-screen "connecting to <SSID>" splash. Shown until the chart fetch
// succeeds; replaces whatever was on screen so the user knows we're alive.
static void paintConnectingScreen(const String& ssid, uint32_t cycle) {
  uint8_t* fb = display.getFrameBuffer();
  fbClear(fb, /*white=*/true);

  // FreeSans18pt7b for the label, FreeSansBold18pt7b scaled ×2 for the
  // big SSID below it. yAdvance is the GFX line height; the baseline
  // sits below the top of the cell by ~80% of yAdvance.
  const int label_h = fbGfxLineHeight(&FreeSans18pt7b);
  const int ssid_h  = fbGfxLineHeight(&FreeSansBold18pt7b) * 2;
  const int gap = 24;
  const int total = label_h + gap + ssid_h;
  const int top = (FB_HEIGHT - total) / 2;

  fbDrawStringGfxCentered(fb, top + label_h, &FreeSans18pt7b,
                          "Connecting to", /*black=*/true);
  fbDrawStringGfxScaledCentered(fb, top + label_h + gap + ssid_h,
                                &FreeSansBold18pt7b,
                                ssid.length() ? ssid.c_str() : "(no SSID)",
                                /*scale=*/2, true);

  char footer[48];
  snprintf(footer, sizeof(footer), "cycle %u", (unsigned)cycle);
  fbDrawStringGfx(fb, 12, FB_HEIGHT - 8, &FreeSans9pt7b, footer, true);

  display.displayBuffer(EInkDisplay::FAST_REFRESH, /*turnOffScreen=*/false);
}

// Painted by paintBootStatus(). Belongs to the area below the SSID + above
// the footer that paintConnectingScreen intentionally leaves blank.
constexpr int SPLASH_STATUS_Y = 410;
constexpr int SPLASH_STATUS_H = 28;
static bool g_splashPainted = false;

// Repaint just the status sub-line on the splash and push a partial refresh.
// No-op if the splash isn't currently on screen (e.g., a normal cycle where
// the previous panel is still visible behind the fetch).
static void paintBootStatus(const char* msg) {
  if (!g_splashPainted) return;
  uint8_t* fb = display.getFrameBuffer();
  fbFillRect(fb, 0, SPLASH_STATUS_Y, FB_WIDTH, SPLASH_STATUS_H, /*black=*/false);
  fbDrawStringGfxCentered(fb, SPLASH_STATUS_Y + 20, &FreeSans9pt7b, msg, /*black=*/true);
  display.displayWindow(0, SPLASH_STATUS_Y, FB_WIDTH, SPLASH_STATUS_H, /*turnOff=*/false);
}

// Wake on the power button (GPIO3 going LOW). TRMNL on the X4 (same C3
// silicon, same hardware family) ships with this exact wake source.
constexpr uint64_t POWER_BUTTON_WAKE_MASK = 1ULL << POWER_BUTTON_GPIO;

// Drive GPIO13 HIGH at boot. Must run before anything else can fail — if the
// battery MOSFET pin floats LOW after a wake, the chip browns out and reboots.
// Releases any hold left over from the previous deep-sleep cycle first.
static void enableBatteryRail() {
  gpio_hold_dis((gpio_num_t)BATTERY_MOSFET_GPIO);
  gpio_deep_sleep_hold_dis();
  pinMode(BATTERY_MOSFET_GPIO, OUTPUT);
  digitalWrite(BATTERY_MOSFET_GPIO, HIGH);
}

// Single deep-sleep entry point. Sleeps for `timerSeconds`, but also wakes
// immediately if the user presses the power button. Always holds the battery
// MOSFET HIGH through sleep to prevent the brownout-reboot loop.
static void deepSleep(uint64_t timerSeconds) {
  // Wait for the button to be fully released. If we arm GPIO-wake-on-LOW
  // while it's still pressed, the chip wakes immediately in a loop.
  pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);
  uint32_t releaseWaitStart = millis();
  while (digitalRead(POWER_BUTTON_GPIO) == LOW &&
         millis() - releaseWaitStart < MAX_PRESS_HOLD_MS + 1000) {
    delay(10);
  }
  // Debounce the released signal briefly.
  delay(BUTTON_DEBOUNCE_MS);
  Log.printf("deep sleep (timer=%llus, also wake on power button)\n",
                (unsigned long long)timerSeconds);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(timerSeconds * 1000000ULL);

  display.deepSleep();
  esp_deep_sleep_enable_gpio_wakeup(POWER_BUTTON_WAKE_MASK,
                                    ESP_GPIO_WAKEUP_GPIO_LOW);

  // CRITICAL: hold GPIO13 (battery MOSFET) HIGH through deep sleep, otherwise
  // the rail drops, the chip browns out, and we get a reboot loop that looks
  // like spurious GPIO wakes. Copied from TRMNL's X4 port.
  gpio_hold_en((gpio_num_t)BATTERY_MOSFET_GPIO);
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
}

enum NavAction { NAV_FORWARD, NAV_TOGGLE_ZOOM, NAV_ENTER_LIST, NAV_NONE };

// MUST be called at the very top of setup(), before any long delays.
//
//   Hold >= LONG_PRESS_MS                                       → enter list mode
//   Short press, second press within DOUBLE_CLICK_WINDOW_MS     → cycle view
//   Short press, no second press within window                  → forward
static NavAction readPressPattern() {
  pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);

  // Time the first press. Return NAV_ENTER_LIST the *instant* LONG_PRESS_MS
  // is reached so the new view renders while the button is still held —
  // visual feedback that the gesture registered. Release-handling happens in
  // deepSleep() so the wake-on-LOW doesn't immediately re-fire.
  uint32_t firstPressStart = millis();
  while (digitalRead(POWER_BUTTON_GPIO) == LOW &&
         millis() - firstPressStart < MAX_PRESS_HOLD_MS) {
    if (millis() - firstPressStart >= LONG_PRESS_MS) return NAV_ENTER_LIST;
    delay(10);
  }

  // Watch for a second press for DOUBLE_CLICK_WINDOW_MS after first release.
  uint32_t releaseTime = millis();
  while (millis() - releaseTime < DOUBLE_CLICK_WINDOW_MS) {
    if (digitalRead(POWER_BUTTON_GPIO) == LOW) {
      // Second press detected → wait for ITS release before returning.
      uint32_t secondPressStart = millis();
      while (digitalRead(POWER_BUTTON_GPIO) == LOW &&
             millis() - secondPressStart < MAX_PRESS_HOLD_MS) {
        delay(10);
      }
      return NAV_TOGGLE_ZOOM;
    }
    delay(10);
  }
  return NAV_FORWARD;
}

static bool connectWifi(const String& ssid, const String& password) {
  if (ssid.isEmpty()) {
    Log.println("no SSID configured; skipping wifi");
    return false;
  }
  Log.printf("wifi: connecting to '%s'\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Log.println("wifi: timeout");
    return false;
  }
  Log.printf("wifi: connected, ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

enum FetchResult { FETCH_OK, FETCH_NOT_MODIFIED, FETCH_ERROR };

// Static storage for the parsed panel. drawPanel() only reads these before we
// leave setup(), so global lifetime is fine and avoids a fat stack frame.
constexpr uint32_t MAX_POINTS = 400;
constexpr uint32_t MAX_LABELS = 12;
constexpr uint32_t MAX_LABEL_BYTES = 24;
constexpr uint32_t MAX_STATS = 3;
constexpr uint32_t MAX_SPARK_POINTS = 80;
static float g_points[MAX_POINTS * 2];
static char g_title[96];
static char g_series_name[64];
static char g_y_label_buf[MAX_LABELS][MAX_LABEL_BYTES];
static char g_x_label_buf[MAX_LABELS][MAX_LABEL_BYTES];
static const char* g_y_labels[MAX_LABELS];
static const char* g_x_labels[MAX_LABELS];
// Stat-group storage. Three columns per screen max; per-column scratch for
// title/unit/formatted-value plus a sparkline buffer.
static char g_stat_title[MAX_STATS][48];
static char g_stat_unit[MAX_STATS][12];
static char g_stat_value[MAX_STATS][24];
static float g_stat_spark[MAX_STATS][MAX_SPARK_POINTS * 2];


static void copyString(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

// Minimal little-endian binary reader for the /data/all bundle. Bounds-
// checked so a truncated/corrupt blob can't run past the buffer.
struct BinReader {
  const uint8_t* p;
  const uint8_t* end;
  BinReader(const uint8_t* data, size_t len) : p(data), end(data + len) {}
  bool ok(size_t n) const { return p + n <= end; }
  uint8_t u8() { if (!ok(1)) return 0; return *p++; }
  uint16_t u16() {
    if (!ok(2)) { p = end; return 0; }
    uint16_t v = p[0] | ((uint16_t)p[1] << 8);
    p += 2;
    return v;
  }
  uint32_t u32() {
    if (!ok(4)) { p = end; return 0; }
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    return v;
  }
  // Read length-prefixed string. Returns the same `out` pointer; truncated
  // if longer than cap-1.
  const char* pstr(char* out, size_t cap) {
    uint8_t n = u8();
    if (!ok(n)) { p = end; out[0] = 0; return out; }
    size_t take = (n < cap - 1) ? n : cap - 1;
    memcpy(out, p, take);
    out[take] = 0;
    p += n;
    return out;
  }
};

constexpr uint16_t BUNDLE_MAGIC = 0xCFB1;
// v4 = "screens": each navigation step shows one screen, which may contain a
// full-screen chart or 1-3 side-by-side stat boxes.
constexpr uint8_t BUNDLE_VERSION_REQUIRED = 4;

// Screen / entry type codes — must match bridge data.py SCREEN_/ENTRY_.
constexpr uint8_t SCREEN_CHART = 0;
constexpr uint8_t SCREEN_STAT_GROUP = 1;
constexpr uint8_t ENTRY_CHART = 0;
constexpr uint8_t ENTRY_STAT = 1;

// Aggregated parse result for one screen; passed to the render dispatch.
struct ScreenData {
  uint8_t type;
  PanelData chart;        // valid iff type == SCREEN_CHART
  StatEntry stats[MAX_STATS];
  uint32_t stat_count;    // valid iff type == SCREEN_STAT_GROUP
};

// Read a chart panel block (the inner part of an ENTRY_CHART) into `out`.
// Storage is the global g_points / g_*_labels / g_title / g_series_name —
// only one chart is alive at a time on the device, so that's fine.
static void readChartBlock(BinReader& r, PanelData& out) {
  r.pstr(g_title, sizeof(g_title));
  out.title = g_title;
  r.pstr(g_series_name, sizeof(g_series_name));
  out.series_name = g_series_name;

  uint8_t yn = r.u8();
  uint32_t ystored = 0;
  for (uint8_t i = 0; i < yn; i++) {
    if (ystored < MAX_LABELS) {
      r.pstr(g_y_label_buf[ystored], MAX_LABEL_BYTES);
      g_y_labels[ystored] = g_y_label_buf[ystored];
      ystored++;
    } else {
      char throwaway[2];
      r.pstr(throwaway, sizeof(throwaway));
    }
  }
  out.y_labels = g_y_labels;
  out.y_label_count = ystored;

  uint8_t xn = r.u8();
  uint32_t xstored = 0;
  for (uint8_t i = 0; i < xn; i++) {
    if (xstored < MAX_LABELS) {
      r.pstr(g_x_label_buf[xstored], MAX_LABEL_BYTES);
      g_x_labels[xstored] = g_x_label_buf[xstored];
      xstored++;
    } else {
      char throwaway[2];
      r.pstr(throwaway, sizeof(throwaway));
    }
  }
  out.x_labels = g_x_labels;
  out.x_label_count = xstored;

  uint16_t pcount = r.u16();
  uint32_t pstored = 0;
  for (uint16_t i = 0; i < pcount; i++) {
    uint16_t nx = r.u16();
    uint16_t ny = r.u16();
    if (pstored < MAX_POINTS) {
      g_points[2 * pstored] = (float)nx / 65535.0f;
      g_points[2 * pstored + 1] = (float)ny / 65535.0f;
      pstored++;
    }
  }
  out.points = g_points;
  out.point_count = pstored;
}

// Read a single stat entry (the inner part of an ENTRY_STAT) into slot
// `stat_idx` of the global stat scratch arrays.
static void readStatEntry(BinReader& r, uint32_t stat_idx, StatEntry& out) {
  r.pstr(g_stat_title[stat_idx], sizeof(g_stat_title[stat_idx]));
  r.pstr(g_stat_unit[stat_idx], sizeof(g_stat_unit[stat_idx]));
  r.pstr(g_stat_value[stat_idx], sizeof(g_stat_value[stat_idx]));
  out.title = g_stat_title[stat_idx];
  out.unit = g_stat_unit[stat_idx];
  out.value_str = g_stat_value[stat_idx];

  uint8_t pcount = r.u8();
  uint32_t stored = 0;
  for (uint8_t i = 0; i < pcount; i++) {
    uint16_t nx = r.u16();
    uint16_t ny = r.u16();
    if (stored < MAX_SPARK_POINTS) {
      g_stat_spark[stat_idx][2 * stored] = (float)nx / 65535.0f;
      g_stat_spark[stat_idx][2 * stored + 1] = (float)ny / 65535.0f;
      stored++;
    }
  }
  out.spark = g_stat_spark[stat_idx];
  out.spark_count = stored;
}

// Decode one screen block (starts with type + entry_count) into `out`.
static void hydrateScreen(const uint8_t* data, size_t data_len, uint32_t ofs,
                          ScreenData& out) {
  BinReader r(data + ofs, data_len - ofs);
  out.type = r.u8();
  uint8_t entries = r.u8();
  out.stat_count = 0;
  if (out.type == SCREEN_STAT_GROUP) {
    for (uint8_t i = 0; i < entries; i++) {
      uint8_t etype = r.u8();
      if (etype == ENTRY_STAT && out.stat_count < MAX_STATS) {
        readStatEntry(r, out.stat_count, out.stats[out.stat_count]);
        out.stat_count++;
      } else {
        // Unknown entry type — abort parsing this screen.
        return;
      }
    }
  } else {
    // Chart screen: exactly one chart entry expected.
    if (entries >= 1) {
      uint8_t etype = r.u8();
      if (etype == ENTRY_CHART) {
        readChartBlock(r, out.chart);
      }
    }
  }
}

// (Unused) JSON variant kept here in case we need to hand-craft a panel from
// a JSON object for debugging.
static void hydratePanel(JsonObjectConst obj, PanelData& out) {
  copyString(g_title, sizeof(g_title), obj["title"].as<const char*>());
  out.title = g_title;

  uint32_t yn = 0;
  for (JsonVariantConst v : obj["y_axis"]["labels"].as<JsonArrayConst>()) {
    if (yn >= MAX_LABELS) break;
    copyString(g_y_label_buf[yn], MAX_LABEL_BYTES, v.as<const char*>());
    g_y_labels[yn] = g_y_label_buf[yn];
    yn++;
  }
  out.y_labels = g_y_labels;
  out.y_label_count = yn;

  uint32_t xn = 0;
  for (JsonVariantConst v : obj["x_axis"]["labels"].as<JsonArrayConst>()) {
    if (xn >= MAX_LABELS) break;
    copyString(g_x_label_buf[xn], MAX_LABEL_BYTES, v.as<const char*>());
    g_x_labels[xn] = g_x_label_buf[xn];
    xn++;
  }
  out.x_labels = g_x_labels;
  out.x_label_count = xn;

  uint32_t pcount = 0;
  JsonArrayConst series = obj["series"].as<JsonArrayConst>();
  if (!series.isNull() && series.size() > 0) {
    JsonObjectConst s0 = series[0].as<JsonObjectConst>();
    copyString(g_series_name, sizeof(g_series_name), s0["name"].as<const char*>());
    out.series_name = g_series_name;
    JsonArrayConst pts = s0["points"].as<JsonArrayConst>();
    for (JsonVariantConst pt : pts) {
      if (pcount >= MAX_POINTS) break;
      g_points[2 * pcount] = pt[0].as<float>();
      g_points[2 * pcount + 1] = pt[1].as<float>();
      pcount++;
    }
  } else {
    g_series_name[0] = 0;
    out.series_name = g_series_name;
  }
  out.points = g_points;
  out.point_count = pcount;
}

static bool setClockFromHttpDate(const String& dateHdr);  // fwd

// Read a 16-bit little-endian register from the BQ27220 fuel gauge on the X3.
// Returns 0xFFFF on bus error so the caller can sniff for "no gauge."
static uint16_t bq27220ReadU16(uint8_t reg) {
  Wire.beginTransmission(BQ_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFFFF;
  if (Wire.requestFrom((uint8_t)BQ_ADDR, (uint8_t)2) != 2) return 0xFFFF;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  return (uint16_t)((hi << 8) | lo);
}

// Battery voltage in millivolts via the BQ27220 fuel gauge. Returns 0 if the
// chip isn't responding (USB power, missing battery, dead gauge).
static uint16_t readBatteryMv() {
  Wire.begin(BQ_SDA_GPIO, BQ_SCL_GPIO);
  Wire.setClock(400000);
  uint16_t mv = bq27220ReadU16(BQ_REG_VOLTAGE);
  if (mv == 0xFFFF || mv < 2500 || mv > 5000) return 0;
  return mv;
}

// Battery state-of-charge percentage (0–100). Returns 0xFF if unavailable.
static uint8_t readBatterySoc() {
  Wire.begin(BQ_SDA_GPIO, BQ_SCL_GPIO);
  Wire.setClock(400000);
  uint16_t soc = bq27220ReadU16(BQ_REG_SOC);
  if (soc == 0xFFFF || soc > 100) return 0xFF;
  return (uint8_t)soc;
}

// Bundle cache: the entire /data/all response is stored as one NVS blob. This
// is small enough (binary-encoded, ~3-5KB even with many panels) that a
// single put+get is atomic and easily fits the partition.
constexpr const char* kBundleKey = "bundle";

// Forward decl — defined just below fetchAndCacheBundleFromWorker.
static bool cacheSealedBundle(const uint8_t* sealedBuf, size_t sealedLen,
                              const uint8_t x3_sk[32], const uint8_t x3_pk[32],
                              const char* uploaded_at, const char* source);

// Pull the sealed bundle from the public Worker, decrypt with the device's
// X25519 private key, validate the header, and store the plaintext in NVS.
//
// Notes on TLS: WiFiClientSecure runs in insecure mode (no cert validation).
// The bundle has end-to-end authenticated encryption — a MITM can't produce
// a payload that decrypts (Poly1305 fail-closed), so confidentiality and
// integrity hold regardless. The bearer token they could harvest only grants
// read of the same encrypted blob.
static bool fetchAndCacheBundleFromWorker(const String& workerUrl,
                                          const String& bearer,
                                          const uint8_t x3_sk[32],
                                          const uint8_t x3_pk[32]) {
  if (workerUrl.length() == 0 || bearer.length() == 0) {
    Log.println("worker: URL or bearer not configured; skipping fetch");
    return false;
  }

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(secure, workerUrl)) {
    Log.println("worker: http begin failed");
    return false;
  }
  http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Accept", "application/octet-stream");
  http.setUserAgent("einkcharts-x3/1");
  const char* collect[] = {"Date", "X-Uploaded-At"};
  http.collectHeaders(collect, 2);

  int code = http.GET();
  Log.printf("worker: GET %s -> %d\n", workerUrl.c_str(), code);
  if (code != 200) {
    http.end();
    return false;
  }

  int sealedLen = http.getSize();
  if (sealedLen <= (int)bundle_seal::OVERHEAD_BYTES || sealedLen > 32 * 1024) {
    Log.printf("worker: bad sealed length %d\n", sealedLen);
    http.end();
    return false;
  }
  uint8_t* sealedBuf = (uint8_t*)malloc(sealedLen);
  if (!sealedBuf) {
    Log.printf("worker: malloc(%d) failed\n", sealedLen);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint32_t got = 0;
  uint32_t lastProgress = millis();
  while ((int)got < sealedLen && http.connected()) {
    if (stream->available()) {
      int n = stream->readBytes(sealedBuf + got, sealedLen - got);
      if (n > 0) { got += n; lastProgress = millis(); }
    } else if (millis() - lastProgress > 5000) {
      Log.println("worker: read stalled");
      break;
    } else {
      delay(1);
    }
  }
  String dateHdr = http.header("Date");
  String uploadedAt = http.header("X-Uploaded-At");
  http.end();
  if (dateHdr.length()) setClockFromHttpDate(dateHdr);
  if ((int)got != sealedLen) {
    Log.printf("worker: short read %u/%d\n", got, sealedLen);
    free(sealedBuf);
    return false;
  }

  bool ok = cacheSealedBundle(sealedBuf, sealedLen, x3_sk, x3_pk,
                              uploadedAt.length() ? uploadedAt.c_str() : nullptr,
                              /*source=*/"worker");
  free(sealedBuf);
  return ok;
}

// Decrypt a sealed bundle into plaintext, validate the header, and write
// it to NVS as the new cached bundle. Shared between the worker fetch and
// BLE fallback paths.
static bool cacheSealedBundle(const uint8_t* sealedBuf, size_t sealedLen,
                              const uint8_t x3_sk[32], const uint8_t x3_pk[32],
                              const char* uploaded_at, const char* source) {
  size_t plaintextCap = sealedLen - bundle_seal::OVERHEAD_BYTES;
  uint8_t* plaintext = (uint8_t*)malloc(plaintextCap);
  if (!plaintext) {
    Log.printf("%s: malloc(%u) for plaintext failed\n", source, (unsigned)plaintextCap);
    return false;
  }
  int plaintextLen = bundle_seal::unseal(x3_sk, x3_pk, sealedBuf, sealedLen,
                                         plaintext, plaintextCap);
  if (plaintextLen < 0) {
    Log.printf("%s: decrypt FAILED (wrong key, wire format mismatch, or tampered)\n",
               source);
    free(plaintext);
    return false;
  }
  Log.printf("%s: decrypted %d bytes (uploaded_at=%s)\n",
             source, plaintextLen, uploaded_at ? uploaded_at : "?");

  if (plaintextLen < 4) {
    Log.printf("%s: plaintext too short for header\n", source);
    free(plaintext);
    return false;
  }
  uint16_t magic = plaintext[0] | ((uint16_t)plaintext[1] << 8);
  uint8_t version = plaintext[2];
  uint8_t panelCount = plaintext[3];
  if (magic != BUNDLE_MAGIC || version != BUNDLE_VERSION_REQUIRED || panelCount == 0) {
    Log.printf("%s: bad bundle header magic=0x%04x ver=%u count=%u\n",
               source, magic, version, panelCount);
    free(plaintext);
    return false;
  }

  Preferences prefs;
  if (!prefs.begin("x3-cache", false)) {
    Log.printf("%s: NVS open failed\n", source);
    free(plaintext);
    return false;
  }
  prefs.remove(kBundleKey);
  size_t wrote = prefs.putBytes(kBundleKey, plaintext, plaintextLen);
  prefs.putUInt("total", panelCount);
  size_t freeEntries = prefs.freeEntries();
  prefs.end();
  free(plaintext);
  Log.printf("%s: NVS putBytes wrote=%u/%d freeEntries=%u\n",
             source, (unsigned)wrote, plaintextLen, (unsigned)freeEntries);
  if (wrote != (size_t)plaintextLen) {
    Log.printf("%s: NVS write FAILED — keeping prior cache\n", source);
    return false;
  }
  rtcPanelTotal = panelCount;
  Log.printf("%s: cached %d bytes, %u panels\n", source, plaintextLen, panelCount);
  return true;
}

// Best-effort: post the current BQ27220 voltage to the Worker so the Pi
// can read it back and synthesize a battery panel into the next bundle.
// Failure here doesn't fail the cycle — the X3 doesn't need the response.
static void postBatteryReadingToWorker(const String& workerUrl,
                                       const String& bearer,
                                       uint16_t batteryMv) {
  if (workerUrl.length() == 0 || bearer.length() == 0 || batteryMv == 0) return;

  // Derive battery URL from the bundle URL: replace the trailing /bundle
  // with /battery so the operator only has to configure one base URL.
  String batteryUrl = workerUrl;
  int slash = batteryUrl.lastIndexOf('/');
  if (slash >= 0) batteryUrl = batteryUrl.substring(0, slash) + "/battery";

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(secure, batteryUrl)) {
    Log.println("battery: http begin failed");
    return;
  }
  http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Content-Type", "application/json");
  http.setUserAgent("einkcharts-x3/1");

  char body[32];
  snprintf(body, sizeof(body), "{\"mv\":%u}", (unsigned)batteryMv);
  int code = http.PUT((uint8_t*)body, strlen(body));
  Log.printf("battery: PUT %s body=%s -> %d\n", batteryUrl.c_str(), body, code);
  http.end();
}

// Read the per-panel default view byte from the cached bundle (v3+ layout).
// Returns VIEW_24H if cache is empty or panel doesn't have an override.
// Note: Preferences.getBytes() refuses partial reads — if maxLen < blob size
// it returns 0 and leaves the buffer untouched. So we always allocate exactly
// the blob length even when we only need a couple of bytes.
static uint8_t defaultViewForPanel(uint32_t idx) {
  Preferences prefs;
  if (!prefs.begin("x3-cache", true)) return VIEW_24H;
  size_t len = prefs.getBytesLength(kBundleKey);
  if (len < 8) { prefs.end(); return VIEW_24H; }
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { prefs.end(); return VIEW_24H; }
  size_t got = prefs.getBytes(kBundleKey, buf, len);
  prefs.end();
  if (got != len) { free(buf); return VIEW_24H; }

  uint16_t magic = buf[0] | ((uint16_t)buf[1] << 8);
  uint8_t version = buf[2];
  uint8_t panelCount = buf[3];
  if (magic != BUNDLE_MAGIC || version != BUNDLE_VERSION_REQUIRED || idx >= panelCount) {
    free(buf);
    return VIEW_24H;
  }
  // Default-view table: header (8) + offset_table (12 * count) + idx.
  uint32_t pos = 8 + 12 * panelCount + idx;
  if (pos >= len) { free(buf); return VIEW_24H; }
  uint8_t v = buf[pos];
  free(buf);
  return (v <= VIEW_7D) ? v : VIEW_24H;
}

static bool loadCachedScreen(uint32_t idx, uint8_t viewMode, ScreenData& out) {
  Preferences prefs;
  if (!prefs.begin("x3-cache", true)) return false;
  size_t len = prefs.getBytesLength(kBundleKey);
  if (len < 8) { prefs.end(); return false; }
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { prefs.end(); return false; }
  prefs.getBytes(kBundleKey, buf, len);
  uint32_t total = prefs.getUInt("total", 0);
  prefs.end();
  if (total > 0) rtcPanelTotal = total;

  uint16_t magic = buf[0] | ((uint16_t)buf[1] << 8);
  uint8_t version = buf[2];
  uint8_t screenCount = buf[3];
  if (magic != BUNDLE_MAGIC || version != BUNDLE_VERSION_REQUIRED || idx >= screenCount) {
    Log.printf("cache: invalid blob magic=0x%04x ver=%u idx=%u/%u\n",
                  magic, version, (unsigned)idx, (unsigned)screenCount);
    free(buf);
    return false;
  }
  uint32_t entry = 8 + idx * 12;
  auto readU32 = [&](uint32_t pos) -> uint32_t {
    return (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
           ((uint32_t)buf[pos + 2] << 16) | ((uint32_t)buf[pos + 3] << 24);
  };
  uint32_t ofs24 = readU32(entry);
  uint32_t ofs2  = readU32(entry + 4);
  uint32_t ofs7  = readU32(entry + 8);
  uint32_t target = (viewMode == VIEW_2H) ? ofs2
                  : (viewMode == VIEW_7D) ? ofs7
                                          : ofs24;
  if (target >= len) {
    Log.printf("cache: bad offset %u (len=%u)\n", (unsigned)target, (unsigned)len);
    free(buf);
    return false;
  }
  hydrateScreen(buf, len, target, out);
  if (out.type == SCREEN_STAT_GROUP) {
    Log.printf("cache: screen idx=%u view=%s STAT_GROUP n=%u\n",
                  (unsigned)idx, viewLabel(viewMode), (unsigned)out.stat_count);
  } else {
    Log.printf("cache: screen idx=%u view=%s CHART title='%s' points=%u\n",
                  (unsigned)idx, viewLabel(viewMode), out.chart.title, out.chart.point_count);
  }
  free(buf);
  return true;
}

// Sync wall clock once per WiFi cycle so we can detect when 15 min has passed
// since the last successful bundle fetch. RTC keeps time across deep sleep.
// Set the wall clock from an HTTP "Date" header. RFC 7231 fixed format:
//   "Sun, 06 Nov 1994 08:49:37 GMT"
// Saves the ~5s NTP roundtrip — every bridge response carries the time.
static bool setClockFromHttpDate(const String& dateHdr) {
  if (dateHdr.length() < 25) return false;
  struct tm tm = {};
  if (!strptime(dateHdr.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm)) return false;
  // No timegm in newlib — temporarily switch TZ to UTC so mktime treats tm
  // as a UTC moment, then restore.
  String savedTz = getenv("TZ") ? getenv("TZ") : "";
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(&tm);
  if (savedTz.length() > 0) setenv("TZ", savedTz.c_str(), 1);
  else unsetenv("TZ");
  tzset();
  if (t <= 1700000000) return false;
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  Log.printf("clock: set from HTTP Date → epoch=%lld local=%02d:%02d\n",
                (long long)t, lt.tm_hour, lt.tm_min);
  return true;
}

// ---------------------------------------------------------------------------
// List mode: long-press POWER while viewing any panel pops a 2-column list of
// every screen's title with a cursor. UP/DOWN move the cursor (read from the
// ADC button cluster on GPIO 1); POWER selects + exits; 30s of inactivity
// auto-selects. The CPU stays awake the whole time so polling is instant —
// power cost is acceptable because list mode is intended to be brief.

constexpr uint32_t MAX_LIST_TITLES = 20;
static char g_list_title_buf[MAX_LIST_TITLES][48];
static const char* g_list_titles[MAX_LIST_TITLES];

// Walk the cached bundle and extract one title per screen. For chart screens
// that's the panel title; for stat-group screens we use the first stat's
// title (which is what the user sees as the "primary" stat).
static uint32_t gatherTitlesFromCache(uint32_t& cursor_idx) {
  Preferences prefs;
  if (!prefs.begin("x3-cache", true)) return 0;
  size_t len = prefs.getBytesLength(kBundleKey);
  if (len < 8) { prefs.end(); return 0; }
  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { prefs.end(); return 0; }
  prefs.getBytes(kBundleKey, buf, len);
  prefs.end();

  uint16_t magic = buf[0] | ((uint16_t)buf[1] << 8);
  uint8_t version = buf[2];
  uint8_t screenCount = buf[3];
  if (magic != BUNDLE_MAGIC || version != BUNDLE_VERSION_REQUIRED) {
    free(buf);
    return 0;
  }
  auto readU32 = [&](uint32_t pos) -> uint32_t {
    return (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
           ((uint32_t)buf[pos + 2] << 16) | ((uint32_t)buf[pos + 3] << 24);
  };
  uint32_t produced = 0;
  for (uint32_t i = 0; i < screenCount && produced < MAX_LIST_TITLES; i++) {
    uint32_t ofs = readU32(8 + i * 12);  // 24h offset
    if (ofs + 4 >= len) break;
    // Screen block: u8 type, u8 entry_count, then per entry: u8 entry_type
    // followed by entry payload (first field of which is always a pstr title).
    uint32_t p = ofs + 2;                 // skip type + entry_count
    if (p >= len) break;
    p++;                                  // skip entry_type
    if (p >= len) break;
    uint8_t tn = buf[p++];
    if (p + tn > len) break;
    size_t take = (tn < sizeof(g_list_title_buf[produced]) - 1)
                      ? tn : sizeof(g_list_title_buf[produced]) - 1;
    memcpy(g_list_title_buf[produced], buf + p, take);
    g_list_title_buf[produced][take] = 0;
    g_list_titles[produced] = g_list_title_buf[produced];
    produced++;
  }
  free(buf);

  // Synthesize the trailing "Device Logs" entry. Not part of the bundle —
  // a virtual panel backed by the RTC log ring buffer.
  if (produced < MAX_LIST_TITLES) {
    constexpr const char kLogsTitle[] = "Device Logs";
    size_t n = sizeof(kLogsTitle) - 1;
    memcpy(g_list_title_buf[produced], kLogsTitle, n);
    g_list_title_buf[produced][n] = 0;
    g_list_titles[produced] = g_list_title_buf[produced];
    produced++;
  }

  if (cursor_idx >= produced) cursor_idx = 0;
  return produced;
}

enum AdcButton { ADC_BTN_NONE, ADC_BTN_UP, ADC_BTN_DOWN, ADC_BTN_BACK, ADC_BTN_OK };

// One raw analogRead on the button-ladder pin → bucketed button identity.
// Returns ADC_BTN_NONE for the idle voltage.
static AdcButton readAdcButton() {
  int v = analogRead(BTN_ADC_PIN);
  if (v >= ADC_NO_BUTTON) return ADC_BTN_NONE;
  if (v >= ADC_UP_MIN)    return ADC_BTN_UP;
  if (v >= ADC_DOWN_MIN)  return ADC_BTN_DOWN;
  if (v >= ADC_BACK_MIN)  return ADC_BTN_BACK;
  return ADC_BTN_OK;
}

// Render the list view and run the active-mode loop until either the user
// selects a panel (returns true with `out_idx` set), the back button exits
// without changing (returns false), or the 30s timeout fires (returns true
// with the current cursor position).
static bool runListMode(uint32_t& out_idx) {
  uint32_t titleCount = gatherTitlesFromCache(out_idx);
  if (titleCount == 0) return false;

  // If we entered list mode while already on the synthetic logs screen,
  // place the cursor on its row so the user sees what they came from.
  uint32_t cursor;
  if (rtcPanelIdx == LOGS_PANEL_IDX) {
    cursor = titleCount - 1;
  } else {
    cursor = (rtcPanelIdx < titleCount) ? rtcPanelIdx : 0;
  }
  out_idx = cursor;

  // Initial draw — full refresh so the prior panel's pixels get scrubbed.
  drawListView(display.getFrameBuffer(), g_list_titles, titleCount, cursor);
  display.requestResync();
  display.displayBuffer(EInkDisplay::FULL_REFRESH, /*turnOffScreen=*/false);

  // Configure ADC pin (external resistor ladder sets the idle voltage, so
  // no internal pull-up).
  pinMode(BTN_ADC_PIN, INPUT);
  analogSetAttenuation(ADC_11db);

  uint32_t last_input_ms = millis();
  AdcButton last_adc = ADC_BTN_NONE;
  bool last_pwr_pressed = false;
  bool selected = true;       // assume we'll select on timeout / POWER

  while (true) {
    delay(LIST_MODE_POLL_MS);

    // Inactivity timeout — pick whatever's under the cursor.
    if (millis() - last_input_ms > LIST_MODE_TIMEOUT_MS) break;

    // POWER button — edge detect on press.
    bool pwr_now = (digitalRead(POWER_BUTTON_GPIO) == LOW);
    if (pwr_now && !last_pwr_pressed) {
      // Wait for release so the post-loop deepSleep doesn't immediately re-fire.
      uint32_t t = millis();
      while (digitalRead(POWER_BUTTON_GPIO) == LOW && millis() - t < MAX_PRESS_HOLD_MS) delay(10);
      break;
    }
    last_pwr_pressed = pwr_now;

    // ADC buttons — edge detect on transition from NONE.
    // Both rockers' top halves move the cursor up; both bottom halves move
    // it down. Selection is exclusively via the POWER button (or 30s timeout).
    // The gist's UP/DOWN/BACK/OK labels are orientation-dependent — on this
    // device the upper rocker's halves landed in the UP/DOWN buckets and the
    // lower rocker's in the BACK/OK buckets, with BACK = top, OK = bottom.
    AdcButton b = readAdcButton();
    if (b != last_adc && b != ADC_BTN_NONE) {
      last_input_ms = millis();
      bool moved = false;
      if (b == ADC_BTN_BACK || b == ADC_BTN_OK) {
        cursor = (cursor == 0) ? titleCount - 1 : cursor - 1;
        moved = true;
      } else if (b == ADC_BTN_UP || b == ADC_BTN_DOWN) {
        cursor = (cursor + 1) % titleCount;
        moved = true;
      }
      if (moved) {
        drawListView(display.getFrameBuffer(), g_list_titles, titleCount, cursor);
        display.displayBuffer(EInkDisplay::FAST_REFRESH, /*turnOffScreen=*/false);
      }
    }
    last_adc = b;
  }

  // The last entry in the title list is always the synthesized "Device
  // Logs" item. If that's what the user picked, return LOGS_PANEL_IDX
  // (the sentinel main.cpp uses to skip bundle-cache load).
  if (cursor == titleCount - 1) {
    out_idx = LOGS_PANEL_IDX;
  } else {
    out_idx = cursor;
  }
  return selected;
}

void setup() {
  Serial.begin(115200);

  // Dump the persisted RTC log buffer immediately. Anyone with a serial
  // monitor attached gets context from previous wakes; the divider lets
  // them distinguish history from this cycle's fresh output. Bypasses
  // Log so the dump itself doesn't get re-buffered (which would double
  // the buffer's history every cycle).
  {
    static uint8_t dumpBuf[log_buffer::SIZE];
    size_t dumpLen = log_buffer::snapshot(dumpBuf);
    Serial.println();
    Serial.println("=== persisted log buffer (RTC, since last cold boot) ===");
    if (dumpLen > 0) Serial.write(dumpBuf, dumpLen);
    Serial.println("=== end persisted log buffer; new lines below ===");
  }

  // Re-enable the battery rail before anything else — the MOSFET hold from
  // the previous deep sleep needs to be released and re-asserted.
  enableBatteryRail();

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const bool buttonWake = (wakeCause == ESP_SLEEP_WAKEUP_GPIO);

  // Capture press direction IMMEDIATELY on button wake — user may release the
  // button before our 3-second flash-recovery delay ends.
  NavAction pendingNav = NAV_NONE;
  if (buttonWake) {
    pendingNav = readPressPattern();
  }

  // Apply TZ on every wake — the env var doesn't survive deep sleep but the
  // RTC wall clock does. Without this, localtime() returns UTC and the quiet-
  // hours window slides 7-8 hours.
  setenv("TZ", LOCAL_TZ, 1);
  tzset();

  esp_reset_reason_t resetReason = esp_reset_reason();
  Log.printf("\n=== boot cycle=%u wake_cause=%d reset_reason=%d nav=%d ===\n",
                rtcCycleCount, (int)wakeCause, (int)resetReason, (int)pendingNav);

  Log.printf("rtcMagic=0x%08x (want 0x%08x) demoIdx=%u\n",
                (unsigned)rtcMagic, (unsigned)RTC_MAGIC_VALUE, (unsigned)rtcDemoIdx);

  Preferences prefs;
  prefs.begin("grafana-x3", true);  // read-only
  String workerUrl = prefs.getString("worker_url", DEFAULT_WORKER_URL);
  String workerBearer = prefs.getString("worker_bearer", DEFAULT_WORKER_BEARER);
  prefs.end();

  // Configured WiFi networks in priority order. Loaded once per boot; only
  // consumed if we actually need to fetch this cycle.
  std::vector<wifi_config::Network> wifiNets = wifi_config::load();

  // Flash-recovery window — give esptool a quiet boot phase to land its
  // reset gesture. Skip on a button-wake so navigation stays responsive;
  // user can replug USB to get the full window for re-flashing.
  if (!buttonWake) {
    delay(FLASH_RECOVERY_WINDOW_MS);
  }

  display.setDisplayX3();
  display.begin();
  // If the controller's RED RAM was populated on a previous cycle, trust it
  // so the SDK does a differential refresh (only changed pixels) instead of
  // its default forced full sync at boot.
  if (rtcEpdRamValid) {
    Log.println("epd: trusting prior RED RAM contents → differential refresh");
    display.markRedRamSynced();
  }
  // New firmware? Old build's pixels are still on screen and in EPD RAM 0x10.
  // Force a one-shot full refresh so the ghost gets scrubbed cleanly.
  const bool firmwareChanged = (rtcFirmwareTag != kFirmwareTag);
  if (firmwareChanged) {
    Log.printf("firmware change: %08x -> %08x → forcing full refresh + cache wipe\n",
                  (unsigned)rtcFirmwareTag, (unsigned)kFirmwareTag);
    display.requestResync();
    rtcFirmwareTag = kFirmwareTag;
    // Wipe the bundle cache — its binary layout may have changed in the new
    // build (version bytes, offset table size, etc). Next wake forces a
    // fresh fetch.
    {
      Preferences cprefs;
      if (cprefs.begin("x3-cache", false)) {
        cprefs.clear();
        cprefs.end();
      }
    }
    rtcLastFetchEpoch = 0;
    rtcPanelTotal = 0;
  }

  // ---------------------------------------------------------------------------
  // Enrollment: ensure we have an X25519 keypair before doing anything else.
  // On first ever boot, generate one and show a QR + base64 text so the user
  // can paste the pubkey into the Pi push config. Private key never leaves
  // NVS. On subsequent boots, just load the keypair and log the pubkey to
  // serial (so re-extracting is a `pio device monitor` away).
  uint8_t x3_sk[x25519_keystore::KEY_LEN];
  uint8_t x3_pk[x25519_keystore::KEY_LEN];
  (void)x3_sk;  // Used by the decrypt path (Task #5); silence unused warning.
  bool keypair_loaded = x25519_keystore::exists() &&
                        x25519_keystore::load(x3_sk, x3_pk);
  if (!keypair_loaded) {
    Log.println("enroll: no keypair in NVS — generating");
    if (x25519_keystore::generate_and_store(x3_sk, x3_pk)) {
      char pk_b64[48];
      x25519_keystore::b64url_encode(x3_pk, x25519_keystore::KEY_LEN, pk_b64, sizeof(pk_b64));
      Log.printf("enroll: X3_PUBKEY_B64=%s\n", pk_b64);
      enroll_screen::render(display.getFrameBuffer(), x3_pk);
      display.requestResync();
      display.displayBuffer(EInkDisplay::FULL_REFRESH, /*turnOffScreen=*/true);
      rtcEpdRamValid = true;
      // Park here. User scans QR + updates Pi config at their leisure; any
      // button press wakes us, and on the next boot the keypair exists and
      // we fall through to the normal render path.
      Log.println("enroll: sleeping 24h (or until power button wake)");
      deepSleep(24ULL * 3600ULL);
    } else {
      Log.println("enroll: KEYGEN FAILED — continuing without crypto");
    }
  } else {
    char pk_b64[48];
    x25519_keystore::b64url_encode(x3_pk, x25519_keystore::KEY_LEN, pk_b64, sizeof(pk_b64));
    Log.printf("crypto: X3_PUBKEY_B64=%s\n", pk_b64);

    // After a fresh flash, show the QR for up to 30s so the operator can
    // re-scan or re-copy the pubkey. Power-button press exits early.
    if (firmwareChanged) {
      Log.println("enroll: post-flash QR display (30s or power-button skip)");
      enroll_screen::render(display.getFrameBuffer(), x3_pk);
      display.requestResync();
      display.displayBuffer(EInkDisplay::FULL_REFRESH, /*turnOffScreen=*/false);
      rtcEpdRamValid = true;

      pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);
      uint32_t start = millis();
      while (millis() - start < 30000) {
        if (digitalRead(POWER_BUTTON_GPIO) == LOW) {
          // Wait for release so the post-loop flow doesn't read it again.
          uint32_t pressStart = millis();
          while (digitalRead(POWER_BUTTON_GPIO) == LOW &&
                 millis() - pressStart < MAX_PRESS_HOLD_MS) {
            delay(10);
          }
          delay(BUTTON_DEBOUNCE_MS);
          Log.println("enroll: skipped by power button");
          break;
        }
        delay(50);
      }
    }
  }

#if DEMO_MODE
  const bool freshBoot = (rtcMagic != RTC_MAGIC_VALUE);
  rtcMagic = RTC_MAGIC_VALUE;
  Log.printf("demo: freshBoot=%d (idx before action=%u)\n",
                (int)freshBoot, (unsigned)rtcDemoIdx);

  if (freshBoot) {
    rtcDemoIdx = 0;
  } else if (buttonWake) {
    if (pendingNav == NAV_FORWARD) {
      rtcDemoIdx = (rtcDemoIdx + 1) % kDemoPanelCount;
      rtcViewMode = VIEW_24H;
      Log.println("demo: single press → forward");
    } else if (pendingNav == NAV_TOGGLE_ZOOM) {
      rtcViewMode = (rtcViewMode + 1) % 3;
      Log.printf("demo: double-click → view=%s\n", viewLabel(rtcViewMode));
    } else if (pendingNav == NAV_ENTER_LIST) {
      Log.println("demo: long press → list mode (no-op in demo)");
    } else {
      Log.println("demo: button wake but press not classified — redraw");
    }
  } else {
    Log.printf("demo: non-GPIO wake (cause=%d) — redraw\n", (int)wakeCause);
  }
  Log.printf("demo: drawing dashboard idx=%u\n", (unsigned)rtcDemoIdx);

  PanelData demoP = *kDemoPanels[rtcDemoIdx % kDemoPanelCount];
  demoP.view_label = viewLabel(rtcViewMode);
  drawPanel(display.getFrameBuffer(), demoP);
  drawStatusOverlay(rtcCycleCount, /*wifiOk=*/true, /*fetchOk=*/true);
  bool wantFull = firmwareChanged || (rtcCycleCount % FULL_REFRESH_EVERY) == 0;
  if (wantFull) display.requestResync();
  Log.printf("displayBuffer(refresh=%s) starting\n", wantFull ? "FULL" : "FAST");
  Serial.flush();
  display.displayBuffer(wantFull ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH,
                        /*turnOffScreen=*/true);
  Log.println("displayBuffer done");
  rtcEpdRamValid = true;
  rtcCycleCount++;
  deepSleep(600);  // 10-min safety timer + power-button wake
#endif

  // Restore the cached panel count if we know it (from a prior bundle fetch).
  {
    Preferences cprefs;
    if (cprefs.begin("x3-cache", true)) {
      uint32_t t = cprefs.getUInt("total", 0);
      if (t > 0) rtcPanelTotal = t;
      cprefs.end();
    }
  }

  // Apply nav action immediately — these always operate on the cache and
  // never touch WiFi, so the response is instant.
  if (buttonWake && rtcPanelTotal > 0) {
    if (pendingNav == NAV_FORWARD) {
      // Forward-press never lands on the logs screen — if we're there,
      // jump back to the first bundle panel. Otherwise cycle modulo the
      // bundle count.
      if (rtcPanelIdx >= rtcPanelTotal) {
        rtcPanelIdx = 0;
      } else {
        rtcPanelIdx = (rtcPanelIdx + 1) % rtcPanelTotal;
      }
      // Switching panels resets to the panel's configured default view —
      // either Grafana's timeFrom override (7d for soil/lawn) or 24h.
      rtcViewMode = defaultViewForPanel(rtcPanelIdx);
      Log.printf("nav: forward → idx=%u view=%s\n",
                    (unsigned)rtcPanelIdx, viewLabel(rtcViewMode));
    } else if (pendingNav == NAV_TOGGLE_ZOOM) {
      // No view modes on the logs screen — leave it alone there.
      if (rtcPanelIdx != LOGS_PANEL_IDX) {
        // Cycle 24h → 2h → 7d → 24h.
        rtcViewMode = (rtcViewMode + 1) % 3;
        Log.printf("nav: double-click → view %s\n", viewLabel(rtcViewMode));
      }
    }
    // NAV_ENTER_LIST handled later in setup() — needs the rendered cache
    // to be loaded so we can show all titles in the list view.
  }

  // Decide whether this wake needs a WiFi cycle. Rule: WiFi only on timer
  // wake (or first boot), and only every currentRefreshInterval(). Button
  // wakes never trigger a fetch — they serve from cache exclusively.
  // Quiet hours suppress background refreshes between 22:00 and 06:00 local
  // to save battery overnight; the next 06:00 wake catches up.
  time_t nowEpoch = time(nullptr);
  bool haveValidClock = (nowEpoch > 1700000000);
  bool quiet = haveValidClock && inQuietHours();
  // Wake cadence is per-view (5/15/60 min) — that's the user's preferred
  // freshness. The per-network floor applies AFTER we know which network we
  // got: it can cancel an individual fetch but doesn't stretch the wake
  // interval. So a user in 2h view at home gets fetches every 5 min; on
  // phone hotspot, they still wake every 5 min (cheap WiFi check) but only
  // fetch once an hour. When home WiFi comes back into range, the very next
  // wake will fetch — no waiting for a network-tied long sleep to elapse.
  bool needFetch = false;
  if (!buttonWake && !quiet) {
    if (rtcLastFetchEpoch == 0 || !haveValidClock) {
      needFetch = true;
    } else {
      time_t elapsed = nowEpoch - rtcLastFetchEpoch;
      needFetch = (elapsed >= (time_t)currentRefreshInterval());
      Log.printf("refresh: %llds since last fetch (view=%s interval=%llus) → try=%d\n",
                    (long long)elapsed, viewLabel(rtcViewMode),
                    (unsigned long long)currentRefreshInterval(), (int)needFetch);
    }
  } else if (quiet) {
    Log.println("quiet hours: skipping background fetch");
  }

  bool wifiOk = false;
  bool fetchOk = false;
  if (needFetch) {
    // Pre-warm the WiFi radio BEFORE painting the splash. Setting the
    // mode here kicks off async driver init (the STA_START event fires
    // a few hundred ms later); the e-ink refresh that follows takes
    // ~800 ms, so by the time connect_any() runs the radio is settled.
    // Without this prewarm, the first post-flash boot races driver init
    // and the initial WiFi.begin() can come back as a hard "no AP" even
    // though it would succeed a second later.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    if (rtcLastFetchEpoch == 0) {
      // First boot splash. With multi-network we don't know which one will
      // connect — show the first configured ssid (best guess) or a generic.
      paintConnectingScreen(
          wifiNets.empty() ? String("(no WiFi configured)") : wifiNets[0].ssid,
          rtcCycleCount);
      g_splashPainted = true;
    }
    wifi_config::ConnectResult wc =
        wifi_config::connect_any(wifiNets, WIFI_CONNECT_TIMEOUT_MS);
    wifiOk = wc.ok;
    if (wifiOk) {
      paintBootStatus("Connected. Fetching bundle…");
      // Per-network floor check: skip the actual fetch if this network's
      // minimum hasn't elapsed yet (e.g. on phone hotspot with 1h floor and
      // only 5 min since last fetch). Note: still costs a connect attempt,
      // which is the price of being responsive to home-WiFi returning.
      time_t elapsedSinceFetch = (rtcLastFetchEpoch == 0)
                                     ? INT32_MAX
                                     : (time(nullptr) - rtcLastFetchEpoch);
      bool floorMet = elapsedSinceFetch >= (time_t)wc.min_refresh_sec;
      if (floorMet) {
        fetchOk = fetchAndCacheBundleFromWorker(workerUrl, workerBearer, x3_sk, x3_pk);
        if (fetchOk) {
          rtcLastFetchEpoch = time(nullptr);
          paintBootStatus("Got bundle. Rendering…");
          // Piggyback battery telemetry while WiFi is still up.
          uint16_t batteryMv = readBatteryMv();
          postBatteryReadingToWorker(workerUrl, workerBearer, batteryMv);
        } else {
          paintBootStatus("Fetch/decrypt failed");
        }
      } else {
        Log.printf("wifi: '%s' floor not met (%llds < %us); skipping fetch this cycle\n",
                      wc.ssid.c_str(), (long long)elapsedSinceFetch,
                      (unsigned)wc.min_refresh_sec);
      }
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    } else {
      // All configured WiFi networks failed — fall back to BLE so the
      // iOS companion app can hand off a fresh bundle. Tear down WiFi
      // first since ESP32-C3 shares the 2.4 GHz radio.
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      paintBootStatus("No Wi-Fi. Waiting for phone over Bluetooth…");
      // 32 KB allocation lives only across this fallback path; heap is
      // safer than the stack given the size.
      uint8_t* sealedBuf = (uint8_t*)malloc(ble_bundle_receiver::MAX_SEALED_BYTES);
      if (sealedBuf) {
        size_t sealedLen = 0;
        if (ble_bundle_receiver::wait_for_bundle(
                sealedBuf, ble_bundle_receiver::MAX_SEALED_BYTES,
                &sealedLen, BLE_WAIT_TIMEOUT_MS)) {
          paintBootStatus("Got bundle over Bluetooth. Rendering…");
          if (cacheSealedBundle(sealedBuf, sealedLen, x3_sk, x3_pk,
                                /*uploaded_at=*/nullptr, /*source=*/"ble")) {
            fetchOk = true;
            rtcLastFetchEpoch = time(nullptr);
          }
        } else {
          paintBootStatus("No phone reachable over Bluetooth");
        }
        free(sealedBuf);
      } else {
        Log.println("ble: malloc(MAX_SEALED_BYTES) failed");
      }
    }
  }

  // Long-press POWER → list-mode UI. Runs to completion (sync, stays awake)
  // and returns the user's pick before we fall through to the panel render.
  // Forces a full refresh on the post-list-mode redraw so the panel's pixels
  // overwrite the list cleanly.
  bool justExitedListMode = false;
  if (buttonWake && pendingNav == NAV_ENTER_LIST && rtcPanelTotal > 0) {
    uint32_t picked = rtcPanelIdx;
    bool changed = runListMode(picked);
    if (changed) {
      // runListMode returns LOGS_PANEL_IDX when the user picks the "Device
      // Logs" entry (synthesized at the end of the title list); we accept
      // that as a valid index even though it's > rtcPanelTotal.
      if (picked == LOGS_PANEL_IDX || picked < rtcPanelTotal) {
        // Reset scroll position when freshly entering the logs screen.
        if (picked == LOGS_PANEL_IDX && rtcPanelIdx != LOGS_PANEL_IDX) {
          rtcLogsScrollOffset = 0;
        }
        rtcPanelIdx = picked;
        if (picked != LOGS_PANEL_IDX) {
          rtcViewMode = defaultViewForPanel(rtcPanelIdx);
        }
      }
    }
    justExitedListMode = true;
  }

  // Render the current screen. For LOGS_PANEL_IDX we don't touch the
  // bundle cache — the data lives in the RTC log ring buffer.
  ScreenData screen = {};
  bool haveCache = false;
  bool showLogs = (rtcPanelIdx == LOGS_PANEL_IDX);
  if (!showLogs) {
    haveCache = loadCachedScreen(rtcPanelIdx, rtcViewMode, screen);
  }
  const char* vl = viewLabel(rtcViewMode);
  if (showLogs) {
    // Initial render at the current scroll position.
    int max_offset = drawLogsScreen(display.getFrameBuffer(), rtcLogsScrollOffset);
    drawStatusOverlay(rtcCycleCount, wifiOk || !needFetch, fetchOk || !needFetch);
    display.requestResync();
    display.displayBuffer(EInkDisplay::FULL_REFRESH, /*turnOffScreen=*/false);
    rtcEpdRamValid = true;

    // Interactive paging. POWER exits (scroll position preserved); 30 s of
    // inactivity also exits. One "page" = ~1/3 of the visible window so
    // 2/3 of the prior content stays on screen for context. Use the actual
    // font line-height so the math matches what drawLogsScreen renders.
    const int line_h = fbGfxLineHeight(&FreeSans9pt7b);
    const int rows_per_screen = (FB_HEIGHT - 16) / line_h;
    const int page_lines = rows_per_screen > 3 ? rows_per_screen / 3 : 1;
    pinMode(BTN_ADC_PIN, INPUT);
    analogSetAttenuation(ADC_11db);
    uint32_t last_input_ms = millis();
    AdcButton last_adc = ADC_BTN_NONE;
    bool last_pwr_pressed = false;
    while (true) {
      delay(LIST_MODE_POLL_MS);
      if (millis() - last_input_ms > LIST_MODE_TIMEOUT_MS) break;

      bool pwr_now = (digitalRead(POWER_BUTTON_GPIO) == LOW);
      if (pwr_now && !last_pwr_pressed) {
        uint32_t t = millis();
        while (digitalRead(POWER_BUTTON_GPIO) == LOW &&
               millis() - t < MAX_PRESS_HOLD_MS) delay(10);
        break;
      }
      last_pwr_pressed = pwr_now;

      AdcButton b = readAdcButton();
      if (b != last_adc && b != ADC_BTN_NONE) {
        last_input_ms = millis();
        int new_offset = rtcLogsScrollOffset;
        // Same mapping as the list view: lower rocker (BACK or OK) goes
        // "back" — toward older logs / larger offset. Upper rocker
        // (UP or DOWN) goes forward — toward newer logs / smaller offset.
        if (b == ADC_BTN_BACK || b == ADC_BTN_OK) {
          new_offset += page_lines;
        } else if (b == ADC_BTN_UP || b == ADC_BTN_DOWN) {
          new_offset -= page_lines;
        }
        if (new_offset < 0) new_offset = 0;
        if (new_offset > max_offset) new_offset = max_offset;
        if (new_offset != rtcLogsScrollOffset) {
          rtcLogsScrollOffset = new_offset;
          max_offset = drawLogsScreen(display.getFrameBuffer(), rtcLogsScrollOffset);
          drawStatusOverlay(rtcCycleCount, wifiOk || !needFetch, fetchOk || !needFetch);
          display.displayBuffer(EInkDisplay::FAST_REFRESH, /*turnOffScreen=*/false);
        }
      }
      last_adc = b;
    }
  } else if (haveCache) {
    if (screen.type == SCREEN_STAT_GROUP) {
      drawStatScreen(display.getFrameBuffer(), screen.stats, screen.stat_count, vl);
    } else {
      screen.chart.view_label = vl;
      drawPanel(display.getFrameBuffer(), screen.chart);
    }
    drawStatusOverlay(rtcCycleCount, wifiOk || !needFetch, fetchOk || !needFetch);
    // Force full refresh when leaving list mode so the list pixels don't
    // ghost-bleed into the panel underneath.
    bool wantFull = firmwareChanged || justExitedListMode ||
                    (rtcCycleCount % FULL_REFRESH_EVERY) == 0;
    if (wantFull) display.requestResync();
    display.displayBuffer(wantFull ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH,
                          /*turnOffScreen=*/true);
    rtcEpdRamValid = true;
  } else if (!needFetch) {
    // Button wake on empty cache: don't paint the WiFi splash (misleading —
    // we never actually try to connect on a button wake). Just bump the
    // status overlay so the user knows we're alive; leave the rest of the
    // screen on whatever was last drawn.
    drawStatusOverlay(rtcCycleCount, /*wifiOk=*/false, /*fetchOk=*/false);
    refreshStatusOverlay();
    Log.printf("cache miss on button wake (idx=%u view=%s) — keeping screen\n",
                  (unsigned)rtcPanelIdx, viewLabel(rtcViewMode));
  }
  rtcCycleCount++;

  // IMU probe — runs late in setup() so HWCDC has time to enumerate. Logs
  // WHO_AM_I + raw accelerometer reading; useful for verifying the chip
  // responds and (eventually) for wake-on-motion configuration.
  Serial.flush();
  delay(50);
  if (imuInit()) {
    int16_t ax = 0, ay = 0, az = 0;
    if (imuReadAccel(ax, ay, az)) {
      Log.printf("imu: accel raw ax=%d ay=%d az=%d\n", ax, ay, az);
    } else {
      Log.println("imu: accel read failed");
    }
  }
  Serial.flush();

  // Sleep duration: aim for the next view-mode refresh boundary. The wake
  // cadence is independent of which network we last used — we always want
  // to be ready to fetch the moment a network with a short-enough floor
  // becomes reachable.
  uint64_t sleepSec = currentRefreshInterval();
  if (haveValidClock && inQuietHours()) {
    // Sleep until the quiet-hours window ends (06:00 local). Button wakes
    // still work for instant cache-served panel switches.
    sleepSec = secondsUntilQuietEnd();
    Log.printf("quiet hours: sleeping %llus until 06:00 local\n",
                  (unsigned long long)sleepSec);
  } else if (rtcLastFetchEpoch == 0) {
    // Never had a successful fetch (first boot or no WiFi yet) — retry
    // quickly rather than waiting a full refresh interval.
    sleepSec = WIFI_FAIL_RETRY_SECONDS;
  } else if (haveValidClock) {
    time_t elapsed = time(nullptr) - rtcLastFetchEpoch;
    uint64_t interval = currentRefreshInterval();
    if (elapsed < (time_t)interval) {
      sleepSec = interval - elapsed;
    } else {
      sleepSec = WIFI_FAIL_RETRY_SECONDS;
    }
  }
  if (sleepSec < 60) sleepSec = 60;
  deepSleep(sleepSec);
}

void loop() {
  // Never reached — setup() always ends in deep sleep.
}

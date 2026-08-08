#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

#include "battery_e1004.h"
#include "bundle_parser.h"
#include "bundle_seal.h"
#include "config.h"
#include "dashboard_renderer.h"
#include "display_e1004.h"
#include "enroll_screen.h"
#include "framebuffer.h"
#include "gfxc.h"
#include "log.h"
#include "serial_console.h"
#include "wifi_config.h"
#include "x25519_keystore.h"

// ---- Persistent state across deep sleep --------------------------------------
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
RTC_DATA_ATTR static uint32_t rtcDashIndex = 0;
RTC_DATA_ATTR static uint32_t rtcWakesSinceFetch = 0;
RTC_DATA_ATTR static uint8_t rtcDashCount = 0;   // cached dashboards (slideshow modulus)
constexpr uint32_t RTC_MAGIC_VALUE = 0x45313034u;  // 'E104'

// Latest battery snapshot, taken once per wake; shared by the on-screen
// indicator and the Wi-Fi telemetry post.
static battery::Status gBattery = {};

// Wi-Fi re-fetch cadence expressed in wake cycles (no NTP needed).
constexpr uint32_t FETCH_EVERY_N_WAKES =
    (REFRESH_INTERVAL_SECONDS + DWELL_SECONDS - 1) / DWELL_SECONDS;

// Each dashboard is cached as its own file on the 28 MB LittleFS "storage"
// partition. NVS holds only the small stuff: keys, wifi, cfg, and the
// per-dashboard etags used for change detection.
constexpr const char* CFG_NS = "e1004-cfg";
constexpr const char* DASH_NS = "e1004-dash";
constexpr uint8_t MAX_DASH = 16;

// Per-dashboard sealed-download ceiling. Sealed + plaintext buffers come from
// PSRAM so this doesn't pressure internal heap.
constexpr size_t MAX_SEALED_BYTES = 224 * 1024;

// Manifest header magic (mirrors data_trmnl.MANIFEST_MAGIC).
constexpr uint16_t MANIFEST_MAGIC = 0xCFB3;

// ---- Local time / quiet hours -------------------------------------------------
static bool getLocalNow(struct tm& out) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // clock never set
  localtime_r(&now, &out);
  return true;
}

static bool inQuietHours() {
  struct tm lt;
  if (!getLocalNow(lt)) return false;
  return lt.tm_hour >= QUIET_HOURS_START || lt.tm_hour < QUIET_HOURS_END;
}

// Seconds from now until the next QUIET_HOURS_END boundary (06:00 local).
static uint64_t secondsUntilQuietEnd() {
  struct tm lt;
  if (!getLocalNow(lt)) return 0;
  struct tm target = lt;
  target.tm_hour = QUIET_HOURS_END;
  target.tm_min = 0;
  target.tm_sec = 0;
  time_t now = time(nullptr);
  time_t end = mktime(&target);
  if (end <= now) end += 24 * 3600;  // already past 6am today -> tomorrow
  return (uint64_t)(end - now);
}

// Set the wall clock from an HTTP "Date" header (RFC 7231 fixed format:
// "Sun, 06 Nov 1994 08:49:37 GMT"). Every Worker response carries it, so
// syncing here is free — no NTP roundtrip. Mirrors firmware-cloud.
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
  Log.printf("clock: set from HTTP Date -> local %02d:%02d\n", lt.tm_hour, lt.tm_min);
  return true;
}

// ---- Task watchdog -----------------------------------------------------------
#if ENABLE_WATCHDOG
static void wdtBegin() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WATCHDOG_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_add(NULL);
}
static inline void wdtFeed() { esp_task_wdt_reset(); }
#else
static void wdtBegin() {}
static inline void wdtFeed() {}
#endif

static bool gFsReady = false;
static bool ensureFs() {
  if (!gFsReady) {
    gFsReady = LittleFS.begin(/*formatOnFail=*/true, "/littlefs", 5, "storage");
    if (!gFsReady) Log.println("littlefs: mount failed");
  }
  return gFsReady;
}

// ---- Config helpers ----------------------------------------------------------
static String workerUrl() {
  Preferences p;
  String v;
  if (p.begin(CFG_NS, true)) { v = p.getString("worker_url", ""); p.end(); }
  return v.length() ? v : String(DEFAULT_WORKER_URL);
}
static String workerBearer() {
  Preferences p;
  String v;
  if (p.begin(CFG_NS, true)) { v = p.getString("bearer", ""); p.end(); }
  return v.length() ? v : String(DEFAULT_WORKER_BEARER);
}

// ---- Per-dashboard cache (LittleFS) + etag bookkeeping (NVS) ------------------
static void dashPath(uint32_t i, char* out, size_t cap) {
  snprintf(out, cap, "/dash-%u.bin", (unsigned)i);
}

static bool dashExists(uint32_t i) {
  if (!ensureFs()) return false;
  char path[24];
  dashPath(i, path, sizeof(path));
  return LittleFS.exists(path);
}

static size_t loadDash(uint32_t i, uint8_t** out) {
  *out = nullptr;
  if (!ensureFs()) return 0;
  char path[24];
  dashPath(i, path, sizeof(path));
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  size_t n = f.size();
  if (n == 0) { f.close(); return 0; }
  uint8_t* buf = (uint8_t*)ps_malloc(n);
  if (!buf) { f.close(); return 0; }
  size_t got = f.read(buf, n);
  f.close();
  if (got != n) { free(buf); return 0; }
  *out = buf;
  return n;
}

static bool storeDash(uint32_t i, const uint8_t* data, size_t len) {
  if (!ensureFs()) return false;
  char path[24];
  dashPath(i, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t w = f.write(data, len);
  f.close();
  if (w != len) { LittleFS.remove(path); return false; }
  return true;
}

static uint32_t storedEtag(uint32_t i) {
  Preferences p;
  uint32_t v = 0;
  if (p.begin(DASH_NS, true)) {
    char k[8];
    snprintf(k, sizeof(k), "e%u", (unsigned)i);
    v = p.getUInt(k, 0);
    p.end();
  }
  return v;
}
static void setStoredEtag(uint32_t i, uint32_t v) {
  Preferences p;
  if (p.begin(DASH_NS, false)) {
    char k[8];
    snprintf(k, sizeof(k), "e%u", (unsigned)i);
    p.putUInt(k, v);
    p.end();
  }
}
static uint8_t storedCount() {
  Preferences p;
  uint8_t v = 0;
  if (p.begin(DASH_NS, true)) { v = p.getUChar("count", 0); p.end(); }
  return v;
}
static void setStoredCount(uint8_t n) {
  Preferences p;
  if (p.begin(DASH_NS, false)) { p.putUChar("count", n); p.end(); }
}

// Max PLAINTEXT bundle this device can accept per dashboard, advertised to the
// bridge (X-Bundle-Capacity) so it downsamples to fit.
static size_t reportCapacityBytes() {
  size_t cap = MAX_SEALED_BYTES - bundle_seal::OVERHEAD_BYTES - 256;
  if (ensureFs()) {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    size_t freeFs = total > used ? total - used : 0;
    if (freeFs && freeFs < cap) cap = freeFs;
  }
  return cap;
}

// ---- Battery telemetry (best-effort) ----------------------------------------
static void postBattery(const String& url, const String& bearer,
                        const battery::Status& b) {
  if (!url.length() || !bearer.length() || b.mv == 0) return;
  String batUrl = url;
  int slash = batUrl.lastIndexOf('/');
  if (slash >= 0) batUrl = batUrl.substring(0, slash) + "/battery-e1004";
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(secure, batUrl)) return;
  http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Content-Type", "application/json");
  char body[96];
  snprintf(body, sizeof(body), "{\"mv\":%u,\"soc\":%d,\"charging\":%s}",
           (unsigned)b.mv, b.soc == 0xFF ? -1 : (int)b.soc,
           b.charging ? "true" : "false");
  int code = http.PUT((uint8_t*)body, strlen(body));
  Log.printf("battery: PUT -> %d\n", code);
  http.end();
}

// ---- Fetch helpers -----------------------------------------------------------
static bool readExact(HTTPClient& http, uint8_t* buf, int len) {
  WiFiClient* stream = http.getStreamPtr();
  int got = 0;
  uint32_t last = millis();
  while (got < len && http.connected()) {
    if (stream->available()) {
      int n = stream->readBytes(buf + got, len - got);
      if (n > 0) { got += n; last = millis(); }
    } else if (millis() - last > 5000) {
      Log.println("worker: read stalled");
      break;
    } else {
      delay(1);
    }
  }
  return got == len;
}

// GET /manifest-e1004 -> fill etagsOut[count] + count + nextPoll.
static bool fetchManifest(const String& baseUrl, const String& bearer,
                          uint32_t* etagsOut, uint8_t* countOut, uint32_t* nextPollOut) {
  String url = baseUrl;
  int slash = url.lastIndexOf('/');
  if (slash >= 0) url = url.substring(0, slash) + "/manifest-e1004";

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  bool ok = false;
  if (http.begin(secure, url)) {
    http.addHeader("Authorization", "Bearer " + bearer);
    http.setUserAgent("einkcharts-e1004/1");
    static const char* kHdrs[] = {"Date"};
    http.collectHeaders(kHdrs, 1);
    int code = http.GET();
    Log.printf("manifest: GET -> %d\n", code);
    // Any response carries a Date header — sync the wall clock (quiet hours).
    if (code > 0 && http.hasHeader("Date")) setClockFromHttpDate(http.header("Date"));
    if (code == 200) {
      int n = http.getSize();
      const int maxN = 8 + 4 * MAX_DASH;
      if (n >= 8 && n <= maxN) {
        uint8_t buf[8 + 4 * MAX_DASH];
        if (readExact(http, buf, n)) {
          BinReader r(buf, n);
          uint16_t magic = r.u16();
          uint8_t ver = r.u8();
          uint8_t count = r.u8();
          uint32_t np = r.u32();
          if (magic == MANIFEST_MAGIC && ver == 1 && count <= MAX_DASH) {
            for (uint8_t i = 0; i < count; i++) etagsOut[i] = r.u32();
            *countOut = count;
            *nextPollOut = np;
            ok = true;
          } else {
            Log.printf("manifest: bad header magic=0x%04X ver=%u count=%u\n",
                       magic, ver, count);
          }
        }
      } else {
        Log.printf("manifest: bad length %d\n", n);
      }
    }
    http.end();
  }
  return ok;
}

// GET /bundle-e1004?d=<index>, decrypt, and cache to /dash-<index>.bin.
static bool fetchDashboard(uint32_t index, const uint8_t sk[32], const uint8_t pk[32],
                           const String& baseUrl, const String& bearer) {
  String url = baseUrl + (baseUrl.indexOf('?') >= 0 ? "&" : "?") + "d=" + String(index);
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  bool ok = false;
  if (http.begin(secure, url)) {
    http.addHeader("Authorization", "Bearer " + bearer);
    http.addHeader("Accept", "application/octet-stream");
    char capbuf[16];
    snprintf(capbuf, sizeof(capbuf), "%u", (unsigned)reportCapacityBytes());
    http.addHeader("X-Bundle-Capacity", capbuf);
    http.setUserAgent("einkcharts-e1004/1");
    int code = http.GET();
    Log.printf("worker: GET d=%u -> %d\n", (unsigned)index, code);
    if (code == 200) {
      int sealedLen = http.getSize();
      if (sealedLen > (int)bundle_seal::OVERHEAD_BYTES && sealedLen < (int)MAX_SEALED_BYTES) {
        uint8_t* sealed = (uint8_t*)ps_malloc(sealedLen);
        if (sealed) {
          if (readExact(http, sealed, sealedLen)) {
            size_t cap = sealedLen - bundle_seal::OVERHEAD_BYTES;
            uint8_t* plain = (uint8_t*)ps_malloc(cap);
            if (plain) {
              int plen = bundle_seal::unseal(sk, pk, sealed, sealedLen, plain, cap);
              if (plen > 0 && bundle::valid(plain, plen)) {
                ok = storeDash(index, plain, plen);
                Log.printf("worker: cached d=%u %d bytes\n", (unsigned)index, plen);
              } else {
                Log.println("worker: decrypt/validate failed");
              }
              free(plain);
            }
          }
          free(sealed);
        }
      } else {
        Log.printf("worker: bad sealed length %d\n", sealedLen);
      }
    }
    http.end();
  }
  return ok;
}

// ---- Prefetch every dashboard ------------------------------------------------
// One Wi-Fi session: read the manifest, download only the dashboards whose etag
// changed (or aren't cached), then hand back the usable count.
static bool prefetchAll(const uint8_t sk[32], const uint8_t pk[32], uint8_t* countOut) {
  *countOut = storedCount();
  auto nets = wifi_config::load();
  auto conn = wifi_config::connect_any(nets, WIFI_CONNECT_TIMEOUT_MS);
  wdtFeed();
  if (!conn.ok) return false;

  String url = workerUrl();
  String bearer = workerBearer();
  if (!url.length() || !bearer.length()) {
    Log.println("worker: URL or bearer not configured");
    WiFi.disconnect(true, true);
    return false;
  }

  uint32_t manEtags[MAX_DASH];
  uint8_t manCount = 0;
  uint32_t nextPoll = 0;
  bool ok = false;
  wdtFeed();
  if (fetchManifest(url, bearer, manEtags, &manCount, &nextPoll)) {
    uint8_t fetched = 0, changed = 0;
    for (uint8_t i = 0; i < manCount; i++) {
      bool needDl = !dashExists(i) || storedEtag(i) != manEtags[i];
      if (!needDl) { fetched++; continue; }
      changed++;
      bool got = fetchDashboard(i, sk, pk, url, bearer);
      wdtFeed();
      if (got) {
        setStoredEtag(i, manEtags[i]);
        fetched++;
      } else {
        Log.printf("prefetch: dashboard %u failed\n", (unsigned)i);
      }
    }
    // Usable count = the contiguous run of cached dashboards from index 0.
    uint8_t usable = 0;
    for (uint8_t i = 0; i < manCount; i++) {
      if (dashExists(i)) usable = i + 1; else break;
    }
    if (usable > 0) {
      setStoredCount(usable);
      *countOut = usable;
      ok = true;
    }
    Log.printf("prefetch: manifest=%u changed=%u cached=%u usable=%u\n",
               (unsigned)manCount, (unsigned)changed, (unsigned)fetched, (unsigned)usable);
  } else {
    Log.println("prefetch: manifest fetch failed — keeping cached set");
    ok = (*countOut > 0);
  }

  postBattery(url, bearer, gBattery);
  WiFi.disconnect(true, true);
  return ok;
}

// ---- Screens ----------------------------------------------------------------
static void drawWaitingScreen() {
  fb::clear(COL_WHITE);
  gfxc::drawTextCenteredFit(0, SCREEN_H / 2 - 20, SCREEN_W, 34,
                            "Waiting for the first dashboard bundle...", COL_BLACK);
}

// Battery badge in the top-right corner: SOC percent (or voltage until the ADC
// settles), small light text over a white pad so it stays legible.
static void drawBatteryBadge(const battery::Status& b) {
  if (b.mv == 0) return;
  const int margin = 12, labelPx = 17;

  char label[16];
  if (b.soc != 0xFF)
    snprintf(label, sizeof(label), "%u%%", (unsigned)b.soc);
  else
    snprintf(label, sizeof(label), "%u.%02uV", b.mv / 1000, (b.mv % 1000) / 10);

  fb::fillRect(SCREEN_W - 90, margin - 4, 90, labelPx + 10, COL_WHITE);
  gfxc::drawTextRightFitLight(SCREEN_W - margin, margin, 80, labelPx, label, COL_BLACK);
}

// Load dashboard `index`'s cached bundle and paint + present it.
static bool renderIndexFromCache(uint32_t index) {
  uint8_t* blob = nullptr;
  size_t len = loadDash(index, &blob);
  bool ok = len && bundle::valid(blob, len);
  if (ok) {
    fb::clear(COL_WHITE);
    dashboard_renderer::render(blob, len, 0);
    drawBatteryBadge(gBattery);
    wdtFeed();
    display_e1004::present();   // ~25 s color refresh
    wdtFeed();
  }
  if (blob) free(blob);
  return ok;
}

// ---- Deep sleep --------------------------------------------------------------
// Any front button (active LOW) wakes the device and advances the slideshow.
// E1003/E1004 use ext1 ANY_LOW (per Seeed's ESPHome config); hold pull-ups
// through sleep so the lines can't float and spin-wake us.
static void armButtonWake() {
  uint64_t mask = 0;
  for (int8_t pin : {BTN_GREEN_GPIO, BTN_WHITE_RIGHT_GPIO, BTN_WHITE_LEFT_GPIO}) {
    if (pin < 0) continue;
    rtc_gpio_pullup_en((gpio_num_t)pin);
    rtc_gpio_pulldown_dis((gpio_num_t)pin);
    mask |= 1ULL << pin;
  }
  if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

static void goToSleep() {
  uint64_t sleepSec = DWELL_SECONDS;
  if (inQuietHours()) {
    // Sleep straight through to 06:00 local instead of waking every dwell.
    // Force a fetch on the morning wake so the first dashboard is fresh.
    sleepSec = secondsUntilQuietEnd();
    rtcWakesSinceFetch = FETCH_EVERY_N_WAKES;
    Log.printf("quiet hours: sleeping %llus until %02d:00 local\n",
               (unsigned long long)sleepSec, QUIET_HOURS_END);
  }
  esp_sleep_enable_timer_wakeup(sleepSec * 1000000ULL);
  display_e1004::sleep();
  armButtonWake();
  Log.printf("deep sleep %llus\n", (unsigned long long)sleepSec);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  wdtBegin();
  // TZ doesn't survive deep sleep but the RTC wall clock does — re-apply every
  // wake so localtime()/quiet-hours math is in local time, not UTC.
  setenv("TZ", LOCAL_TZ, 1);
  tzset();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool buttonWake = (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_EXT0);
  bool firstBoot = (rtcMagic != RTC_MAGIC_VALUE);
  delay((cause == ESP_SLEEP_WAKEUP_UNDEFINED) ? 800 : 100);

  Log.printf("\n=== E1004 boot wake=%d button=%d first=%d index=%u ===\n",
             (int)cause, (int)buttonWake, (int)firstBoot, (unsigned)rtcDashIndex);
  {
    struct tm lt;
    if (getLocalNow(lt)) {
      Log.printf("clock: local %02d:%02d%s\n", lt.tm_hour, lt.tm_min,
                 inQuietHours() ? " (quiet hours)" : "");
    }
  }

  // A timer wake during quiet hours does NO work — no display power-up, no
  // Wi-Fi, no refresh — and goToSleep() puts us back down until 06:00. A
  // button press still runs the full advance-from-cache cycle below.
  if (!buttonWake && !firstBoot && inQuietHours()) {
    Log.println("quiet hours: skipping timed advance");
    goToSleep();
    return;
  }

  for (int8_t pin : {BTN_GREEN_GPIO, BTN_WHITE_RIGHT_GPIO, BTN_WHITE_LEFT_GPIO}) {
    if (pin >= 0) pinMode(pin, INPUT_PULLUP);
  }
  battery::begin();
  gBattery = battery::read();
  Log.printf("battery: %umV soc=%d%%\n", (unsigned)gBattery.mv,
             gBattery.soc == 0xFF ? -1 : (int)gBattery.soc);

  if (firstBoot) {
    rtcMagic = RTC_MAGIC_VALUE;
    rtcDashIndex = 0;
    rtcWakesSinceFetch = FETCH_EVERY_N_WAKES;  // force a fetch on first boot
    rtcDashCount = storedCount();  // survive a power-cycle via NVS
  }

  if (!fb::begin()) {
    Log.println("fb: PSRAM alloc failed — sleeping");
    esp_sleep_enable_timer_wakeup(REFRESH_INTERVAL_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
  }
  if (!display_e1004::begin()) {
    Log.println("display: init failed — sleeping");
    esp_sleep_enable_timer_wakeup(REFRESH_INTERVAL_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
  }
  wdtFeed();

  // X25519 key: first boot generates + shows the enrollment QR, then sleeps.
  uint8_t sk[32], pk[32];
  if (!x25519_keystore::exists()) {
    if (x25519_keystore::generate_and_store(sk, pk)) {
      char b64[64];
      x25519_keystore::b64url_encode(pk, 32, b64, sizeof(b64));
      Log.printf("crypto: E1004_PUBKEY_B64=%s\n", b64);
      enroll_screen::show(b64);
      display_e1004::present();
    }
    esp_sleep_enable_timer_wakeup(24ULL * 3600 * 1000000ULL);
    armButtonWake();
    display_e1004::sleep();
    esp_deep_sleep_start();
  }
  x25519_keystore::load(sk, pk);
  {
    char b64[64];
    x25519_keystore::b64url_encode(pk, 32, b64, sizeof(b64));
    Log.printf("crypto: E1004_PUBKEY_B64=%s\n", b64);
  }

  // A button wake advances; a timer wake advances and counts toward the fetch
  // cadence; first boot shows dashboard 0.
  bool advance = !firstBoot;
  bool didTimerWake = !buttonWake && !firstBoot;
  if (advance) rtcDashIndex++;
  if (didTimerWake) rtcWakesSinceFetch++;

  uint8_t count = rtcDashCount ? rtcDashCount : storedCount();

  bool haveCache = count > 0 && dashExists(0);
  bool needFetch = firstBoot || !haveCache ||
                   (didTimerWake && rtcWakesSinceFetch >= FETCH_EVERY_N_WAKES);
#if DEMO_MODE
  needFetch = false;
#endif
  if (needFetch) {
    Log.printf("fetch: prefetching all (wakesSinceFetch=%u, haveCache=%d)\n",
               (unsigned)rtcWakesSinceFetch, (int)haveCache);
    uint8_t newCount = count;
    if (prefetchAll(sk, pk, &newCount)) {
      rtcWakesSinceFetch = 0;
      count = newCount;
    }
  }
  rtcDashCount = count;

  if (count && renderIndexFromCache(rtcDashIndex % count)) {
    // rendered
  } else {
    Log.println("render: no valid cache — waiting screen");
    drawWaitingScreen();
    display_e1004::present();
  }
  wdtFeed();

#if SERIAL_CONSOLE
  serial_console::run(count, rtcDashIndex, renderIndexFromCache);
#endif

  goToSleep();
}

void loop() {}

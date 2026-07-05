#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

#include "battery_bq27427.h"
#include "bundle_parser.h"
#include "bundle_seal.h"
#include "config.h"
#include "dashboard_renderer.h"
#include "display_trmnl.h"
#include "enroll_screen.h"
#include "gfx4.h"
#include "log.h"
#include "serial_console.h"
#include "touch_iqs323.h"
#include "wifi_config.h"
#include "x25519_keystore.h"

// ---- Persistent state across deep sleep --------------------------------------
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
RTC_DATA_ATTR static uint32_t rtcDashIndex = 0;
RTC_DATA_ATTR static uint32_t rtcWakesSinceFetch = 0;
RTC_DATA_ATTR static uint8_t rtcTouchReady = 0;  // IQS323 configured into event mode
RTC_DATA_ATTR static uint8_t rtcDashCount = 0;   // cached dashboards (slideshow modulus)
// Re-arm outcome from the PREVIOUS goToSleep, printed on the next boot. The
// re-arm logs themselves are lost to the USB-CDC power-down at deep sleep, so we
// stash the result in RTC RAM and report it after the next wake instead.
RTC_DATA_ATTR static uint8_t rtcLastCfgOk = 0xFF;   // configure() return (ATI etc.)
RTC_DATA_ATTR static uint8_t rtcLastArmed = 0xFF;   // RDY idled HIGH → ext0 armed
RTC_DATA_ATTR static uint8_t rtcLastCfgTries = 0;   // how many configure() attempts
constexpr uint32_t RTC_MAGIC_VALUE = 0x54524d4eu;  // 'TRMN'

// Wi-Fi re-fetch cadence expressed in wake cycles (no NTP needed): fetch a
// fresh bundle roughly every REFRESH_INTERVAL, advancing the slideshow from
// cache on the wakes in between.
constexpr uint32_t FETCH_EVERY_N_WAKES =
    (REFRESH_INTERVAL_SECONDS + DWELL_SECONDS - 1) / DWELL_SECONDS;

// Each dashboard is cached as its own file on the 12 MB LittleFS "storage"
// partition (NOT NVS — the sealed bundle dwarfs the 24 KB NVS partition). The
// device prefetches all dashboards, then serves the slideshow from these files
// so advancing is instant (no Wi-Fi). NVS holds only the small stuff: keys,
// wifi, cfg, and the per-dashboard etags used for change detection.
constexpr const char* CFG_NS = "trmnlx-cfg";
constexpr const char* DASH_NS = "trmnlx-dash";  // per-dashboard etags + count
constexpr uint8_t MAX_DASH = 16;

// Per-dashboard sealed-download ceiling. Sized to hold a full-resolution single
// dashboard; sealed + plaintext buffers come from PSRAM (8 MB) so this doesn't
// pressure internal heap. Also the basis for the capacity we advertise.
constexpr size_t MAX_SEALED_BYTES = 224 * 1024;

// Manifest header magic (mirrors data_trmnl.MANIFEST_MAGIC).
constexpr uint16_t MANIFEST_MAGIC = 0xCFB3;

// ---- Task watchdog -----------------------------------------------------------
#if ENABLE_WATCHDOG
static void wdtBegin() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WATCHDOG_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  // Arduino may already have inited the TWDT; reconfigure rather than fail.
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_add(NULL);  // watch this (setup/loop) task
}
static inline void wdtFeed() { esp_task_wdt_reset(); }
#else
static inline void wdtBegin() {}
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

// Load dashboard `i`'s cached bundle into a fresh PSRAM buffer (caller frees).
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
  uint8_t* buf = (uint8_t*)ps_malloc(n);  // up to ~200KB — keep it out of DRAM
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

// Max PLAINTEXT bundle this device can accept per dashboard. Advertised to the
// bridge (X-Bundle-Capacity) so it scales chart resolution to fit rather than
// producing a bundle we can't store/decode. Bounded by the sealed-download
// ceiling minus seal overhead, and by real free space on the storage partition.
static size_t reportCapacityBytes() {
  // Small margin below the ceiling so plaintext(=budget)+overhead stays strictly
  // under MAX_SEALED_BYTES (the download reject bound).
  size_t cap = MAX_SEALED_BYTES - bundle_seal::OVERHEAD_BYTES - 256;
  if (ensureFs()) {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    size_t freeFs = total > used ? total - used : 0;
    if (freeFs && freeFs < cap) cap = freeFs;
  }
  return cap;
}

// ---- Battery telemetry (best-effort) ----------------------------------------
static void postBattery(const String& url, const String& bearer, uint16_t mv) {
  if (!url.length() || !bearer.length() || mv == 0) return;
  String batUrl = url;
  int slash = batUrl.lastIndexOf('/');
  if (slash >= 0) batUrl = batUrl.substring(0, slash) + "/battery-trmnl";
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(secure, batUrl)) return;
  http.addHeader("Authorization", "Bearer " + bearer);
  http.addHeader("Content-Type", "application/json");
  char body[32];
  snprintf(body, sizeof(body), "{\"mv\":%u}", (unsigned)mv);
  int code = http.PUT((uint8_t*)body, strlen(body));
  Log.printf("battery: PUT -> %d\n", code);
  http.end();
}

// ---- Fetch helpers -----------------------------------------------------------
// Read exactly `len` bytes from the response stream, aborting on a 5s stall.
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

// GET /manifest-trmnl → fill etagsOut[count] + count + nextPoll. The device uses
// the etags to skip re-downloading dashboards whose data didn't change.
static bool fetchManifest(const String& baseUrl, const String& bearer,
                          uint32_t* etagsOut, uint8_t* countOut, uint32_t* nextPollOut) {
  String url = baseUrl;
  int slash = url.lastIndexOf('/');
  if (slash >= 0) url = url.substring(0, slash) + "/manifest-trmnl";

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  bool ok = false;
  if (http.begin(secure, url)) {
    http.addHeader("Authorization", "Bearer " + bearer);
    http.setUserAgent("einkcharts-trmnlx/1");
    int code = http.GET();
    Log.printf("manifest: GET -> %d\n", code);
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

// GET /bundle-trmnl?d=<index>, decrypt, and cache to /dash-<index>.bin.
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
    http.setUserAgent("einkcharts-trmnlx/1");
    int code = http.GET();
    Log.printf("worker: GET d=%u -> %d\n", (unsigned)index, code);
    if (code == 200) {
      int sealedLen = http.getSize();
      if (sealedLen > (int)bundle_seal::OVERHEAD_BYTES && sealedLen < (int)MAX_SEALED_BYTES) {
        uint8_t* sealed = (uint8_t*)ps_malloc(sealedLen);  // PSRAM: keep off DRAM
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
// changed (or aren't cached), then hand back the usable count. Peak memory stays
// bounded because dashboards are fetched/decrypted one at a time. Advancing the
// slideshow later reads purely from cache — no radio, instant.
static bool prefetchAll(const uint8_t sk[32], const uint8_t pk[32], uint8_t* countOut) {
  *countOut = storedCount();
  auto nets = wifi_config::load();
  auto conn = wifi_config::connect_any(nets, WIFI_CONNECT_TIMEOUT_MS);
  wdtFeed();  // WiFi association can block up to the connect timeout
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
  wdtFeed();  // manifest GET has its own HTTP timeout
  if (fetchManifest(url, bearer, manEtags, &manCount, &nextPoll)) {
    uint8_t fetched = 0, changed = 0;
    for (uint8_t i = 0; i < manCount; i++) {
      bool needDl = !dashExists(i) || storedEtag(i) != manEtags[i];
      if (!needDl) { fetched++; continue; }
      changed++;
      bool got = fetchDashboard(i, sk, pk, url, bearer);
      wdtFeed();  // one dashboard fetch fits under the timeout; feed between them
      if (got) {
        setStoredEtag(i, manEtags[i]);
        fetched++;
      } else {
        Log.printf("prefetch: dashboard %u failed\n", (unsigned)i);
      }
    }
    // Usable count = the contiguous run of cached dashboards from index 0, so a
    // mid-list failure never strands the slideshow on a missing file.
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
    ok = (*countOut > 0);  // fall back to whatever is already cached
  }

  // Best-effort battery telemetry while Wi-Fi is still up.
  postBattery(url, bearer, battery::voltageMv());
  WiFi.disconnect(true, true);
  return ok;
}

// ---- Fallback "no data yet" screen ------------------------------------------
static void drawWaitingScreen() {
  display_trmnl::clear(GRAY_WHITE);
  gfx4::drawTextCenteredFit(0, SCREEN_H / 2 - 40, SCREEN_W, 44,
                            "Waiting for the first dashboard bundle...", GRAY_BLACK);
}

// Load dashboard `index`'s cached bundle and paint it. Each cached file holds a
// single dashboard, so we always render its block 0. Returns false if the file
// is missing/corrupt. Signature matches serial_console's render callback.
static bool renderIndexFromCache(uint32_t index) {
  uint8_t* blob = nullptr;
  size_t len = loadDash(index, &blob);
  bool ok = len && bundle::valid(blob, len);
  if (ok) {
    dashboard_renderer::render(blob, len, 0);
    display_trmnl::present();
  }
  if (blob) free(blob);
  return ok;
}

// (The stay-awake interactive tap window was removed — the 4bpp full refresh
// electrically resets the touch chip every advance, so polling it while awake
// fought that and oscillated between stuck and runaway. Tap-to-advance is handled
// by the deep-sleep ext0 wake path: one tap = one wake->advance->render->sleep,
// and the sleep re-arm cleanly resets + re-ATIs the chip. See git history for the
// failed polling approach; a proper interrupt-driven background task is future work.)

// ---- Deep sleep --------------------------------------------------------------
static void goToSleep() {
  esp_sleep_enable_timer_wakeup(DWELL_SECONDS * 1000000ULL);

  // Power the EPD down FIRST, before we re-arm touch. The full-screen refresh
  // disrupts the IQS323 on the shared sensor rail — reconfiguring while the panel
  // is still powered makes ATI fail (it converges fine on a fresh boot precisely
  // because configure() runs before display::begin()). Cutting EPD power and
  // letting the rail settle recreates that clean condition, so ATI converges and
  // the chip actually holds event mode.
  display_trmnl::sleep();

#if ENABLE_TOUCH
  delay(150);  // let the sensor rail settle after EPD power-down before ATI
  // Re-arm event mode and CONFIRM RDY idles HIGH before trusting ext0: a chip
  // left STREAMING (RDY pulsing every ~60ms) would assert ext0 immediately and
  // spin-wake us forever, draining the battery. If it won't hold event mode after
  // a few tries, fall back to timer-only wake for this cycle rather than risk a
  // wake loop — touch just won't wake us until the next clean cycle.
  // Always re-stream config + ATI + event mode here. We must NOT trust a single
  // rdyIdleHigh() sample to skip this: after a refresh the chip streams (RDY
  // flapping), and a momentary HIGH read would wrongly skip the reconfigure and
  // arm ext0 on a chip that never fires on touch — killing touch-wake entirely.
  bool touchArmed = false;
  bool cfgOk = false;
  uint8_t tries = 0;
  for (int i = 0; i < 3 && !touchArmed; i++) {
    tries++;
    // Hardware-reset to a known-clean state BEFORE configuring. The 4bpp full
    // refresh reliably leaves the IQS323 wedged (RDY stuck LOW, config writes NAK,
    // e.g. "config write @0x30 failed") — a software reset can't reach a wedged
    // chip, so configure() alone fails every retry and ext0 never arms. Pulsing
    // master-clear (RDY/GPIO3) reboots it into streaming with SHOW_RESET set,
    // exactly the clean state configure() expects.
    touch::hwReset();
    cfgOk = touch::configure();          // re-stream config, ATI, event mode
    touchArmed = cfgOk && touch::rdyIdleHigh();  // configure() already waited a cycle
    if (!touchArmed)
      Log.printf("touch: re-arm %d — cfg=%d RDY=%d\n", i, (int)cfgOk,
                 digitalRead(TOUCH_RDY_GPIO));
  }
  rtcTouchReady = touchArmed ? 1 : 0;
  rtcLastCfgOk = cfgOk ? 1 : 0;      // stashed for the next boot to report
  rtcLastArmed = touchArmed ? 1 : 0;
  rtcLastCfgTries = tries;
  if (touchArmed) {
    // Touch RDY is active-LOW and idles HIGH via the board pull-up; hold a pull-up
    // through sleep so a floating line can't spuriously wake ext0.
    rtc_gpio_pullup_en((gpio_num_t)TOUCH_RDY_GPIO);
    rtc_gpio_pulldown_dis((gpio_num_t)TOUCH_RDY_GPIO);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_RDY_GPIO, 0);  // wake on RDY LOW
    Log.println("touch: ext0 armed (RDY idle HIGH)");
  } else {
    Log.println("touch: ext0 NOT armed — timer-only wake (chip won't hold event mode)");
  }
#endif
  Log.printf("deep sleep %llus (touchArmed=%d)\n", (unsigned long long)DWELL_SECONDS,
#if ENABLE_TOUCH
             (int)touchArmed);
#else
             0);
#endif
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  wdtBegin();  // arm before any blocking work so a wedged wake path self-recovers
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool touchWake = (cause == ESP_SLEEP_WAKEUP_EXT0);
  bool firstBoot = (rtcMagic != RTC_MAGIC_VALUE);
  // Short settle on a deep-sleep wake so we service the IQS323's comms window
  // before its (widened) I2C timeout closes it; a full power-on/flash gets the
  // longer USB-CDC + esptool reset window.
  delay((cause == ESP_SLEEP_WAKEUP_UNDEFINED) ? 1200 : 300);

  Log.printf("\n=== TRMNL X boot wake=%d touch=%d first=%d index=%u ===\n",
             (int)cause, (int)touchWake, (int)firstBoot, (unsigned)rtcDashIndex);
  // Report the previous sleep's touch re-arm outcome (its own logs were cut off by
  // the USB-CDC power-down). 0xFF = no prior sleep this power cycle.
  if (rtcLastArmed != 0xFF) {
    Log.printf("touch: last re-arm cfgOk=%d armed=%d tries=%u (this wake %s)\n",
               (int)rtcLastCfgOk, (int)rtcLastArmed, (unsigned)rtcLastCfgTries,
               touchWake ? "IS ext0/touch" : "is timer/other");
  }

  // Shared sensor I2C bus (touch + fuel gauge). Begin once — the TRMNL X S3
  // dislikes repeated Wire.begin(). 100 kHz for reliable IQS323 windowed comms.
  Wire.begin(SENSOR_SDA_PIN, SENSOR_SCL_PIN);
  Wire.setClock(100000);
  wdtFeed();
  bool touchPresent = touch::present();
  Log.printf("sensors: touch=%d gauge=%d batt=%umV\n",
             (int)touchPresent, (int)battery::present(), battery::voltageMv());

  if (firstBoot) {
    rtcMagic = RTC_MAGIC_VALUE;
    rtcDashIndex = 0;
    rtcWakesSinceFetch = FETCH_EVERY_N_WAKES;  // force a fetch on first boot
    rtcTouchReady = 0;
    rtcDashCount = storedCount();  // survive a power-cycle via NVS (cache persists)
  }

  // Configure the touch bar into event mode once (first boot), so ext0 wakes
  // only on a real tap rather than every measurement cycle. The IQS323 keeps
  // its config while powered, so later wakes just read it.
#if ENABLE_TOUCH
  // Reconfigure on first boot, if we've never marked it ready, OR if the chip is
  // currently flagging a power-on reset (SHOW_RESET) — a reset wipes its config,
  // and doing it here (before any other I2C traffic) keeps the IQS323's comms
  // windows clean, which its streamed config needs to land.
  if (touchPresent) {
    touch::Event st = touch::readEvent();
    if (firstBoot || !rtcTouchReady || st.reset) {
      Log.printf("touch: configuring (first=%d ready=%d reset=%d)\n",
                 (int)firstBoot, (int)rtcTouchReady, (int)st.reset);
      rtcTouchReady = touch::configure() ? 1 : 0;
    }
  }
#endif

  // Decide whether to advance the slideshow and whether to redraw. An ext0 wake
  // is itself the tap signal: in event mode the IQS323 only asserts RDY on a real
  // touch/slider event, so the wake proves a tap happened. We do NOT re-check the
  // touch bit — by the time we boot (~300ms of wake+I2C latency) the finger has
  // usually already lifted, so it reads touched=0 and we'd wrongly skip the
  // advance (the "tap does nothing" bug). Only a chip reset wake is not a tap.
  bool advance = true;
  bool doRender = true;
  bool didTimerWake = !touchWake && !firstBoot;
  if (firstBoot) {
    advance = false;  // show dashboard 0
  } else if (touchWake) {
#if ENABLE_TOUCH
    touch::Event ev = touch::readEvent();
    Log.printf("touch: wake status=0x%04X touched=%d reset=%d -> advancing\n",
               ev.raw, (int)ev.touched, (int)ev.reset);
    if (ev.reset && touchPresent) {
      // Reset wake (not a tap): recover config and don't advance/render.
      rtcTouchReady = touch::configure() ? 1 : 0;
      advance = false;
      doRender = false;
    }
    // else: genuine tap wake — advance even though touched may already read 0.
#endif
  }

  if (!doRender) {
    goToSleep();  // release-only / spurious touch wake — no display work
    return;
  }

  if (!display_trmnl::begin()) {
    Log.println("display: init failed — sleeping");
    esp_sleep_enable_timer_wakeup(REFRESH_INTERVAL_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
  }
  wdtFeed();  // panel init + first full refresh done

  // X25519 key: first boot generates + shows the enrollment QR, then sleeps.
  uint8_t sk[32], pk[32];
  if (!x25519_keystore::exists()) {
    if (x25519_keystore::generate_and_store(sk, pk)) {
      char b64[64];
      x25519_keystore::b64url_encode(pk, 32, b64, sizeof(b64));
      Log.printf("crypto: TRMNL_PUBKEY_B64=%s\n", b64);
      enroll_screen::show(b64);
      display_trmnl::present();
    }
    esp_sleep_enable_timer_wakeup(24ULL * 3600 * 1000000ULL);
#if ENABLE_TOUCH
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_RDY_GPIO, 0);
#endif
    display_trmnl::sleep();
    esp_deep_sleep_start();
  }
  x25519_keystore::load(sk, pk);
  {
    char b64[64];
    x25519_keystore::b64url_encode(pk, 32, b64, sizeof(b64));
    Log.printf("crypto: TRMNL_PUBKEY_B64=%s\n", b64);
  }

  if (advance) rtcDashIndex++;
  // Fetch cadence is counted in timer wakes only, so tapping through dashboards
  // doesn't drag the Wi-Fi re-fetch forward.
  if (didTimerWake) rtcWakesSinceFetch++;

  uint8_t count = rtcDashCount ? rtcDashCount : storedCount();

  // Decide whether to prefetch this wake (never on a plain touch advance): the
  // device pulls ALL dashboards up front so advancing is instant from cache.
  bool haveCache = count > 0 && dashExists(0);
  bool needFetch = firstBoot || !haveCache ||
                   (didTimerWake && rtcWakesSinceFetch >= FETCH_EVERY_N_WAKES);
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

  // Render the current dashboard straight from its cached file.
  if (count && renderIndexFromCache(rtcDashIndex % count)) {
    // rendered
  } else {
    Log.println("render: no valid cache — waiting screen");
    drawWaitingScreen();
    display_trmnl::present();
  }
  wdtFeed();  // render done

#if SERIAL_CONSOLE
  // Dev console on timer/first-boot wakes (host dumps, manual nav). On a TOUCH
  // wake the interactive tap window below owns the awake period instead — it
  // reconfigures the chip after the refresh reset, which the console poll doesn't.
  if (!touchWake) serial_console::run(count, rtcDashIndex, renderIndexFromCache);
#endif

  // A tap wake has already advanced + rendered the new dashboard above; we now go
  // straight to sleep, whose re-arm path powers the EPD down and cleanly resets +
  // re-ATIs the touch chip for the next tap. (The stay-awake interactive window was
  // removed — see the DISABLED block above: polling the chip while awake fights the
  // refresh-induced reset and oscillates between stuck and runaway.)
  (void)touchWake;

  goToSleep();
}

void loop() {}

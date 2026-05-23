#include "ble_bundle_receiver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "log_buffer.h"

namespace ble_bundle_receiver {

// Shared between the BLE task and the main waiting loop. Volatile reads on
// the main side give us "publication" semantics without needing a mutex —
// we only care that the flag flips and the bytes are present.
static volatile bool g_bundle_received = false;
static volatile bool g_peer_disconnected = false;
static volatile bool g_battery_read = false;
static uint8_t* g_out_buf = nullptr;
static size_t g_out_cap = 0;
static size_t g_out_len = 0;
// 0 until we've seen the first write (which carries a 4-byte LE u32
// length prefix). Then it's the declared total bundle size.
static uint32_t g_expected_len = 0;

// Per-attribute max BLE value length. 512 is the GATT spec ceiling
// (BLE_ATT_ATTR_MAX_LEN). The iOS side chunks the bundle to fit.
static constexpr uint16_t BUNDLE_ATTR_MAX_LEN = 512;

// NimBLE-Arduino 2.x callback signatures. The 2.x line is required on
// Arduino-ESP32 3.x (pioarduino 55.x) because the IDF already ships a
// NimBLE host — bundling our own (1.4.x behavior) double-links the host
// and lands controller→host vtables with NULL entries, crashing in
// btdm_controller_init.
class BundleCharCallbacks : public NimBLECharacteristicCallbacks {
  // Each GATT write delivers up to BUNDLE_ATTR_MAX_LEN bytes. iOS prepends
  // a 4-byte LE u32 total-length to the first chunk; subsequent chunks are
  // raw bundle bytes. We accumulate into g_out_buf until we've seen the
  // declared total.
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*connInfo*/) override {
    if (g_bundle_received) {
      return;
    }
    NimBLEAttValue v = c->getValue();
    const uint8_t* data = v.data();
    size_t n = v.length();

    if (g_expected_len == 0) {
      if (n < 4) {
        Log.printf("ble: first chunk too short (%u bytes)\n", (unsigned)n);
        return;
      }
      g_expected_len = (uint32_t)data[0] |
                      ((uint32_t)data[1] << 8) |
                      ((uint32_t)data[2] << 16) |
                      ((uint32_t)data[3] << 24);
      if (g_expected_len < 60 || g_expected_len > g_out_cap) {
        Log.printf("ble: rejecting (declared len=%u out of range, cap=%u)\n",
                   (unsigned)g_expected_len, (unsigned)g_out_cap);
        g_expected_len = 0;
        return;
      }
      Log.printf("ble: receive start, expecting %u bytes\n", (unsigned)g_expected_len);
      data += 4;
      n -= 4;
    }

    if (g_out_len + n > g_expected_len) {
      Log.printf("ble: chunk overruns (have=%u +%u > expected=%u) — resetting\n",
                 (unsigned)g_out_len, (unsigned)n, (unsigned)g_expected_len);
      g_expected_len = 0;
      g_out_len = 0;
      return;
    }

    memcpy(g_out_buf + g_out_len, data, n);
    g_out_len += n;
    Log.printf("ble: chunk %u bytes, total %u/%u\n",
               (unsigned)n, (unsigned)g_out_len, (unsigned)g_expected_len);

    if (g_out_len == g_expected_len) {
      Log.println("ble: bundle complete");
      g_bundle_received = true;
    }
  }
};

class ConnCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*s*/, NimBLEConnInfo& /*connInfo*/) override {
    Log.println("ble: peer connected");
  }
  void onDisconnect(NimBLEServer* /*s*/, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
    Log.println("ble: peer disconnected");
    g_peer_disconnected = true;
  }
};

// Fires when the central reads the battery characteristic. We track this
// so the post-bundle grace window can exit early on read — if the central
// doesn't care about the battery characteristic, we still bail out on
// disconnect or after the grace window expires.
class BatteryCharCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* /*c*/, NimBLEConnInfo& /*connInfo*/) override {
    Log.println("ble: battery characteristic read by peer");
    g_battery_read = true;
  }
};

bool wait_for_bundle(uint8_t* out_buf, size_t cap,
                     size_t* out_len, uint32_t timeout_ms,
                     uint16_t battery_mv) {
  g_bundle_received = false;
  g_peer_disconnected = false;
  g_battery_read = false;
  g_out_buf = out_buf;
  g_out_cap = (cap < MAX_SEALED_BYTES) ? cap : MAX_SEALED_BYTES;
  g_out_len = 0;
  g_expected_len = 0;

  Log.printf("ble: starting peripheral, advertising for up to %ums\n",
             (unsigned)timeout_ms);

  // NimBLE init. ESP32-C3 shares the 2.4GHz radio between WiFi and BLE;
  // the caller has already torn down WiFi (WiFi.mode(WIFI_OFF)) before
  // getting here.
  NimBLEDevice::init(DEVICE_NAME);
  // Bump MTU above the 23-byte minimum so iOS Long Writes don't have to
  // shred a 20 KB bundle into ~900 round-trips.
  NimBLEDevice::setMTU(517);

  NimBLEServer* server = NimBLEDevice::createServer();
  static ConnCallbacks conn_cb;
  server->setCallbacks(&conn_cb);

  NimBLEService* service = server->createService(SERVICE_UUID);
  NimBLECharacteristic* bundle_char = service->createCharacteristic(
      CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::WRITE,
      BUNDLE_ATTR_MAX_LEN);
  static BundleCharCallbacks ch_cb;
  bundle_char->setCallbacks(&ch_cb);

  // Read-only battery characteristic. Value is snapshotted now (before
  // advertising) — there's no real cost to staleness because the BLE
  // session is short-lived and we entered it specifically because we
  // couldn't post the same reading over WiFi.
  NimBLECharacteristic* battery_char = service->createCharacteristic(
      BATTERY_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::READ,
      sizeof(battery_mv));
  const uint8_t battery_bytes[2] = {
      (uint8_t)(battery_mv & 0xFF),
      (uint8_t)((battery_mv >> 8) & 0xFF),
  };
  battery_char->setValue(battery_bytes, sizeof(battery_bytes));
  static BatteryCharCallbacks bat_cb;
  battery_char->setCallbacks(&bat_cb);
  Log.printf("ble: battery characteristic = %u mV\n", (unsigned)battery_mv);

  service->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setName(DEVICE_NAME);
  adv->start();

  // Block until the callback signals success or we time out. The 50 ms
  // poll is fine — BLE work happens on the NimBLE task.
  uint32_t start = millis();
  while (!g_bundle_received && (millis() - start) < timeout_ms) {
    delay(50);
  }

  bool ok = g_bundle_received;
  if (ok) {
    *out_len = g_out_len;
    Log.printf("ble: bundle received OK (%u bytes)\n", (unsigned)g_out_len);

    // Grace window so the iOS central can read the battery characteristic
    // before we tear the stack down. Exits early on (a) iOS reading the
    // battery and disconnecting cleanly, or (b) iOS disconnecting without
    // reading. Worst case is the full grace duration, which is bounded.
    Log.println("ble: grace window for battery read");
    constexpr uint32_t POST_BUNDLE_GRACE_MS = 5000;
    uint32_t grace_start = millis();
    while ((millis() - grace_start) < POST_BUNDLE_GRACE_MS) {
      if (g_peer_disconnected) {
        Log.printf("ble: peer disconnected during grace (battery_read=%d)\n",
                   g_battery_read ? 1 : 0);
        break;
      }
      delay(50);
    }
    if (!g_peer_disconnected) {
      Log.printf("ble: grace timeout (battery_read=%d)\n", g_battery_read ? 1 : 0);
    }
  } else {
    Log.println("ble: timeout — no bundle received");
  }

  // Teardown. deinit(true) is the natural choice but NimBLE-Arduino 2.5.0
  // asserts in heap_caps_free during full teardown (it tries to free a
  // static buffer). deinit(false) just stops the stack — the chip enters
  // deep sleep right after, which wipes RAM, so the leak is academic.
  NimBLEDevice::deinit(/*clearAll=*/false);
  return ok;
}

}  // namespace ble_bundle_receiver

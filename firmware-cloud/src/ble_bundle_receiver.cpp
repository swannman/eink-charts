#include "ble_bundle_receiver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "log_buffer.h"

namespace ble_bundle_receiver {

// Shared between the BLE task and the main waiting loop. Volatile reads on
// the main side give us "publication" semantics without needing a mutex —
// we only care that the flag flips and the bytes are present.
static volatile bool g_bundle_received = false;
static uint8_t* g_out_buf = nullptr;
static size_t g_out_cap = 0;
static size_t g_out_len = 0;

// NimBLE-Arduino 1.x callback signatures (no NimBLEConnInfo param —
// that's a 2.x addition).
class BundleCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    if (g_bundle_received) {
      // Ignore writes after we already accepted one — keeps the data race
      // window tiny and the main loop the sole reader.
      return;
    }
    const std::string& v = c->getValue();
    const size_t n = v.length();
    Log.printf("ble: onWrite n=%u\n", (unsigned)n);
    if (n < 60 || n > g_out_cap) {
      Log.printf("ble: rejecting (size out of range, cap=%u)\n", (unsigned)g_out_cap);
      return;
    }
    memcpy(g_out_buf, v.data(), n);
    g_out_len = n;
    g_bundle_received = true;
  }
};

class ConnCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*s*/) override {
    Log.println("ble: peer connected");
  }
  void onDisconnect(NimBLEServer* /*s*/) override {
    Log.println("ble: peer disconnected");
  }
};

bool wait_for_bundle(uint8_t* out_buf, size_t cap,
                     size_t* out_len, uint32_t timeout_ms) {
  g_bundle_received = false;
  g_out_buf = out_buf;
  g_out_cap = (cap < MAX_SEALED_BYTES) ? cap : MAX_SEALED_BYTES;
  g_out_len = 0;

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
      NIMBLE_PROPERTY::WRITE);
  static BundleCharCallbacks ch_cb;
  bundle_char->setCallbacks(&ch_cb);
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
  } else {
    Log.println("ble: timeout — no bundle received");
  }

  // Teardown. NimBLEDevice::deinit(true) frees memory so we don't carry
  // BLE state into deep sleep.
  NimBLEDevice::deinit(/*clearAll=*/true);
  return ok;
}

}  // namespace ble_bundle_receiver

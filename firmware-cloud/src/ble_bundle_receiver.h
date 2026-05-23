#pragma once
// BLE peripheral fallback used when WiFi is unavailable. The X3 advertises
// a single service with one writable characteristic; an iOS companion app
// (acting as BLE central) fetches the sealed bundle from the Cloudflare
// Worker and writes it here. The bundle stays end-to-end encrypted — the
// phone is just a transport.
//
// Trust boundary unchanged: the iOS app sees only ciphertext, and a fake
// peer can't forge a valid bundle because AES-GCM tag verification fails
// closed.

#include <stdint.h>
#include <stddef.h>

namespace ble_bundle_receiver {

// 0e1c... = "eink-charts" mnemonic. UUIDs must match the iOS app.
constexpr const char* SERVICE_UUID                 = "0e1c0a9c-1bb1-4f1e-8e26-1c3c5a3e9c7f";
constexpr const char* CHARACTERISTIC_UUID          = "0e1c0a9c-1bb1-4f1e-8e26-1c3c5a3e9c80";
// Read-only u16 LE millivolts (BQ27220 voltage at advertise time). The
// iOS app reads this and forwards it to the Worker's /battery endpoint
// so the synthetic battery panel keeps updating during BLE-only cycles.
// 0 means "no reading" (USB-powered, missing gauge, dead battery).
constexpr const char* BATTERY_CHARACTERISTIC_UUID  = "0e1c0a9c-1bb1-4f1e-8e26-1c3c5a3e9c81";
constexpr const char* DEVICE_NAME                  = "EInkCharts X3";

// Start advertising and block waiting for an iOS device to connect + write
// a sealed bundle. Returns true on success, populating `*out_len` with the
// number of bytes copied into `out_buf` (which must hold at least
// MAX_SEALED_BYTES). Returns false on timeout or any error. Always tears
// down BLE before returning so the caller can deep-sleep cleanly.
constexpr size_t MAX_SEALED_BYTES = 32 * 1024;
bool wait_for_bundle(uint8_t* out_buf, size_t cap,
                     size_t* out_len, uint32_t timeout_ms,
                     uint16_t battery_mv);

}  // namespace ble_bundle_receiver

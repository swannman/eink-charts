#pragma once
// Multi-credential Wi-Fi loader (native ESP32-S3 2.4 GHz radio). Ported from
// the X3 firmware, minus the per-network refresh floors (the TRMNL X uses a
// single global refresh cadence).
#include <Arduino.h>

#include <vector>

namespace wifi_config {

struct Network {
  String ssid;
  String password;
};

struct ConnectResult {
  bool ok;
  int index;
  String ssid;
};

constexpr int MAX_NETWORKS = 4;

// Load networks in priority order: NVS wifi_ssid_<i>/wifi_pass_<i>, then legacy
// wifi_ssid/wifi_pass, then compile-time DEFAULT_WIFI_SSID_<i>/DEFAULT_WIFI_SSID.
std::vector<Network> load();

// Try each network until one connects. per_net_timeout_ms is the per-attempt
// budget.
ConnectResult connect_any(const std::vector<Network>& nets, uint32_t per_net_timeout_ms);

}  // namespace wifi_config

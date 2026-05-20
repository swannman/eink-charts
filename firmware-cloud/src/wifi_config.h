#pragma once
// Multi-credential WiFi: try a prioritized list of networks on wake, and
// remember which one connected so we can honor that network's minimum
// refresh interval. Home WiFi gets a 5-min floor; phone hotspot gets a
// 1h floor so we don't gobble cellular.

#include <Arduino.h>

#include <vector>

namespace wifi_config {

struct Network {
  String ssid;
  String password;
  uint32_t min_refresh_sec;  // floor on time between WiFi fetches via this net
};

struct ConnectResult {
  bool ok;
  int index;               // index in the list (-1 = none)
  String ssid;             // for logging
  uint32_t min_refresh_sec;
};

// Default min refresh when nothing else specifies one (legacy single-net mode).
constexpr uint32_t DEFAULT_MIN_REFRESH_SEC = 300;

// Maximum number of indexed networks read from NVS (wifi_ssid_0..N-1).
constexpr int MAX_NETWORKS = 4;

// Load networks in priority order. First populated source wins:
//   1. NVS keys wifi_ssid_<i>/wifi_pass_<i>/wifi_min_<i> (i = 0..MAX_NETWORKS-1)
//   2. NVS legacy keys wifi_ssid/wifi_pass (single network, default floor)
//   3. Compile-time DEFAULT_WIFI_SSID_<i> (from secrets.h)
//   4. Compile-time legacy DEFAULT_WIFI_SSID
std::vector<Network> load();

// Try each network in order until one connects. `per_net_timeout_ms` is the
// per-attempt budget; total wall time = nets.size() * per_net_timeout_ms.
ConnectResult connect_any(const std::vector<Network>& nets,
                          uint32_t per_net_timeout_ms);

}  // namespace wifi_config

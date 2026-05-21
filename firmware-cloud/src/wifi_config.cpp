#include "wifi_config.h"

#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>

#include "config.h"
#include "log_buffer.h"

namespace wifi_config {

static bool nonEmpty(const char* s) { return s != nullptr && s[0] != '\0'; }

static void appendCompileTimeDefaults(std::vector<Network>& nets) {
#ifdef DEFAULT_WIFI_SSID_0
  if (nonEmpty(DEFAULT_WIFI_SSID_0)) {
    uint32_t mn =
#ifdef DEFAULT_WIFI_MIN_0
        DEFAULT_WIFI_MIN_0;
#else
        DEFAULT_MIN_REFRESH_SEC;
#endif
    nets.push_back({DEFAULT_WIFI_SSID_0, DEFAULT_WIFI_PASSWORD_0, mn});
  }
#endif
#ifdef DEFAULT_WIFI_SSID_1
  if (nonEmpty(DEFAULT_WIFI_SSID_1)) {
    uint32_t mn =
#ifdef DEFAULT_WIFI_MIN_1
        DEFAULT_WIFI_MIN_1;
#else
        DEFAULT_MIN_REFRESH_SEC;
#endif
    nets.push_back({DEFAULT_WIFI_SSID_1, DEFAULT_WIFI_PASSWORD_1, mn});
  }
#endif
#ifdef DEFAULT_WIFI_SSID_2
  if (nonEmpty(DEFAULT_WIFI_SSID_2)) {
    uint32_t mn =
#ifdef DEFAULT_WIFI_MIN_2
        DEFAULT_WIFI_MIN_2;
#else
        DEFAULT_MIN_REFRESH_SEC;
#endif
    nets.push_back({DEFAULT_WIFI_SSID_2, DEFAULT_WIFI_PASSWORD_2, mn});
  }
#endif
  // Legacy single-network compile-time default.
  if (nets.empty() && nonEmpty(DEFAULT_WIFI_SSID)) {
    nets.push_back({DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD, DEFAULT_MIN_REFRESH_SEC});
  }
}

std::vector<Network> load() {
  std::vector<Network> nets;

  Preferences prefs;
  if (prefs.begin("grafana-x3", /*readOnly=*/true)) {
    // Indexed entries take priority.
    for (int i = 0; i < MAX_NETWORKS; i++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", i);
      String ssid = prefs.getString(key, "");
      if (ssid.length() == 0) continue;
      snprintf(key, sizeof(key), "wifi_pass_%d", i);
      String pass = prefs.getString(key, "");
      snprintf(key, sizeof(key), "wifi_min_%d", i);
      uint32_t mn = prefs.getUInt(key, DEFAULT_MIN_REFRESH_SEC);
      nets.push_back({ssid, pass, mn});
    }
    // Legacy single-credential keys if no indexed entries were found.
    if (nets.empty()) {
      String legacy_ssid = prefs.getString("wifi_ssid", "");
      if (legacy_ssid.length() > 0) {
        nets.push_back({
            legacy_ssid,
            prefs.getString("wifi_pass", ""),
            DEFAULT_MIN_REFRESH_SEC,
        });
      }
    }
    prefs.end();
  }

  if (nets.empty()) {
    appendCompileTimeDefaults(nets);
  }

  // Log the resolved list so the device-logs screen shows which networks
  // were even candidates this boot. Helpful when debugging "did it try
  // my phone hotspot?" away from home.
  Log.printf("wifi_config: %u network(s) configured:\n", (unsigned)nets.size());
  for (size_t i = 0; i < nets.size(); i++) {
    Log.printf("  [%u] ssid='%s' floor=%us\n",
               (unsigned)i, nets[i].ssid.c_str(),
               (unsigned)nets[i].min_refresh_sec);
  }

  return nets;
}

ConnectResult connect_any(const std::vector<Network>& nets,
                          uint32_t per_net_timeout_ms) {
  if (nets.empty()) {
    Log.println("wifi: no networks configured");
    return {false, -1, String(), 0};
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (size_t i = 0; i < nets.size(); i++) {
    const Network& net = nets[i];
    if (net.ssid.length() == 0) continue;
    Log.printf("wifi: try %u/%u ssid='%s' (floor=%us)\n",
                  (unsigned)(i + 1), (unsigned)nets.size(),
                  net.ssid.c_str(), (unsigned)net.min_refresh_sec);
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
    WiFi.begin(net.ssid.c_str(), net.password.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < per_net_timeout_ms) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Log.printf("wifi: connected to '%s' ip=%s rssi=%d\n",
                    net.ssid.c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return {true, (int)i, net.ssid, net.min_refresh_sec};
    }
    Log.printf("wifi: '%s' timeout after %ums\n",
                  net.ssid.c_str(), (unsigned)per_net_timeout_ms);
  }

  Log.printf("wifi: ALL %u network(s) failed — no fetch this cycle\n",
             (unsigned)nets.size());
  return {false, -1, String(), 0};
}

}  // namespace wifi_config

#include "wifi_config.h"

#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>

#include "config.h"
#include "log.h"

namespace wifi_config {

static bool nonEmpty(const char* s) { return s != nullptr && s[0] != '\0'; }

static void appendCompileTimeDefaults(std::vector<Network>& nets) {
#ifdef DEFAULT_WIFI_SSID_0
  if (nonEmpty(DEFAULT_WIFI_SSID_0)) nets.push_back({DEFAULT_WIFI_SSID_0, DEFAULT_WIFI_PASSWORD_0});
#endif
#ifdef DEFAULT_WIFI_SSID_1
  if (nonEmpty(DEFAULT_WIFI_SSID_1)) nets.push_back({DEFAULT_WIFI_SSID_1, DEFAULT_WIFI_PASSWORD_1});
#endif
#ifdef DEFAULT_WIFI_SSID_2
  if (nonEmpty(DEFAULT_WIFI_SSID_2)) nets.push_back({DEFAULT_WIFI_SSID_2, DEFAULT_WIFI_PASSWORD_2});
#endif
  if (nets.empty() && nonEmpty(DEFAULT_WIFI_SSID)) {
    nets.push_back({DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD});
  }
}

std::vector<Network> load() {
  std::vector<Network> nets;

  Preferences prefs;
  if (prefs.begin("grafana-trmnlx", /*readOnly=*/true)) {
    for (int i = 0; i < MAX_NETWORKS; i++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", i);
      String ssid = prefs.getString(key, "");
      if (ssid.length() == 0) continue;
      snprintf(key, sizeof(key), "wifi_pass_%d", i);
      nets.push_back({ssid, prefs.getString(key, "")});
    }
    if (nets.empty()) {
      String legacy = prefs.getString("wifi_ssid", "");
      if (legacy.length() > 0) nets.push_back({legacy, prefs.getString("wifi_pass", "")});
    }
    prefs.end();
  }

  if (nets.empty()) appendCompileTimeDefaults(nets);

  Log.printf("wifi_config: %u network(s) configured\n", (unsigned)nets.size());
  return nets;
}

ConnectResult connect_any(const std::vector<Network>& nets, uint32_t per_net_timeout_ms) {
  if (nets.empty()) {
    Log.println("wifi: no networks configured");
    return {false, -1, String()};
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (size_t i = 0; i < nets.size(); i++) {
    const Network& net = nets[i];
    if (net.ssid.length() == 0) continue;
    Log.printf("wifi: try %u/%u ssid='%s'\n", (unsigned)(i + 1), (unsigned)nets.size(),
               net.ssid.c_str());
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
    WiFi.begin(net.ssid.c_str(), net.password.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < per_net_timeout_ms) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Log.printf("wifi: connected to '%s' ip=%s rssi=%d\n", net.ssid.c_str(),
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return {true, (int)i, net.ssid};
    }
    Log.printf("wifi: '%s' timeout\n", net.ssid.c_str());
  }
  Log.printf("wifi: ALL %u network(s) failed\n", (unsigned)nets.size());
  return {false, -1, String()};
}

}  // namespace wifi_config

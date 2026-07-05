#include "display_trmnl.h"

#include "config.h"
#include "gray_table.h"
#include "log.h"

FASTEPD epd;

namespace display_trmnl {

// Refresh counter, persisted across deep sleep. Like the official TRMNL firmware
// (display.cpp), we use the FAST (non-flashing, ~1s) clear mode for most refreshes
// and only the SLOW full clear (~3-4s, removes ghosting) every 8th update. Using
// CLEAR_SLOW on every advance was the bulk of the touch-advance "dead time".
RTC_DATA_ATTR static uint32_t sUpdateCount = 0;

bool begin() {
  int rc = epd.initPanel(BB_PANEL_TRMNL_X);
  if (rc != BBEP_SUCCESS) {
    Log.printf("display: initPanel failed rc=%d (PSRAM enabled?)\n", rc);
    return false;
  }
  // 3 passes each direction — the official firmware's setting for this panel.
  epd.setPasses(3, 3);
  epd.setMode(BB_MODE_4BPP);
  return true;
}

void clear(uint8_t gray) {
  epd.fillScreen(gray);
}

void present(bool keepOn) {
  // Load the vendor-calibrated 16-gray waveform right before the refresh, the
  // same order the official firmware uses.
  epd.setCustomMatrix(TRMNL_X_GRAY_MATRIX, sizeof(TRMNL_X_GRAY_MATRIX));
  // Fast (non-flashing) clear most of the time; full slow clear every 8th refresh
  // to purge accumulated ghosting — the official firmware's cadence.
  int clearMode = (sUpdateCount++ % 8 == 0) ? CLEAR_SLOW : CLEAR_FAST;
  epd.fullUpdate(clearMode, keepOn);
}

void sleep() {
  epd.einkPower(0);
  epd.deInit();
}

}  // namespace display_trmnl

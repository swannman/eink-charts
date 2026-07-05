#include "display_trmnl.h"

#include "config.h"
#include "gray_table.h"
#include "log.h"

FASTEPD epd;

namespace display_trmnl {

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
  epd.fullUpdate(CLEAR_SLOW, keepOn);
}

void sleep() {
  epd.einkPower(0);
  epd.deInit();
}

}  // namespace display_trmnl

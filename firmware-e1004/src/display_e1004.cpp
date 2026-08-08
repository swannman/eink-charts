#include "display_e1004.h"

#include <TFT_eSPI.h>   // Seeed_GFX; EPaper comes in via EPAPER_ENABLE (combo 523)

#include "config.h"
#include "framebuffer.h"
#include "log.h"
#include "sht4x_e1004.h"

// The one EPaper instance. Its constructor creates the 1200x1600 4bpp sprite
// (PSRAM via the CONFIG_SPIRAM_SUPPORT build flag — see platformio.ini).
static EPaper epaper;

namespace display_e1004 {

namespace {

// Spectra 6 palette code (config.h COL_*) -> Seeed_GFX 4bpp EPD color index.
// From Seeed_GFX TFT_eSPI.h under USE_COLORFULL_EPAPER:
//   TFT_WHITE=0x0, TFT_GREEN=0x2, TFT_RED=0x6, TFT_YELLOW=0xB, TFT_BLUE=0xD,
//   TFT_BLACK=0xF.
const uint8_t PAL2EPD[COL_COUNT] = {
    0x0F,  // COL_BLACK
    0x00,  // COL_WHITE
    0x06,  // COL_RED
    0x0B,  // COL_YELLOW
    0x02,  // COL_GREEN
    0x0D,  // COL_BLUE
};

bool gUp = false;

// Ambient temperature fed to the EPD controller. Spectra 6 waveform timing is
// temperature-banded: the library's default is a conservative 16 C, which at
// normal room temperature runs a slower, longer-flashing sequence than the
// panel needs. Reading the onboard SHT4x and handing the controller the REAL
// temperature lets it pick the correct (faster) waveform for the transition.
float gAmbientC = 16.0f;
float ambientTempCb() { return gAmbientC; }

}  // namespace

bool begin() {
  if (!gUp) {
    epaper.begin();
    gUp = true;
  }
  if (epaper.getPointer() == nullptr) {
    Log.println("display: sprite alloc FAILED (PSRAM?)");
    return false;
  }
  float t = 0, rh = 0;
  if (sht4x::read(&t, &rh) && t > -20.0f && t < 60.0f) {
    gAmbientC = t;
    epaper.setTemp(ambientTempCb);  // also re-sent by wake() on every update
    Log.printf("display: ambient %.1fC rh %.0f%% -> EPD waveform temp\n", t, rh);
  } else {
    Log.println("display: SHT4x read failed — EPD stays on default 16C waveform");
  }
  return true;
}

void present() {
  uint8_t* img = (uint8_t*)epaper.getPointer();
  const uint8_t* src = fb::buffer();
  if (!img || !src) return;

  // Landscape fb (1600x1200) -> portrait sprite (1200x1600), 90° rotation:
  // portrait (px, py) = (PORTRAIT_W - 1 - ly, lx). 4bpp: even px in the HIGH
  // nibble (Seeed_GFX sprite layout). Flip ROTATE_180 in config if the image
  // comes out upside down on the glass.
  constexpr int PW = SCREEN_H;   // 1200 (portrait width == sprite _iwidth)
#ifdef ROTATE_180
  constexpr bool flip = true;
#else
  constexpr bool flip = false;
#endif
  for (int ly = 0; ly < SCREEN_H; ly++) {
    const uint8_t* row = src + (size_t)ly * SCREEN_W;
    for (int lx = 0; lx < SCREEN_W; lx++) {
      uint8_t code = row[lx];
      uint8_t c = PAL2EPD[code < COL_COUNT ? code : COL_WHITE];
      int px = PW - 1 - ly;
      int py = lx;
      if (flip) { px = PW - 1 - px; py = SCREEN_W - 1 - py; }
      size_t idx = ((size_t)px + (size_t)py * PW) >> 1;
      if ((px & 1) == 0) {
        img[idx] = (uint8_t)((c << 4) | (img[idx] & 0x0F));
      } else {
        img[idx] = (uint8_t)((img[idx] & 0xF0) | c);
      }
    }
  }

  Log.println("display: refreshing (Spectra 6 full refresh, ~25s)...");
  uint32_t t0 = millis();
  epaper.update();   // wakes the panel, pushes both planes, refreshes, sleeps
  Log.printf("display: refresh done in %lums\n", (unsigned long)(millis() - t0));
}

void sleep() {
  if (gUp) epaper.sleep();
}

}  // namespace display_e1004

#include "enroll_screen.h"

#include <qrcode.h>
#include <string.h>

#include "config.h"
#include "display_trmnl.h"   // extern FASTEPD epd
#include "gfx4.h"

namespace enroll_screen {

void show(const char* pubkey_b64) {
  display_trmnl::clear(GRAY_WHITE);

  gfx4::drawTextCenteredFit(0, 60, SCREEN_W, 56, "TRMNL X - scan to enroll", GRAY_BLACK);

  // QR of the base64url pubkey. Version 4 (33x33) fits a ~43-char key in byte
  // mode at ECC_LOW comfortably.
  const int VERSION = 4;
  // Version 4 (33x33) needs ~137 bytes; qrcode_getBufferSize() isn't
  // constexpr so we over-allocate a fixed buffer.
  static uint8_t qrData[200];
  QRCode qr;
  qrcode_initText(&qr, qrData, VERSION, ECC_LOW, pubkey_b64);

  const int module = 28;
  const int qrPx = qr.size * module;
  const int qx = (SCREEN_W - qrPx) / 2;
  const int qy = 180;
  // Quiet-zone: the clear() already left white around it.
  for (int my = 0; my < qr.size; my++) {
    for (int mx = 0; mx < qr.size; mx++) {
      if (qrcode_getModule(&qr, mx, my)) {
        epd.fillRect(qx + mx * module, qy + my * module, module, module, GRAY_BLACK);
      }
    }
  }

  // Visible key text (fallback for manual entry) + instructions.
  int ty = qy + qrPx + 60;
  gfx4::drawTextCenteredFit(0, ty, SCREEN_W, 34, pubkey_b64, GRAY_BLACK);
  gfx4::drawTextCenteredFit(0, ty + 70, SCREEN_W, 30,
                            "Paste into the bridge as TRMNL_PUBKEY_B64, then press touch.",
                            4);
}

}  // namespace enroll_screen

#include "enroll_screen.h"

#include <qrcode.h>
#include <string.h>

#include "config.h"
#include "framebuffer.h"
#include "gfxc.h"

namespace enroll_screen {

void show(const char* pubkey_b64) {
  fb::clear(COL_WHITE);

  gfxc::drawTextCenteredFit(0, 60, SCREEN_W, 50, "reTerminal E1004 - scan to enroll",
                            COL_BLACK);

  // QR of the base64url pubkey. Version 4 (33x33) fits a ~43-char key in byte
  // mode at ECC_LOW comfortably.
  const int VERSION = 4;
  static uint8_t qrData[200];
  QRCode qr;
  qrcode_initText(&qr, qrData, VERSION, ECC_LOW, pubkey_b64);

  const int module = 24;
  const int qrPx = qr.size * module;
  const int qx = (SCREEN_W - qrPx) / 2;
  const int qy = 170;
  // Quiet-zone: the clear() already left white around it.
  for (int my = 0; my < qr.size; my++) {
    for (int mx = 0; mx < qr.size; mx++) {
      if (qrcode_getModule(&qr, mx, my)) {
        fb::fillRect(qx + mx * module, qy + my * module, module, module, COL_BLACK);
      }
    }
  }

  // Visible key text (fallback for manual entry) + instructions.
  int ty = qy + qrPx + 50;
  gfxc::drawTextCenteredFit(0, ty, SCREEN_W, 30, pubkey_b64, COL_BLACK);
  gfxc::drawTextCenteredFit(0, ty + 60, SCREEN_W, 26,
                            "Paste into the bridge as E1004_PUBKEY_B64, then press the green button.",
                            COL_BLUE);
}

}  // namespace enroll_screen

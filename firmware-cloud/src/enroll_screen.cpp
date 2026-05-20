#include "enroll_screen.h"

#include <qrcode.h>
#include <string.h>

#include "gfx_lite.h"
#include "x25519_keystore.h"

namespace enroll_screen {

// QR Version 3 (29×29 modules) at ECC_LOW holds 53 bytes — the 43-char
// base64url pubkey fits with margin. 12 px per module → 348×348 image
// (large enough to scan from arm's length).
static constexpr uint8_t QR_VERSION = 3;
static constexpr int QR_SCALE = 12;
static constexpr int QR_QUIET_MODULES = 2;  // 2-module white border each side

static void drawQr(uint8_t* fb, const char* text, int x0, int y0) {
  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(QR_VERSION)];
  qrcode_initText(&qr, buf, QR_VERSION, ECC_LOW, text);

  // Paint the quiet zone (white background) first.
  int total_modules = qr.size + 2 * QR_QUIET_MODULES;
  fbFillRect(fb, x0, y0, total_modules * QR_SCALE, total_modules * QR_SCALE, /*black=*/false);

  int origin_x = x0 + QR_QUIET_MODULES * QR_SCALE;
  int origin_y = y0 + QR_QUIET_MODULES * QR_SCALE;
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        fbFillRect(fb, origin_x + x * QR_SCALE, origin_y + y * QR_SCALE,
                   QR_SCALE, QR_SCALE, /*black=*/true);
      }
    }
  }
}

void render(uint8_t* fb, const uint8_t pubkey[32]) {
  fbClear(fb, /*white=*/true);

  char b64[48];
  x25519_keystore::b64url_encode(pubkey, 32, b64, sizeof(b64));

  // Title (scale 4 → 20 px wide, 28 px tall glyphs).
  fbDrawStringCentered(fb, /*y=*/14, /*scale=*/4, "X3 ENROLLMENT", /*black=*/true);
  fbDrawStringCentered(fb, /*y=*/56, /*scale=*/2, "Scan or copy the public key below", true);

  // QR. Total footprint = (qr.size + 2*quiet) * scale.
  // V3: 29 + 4 = 33 modules × 12 px = 396 px.
  constexpr int QR_PX = (29 + 2 * QR_QUIET_MODULES) * QR_SCALE;
  int qr_x = (FB_WIDTH - QR_PX) / 2;
  int qr_y = 90;
  drawQr(fb, b64, qr_x, qr_y);

  // Key label + base64 text below.
  int text_y = qr_y + QR_PX + 12;
  fbDrawStringCentered(fb, text_y, /*scale=*/2, "X3 pubkey (base64url):", true);
  fbDrawStringCentered(fb, text_y + 22, /*scale=*/2, b64, true);

  // Footer hint.
  fbDrawStringCentered(fb, FB_HEIGHT - 18, /*scale=*/1,
                       "Paste into /etc/default/grafana-push as X3_PUBKEY_B64", true);
}

}  // namespace enroll_screen

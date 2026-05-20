#pragma once
// First-boot enrollment screen: renders a QR code of the X3's base64url
// public key plus the same key as legible text, with instructions for
// pasting it into the Pi push config.

#include <stdint.h>

namespace enroll_screen {

// pubkey: 32-byte X25519 public key (raw bytes).
// fb: 1-bit framebuffer (792×528, 99 bytes per row, MSB = leftmost pixel).
void render(uint8_t* fb, const uint8_t pubkey[32]);

}  // namespace enroll_screen

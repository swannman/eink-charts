#pragma once
// First-boot enrollment screen: a big QR code of the device's X25519 public
// key plus the key as visible text, so the operator can paste E1004_PUBKEY_B64
// into the bridge config. Draws into the fb:: framebuffer; caller presents.

namespace enroll_screen {

void show(const char* pubkey_b64);

}  // namespace enroll_screen

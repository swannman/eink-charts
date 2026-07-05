#pragma once
// Persistent X25519 keypair for the TRMNL X, used to decrypt bundles sealed by
// the Pi. Private key never leaves the device; the pubkey is shown via QR +
// serial for the bridge's TRMNL_PUBKEY_B64 config.
//
// Storage: NVS namespace "trmnlx-crypto", separate from the bundle cache.
#include <stddef.h>
#include <stdint.h>

namespace x25519_keystore {

constexpr size_t KEY_LEN = 32;

bool exists();
bool load(uint8_t sk_out[KEY_LEN], uint8_t pk_out[KEY_LEN]);
bool generate_and_store(uint8_t sk_out[KEY_LEN], uint8_t pk_out[KEY_LEN]);

// Base64url encode (no padding). out needs >= 4*((n+2)/3)+1 bytes.
size_t b64url_encode(const uint8_t* in, size_t n, char* out, size_t cap);

}  // namespace x25519_keystore

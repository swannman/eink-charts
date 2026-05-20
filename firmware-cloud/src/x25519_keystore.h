#pragma once
// Persistent X25519 keypair for the X3, used to decrypt bundles sealed by
// the Pi (push) and pulled from the Worker. The private key never leaves
// the device — the pubkey is displayed via QR + serial for the Pi config.
//
// Storage: NVS namespace "x3-crypto", separate from "x3-cache" so the
// firmware-update cache wipe in main.cpp doesn't blow away keys.

#include <stdint.h>
#include <stddef.h>

namespace x25519_keystore {

constexpr size_t KEY_LEN = 32;

// True if both private + public key are present in NVS.
bool exists();

// Load existing keypair. Returns true on success.
bool load(uint8_t sk_out[KEY_LEN], uint8_t pk_out[KEY_LEN]);

// Generate a fresh keypair via mbedTLS, persist to NVS, and return both
// halves. Returns true on success. Overwrites any prior keypair.
bool generate_and_store(uint8_t sk_out[KEY_LEN], uint8_t pk_out[KEY_LEN]);

// Base64url encode (no padding). Caller must size out to at least 4*((n+2)/3)+1
// bytes (47 for KEY_LEN=32). Returns the C-string length written (excluding
// NUL).
size_t b64url_encode(const uint8_t* in, size_t n, char* out, size_t cap);

}  // namespace x25519_keystore

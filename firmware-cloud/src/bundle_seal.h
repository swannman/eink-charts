#pragma once
// Decryption of bundles sealed by the Pi push (bridge/.../push.py).
//
// Wire format (matches push.py seal()):
//   [32B ephemeral_pk] [12B nonce] [ciphertext] [16B Poly1305 tag]
// Overhead: 60 bytes.
//
// Scheme: X25519(x3_sk, ephemeral_pk) → shared, then
//         HKDF-SHA256(shared, salt = ephemeral_pk || x3_pk,
//                     info = "EInkCharts seal v1") → 32-byte key,
//         then ChaCha20-Poly1305 decrypt with that key + nonce.
//
// The HKDF info string and salt construction MUST match push.py exactly,
// or decryption fails closed (Poly1305 tag will not validate).

#include <stddef.h>
#include <stdint.h>

namespace bundle_seal {

// Bytes added by seal() on top of the plaintext.
constexpr size_t OVERHEAD_BYTES = 32 + 12 + 16;

// Decrypt `sealed` into `out_buf`. `sk` and `pk` are this device's own
// X25519 keypair (loaded from NVS by x25519_keystore).
//
// Returns plaintext length on success, -1 on auth failure / malformed input.
int unseal(const uint8_t sk[32], const uint8_t pk[32],
           const uint8_t* sealed, size_t sealed_len,
           uint8_t* out_buf, size_t out_cap);

}  // namespace bundle_seal

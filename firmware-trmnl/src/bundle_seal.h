#pragma once
// Decrypt bundles sealed by the Pi push (bridge-cloud/.../push.py:seal()).
// Identical scheme + wire format to the X3 firmware — the bridge uses the same
// seal() for both devices, just with different recipient keys.
//
// Wire format: [32B ephemeral_pk][12B nonce][ciphertext][16B GCM tag]
// Scheme: X25519(sk, epk) -> shared; HKDF-SHA256(shared, salt=epk||pk,
//         info="EInkCharts seal v1") -> key; AES-256-GCM decrypt.
// The HKDF info string MUST match push.py exactly or decryption fails closed.
#include <stddef.h>
#include <stdint.h>

namespace bundle_seal {

constexpr size_t OVERHEAD_BYTES = 32 + 12 + 16;

// Decrypt `sealed` into `out_buf` using this device's own X25519 keypair.
// Returns plaintext length on success, -1 on auth failure / malformed input.
int unseal(const uint8_t sk[32], const uint8_t pk[32],
           const uint8_t* sealed, size_t sealed_len,
           uint8_t* out_buf, size_t out_cap);

}  // namespace bundle_seal

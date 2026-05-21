#include "bundle_seal.h"

#include <Arduino.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

namespace bundle_seal {

// Must match HKDF_INFO in bridge/src/grafana_bridge/push.py — any divergence
// causes the Poly1305 tag check to fail rather than producing wrong data.
static const char INFO_STR[] = "EInkCharts seal v1";

// Curve25519 scalar multiplication: shared = sk * peer_pk (X coordinate only).
// Both inputs are 32-byte little-endian. Returns true on success.
//
// mbedtls_ecp_mul on Montgomery curves needs an RNG callback for side-channel
// blinding. Passing NULL produces a misleading MBEDTLS_ERR_ECP_INVALID_KEY
// (-0x4F80) — burnt a session debugging that, so the entropy + ctr_drbg
// dance below is mandatory, not optional.
static bool x25519_ecdh(const uint8_t sk[32], const uint8_t peer_pk[32],
                        uint8_t shared_out[32]) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ecp_group grp;
  mbedtls_mpi sk_mpi;
  mbedtls_ecp_point peer_point;
  mbedtls_ecp_point shared_point;

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&sk_mpi);
  mbedtls_ecp_point_init(&peer_point);
  mbedtls_ecp_point_init(&shared_point);

  bool ok = false;
  int ret;
  do {
    static const char pers[] = "x3-x25519-ecdh";
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const uint8_t*)pers, sizeof(pers) - 1);
    if (ret != 0) break;

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret != 0) break;

    ret = mbedtls_mpi_read_binary_le(&sk_mpi, sk, 32);
    if (ret != 0) break;

    // For Curve25519 (Montgomery form), the public point is just its X
    // coordinate (little-endian). Z = 1 makes the point affine.
    ret = mbedtls_mpi_lset(&peer_point.MBEDTLS_PRIVATE(Z), 1);
    if (ret != 0) break;
    ret = mbedtls_mpi_read_binary_le(&peer_point.MBEDTLS_PRIVATE(X),
                                     peer_pk, 32);
    if (ret != 0) break;

    ret = mbedtls_ecp_mul(&grp, &shared_point, &sk_mpi, &peer_point,
                          mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) break;

    ret = mbedtls_mpi_write_binary_le(&shared_point.MBEDTLS_PRIVATE(X),
                                      shared_out, 32);
    if (ret != 0) break;

    ok = true;
  } while (0);

  if (!ok) {
    Serial.printf("bundle_seal: x25519 ecdh err=-0x%04x\n", -ret);
  }

  mbedtls_ecp_point_free(&shared_point);
  mbedtls_ecp_point_free(&peer_point);
  mbedtls_mpi_free(&sk_mpi);
  mbedtls_ecp_group_free(&grp);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

int unseal(const uint8_t sk[32], const uint8_t pk[32],
           const uint8_t* sealed, size_t sealed_len,
           uint8_t* out_buf, size_t out_cap) {
  if (sealed_len < OVERHEAD_BYTES) {
    Serial.println("bundle_seal: sealed too short");
    return -1;
  }
  const size_t ct_len = sealed_len - OVERHEAD_BYTES;
  if (ct_len > out_cap) {
    Serial.printf("bundle_seal: out_cap=%u < needed=%u\n",
                  (unsigned)out_cap, (unsigned)ct_len);
    return -1;
  }

  const uint8_t* epk = sealed;
  const uint8_t* nonce = sealed + 32;
  const uint8_t* ct = sealed + 32 + 12;
  const uint8_t* tag = ct + ct_len;

  // Step 1: ECDH.
  uint8_t shared[32];
  if (!x25519_ecdh(sk, epk, shared)) return -1;

  // Step 2: HKDF-SHA256. salt = epk || pk; info = INFO_STR; ikm = shared.
  uint8_t salt[64];
  memcpy(salt, epk, 32);
  memcpy(salt + 32, pk, 32);

  uint8_t key[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) {
    Serial.println("bundle_seal: SHA256 md_info missing");
    return -1;
  }
  int ret = mbedtls_hkdf(md, salt, sizeof(salt), shared, sizeof(shared),
                         (const uint8_t*)INFO_STR, sizeof(INFO_STR) - 1,
                         key, sizeof(key));
  if (ret != 0) {
    Serial.printf("bundle_seal: hkdf err=-0x%04x\n", -ret);
    return -1;
  }

  // Step 3: AES-256-GCM authenticated decrypt. ESP32-C3's mbedTLS ships
  // chachapoly disabled but GCM enabled (it's required by TLS). Same wire
  // shape: 12-byte nonce, 16-byte tag, no AAD.
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, /*keybits=*/256);
  if (ret == 0) {
    ret = mbedtls_gcm_auth_decrypt(
        &gcm, ct_len,
        nonce, /*iv_len=*/12,
        /*add=*/nullptr, /*add_len=*/0,
        tag, /*tag_len=*/16,
        ct, out_buf);
  }
  mbedtls_gcm_free(&gcm);

  if (ret != 0) {
    // Most common failure: tag mismatch. Could be wrong key (Pi has a
    // different X3_PUBKEY_B64 than this device), wrong INFO_STR, or
    // tampered ciphertext.
    Serial.printf("bundle_seal: gcm_decrypt err=-0x%04x\n", -ret);
    return -1;
  }
  return (int)ct_len;
}

}  // namespace bundle_seal

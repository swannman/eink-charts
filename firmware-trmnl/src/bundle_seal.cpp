#include "bundle_seal.h"

#include <Arduino.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include "log.h"

namespace bundle_seal {

// MUST match HKDF_INFO in bridge-cloud/.../push.py.
static const char INFO_STR[] = "EInkCharts seal v1";

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
    static const char pers[] = "trmnlx-x25519-ecdh";
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const uint8_t*)pers, sizeof(pers) - 1);
    if (ret != 0) break;

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret != 0) break;

    ret = mbedtls_mpi_read_binary_le(&sk_mpi, sk, 32);
    if (ret != 0) break;

    ret = mbedtls_mpi_lset(&peer_point.MBEDTLS_PRIVATE(Z), 1);
    if (ret != 0) break;
    ret = mbedtls_mpi_read_binary_le(&peer_point.MBEDTLS_PRIVATE(X), peer_pk, 32);
    if (ret != 0) break;

    ret = mbedtls_ecp_mul(&grp, &shared_point, &sk_mpi, &peer_point,
                          mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) break;

    ret = mbedtls_mpi_write_binary_le(&shared_point.MBEDTLS_PRIVATE(X), shared_out, 32);
    if (ret != 0) break;

    ok = true;
  } while (0);

  if (!ok) Log.printf("bundle_seal: x25519 ecdh err=-0x%04x\n", -ret);

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
    Log.println("bundle_seal: sealed too short");
    return -1;
  }
  const size_t ct_len = sealed_len - OVERHEAD_BYTES;
  if (ct_len > out_cap) {
    Log.printf("bundle_seal: out_cap=%u < needed=%u\n", (unsigned)out_cap, (unsigned)ct_len);
    return -1;
  }

  const uint8_t* epk = sealed;
  const uint8_t* nonce = sealed + 32;
  const uint8_t* ct = sealed + 32 + 12;
  const uint8_t* tag = ct + ct_len;

  uint8_t shared[32];
  if (!x25519_ecdh(sk, epk, shared)) return -1;

  uint8_t salt[64];
  memcpy(salt, epk, 32);
  memcpy(salt + 32, pk, 32);

  uint8_t key[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) {
    Log.println("bundle_seal: SHA256 md_info missing");
    return -1;
  }
  int ret = mbedtls_hkdf(md, salt, sizeof(salt), shared, sizeof(shared),
                         (const uint8_t*)INFO_STR, sizeof(INFO_STR) - 1,
                         key, sizeof(key));
  if (ret != 0) {
    Log.printf("bundle_seal: hkdf err=-0x%04x\n", -ret);
    return -1;
  }

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, /*keybits=*/256);
  if (ret == 0) {
    ret = mbedtls_gcm_auth_decrypt(&gcm, ct_len, nonce, /*iv_len=*/12,
                                   /*add=*/nullptr, /*add_len=*/0,
                                   tag, /*tag_len=*/16, ct, out_buf);
  }
  mbedtls_gcm_free(&gcm);

  if (ret != 0) {
    Log.printf("bundle_seal: gcm_decrypt err=-0x%04x (wrong key / tampered?)\n", -ret);
    return -1;
  }
  return (int)ct_len;
}

}  // namespace bundle_seal

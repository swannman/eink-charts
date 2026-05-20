#include "x25519_keystore.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>

namespace x25519_keystore {

static constexpr const char* NS = "x3-crypto";
static constexpr const char* K_SK = "sk";
static constexpr const char* K_PK = "pk";

bool exists() {
  Preferences p;
  if (!p.begin(NS, /*readOnly=*/true)) return false;
  bool ok = (p.getBytesLength(K_SK) == KEY_LEN) &&
            (p.getBytesLength(K_PK) == KEY_LEN);
  p.end();
  return ok;
}

bool load(uint8_t sk_out[KEY_LEN], uint8_t pk_out[KEY_LEN]) {
  Preferences p;
  if (!p.begin(NS, /*readOnly=*/true)) return false;
  size_t n_sk = p.getBytes(K_SK, sk_out, KEY_LEN);
  size_t n_pk = p.getBytes(K_PK, pk_out, KEY_LEN);
  p.end();
  return (n_sk == KEY_LEN) && (n_pk == KEY_LEN);
}

bool generate_and_store(uint8_t sk_out[KEY_LEN], uint8_t pk_out[KEY_LEN]) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ecp_group grp;
  mbedtls_mpi sk_mpi;
  mbedtls_ecp_point pk_point;

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&sk_mpi);
  mbedtls_ecp_point_init(&pk_point);

  bool ok = false;
  int ret;
  do {
    static const char pers[] = "x3-x25519-keygen";
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const uint8_t*)pers, sizeof(pers) - 1);
    if (ret != 0) { Serial.printf("keystore: ctr_drbg_seed err=-0x%04x\n", -ret); break; }

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret != 0) { Serial.printf("keystore: group_load err=-0x%04x\n", -ret); break; }

    ret = mbedtls_ecp_gen_keypair(&grp, &sk_mpi, &pk_point,
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) { Serial.printf("keystore: gen_keypair err=-0x%04x\n", -ret); break; }

    // Curve25519 stores the scalar in little-endian; X25519's "public key" is
    // the X coordinate, also little-endian.
    ret = mbedtls_mpi_write_binary_le(&sk_mpi, sk_out, KEY_LEN);
    if (ret != 0) { Serial.printf("keystore: write sk err=-0x%04x\n", -ret); break; }

    size_t pk_olen = 0;
    ret = mbedtls_ecp_point_write_binary(&grp, &pk_point,
                                         MBEDTLS_ECP_PF_UNCOMPRESSED,
                                         &pk_olen, pk_out, KEY_LEN);
    if (ret != 0 || pk_olen != KEY_LEN) {
      Serial.printf("keystore: write pk err=-0x%04x olen=%u\n", -ret, (unsigned)pk_olen);
      break;
    }

    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) {
      Serial.println("keystore: NVS open failed");
      break;
    }
    // Drop any prior pair first so a partial write doesn't leave inconsistent
    // sk/pk.
    p.remove(K_SK);
    p.remove(K_PK);
    size_t w_sk = p.putBytes(K_SK, sk_out, KEY_LEN);
    size_t w_pk = p.putBytes(K_PK, pk_out, KEY_LEN);
    p.end();
    if (w_sk != KEY_LEN || w_pk != KEY_LEN) {
      Serial.printf("keystore: NVS write failed sk=%u pk=%u\n",
                    (unsigned)w_sk, (unsigned)w_pk);
      break;
    }
    ok = true;
  } while (0);

  mbedtls_ecp_point_free(&pk_point);
  mbedtls_mpi_free(&sk_mpi);
  mbedtls_ecp_group_free(&grp);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

size_t b64url_encode(const uint8_t* in, size_t n, char* out, size_t cap) {
  static const char ALPHA[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t i = 0, o = 0;
  while (i + 2 < n) {
    if (o + 4 >= cap) break;
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = ALPHA[(v >> 18) & 0x3f];
    out[o++] = ALPHA[(v >> 12) & 0x3f];
    out[o++] = ALPHA[(v >> 6) & 0x3f];
    out[o++] = ALPHA[v & 0x3f];
    i += 3;
  }
  // Tail (no padding, base64url convention).
  if (i < n && o + 2 < cap) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    out[o++] = ALPHA[(v >> 18) & 0x3f];
    out[o++] = ALPHA[(v >> 12) & 0x3f];
    if (i + 1 < n && o < cap) out[o++] = ALPHA[(v >> 6) & 0x3f];
  }
  if (o < cap) out[o] = 0;
  return o;
}

}  // namespace x25519_keystore

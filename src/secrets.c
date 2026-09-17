/** @file secrets.c
 *  @brief AES-256-GCM helpers (see secrets.h). */
#include "secrets.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

int hex_to_bytes32(const char *hex64, uint8_t out[32])
{
  if (hex64 == NULL || strlen(hex64) != 64)
    return -1;
  for (int i = 0; i < 64; i++) {
    char c = hex64[i];
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    if (i % 2 == 0)
      out[i / 2] = (uint8_t)(v << 4);
    else
      out[i / 2] |= (uint8_t)v;
  }
  return 0;
}

static const char HEXD[] = "0123456789abcdef";

int secret_encrypt(const uint8_t master[32], const void *plain, size_t plain_len,
                   char *out, size_t out_cap)
{
  unsigned char nonce[12], tag[16];
  size_t blob_len = 3 + 12 + plain_len + 16; /* "v1:" + nonce + ct + tag */
  unsigned char *blob = malloc(blob_len);
  if (blob == NULL)
    return -1;
  if (out_cap < 2 * blob_len + 1) {
    free(blob);
    return -1;
  }

  memcpy(blob, "v1:", 3);
  if (RAND_bytes(nonce, 12) != 1) {
    free(blob);
    return -1;
  }
  memcpy(blob + 3, nonce, 12);

  EVP_CIPHER_CTX *cx = EVP_CIPHER_CTX_new();
  size_t ct_off = 3 + 12;
  int ct_len = 0;
  int rc = -1;
  if (cx == NULL)
    goto done;
  if (EVP_EncryptInit_ex(cx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
    goto done;
  if (EVP_CIPHER_CTX_ctrl(cx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
    goto done;
  if (EVP_EncryptInit_ex(cx, NULL, NULL, master, nonce) != 1)
    goto done;
  if (EVP_EncryptUpdate(cx, blob + ct_off, &ct_len, plain, plain_len) != 1)
    goto done;
  if ((size_t)ct_len != plain_len)
    goto done;
  if (EVP_EncryptFinal_ex(cx, blob + ct_off + (size_t)ct_len, &ct_len) != 1)
    goto done;
  if (EVP_CIPHER_CTX_ctrl(cx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
    goto done;
  memcpy(blob + ct_off + plain_len, tag, 16);

  for (size_t i = 0; i < blob_len; i++) {
    out[2 * i] = HEXD[blob[i] >> 4];
    out[2 * i + 1] = HEXD[blob[i] & 0xF];
  }
  out[2 * blob_len] = '\0';
  rc = 0;

done:
  EVP_CIPHER_CTX_free(cx);
  free(blob);
  return rc;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int secret_decrypt(const uint8_t master[32], const char *blob,
                   char *out, size_t out_cap, size_t *out_len)
{
  size_t hexlen = strlen(blob);
  if (hexlen % 2 != 0 || hexlen < 2 * (3 + 12 + 16))
    return -1;
  size_t blob_bytes = hexlen / 2;
  unsigned char *raw = malloc(blob_bytes);
  if (raw == NULL)
    return -1;

  for (size_t i = 0; i < blob_bytes; i++) {
    int hi = hexval(blob[2 * i]);
    int lo = hexval(blob[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      free(raw);
      return -1;
    }
    raw[i] = (unsigned char)((hi << 4) | lo);
  }
  if (memcmp(raw, "v1:", 3) != 0) {
    free(raw);
    return -1;
  }
  unsigned char *nonce = raw + 3;
  size_t plain_len = blob_bytes - 3 - 12 - 16;
  if (out_cap < plain_len + 1) {
    free(raw);
    return -1;
  }
  unsigned char *ct = raw + 3 + 12;
  unsigned char *tag = raw + 3 + 12 + plain_len;

  EVP_CIPHER_CTX *cx = EVP_CIPHER_CTX_new();
  int rc = -1;
  int got = 0;
  if (cx == NULL)
    goto done;
  if (EVP_DecryptInit_ex(cx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
    goto done;
  if (EVP_CIPHER_CTX_ctrl(cx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
    goto done;
  if (EVP_DecryptInit_ex(cx, NULL, NULL, master, nonce) != 1)
    goto done;
  if (EVP_DecryptUpdate(cx, (unsigned char *)out, &got, ct, plain_len) != 1)
    goto done;
  if (EVP_CIPHER_CTX_ctrl(cx, EVP_CTRL_GCM_SET_TAG, 16, tag) != 1)
    goto done;
  int fin = 0;
  if (EVP_DecryptFinal_ex(cx, (unsigned char *)out + (size_t)got, &fin) != 1)
    goto done;
  if (out_len != NULL)
    *out_len = plain_len;
  out[plain_len] = '\0';
  rc = 0;

done:
  EVP_CIPHER_CTX_free(cx);
  free(raw);
  return rc;
}

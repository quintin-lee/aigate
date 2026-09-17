/** @file test_secrets.c
 *  @brief AES-256-GCM round-trip + tamper + master-key mismatch tests. */
#include "run_tests.h"
#include "secrets.h"
#include <string.h>

static void make_master(uint8_t m[32])
{
  for (int i = 0; i < 32; i++)
    m[i] = (uint8_t)i;
}

TEST_CASE(test_secret_roundtrip)
{
  uint8_t master[32];
  make_master(master);
  char blob[4096], plain[2048];

  const char *msg = "sk-upstream-secret-1234567890";
  size_t out_len = 0;

  TEST_ASSERT(secret_encrypt(master, msg, strlen(msg), blob, sizeof blob) == 0,
              "encrypt ok");
  TEST_ASSERT(strncmp(blob, "76313a", 6) == 0, "hex starts with 'v1:'");
  TEST_ASSERT(secret_decrypt(master, blob, plain, sizeof plain, &out_len) == 0,
              "decrypt ok");
  TEST_ASSERT(out_len == strlen(msg), "length round-trip, got %zu", out_len);
  TEST_ASSERT(strncmp(plain, msg, out_len) == 0, "content round-trip");
}

TEST_CASE(test_secret_tamper_and_wrong_key)
{
  uint8_t master[32], other[32];
  char blob[4096], plain[2048];
  make_master(master);
  for (int i = 0; i < 32; i++)
    other[i] = (uint8_t)(i + 1);

  TEST_ASSERT(secret_encrypt(master, "data", 4, blob, sizeof blob) == 0,
              "encrypt");
  /* tamper ciphertext (first byte of hex payload region after "v1:" prefix) */
  char tampered[4096];
  strcpy(tampered, blob);
  tampered[12] = (tampered[12] == 'a') ? 'b' : 'a';
  TEST_ASSERT(secret_decrypt(master, tampered, plain, sizeof plain, NULL) != 0,
              "tampered blob rejected");

  TEST_ASSERT(secret_decrypt(other, blob, plain, sizeof plain, NULL) != 0,
              "wrong master rejected");
}

TEST_CASE(test_secret_hex_to_bytes)
{
  uint8_t b[32];
  TEST_ASSERT(hex_to_bytes32(
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", b) == 0,
              "hex_to_bytes32 ok");
  TEST_ASSERT(b[0] == 0x00 && b[1] == 0x11 && b[15] == 0xff, "bytes decoded");
  TEST_ASSERT(hex_to_bytes32("short", b) == -1, "short rejected");
  TEST_ASSERT(hex_to_bytes32(
      "zzzz2233445566778899aabbccddeeff00112233445566778899aabbccddeeff", b) == -1,
              "bad hex rejected");
}

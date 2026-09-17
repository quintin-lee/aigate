/** @file test_sha256.c
 *  @brief Known-answer tests for the SHA-256 helpers. */
#include "run_tests.h"
#include "sha256.h"
#include <string.h>

TEST_CASE(test_sha256_kat)
{
  char out[65];
  TEST_ASSERT(sha256_hex("abc", 3, out) == 0, "sha256_hex returns 0");
  TEST_ASSERT(strcmp(out,
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
    "sha256('abc') mismatch: %s", out);

  TEST_ASSERT(sha256_hex("", 0, out) == 0, "empty input ok");
  TEST_ASSERT(strcmp(out,
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
    "sha256('') mismatch: %s", out);
}

TEST_CASE(test_sha256_equal)
{
  const char a[65] =
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90";
  char b[65];
  strcpy(b, a);
  TEST_ASSERT(sha256_hex_equal(a, b) == 1, "equal digests");
  b[1] = 'b';
  TEST_ASSERT(sha256_hex_equal(a, b) == 0, "unequal digests");
}

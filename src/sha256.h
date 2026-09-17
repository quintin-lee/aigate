/** @file sha256.h
 *  @brief SHA-256 hex digest helpers built on OpenSSL EVP.
 *
 *  Used to hash API keys and admin tokens at rest (spec section 3:
 *  "keys: SHA-256 at rest, constant-time compare").
 */
#ifndef AIGATE_SHA256_H
#define AIGATE_SHA256_H

#include <stddef.h>

/** @brief Compute the lowercase SHA-256 hex digest of @p in.
 * @param in     input bytes
 * @param in_len input length
 * @param out    output buffer, MUST be at least 65 bytes (64 hex + NUL)
 * @return 0 on success, -1 on EVP failure.
 * @note @p out is filled with exactly 65 bytes (64 lowercase hex chars + NUL). */
int sha256_hex(const void *in, size_t in_len, char out[65]);

/** @brief Constant-time comparison of two 64-char lowercase hex digests.
 * @return 1 if equal, 0 if not; -1 on length mismatch.
 * @invariant Both inputs must be 64 hex chars for the comparison to be meaningful. */
int sha256_hex_equal(const char a[64], const char b[64]);

#endif /* AIGATE_SHA256_H */

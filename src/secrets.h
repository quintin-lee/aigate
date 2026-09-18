/** @file secrets.h
 *  @brief AES-256-GCM encryption for upstream secrets at rest (spec section 5).
 *
 *  Wire format: hex( "v1:" || nonce[12] || tag[16] || ciphertext ). The whole
 *  blob is hex-encoded so it fits in a TEXT column.
 */
#ifndef AIGATE_SECRETS_H
#define AIGATE_SECRETS_H

#include <stddef.h>
#include <stdint.h>

/** @brief Encrypt @p plain with @p master (32 bytes) into @p out.
 * @param plain    plaintext bytes
 * @param plain_len plaintext length
 * @param master   32-byte master key (from AIGATE_MASTER_KEY hex)
 * @param out      hex blob out buffer
 * @param out_cap  capacity of @p out; must be >= 2*(plain_len+12+16)+8
 * @return 0 on success, -1 on EVP failure or capacity overflow.
 * @invariant a fresh random 12-byte nonce is generated per call. */
int secret_encrypt(
    const uint8_t master[32], const void* plain, size_t plain_len, char* out, size_t out_cap);

/** @brief Decrypt a secret_encrypt blob.
 * @param master 32-byte master key
 * @param blob   hex blob
 * @param out    plaintext out buffer
 * @param out_cap capacity of @p out
 * @param out_len receives the plaintext length (may be NULL)
 * @return 0 on success, -1 on bad format, key mismatch, or tag failure. */
int secret_decrypt(
    const uint8_t master[32], const char* blob, char* out, size_t out_cap, size_t* out_len);

/** @brief Decode a 64-char hex string into 32 bytes. @return 0 ok, -1 on bad length/char. */
int hex_to_bytes32(const char* hex64, uint8_t out[32]);

#endif /* AIGATE_SECRETS_H */

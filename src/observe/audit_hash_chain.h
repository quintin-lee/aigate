/**
 * @file audit_hash_chain.h
 * @brief HMAC-SHA256 cryptographic tamper-proof audit log hash chain.
 */
#ifndef AIGATE_AUDIT_HASH_CHAIN_H
#define AIGATE_AUDIT_HASH_CHAIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char            secret_key[128];
    char            last_hash[65];    /**< 64-char hex + '\0' */
    char            genesis_hash[65]; /**< Initial genesis hash */
    uint64_t        current_seq;
    pthread_mutex_t lock;
} audit_hash_chain_ctx_t;

/**
 * @brief Create a new audit hash chain context with secret key.
 * @param secret Secret HMAC key.
 * @return Allocated context, or NULL on error.
 */
audit_hash_chain_ctx_t* audit_hash_chain_create(const char* secret);

/**
 * @brief Destroy an audit hash chain context.
 */
void audit_hash_chain_destroy(audit_hash_chain_ctx_t* ctx);

/**
 * @brief Sign an audit event NDJSON payload, appending seq, prev_hash, and hash.
 * @param ctx Hash chain context.
 * @param event_json Original event JSON string.
 * @param[out] out_signed_json Buffer to receive signed JSON line.
 * @param out_sz Buffer capacity.
 * @return 0 on success, non-zero on error.
 */
int audit_hash_chain_sign(audit_hash_chain_ctx_t* ctx,
                          const char*             event_json,
                          char*                   out_signed_json,
                          size_t                  out_sz);

/**
 * @brief Verify integrity of an entire NDJSON audit log file.
 * @param ctx Hash chain context.
 * @param filepath Path to the audit log file.
 * @param[out] out_verified_count Number of verified continuous records.
 * @param[out] out_broken_seq Set to sequence ID of first broken block (0 if completely valid).
 * @param[out] out_error_msg Human readable error description buffer.
 * @param error_msg_sz Capacity of out_error_msg.
 * @return 0 if entire file is valid; -1 if tampered, truncated, or broken.
 */
int audit_hash_chain_verify_file(audit_hash_chain_ctx_t* ctx,
                                 const char*             filepath,
                                 uint64_t*               out_verified_count,
                                 uint64_t*               out_broken_seq,
                                 char*                   out_error_msg,
                                 size_t                  error_msg_sz);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_AUDIT_HASH_CHAIN_H */

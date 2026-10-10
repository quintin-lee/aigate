#include "audit_hash_chain.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
compute_hmac_hex(const char* key, const char* data, size_t data_len, char out_hex[65])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  md_len = 0;

    HMAC(EVP_sha256(), key, (int)strlen(key), (const unsigned char*)data, data_len, md, &md_len);

    for (unsigned int i = 0; i < md_len && i < 32; i++) {
        sprintf(&out_hex[i * 2], "%02x", md[i]);
    }
    out_hex[64] = '\0';
}

audit_hash_chain_ctx_t*
audit_hash_chain_create(const char* secret)
{
    if (secret == NULL || secret[0] == '\0') {
        secret = "aigate_default_audit_chain_secret_2026";
    }
    audit_hash_chain_ctx_t* ctx =
        (audit_hash_chain_ctx_t*)calloc(1, sizeof(audit_hash_chain_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }

    snprintf(ctx->secret_key, sizeof(ctx->secret_key), "%s", secret);
    pthread_mutex_init(&ctx->lock, NULL);

    /* Compute genesis hash H0 */
    compute_hmac_hex(
        ctx->secret_key, "AIGATE_AUDIT_GENESIS", strlen("AIGATE_AUDIT_GENESIS"), ctx->genesis_hash);
    memcpy(ctx->last_hash, ctx->genesis_hash, sizeof(ctx->last_hash));
    ctx->current_seq = 0;

    return ctx;
}

void
audit_hash_chain_destroy(audit_hash_chain_ctx_t* ctx)
{
    if (ctx != NULL) {
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
    }
}

int
audit_hash_chain_sign(audit_hash_chain_ctx_t* ctx,
                      const char*             event_json,
                      char*                   out_signed_json,
                      size_t                  out_sz)
{
    if (ctx == NULL || event_json == NULL || out_signed_json == NULL || out_sz == 0) {
        return -1;
    }

    json_error_t err;
    json_t*      root = json_loads(event_json, 0, &err);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return -1;
    }

    /* Remove any existing chain metadata if present */
    json_object_del(root, "seq");
    json_object_del(root, "prev_hash");
    json_object_del(root, "hash");

    char* canonical_payload = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT);
    if (canonical_payload == NULL) {
        json_decref(root);
        return -1;
    }

    pthread_mutex_lock(&ctx->lock);
    uint64_t seq = ctx->current_seq + 1;
    char     prev_hash[65];
    strncpy(prev_hash, ctx->last_hash, sizeof(prev_hash));

    /* Build string to sign: prev_hash:seq:canonical_payload */
    size_t to_sign_len = strlen(prev_hash) + 32 + strlen(canonical_payload) + 8;
    char*  to_sign = (char*)malloc(to_sign_len);
    if (to_sign == NULL) {
        pthread_mutex_unlock(&ctx->lock);
        free(canonical_payload);
        json_decref(root);
        return -1;
    }

    snprintf(to_sign, to_sign_len, "%s:%lu:%s", prev_hash, (unsigned long)seq, canonical_payload);

    char current_hash[65];
    compute_hmac_hex(ctx->secret_key, to_sign, strlen(to_sign), current_hash);
    free(to_sign);
    free(canonical_payload);

    /* Update ctx state */
    ctx->current_seq = seq;
    strncpy(ctx->last_hash, current_hash, sizeof(ctx->last_hash));
    pthread_mutex_unlock(&ctx->lock);

    /* Embed seq, prev_hash, and hash into json */
    json_object_set_new(root, "seq", json_integer((json_int_t)seq));
    json_object_set_new(root, "prev_hash", json_string(prev_hash));
    json_object_set_new(root, "hash", json_string(current_hash));

    char* final_json = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT);
    json_decref(root);
    if (final_json == NULL) {
        return -1;
    }

    if (strlen(final_json) >= out_sz) {
        free(final_json);
        return -1;
    }

    strncpy(out_signed_json, final_json, out_sz - 1);
    out_signed_json[out_sz - 1] = '\0';
    free(final_json);

    return 0;
}

int
audit_hash_chain_verify_file(audit_hash_chain_ctx_t* ctx,
                             const char*             filepath,
                             uint64_t*               out_verified_count,
                             uint64_t*               out_broken_seq,
                             char*                   out_error_msg,
                             size_t                  error_msg_sz)
{
    if (out_verified_count != NULL) {
        *out_verified_count = 0;
    }
    if (out_broken_seq != NULL) {
        *out_broken_seq = 0;
    }
    if (out_error_msg != NULL && error_msg_sz > 0) {
        out_error_msg[0] = '\0';
    }

    if (ctx == NULL || filepath == NULL) {
        if (out_error_msg && error_msg_sz > 0) {
            snprintf(out_error_msg, error_msg_sz, "Invalid context or filepath");
        }
        return -1;
    }

    FILE* fp = fopen(filepath, "r");
    if (fp == NULL) {
        if (out_error_msg && error_msg_sz > 0) {
            snprintf(out_error_msg, error_msg_sz, "Cannot open audit log file");
        }
        return -1;
    }

    char prev_hash[65];
    compute_hmac_hex(
        ctx->secret_key, "AIGATE_AUDIT_GENESIS", strlen("AIGATE_AUDIT_GENESIS"), prev_hash);
    uint64_t expected_seq = 1;
    uint64_t verified = 0;

    char*   line = NULL;
    size_t  linecap = 0;
    ssize_t linelen = 0;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        /* Strip trailing whitespace/newlines */
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r' ||
                               line[linelen - 1] == ' ')) {
            line[--linelen] = '\0';
        }
        if (linelen == 0) {
            continue;
        }

        json_error_t jerr;
        json_t*      root = json_loads(line, 0, &jerr);
        if (root == NULL || !json_is_object(root)) {
            if (out_broken_seq != NULL) {
                *out_broken_seq = expected_seq;
            }
            if (out_error_msg != NULL && error_msg_sz > 0) {
                snprintf(out_error_msg,
                         error_msg_sz,
                         "Malformed JSON on line %lu",
                         (unsigned long)expected_seq);
            }
            if (root != NULL) {
                json_decref(root);
            }
            free(line);
            fclose(fp);
            return -1;
        }

        json_t* seq_val = json_object_get(root, "seq");
        json_t* prev_hash_val = json_object_get(root, "prev_hash");
        json_t* hash_val = json_object_get(root, "hash");

        if (seq_val == NULL || prev_hash_val == NULL || hash_val == NULL) {
            if (out_broken_seq != NULL) {
                *out_broken_seq = expected_seq;
            }
            if (out_error_msg != NULL && error_msg_sz > 0) {
                snprintf(out_error_msg,
                         error_msg_sz,
                         "Missing signature fields on sequence %lu",
                         (unsigned long)expected_seq);
            }
            json_decref(root);
            free(line);
            fclose(fp);
            return -1;
        }

        uint64_t rec_seq = (uint64_t)json_integer_value(seq_val);
        char     saved_prev_hash[65];
        char     saved_hash[65];
        strncpy(saved_prev_hash, json_string_value(prev_hash_val), sizeof(saved_prev_hash) - 1);
        saved_prev_hash[64] = '\0';
        strncpy(saved_hash, json_string_value(hash_val), sizeof(saved_hash) - 1);
        saved_hash[64] = '\0';

        /* 1. Check sequence monotonicity */
        if (rec_seq != expected_seq) {
            if (out_broken_seq != NULL) {
                *out_broken_seq = expected_seq;
            }
            if (out_error_msg != NULL && error_msg_sz > 0) {
                snprintf(out_error_msg,
                         error_msg_sz,
                         "Sequence break: expected %lu, got %lu (line deleted or out of order)",
                         (unsigned long)expected_seq,
                         (unsigned long)rec_seq);
            }
            json_decref(root);
            free(line);
            fclose(fp);
            return -1;
        }

        /* 2. Check previous hash continuity */
        if (strcmp(saved_prev_hash, prev_hash) != 0) {
            if (out_broken_seq != NULL) {
                *out_broken_seq = rec_seq;
            }
            if (out_error_msg != NULL && error_msg_sz > 0) {
                snprintf(out_error_msg,
                         error_msg_sz,
                         "Broken hash chain at seq %lu: prev_hash mismatch",
                         (unsigned long)rec_seq);
            }
            json_decref(root);
            free(line);
            fclose(fp);
            return -1;
        }

        /* 3. Re-compute HMAC and verify current hash */
        json_object_del(root, "seq");
        json_object_del(root, "prev_hash");
        json_object_del(root, "hash");

        char* canonical_payload = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT);
        json_decref(root);
        if (canonical_payload == NULL) {
            free(line);
            fclose(fp);
            return -1;
        }

        size_t to_sign_len = strlen(saved_prev_hash) + 32 + strlen(canonical_payload) + 8;
        char*  to_sign = (char*)malloc(to_sign_len);
        if (to_sign == NULL) {
            free(canonical_payload);
            free(line);
            fclose(fp);
            return -1;
        }

        snprintf(to_sign,
                 to_sign_len,
                 "%s:%lu:%s",
                 saved_prev_hash,
                 (unsigned long)rec_seq,
                 canonical_payload);
        char expected_hash[65];
        compute_hmac_hex(ctx->secret_key, to_sign, strlen(to_sign), expected_hash);
        free(to_sign);
        free(canonical_payload);

        if (strcmp(expected_hash, saved_hash) != 0) {
            if (out_broken_seq != NULL) {
                *out_broken_seq = rec_seq;
            }
            if (out_error_msg != NULL && error_msg_sz > 0) {
                snprintf(out_error_msg,
                         error_msg_sz,
                         "Tamper detected at seq %lu: hash verification failed",
                         (unsigned long)rec_seq);
            }
            free(line);
            fclose(fp);
            return -1;
        }

        /* Update chain progress */
        strncpy(prev_hash, saved_hash, sizeof(prev_hash));
        expected_seq++;
        verified++;
    }

    free(line);
    fclose(fp);

    if (out_verified_count != NULL) {
        *out_verified_count = verified;
    }
    return 0;
}

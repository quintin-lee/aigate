/** @file test_auth_key.c
 *  @brief Key resolution + allowlist + cache-hit counting against fake ops. */
#include "run_tests.h"
#include "auth_key.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

struct akg_db {
    int       get_calls;
    int       n;
    int       fail_next; /* when nonzero, the next lookup returns a storage error */
    key_rec_t recs[4];
};

static int
akg_get_key(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct akg_db* db = ctx;
    db->get_calls++;
    for (int i = 0; i < db->n; i++) {
        if (strcmp(db->recs[i].key_hash, key_hash) == 0) {
            *out = db->recs[i];
            /* mirror ownership: deep-copy the allowlist */
            out->allowed_models = NULL;
            out->n_allowed = db->recs[i].n_allowed;
            if (out->n_allowed > 0) {
                out->allowed_models = malloc(sizeof(char*) * (size_t)out->n_allowed);
                for (int j = 0; j < out->n_allowed; j++) {
                    out->allowed_models[j] = strdup(db->recs[i].allowed_models[j]);
                }
            }
            return 0;
        }
    }
    if (db->fail_next) {
        db->fail_next = 0;
        return -1;
    }
    return 1; /* definite miss, not a storage error */
}

static int
akg_other(void* ctx, ...)
{
    (void)ctx;
    return -1;
}

static pg_ops_t
build_akg_ops(struct akg_db* db)
{
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.ctx = db;
    ops.get_key_by_hash = akg_get_key;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))akg_other;
    ops.get_model = (int (*)(void*, const char*, model_rec_t*))akg_other;
    ops.create_key = (int (*)(void*, const key_rec_t*, long*))akg_other;
    ops.update_key = (int (*)(void*, const key_rec_t*, int))akg_other;
    ops.revoke_key = (int (*)(void*, long))akg_other;
    ops.create_model = (int (*)(void*, const model_rec_t*))akg_other;
    ops.update_model = (int (*)(void*, const model_rec_t*, int))akg_other;
    ops.delete_model = (int (*)(void*, const char*))akg_other;
    ops.flush_usage = (int (*)(void*, const usage_row_t*, int))akg_other;
    ops.query_usage =
        (int (*)(void*, long, const char*, time_t, time_t, usage_row_t*, int, int*))akg_other;
    return ops;
}

/* open a fake store with these ops (pg_store_open copies the table) */
static pg_store_t*
open_akg_store(struct akg_db* db)
{
    pg_ops_t ops = build_akg_ops(db);
    return pg_store_open("unused", &ops);
}

TEST_CASE(test_auth_key_resolve_normal)
{
    struct akg_db db;
    char          hash[65];
    memset(&db, 0, sizeof db);
    db.n = 1;
    sha256_hex("plaintext-secret", strlen("plaintext-secret"), hash);
    strcpy(db.recs[0].key_hash, hash);
    strcpy(db.recs[0].name, "client-a");
    db.recs[0].rate_qps = 10;
    db.recs[0].daily_token_quota = 1000;

    pg_store_t* ps = open_akg_store(&db);
    TEST_ASSERT(ps != NULL, "store open");

    auth_key_cache akc;
    TEST_ASSERT(auth_key_init(&akc, ps) == 0, "auth init");

    key_rec_t out;
    TEST_ASSERT(auth_key_resolve(&akc, "plaintext-secret", &out) == 0, "resolve known key");
    TEST_ASSERT(strcmp(out.name, "client-a") == 0, "name round-trip");
    int calls_after_first = db.get_calls;
    TEST_ASSERT(calls_after_first >= 1, "first resolve hit the store");

    /* second resolve of the same bearer must be served from cache */
    key_rec_t out2;
    TEST_ASSERT(auth_key_resolve(&akc, "plaintext-secret", &out2) == 0, "second resolve");
    TEST_ASSERT(db.get_calls == calls_after_first, "cache hit, no extra lookup");

    key_rec_free(&out);
    key_rec_free(&out2);
    auth_key_shutdown(&akc);
    pg_store_close(ps);
}

TEST_CASE(test_auth_key_unknown_revoked_expired)
{
    struct akg_db db;
    char          hash[65];
    memset(&db, 0, sizeof db);
    db.n = 2;
    sha256_hex("revoked-bearer", strlen("revoked-bearer"), hash);
    strcpy(db.recs[0].key_hash, hash);
    db.recs[0].revoked = 1;
    sha256_hex("expired-bearer", strlen("expired-bearer"), hash);
    strcpy(db.recs[1].key_hash, hash);
    db.recs[1].has_expiry = 1;
    db.recs[1].expires_at = 1; /* 1970 */

    pg_store_t* ps = open_akg_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    auth_key_cache akc;
    TEST_ASSERT(auth_key_init(&akc, ps) == 0, "auth init");
    key_rec_t out;

    TEST_ASSERT(auth_key_resolve(&akc, "nope", &out) == -1, "unknown key");
    key_rec_free(&out);
    TEST_ASSERT(auth_key_resolve(&akc, "revoked-bearer", &out) == -2, "revoked");
    key_rec_free(&out);
    TEST_ASSERT(auth_key_resolve(&akc, "expired-bearer", &out) == -3, "expired");
    key_rec_free(&out);

    /* release cached records owned by the LRU via shutdown */
    auth_key_shutdown(&akc);
    pg_store_close(ps);
}

TEST_CASE(test_auth_key_unknown_neg_cache)
{
    struct akg_db db;
    memset(&db, 0, sizeof db);
    db.n = 0; /* no keys: every hash is unknown */

    pg_store_t* ps = open_akg_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    auth_key_cache akc;
    TEST_ASSERT(auth_key_init(&akc, ps) == 0, "auth init");
    key_rec_t out;

    /* First unknown resolve must query PG exactly once */
    TEST_ASSERT(auth_key_resolve(&akc, "ghost-key", &out) == -1, "unknown -> -1");
    key_rec_free(&out);
    int calls_after_first = db.get_calls;
    TEST_ASSERT(calls_after_first == 1, "first unknown resolve hit the store");

    /* Second resolve of the same unknown key: served by the neg cache */
    TEST_ASSERT(auth_key_resolve(&akc, "ghost-key", &out) == -1, "unknown again -> -1");
    key_rec_free(&out);
    TEST_ASSERT(db.get_calls == calls_after_first, "neg cache: no extra lookup");

    /* invalidate clears the neg entry (e.g. a new key was created) */
    char hash[65];
    TEST_ASSERT(sha256_hex("ghost-key", strlen("ghost-key"), hash) == 0, "hash ok");
    auth_key_invalidate(&akc, hash);
    TEST_ASSERT(auth_key_resolve(&akc, "ghost-key", &out) == -1, "re-lookup after invalidate");
    key_rec_free(&out);
    TEST_ASSERT(db.get_calls == calls_after_first + 1, "invalidate cleared neg entry");

    auth_key_shutdown(&akc);
    pg_store_close(ps);
}

TEST_CASE(test_auth_key_storage_error_not_neg_cached)
{
    struct akg_db db;
    memset(&db, 0, sizeof db);
    db.n = 0;

    pg_store_t* ps = open_akg_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    auth_key_cache akc;
    TEST_ASSERT(auth_key_init(&akc, ps) == 0, "auth init");
    key_rec_t out;

    /* Simulate a PG failure: the first lookup is a storage error (-1), which
     * MUST NOT be negative-cached. The next resolve must re-query. */
    db.fail_next = 1;
    TEST_ASSERT(auth_key_resolve(&akc, "ghost-key", &out) == -1, "error -> -1");
    key_rec_free(&out);
    int calls_after_error = db.get_calls;
    TEST_ASSERT(calls_after_error == 1, "first resolve hit the store");

    /* No error flag now: the hash is a definite miss (1) and gets cached. */
    TEST_ASSERT(auth_key_resolve(&akc, "ghost-key", &out) == -1, "miss -> -1");
    key_rec_free(&out);
    TEST_ASSERT(db.get_calls == calls_after_error + 1, "second resolve re-queried store");

    /* Third resolve is served from the negative cache: no extra lookup. */
    TEST_ASSERT(auth_key_resolve(&akc, "ghost-key", &out) == -1, "cached miss -> -1");
    key_rec_free(&out);
    TEST_ASSERT(db.get_calls == calls_after_error + 1, "neg cache: no extra lookup");

    auth_key_shutdown(&akc);
    pg_store_close(ps);
}

TEST_CASE(test_key_allows_model)
{
    key_rec_t k;
    memset(&k, 0, sizeof k);
    TEST_ASSERT(key_allows_model(&k, "anything") == 1, "empty allowlist = all");

    char* models[] = {"gpt-4o", "claude-3"};
    k.allowed_models = models;
    k.n_allowed = 2;
    TEST_ASSERT(key_allows_model(&k, "gpt-4o") == 1, "allowed model");
    TEST_ASSERT(key_allows_model(&k, "llama") == 0, "disallowed model");
    TEST_ASSERT(key_allows_model(NULL, "gpt-4o") == 0, "NULL record");
}

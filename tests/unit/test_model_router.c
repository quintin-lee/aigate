/** @file test_model_router.c
 *  @brief model_router resolve/invalidate + key-ref resolution tests. */
#include "run_tests.h"
#include "model_router.h"
#include <stdlib.h>
#include <string.h>

struct mro_db {
    int         get_model_calls;
    model_rec_t models[4];
    int         n_models;
};

static int
mro_get_model(void* ctx, const char* name, model_rec_t* out)
{
    struct mro_db* db = ctx;
    db->get_model_calls++;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            *out = db->models[i];
            return 0;
        }
    }
    return -1;
}

static int
mro_fail(void* ctx, ...)
{
    (void)ctx;
    return -1;
}

static pg_ops_t
build_mro_ops(struct mro_db* db)
{
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.ctx = db;
    ops.get_model = mro_get_model;
    ops.get_key_by_hash = (int (*)(void*, const char*, key_rec_t*))mro_fail;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))mro_fail;
    ops.create_key = (int (*)(void*, const key_rec_t*, long*))mro_fail;
    ops.update_key = (int (*)(void*, const key_rec_t*, int))mro_fail;
    ops.revoke_key = (int (*)(void*, long))mro_fail;
    ops.create_model = (int (*)(void*, const model_rec_t*))mro_fail;
    ops.update_model = (int (*)(void*, const model_rec_t*, int))mro_fail;
    ops.delete_model = (int (*)(void*, const char*))mro_fail;
    ops.flush_usage = (int (*)(void*, const usage_row_t*, int))mro_fail;
    ops.query_usage =
        (int (*)(void*, long, const char*, time_t, time_t, usage_row_t*, int, int*))mro_fail;
    return ops;
}

static pg_store_t*
open_mro_store(struct mro_db* db)
{
    pg_ops_t ops = build_mro_ops(db);
    return pg_store_open("unused", &ops);
}

TEST_CASE(test_model_router_env_key)
{
    struct mro_db db;
    memset(&db, 0, sizeof db);
    db.n_models = 1;
    strcpy(db.models[0].name, "gpt-4o");
    strcpy(db.models[0].provider, "openai");
    strcpy(db.models[0].endpoint, "http://127.0.0.1:9999/v1");
    strcpy(db.models[0].upstream_key_ref, "env:TEST_UPSTREAM_KEY");
    strcpy(db.models[0].default_params_json, "{\"model\":\"gpt-4o\"}");
    db.models[0].enabled = 1;

    setenv("TEST_UPSTREAM_KEY", "sk-test-123", 1);
    pg_store_t* ps = open_mro_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    model_router_t* mr = model_router_new(ps, NULL);
    TEST_ASSERT(mr != NULL, "router new");

    model_rec_t out;
    int         calls_before = db.get_model_calls;
    TEST_ASSERT(model_router_resolve(mr, "gpt-4o", &out) == 0, "resolve");
    TEST_ASSERT(strcmp(out.upstream_key, "sk-test-123") == 0, "env key filled");
    TEST_ASSERT(strcmp(out.endpoint, "http://127.0.0.1:9999/v1") == 0, "endpoint");
    int calls_after = db.get_model_calls;
    TEST_ASSERT(calls_after == calls_before + 1, "first resolve hit store");

    /* second resolve is cached */
    model_rec_t out2;
    TEST_ASSERT(model_router_resolve(mr, "gpt-4o", &out2) == 0, "cached resolve");
    TEST_ASSERT(db.get_model_calls == calls_after, "cache hit, no store call");

    model_router_invalidate(mr, "gpt-4o");
    model_router_resolve(mr, "gpt-4o", &out2);
    TEST_ASSERT(db.get_model_calls == calls_after + 1, "invalidation re-reads");

    /* unknown model */
    TEST_ASSERT(model_router_resolve(mr, "nope", &out2) == -1, "unknown model");

    model_router_free(mr);
    pg_store_close(ps);
    unsetenv("TEST_UPSTREAM_KEY");
}

TEST_CASE(test_model_router_missing_env_key)
{
    struct mro_db db;
    memset(&db, 0, sizeof db);
    db.n_models = 1;
    strcpy(db.models[0].name, "x");
    strcpy(db.models[0].provider, "openai");
    strcpy(db.models[0].endpoint, "http://127.0.0.1:9999");
    strcpy(db.models[0].upstream_key_ref, "env:DOES_NOT_EXIST_KEY");
    db.models[0].enabled = 1;

    pg_store_t* ps = open_mro_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    model_router_t* mr = model_router_new(ps, NULL);
    TEST_ASSERT(mr != NULL, "router new");

    model_rec_t out;
    TEST_ASSERT(model_router_resolve(mr, "x", &out) == -1, "missing env key → -1");
    model_router_free(mr);
    pg_store_close(ps);
}

TEST_CASE(test_model_router_multi_target_keys)
{
    struct mro_db db;
    memset(&db, 0, sizeof db);
    db.n_models = 1;
    strcpy(db.models[0].name, "multi-keys");
    strcpy(db.models[0].provider, "openai");
    strcpy(db.models[0].endpoint, "http://default/v1");
    db.models[0].n_targets = 2;
    strcpy(db.models[0].targets[0].provider, "openai");
    strcpy(db.models[0].targets[0].endpoint, "http://t1/v1");
    strcpy(db.models[0].targets[0].upstream_key_ref, "env:KEY_T1");
    db.models[0].targets[0].weight = 1;
    db.models[0].targets[0].priority = 0;

    strcpy(db.models[0].targets[1].provider, "azure");
    strcpy(db.models[0].targets[1].endpoint, "http://t2/v1");
    strcpy(db.models[0].targets[1].upstream_key_ref, "env:KEY_T2");
    db.models[0].targets[1].weight = 1;
    db.models[0].targets[1].priority = 1;
    db.models[0].enabled = 1;

    setenv("KEY_T1", "secret-key-1", 1);
    setenv("KEY_T2", "secret-key-2", 1);

    pg_store_t* ps = open_mro_store(&db);
    model_router_t* mr = model_router_new(ps, NULL);

    model_rec_t out;
    TEST_ASSERT(model_router_resolve(mr, "multi-keys", &out) == 0, "resolve multi-keys");
    TEST_ASSERT(out.n_targets == 2, "2 targets");
    TEST_ASSERT(strcmp(out.targets[0].upstream_key, "secret-key-1") == 0, "t0 key");
    TEST_ASSERT(strcmp(out.targets[1].upstream_key, "secret-key-2") == 0, "t1 key");

    model_router_free(mr);
    pg_store_close(ps);
    unsetenv("KEY_T1");
    unsetenv("KEY_T2");
}

TEST_CASE(test_model_router_priority_selection)
{
    model_rec_t m;
    memset(&m, 0, sizeof m);
    strcpy(m.name, "tiered");
    strcpy(m.lb_policy, "priority");
    m.n_targets = 3;

    strcpy(m.targets[0].endpoint, "http://p0-a");
    m.targets[0].priority = 0;

    strcpy(m.targets[1].endpoint, "http://p0-b");
    m.targets[1].priority = 0;

    strcpy(m.targets[2].endpoint, "http://p1-c");
    m.targets[2].priority = 1;

    upstream_target_t cands[8];
    int count = 0;
    TEST_ASSERT(model_router_select_candidates(NULL, &m, cands, 8, &count) == 0, "select");
    TEST_ASSERT(count == 3, "3 candidates");
    TEST_ASSERT(strcmp(cands[0].endpoint, "http://p0-a") == 0, "first is p0-a");
    TEST_ASSERT(strcmp(cands[1].endpoint, "http://p0-b") == 0, "second is p0-b");
    TEST_ASSERT(strcmp(cands[2].endpoint, "http://p1-c") == 0, "third is p1-c fallback");
}

TEST_CASE(test_model_router_round_robin)
{
    model_rec_t m;
    memset(&m, 0, sizeof m);
    strcpy(m.name, "rr-model");
    strcpy(m.lb_policy, "round_robin");
    m.n_targets = 2;

    strcpy(m.targets[0].endpoint, "http://rr-1");
    m.targets[0].priority = 0;
    m.targets[0].weight = 1;

    strcpy(m.targets[1].endpoint, "http://rr-2");
    m.targets[1].priority = 0;
    m.targets[1].weight = 1;

    upstream_target_t cands1[4], cands2[4];
    int count1 = 0, count2 = 0;
    TEST_ASSERT(model_router_select_candidates(NULL, &m, cands1, 4, &count1) == 0, "sel 1");
    TEST_ASSERT(model_router_select_candidates(NULL, &m, cands2, 4, &count2) == 0, "sel 2");
    TEST_ASSERT(count1 == 2 && count2 == 2, "counts match");
    /* The first candidate in cands1 and cands2 should alternate */
    TEST_ASSERT(strcmp(cands1[0].endpoint, cands2[0].endpoint) != 0, "rr rotated first candidate");
}

TEST_CASE(test_model_router_weighted)
{
    model_rec_t m;
    memset(&m, 0, sizeof m);
    strcpy(m.name, "w-model");
    strcpy(m.lb_policy, "weighted");
    m.n_targets = 2;

    strcpy(m.targets[0].endpoint, "http://heavy");
    m.targets[0].priority = 0;
    m.targets[0].weight = 10;

    strcpy(m.targets[1].endpoint, "http://light");
    m.targets[1].priority = 0;
    m.targets[1].weight = 1;

    int heavy_first = 0;
    for (int i = 0; i < 11; i++) {
        upstream_target_t cands[4];
        int count = 0;
        model_router_select_candidates(NULL, &m, cands, 4, &count);
        if (strcmp(cands[0].endpoint, "http://heavy") == 0) {
            heavy_first++;
        }
    }
    TEST_ASSERT(heavy_first == 10, "weighted 10:1 ratio observed");
}

TEST_CASE(test_model_router_cb_exclusion_and_fallback)
{
    circuit_breaker_t* cb = cb_create();
    cb_set_params(cb, 3, 30);

    model_rec_t m;
    memset(&m, 0, sizeof m);
    strcpy(m.name, "cb-model");
    strcpy(m.lb_policy, "priority");
    m.n_targets = 3;

    strcpy(m.targets[0].endpoint, "http://p0-bad");
    m.targets[0].priority = 0;

    strcpy(m.targets[1].endpoint, "http://p0-good");
    m.targets[1].priority = 0;

    strcpy(m.targets[2].endpoint, "http://p1-backup");
    m.targets[2].priority = 1;

    /* Trip p0-bad */
    cb_record_failure(cb, "cb-model", "http://p0-bad", 500);
    cb_record_failure(cb, "cb-model", "http://p0-bad", 500);
    cb_record_failure(cb, "cb-model", "http://p0-bad", 500);
    TEST_ASSERT(cb_get_state(cb, "cb-model", "http://p0-bad") == CB_OPEN, "p0-bad tripped");

    upstream_target_t cands[8];
    int count = 0;
    TEST_ASSERT(model_router_select_candidates(cb, &m, cands, 8, &count) == 0, "select with cb");
    TEST_ASSERT(count == 2, "2 healthy candidates");
    TEST_ASSERT(strcmp(cands[0].endpoint, "http://p0-good") == 0, "p0-good chosen first");
    TEST_ASSERT(strcmp(cands[1].endpoint, "http://p1-backup") == 0, "p1-backup fallback");

    /* Now trip all targets */
    cb_record_failure(cb, "cb-model", "http://p0-good", 500);
    cb_record_failure(cb, "cb-model", "http://p0-good", 500);
    cb_record_failure(cb, "cb-model", "http://p0-good", 500);

    cb_record_failure(cb, "cb-model", "http://p1-backup", 500);
    cb_record_failure(cb, "cb-model", "http://p1-backup", 500);
    cb_record_failure(cb, "cb-model", "http://p1-backup", 500);

    /* When all are tripped, fallback to earliest expiry (or lowest priority) */
    TEST_ASSERT(model_router_select_candidates(cb, &m, cands, 8, &count) == 0, "select all tripped");
    TEST_ASSERT(count == 3, "returns all tripped targets in recovery order");

    cb_destroy(cb);
}

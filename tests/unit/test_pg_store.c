/** @file test_pg_store.c
 *  @brief pg_store tests: fake-ops round-trips + optional real-DB checks.
 *
 *  When TEST_PG_DSN is set, the real libpq ops are exercised against it:
 *  migrate → create key/model → read back → flush usage → query usage.
 */
#include "run_tests.h"
#include "pg_store.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- fake ops layer */

#define FAKE_CAP 16

struct fake_key {
    int       in_use;
    key_rec_t k;
};

struct fake_db {
    struct fake_key keys[FAKE_CAP];
    model_rec_t     models[FAKE_CAP];
    int             n_models;
    usage_row_t     usage[FAKE_CAP];
    int             n_usage;
    long            next_key_id;
    /* counters for assertions */
    int lookup_calls;
};

static int
deep_copy_allowlist(key_rec_t* dst, const key_rec_t* src)
{
    dst->allowed_models = NULL;
    dst->n_allowed = src->n_allowed;
    if (src->n_allowed > 0) {
        dst->allowed_models = malloc(sizeof(char*) * (size_t)src->n_allowed);
        if (dst->allowed_models == NULL) {
            return -1;
        }
        for (int j = 0; j < src->n_allowed; j++) {
            dst->allowed_models[j] = strdup(src->allowed_models[j]);
            if (dst->allowed_models[j] == NULL) {
                key_rec_free(dst);
                return -1;
            }
        }
    }
    return 0;
}

static int
fake_get_key_by_hash(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct fake_db* db = ctx;
    db->lookup_calls++;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_key* fk = &db->keys[i];
        if (!fk->in_use) {
            continue;
        }
        if (strcmp(fk->k.key_hash, key_hash) == 0) {
            *out = fk->k;
            /* Mirror the libpq contract: out owns a heap allowlist (key_rec_free). */
            if (deep_copy_allowlist(out, &fk->k) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static void
fake_sanitize_model(model_rec_t* out)
{
    if (out->lb_policy[0] == '\0') {
        snprintf(out->lb_policy, sizeof out->lb_policy, "priority");
    }
    if (out->n_targets == 0) {
        out->n_targets = 1;
        snprintf(out->targets[0].provider, sizeof out->targets[0].provider, "%s", out->provider[0] != '\0' ? out->provider : "openai");
        snprintf(out->targets[0].endpoint, sizeof out->targets[0].endpoint, "%s", out->endpoint);
        snprintf(out->targets[0].upstream_key_ref, sizeof out->targets[0].upstream_key_ref, "%s", out->upstream_key_ref);
        out->targets[0].weight = 1;
        out->targets[0].priority = 0;
    }
}

static int
fake_list_models(void* ctx, model_rec_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    *n = db->n_models < cap ? db->n_models : cap;
    for (int i = 0; i < *n; i++) {
        out[i] = db->models[i];
        fake_sanitize_model(&out[i]);
    }
    return 0;
}

static int
fake_get_model(void* ctx, const char* name, model_rec_t* out)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            *out = db->models[i];
            fake_sanitize_model(out);
            return 0;
        }
    }
    return -1;
}

static int
fake_list_keys(void* ctx, key_rec_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < FAKE_CAP && *n < cap; i++) {
        struct fake_key* fk = &db->keys[i];
        if (!fk->in_use) {
            continue;
        }
        out[*n] = fk->k;
        /* Mirror the libpq contract: out owns a heap allowlist copy. */
        out[*n].allowed_models = NULL;
        out[*n].n_allowed = 0;
        if (deep_copy_allowlist(&out[*n], &fk->k) != 0) {
            return -1;
        }
        (*n)++;
    }
    return 0;
}

static int
fake_get_key_by_id(void* ctx, long key_id, key_rec_t* out)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_key* fk = &db->keys[i];
        if (fk->in_use && fk->k.key_id == key_id) {
            *out = fk->k;
            out->allowed_models = NULL;
            out->n_allowed = 0;
            if (deep_copy_allowlist(out, &fk->k) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static int
fake_create_key(void* ctx, const key_rec_t* k, long* out_key_id)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_key* fk = &db->keys[i];
        if (!fk->in_use) {
            *fk = (struct fake_key){.in_use = 1, .k = *k};
            /* Take ownership of a heap allowlist copy (mirrors libpq). */
            if (deep_copy_allowlist(&fk->k, k) != 0) {
                return -1;
            }
            fk->k.key_id = db->next_key_id++;
            *out_key_id = fk->k.key_id;
            return 0;
        }
    }
    return -1;
}

static int
fake_update_key(void* ctx, const key_rec_t* k, int mask)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_key* fk = &db->keys[i];
        if (!fk->in_use || fk->k.key_id != k->key_id) {
            continue;
        }
        if (mask & KMASK_RATE) {
            fk->k.rate_qps = k->rate_qps;
        }
        if (mask & KMASK_QUOTA) {
            fk->k.daily_token_quota = k->daily_token_quota;
        }
        if (mask & KMASK_ALLOWLIST) {
            for (int j = 0; j < fk->k.n_allowed; j++) {
                free(fk->k.allowed_models[j]);
            }
            free(fk->k.allowed_models);
            if (deep_copy_allowlist(&fk->k, k) != 0) {
                return -1;
            }
        }
        if (mask & KMASK_EXPIRY) {
            fk->k.expires_at = k->expires_at;
            fk->k.has_expiry = k->has_expiry;
        }
        return 0;
    }
    return -1;
}

static int
fake_revoke_key(void* ctx, long key_id)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_key* fk = &db->keys[i];
        if (fk->in_use && fk->k.key_id == key_id) {
            fk->k.revoked = 1;
            return 0;
        }
    }
    return -1;
}

static int
fake_create_model(void* ctx, const model_rec_t* m)
{
    struct fake_db* db = ctx;
    if (db->n_models >= FAKE_CAP) {
        return -1;
    }
    db->models[db->n_models] = *m;
    fake_sanitize_model(&db->models[db->n_models]);
    db->n_models++;
    return 0;
}

static int
fake_update_model(void* ctx, const model_rec_t* m, int mask)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, m->name) != 0) {
            continue;
        }
        if (mask & MMASK_ENDPOINT) {
            snprintf(db->models[i].endpoint, sizeof db->models[i].endpoint, "%s", m->endpoint);
        }
        if (mask & MMASK_PARAMS) {
            snprintf(db->models[i].default_params_json,
                     sizeof db->models[i].default_params_json,
                     "%s",
                     m->default_params_json);
        }
        if (mask & MMASK_KEYREF) {
            snprintf(db->models[i].upstream_key_ref,
                     sizeof db->models[i].upstream_key_ref,
                     "%s",
                     m->upstream_key_ref);
        }
        if (mask & MMASK_ENABLED) {
            db->models[i].enabled = m->enabled;
        }
        if (mask & MMASK_TARGETS) {
            db->models[i].n_targets = m->n_targets;
            memcpy(db->models[i].targets, m->targets, sizeof(m->targets));
        }
        if (mask & MMASK_LB_POLICY) {
            snprintf(db->models[i].lb_policy, sizeof db->models[i].lb_policy, "%s", m->lb_policy);
        }
        return 0;
    }
    return -1;
}

static int
fake_delete_model(void* ctx, const char* name)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            for (int j = i; j < db->n_models - 1; j++) {
                db->models[j] = db->models[j + 1];
            }
            db->n_models--;
            return 0;
        }
    }
    return -1;
}

static int
fake_flush_usage(void* ctx, const usage_row_t* rows, int n)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < db->n_usage; j++) {
            usage_row_t* r = &db->usage[j];
            if (r->key_id == rows[i].key_id && strcmp(r->model_name, rows[i].model_name) == 0 &&
                r->day == rows[i].day) {
                r->requests += rows[i].requests;
                r->prompt_tokens += rows[i].prompt_tokens;
                r->completion_tokens += rows[i].completion_tokens;
                r->errors += rows[i].errors;
                found = 1;
                break;
            }
        }
        if (!found && db->n_usage < FAKE_CAP) {
            db->usage[db->n_usage++] = rows[i];
        }
    }
    return 0;
}

static int
fake_query_usage(void*        ctx,
                 long         key_id,
                 const char*  model,
                 time_t       from,
                 time_t       to,
                 usage_row_t* out,
                 int          cap,
                 int*         n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < db->n_usage && *n < cap; i++) {
        usage_row_t* r = &db->usage[i];
        if (r->key_id != key_id) {
            continue;
        }
        if (model != NULL && model[0] != '\0' && strcmp(model, "all") != 0 &&
            strcmp(r->model_name, model) != 0) {
            continue;
        }
        if (r->day < from || r->day > to) {
            continue;
        }
        out[(*n)++] = *r;
    }
    return 0;
}

static void
build_fake_ops(struct fake_db* db, pg_ops_t* ops)
{
    memset(ops, 0, sizeof *ops);
    ops->ctx = db;
    ops->get_key_by_hash = fake_get_key_by_hash;
    ops->list_keys = fake_list_keys;
    ops->get_key_by_id = fake_get_key_by_id;
    ops->list_models = fake_list_models;
    ops->get_model = fake_get_model;
    ops->create_key = fake_create_key;
    ops->update_key = fake_update_key;
    ops->revoke_key = fake_revoke_key;
    ops->create_model = fake_create_model;
    ops->update_model = fake_update_model;
    ops->delete_model = fake_delete_model;
    ops->flush_usage = fake_flush_usage;
    ops->query_usage = fake_query_usage;
}

TEST_CASE(test_pg_fake_key_lifecycle)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    key_rec_t      k;

    memset(&db, 0, sizeof db);
    db.next_key_id = 1;
    build_fake_ops(&db, &ops);
    ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake store open");

    memset(&k, 0, sizeof k);
    strcpy(k.key_hash, "ab11");
    strcpy(k.name, "tester");
    k.rate_qps = 5;
    k.daily_token_quota = 1000;
    char* models[] = {"gpt-4o", "claude"};
    k.allowed_models = models;
    k.n_allowed = 2;

    long id = 0;
    TEST_ASSERT(pg_store_ops(ps)->create_key(&db, &k, &id) == 0, "create_key");
    TEST_ASSERT(id == 1, "first key id");

    key_rec_t out;
    TEST_ASSERT(pg_store_ops(ps)->get_key_by_hash(&db, "ab11", &out) == 0, "get by hash");
    TEST_ASSERT(out.key_id == 1 && out.rate_qps == 5 && out.n_allowed == 2, "round-trip fields");
    TEST_ASSERT(strcmp(out.allowed_models[1], "claude") == 0, "allowlist");
    key_rec_free(&out); /* each get owns a fresh allowlist copy */

    k.key_id = id;
    k.rate_qps = 9;
    TEST_ASSERT(pg_store_ops(ps)->update_key(&db, &k, KMASK_RATE) == 0, "update rate");
    memset(&out, 0, sizeof out);
    TEST_ASSERT(pg_store_ops(ps)->get_key_by_hash(&db, "ab11", &out) == 0, "re-get");
    TEST_ASSERT(out.rate_qps == 9, "rate updated");
    key_rec_free(&out);

    TEST_ASSERT(pg_store_ops(ps)->revoke_key(&db, id) == 0, "revoke");
    memset(&out, 0, sizeof out);
    TEST_ASSERT(pg_store_ops(ps)->get_key_by_hash(&db, "ab11", &out) == 0, "still findable");
    TEST_ASSERT(out.revoked == 1, "revoked flag");
    key_rec_free(&out);

    /* release allowlist copies held by fake slots */
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db.keys[i].in_use) {
            key_rec_free(&db.keys[i].k);
        }
    }

    pg_store_close(ps);
}

TEST_CASE(test_pg_fake_model_lifecycle)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    model_rec_t    m, out;

    memset(&db, 0, sizeof db);
    build_fake_ops(&db, &ops);
    ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake store open");

    memset(&m, 0, sizeof m);
    strcpy(m.name, "mock-model");
    strcpy(m.provider, "openai");
    strcpy(m.endpoint, "http://127.0.0.1:9999");
    strcpy(m.upstream_key_ref, "env:MOCK_KEY");
    strcpy(m.default_params_json, "{\"temperature\":0.2}");
    m.enabled = 1;
    TEST_ASSERT(pg_store_ops(ps)->create_model(&db, &m) == 0, "create model");

    model_rec_t buf[FAKE_CAP];
    int         n = 0;
    TEST_ASSERT(pg_store_ops(ps)->list_models(&db, buf, FAKE_CAP, &n) == 0, "list models");
    TEST_ASSERT(n == 1, "one model");

    memset(&out, 0, sizeof out);
    TEST_ASSERT(pg_store_ops(ps)->get_model(&db, "mock-model", &out) == 0, "get model");
    TEST_ASSERT(strcmp(out.endpoint, "http://127.0.0.1:9999") == 0, "endpoint");

    strcpy(m.endpoint, "http://127.0.0.1:9998");
    TEST_ASSERT(pg_store_ops(ps)->update_model(&db, &m, MMASK_ENDPOINT) == 0, "update endpoint");
    memset(&out, 0, sizeof out);
    pg_store_ops(ps)->get_model(&db, "mock-model", &out);
    TEST_ASSERT(strcmp(out.endpoint, "http://127.0.0.1:9998") == 0, "endpoint changed");

    m.enabled = 0;
    TEST_ASSERT(pg_store_ops(ps)->update_model(&db, &m, MMASK_ENABLED) == 0, "disable model");
    TEST_ASSERT(pg_store_ops(ps)->delete_model(&db, "mock-model") == 0, "delete model");
    TEST_ASSERT(pg_store_ops(ps)->get_model(&db, "mock-model", &out) == -1, "gone after delete");

    pg_store_close(ps);
}

TEST_CASE(test_pg_fake_multi_target_model)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    model_rec_t    m, out;

    memset(&db, 0, sizeof db);
    build_fake_ops(&db, &ops);
    ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake store open");

    /* 1. Legacy single-target model auto-synthesizes targets[0] and lb_policy */
    memset(&m, 0, sizeof m);
    strcpy(m.name, "single-target");
    strcpy(m.provider, "openai");
    strcpy(m.endpoint, "http://127.0.0.1:9000");
    strcpy(m.upstream_key_ref, "env:SINGLE_KEY");
    m.enabled = 1;
    TEST_ASSERT(pg_store_ops(ps)->create_model(&db, &m) == 0, "create single model");

    memset(&out, 0, sizeof out);
    TEST_ASSERT(pg_store_ops(ps)->get_model(&db, "single-target", &out) == 0, "get single model");
    TEST_ASSERT(out.n_targets == 1, "synthesized 1 target");
    TEST_ASSERT(strcmp(out.targets[0].provider, "openai") == 0, "tgt0 provider");
    TEST_ASSERT(strcmp(out.targets[0].endpoint, "http://127.0.0.1:9000") == 0, "tgt0 endpoint");
    TEST_ASSERT(strcmp(out.targets[0].upstream_key_ref, "env:SINGLE_KEY") == 0, "tgt0 key_ref");
    TEST_ASSERT(out.targets[0].weight == 1, "tgt0 weight default 1");
    TEST_ASSERT(out.targets[0].priority == 0, "tgt0 priority default 0");
    TEST_ASSERT(strcmp(out.lb_policy, "priority") == 0, "default lb_policy priority");

    /* 2. Multi-target model */
    memset(&m, 0, sizeof m);
    strcpy(m.name, "multi-target");
    strcpy(m.lb_policy, "round_robin");
    m.n_targets = 2;
    strcpy(m.targets[0].provider, "openai");
    strcpy(m.targets[0].endpoint, "http://primary:8000");
    strcpy(m.targets[0].upstream_key_ref, "env:PRI_KEY");
    m.targets[0].weight = 3;
    m.targets[0].priority = 0;

    strcpy(m.targets[1].provider, "azure");
    strcpy(m.targets[1].endpoint, "http://backup:8000");
    strcpy(m.targets[1].upstream_key_ref, "env:BAK_KEY");
    m.targets[1].weight = 1;
    m.targets[1].priority = 1;
    m.enabled = 1;

    TEST_ASSERT(pg_store_ops(ps)->create_model(&db, &m) == 0, "create multi model");
    memset(&out, 0, sizeof out);
    TEST_ASSERT(pg_store_ops(ps)->get_model(&db, "multi-target", &out) == 0, "get multi model");
    TEST_ASSERT(out.n_targets == 2, "2 targets returned");
    TEST_ASSERT(strcmp(out.lb_policy, "round_robin") == 0, "policy round_robin");
    TEST_ASSERT(strcmp(out.targets[0].endpoint, "http://primary:8000") == 0, "tgt 0 ep");
    TEST_ASSERT(out.targets[0].weight == 3, "tgt 0 weight");
    TEST_ASSERT(out.targets[0].priority == 0, "tgt 0 prio");
    TEST_ASSERT(strcmp(out.targets[1].endpoint, "http://backup:8000") == 0, "tgt 1 ep");
    TEST_ASSERT(out.targets[1].weight == 1, "tgt 1 weight");
    TEST_ASSERT(out.targets[1].priority == 1, "tgt 1 prio");

    /* 3. Update targets & lb_policy */
    strcpy(m.lb_policy, "weighted");
    m.targets[0].weight = 5;
    TEST_ASSERT(pg_store_ops(ps)->update_model(&db, &m, MMASK_TARGETS | MMASK_LB_POLICY) == 0, "update targets & lb");
    memset(&out, 0, sizeof out);
    TEST_ASSERT(pg_store_ops(ps)->get_model(&db, "multi-target", &out) == 0, "get updated model");
    TEST_ASSERT(strcmp(out.lb_policy, "weighted") == 0, "updated policy weighted");
    TEST_ASSERT(out.targets[0].weight == 5, "updated tgt 0 weight");

    pg_store_close(ps);
}

TEST_CASE(test_pg_fake_usage_flush_and_query)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    usage_row_t    r1, r2, buf[4];
    int            n = 0;

    memset(&db, 0, sizeof db);
    build_fake_ops(&db, &ops);
    ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake store open");

    memset(&r1, 0, sizeof r1);
    r1.key_id = 7;
    strcpy(r1.model_name, "mock-model");
    r1.day = 1700000000;
    r1.requests = 3;
    r1.prompt_tokens = 30;
    r1.completion_tokens = 33;
    TEST_ASSERT(pg_store_ops(ps)->flush_usage(&db, &r1, 1) == 0, "flush r1");

    r2 = r1;
    r2.requests = 2;
    r2.prompt_tokens = 20;
    r2.completion_tokens = 22;
    TEST_ASSERT(pg_store_ops(ps)->flush_usage(&db, &r2, 1) == 0, "flush r2");

    TEST_ASSERT(pg_store_ops(ps)->query_usage(&db, 7, NULL, 1699900000, 1700100000, buf, 4, &n) ==
                    0,
                "query all models");
    TEST_ASSERT(n == 1, "one merged row");
    TEST_ASSERT(buf[0].requests == 5 && buf[0].prompt_tokens == 50 &&
                    buf[0].completion_tokens == 55,
                "aggregated counts");

    n = 0;
    TEST_ASSERT(pg_store_ops(ps)->query_usage(&db, 8, NULL, 1699900000, 1700100000, buf, 4, &n) ==
                    0,
                "query other key");
    TEST_ASSERT(n == 0, "no rows for other key");

    pg_store_close(ps);
}

TEST_CASE(test_pg_migrate_noop_for_fake)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;

    memset(&db, 0, sizeof db);
    build_fake_ops(&db, &ops);
    ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake store open");
    TEST_ASSERT(pg_store_migrate(ps) == 0, "fake migrate is a no-op success");
    pg_store_close(ps);
}

TEST_CASE(test_pg_real_roundtrip)
{
    const char* dsn = getenv("TEST_PG_DSN");
    if (dsn == NULL || dsn[0] == '\0') {
        return; /* skip gracefully when no test DB is available */
    }

    pg_store_t* ps = pg_store_open(dsn, NULL);
    TEST_ASSERT(ps != NULL, "real store open");
    TEST_ASSERT(pg_store_migrate(ps) == 0, "migrate");

    key_rec_t k;
    memset(&k, 0, sizeof k);
    strcpy(k.key_hash, "unittest_key_hash");
    strcpy(k.name, "itest");
    char* am[] = {"m1"};
    k.allowed_models = am;
    k.n_allowed = 1;
    long id = 0;
    TEST_ASSERT(pg_store_ops(ps)->create_key(pg_store_ops(ps)->ctx, &k, &id) == 0,
                "real create_key");
    (void)id;
    pg_store_close(ps);
}

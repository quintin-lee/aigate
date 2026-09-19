/** @file test_admin_api.c
 *  @brief Unit tests for /admin/v1 management plane (spec §4.2, Task 10).
 */
#include "run_tests.h"
#include "admin_api.h"
#include "aigate_core.h"
#include "pg_store.h"
#include "sha256.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_CAP 32

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
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_key* fk = &db->keys[i];
        if (!fk->in_use) {
            continue;
        }
        if (strcmp(fk->k.key_hash, key_hash) == 0) {
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
            fk->in_use = 1;
            fk->k = *k;
            fk->k.key_id = db->next_key_id++;
            fk->k.allowed_models = NULL;
            fk->k.n_allowed = 0;
            if (deep_copy_allowlist(&fk->k, k) != 0) {
                fk->in_use = 0;
                return -1;
            }
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
        if (fk->in_use && fk->k.key_id == k->key_id) {
            if (mask & KMASK_RATE) {
                fk->k.rate_qps = k->rate_qps;
            }
            if (mask & KMASK_QUOTA) {
                fk->k.daily_token_quota = k->daily_token_quota;
            }
            if (mask & KMASK_EXPIRY) {
                fk->k.expires_at = k->expires_at;
                fk->k.has_expiry = k->has_expiry;
            }
            if (mask & KMASK_ALLOWLIST) {
                key_rec_free(&fk->k);
                fk->k.allowed_models = NULL;
                fk->k.n_allowed = 0;
                if (deep_copy_allowlist(&fk->k, k) != 0) {
                    return -1;
                }
            }
            return 0;
        }
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
fake_list_models(void* ctx, model_rec_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    int count = db->n_models < cap ? db->n_models : cap;
    for (int i = 0; i < count; i++) {
        out[i] = db->models[i];
    }
    *n = count;
    return 0;
}

static int
fake_get_model(void* ctx, const char* name, model_rec_t* out)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            *out = db->models[i];
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
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, m->name) == 0) {
            db->models[i] = *m;
            return 0;
        }
    }
    db->models[db->n_models++] = *m;
    return 0;
}

static int
fake_update_model(void* ctx, const model_rec_t* m, int mask)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, m->name) == 0) {
            if (mask & MMASK_ENDPOINT) {
                snprintf(db->models[i].endpoint, sizeof db->models[i].endpoint, "%s", m->endpoint);
            }
            if (mask & MMASK_PARAMS) {
                snprintf(db->models[i].default_params_json, sizeof db->models[i].default_params_json, "%s", m->default_params_json);
            }
            if (mask & MMASK_KEYREF) {
                snprintf(db->models[i].upstream_key_ref, sizeof db->models[i].upstream_key_ref, "%s", m->upstream_key_ref);
            }
            if (mask & MMASK_ENABLED) {
                db->models[i].enabled = m->enabled;
            }
            return 0;
        }
    }
    return -1;
}

static int
fake_delete_model(void* ctx, const char* name)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            db->models[i] = db->models[--db->n_models];
            return 0;
        }
    }
    return -1;
}

static int
fake_flush_usage(void* ctx, const usage_row_t* rows, int n)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < n && db->n_usage < FAKE_CAP; i++) {
        db->usage[db->n_usage++] = rows[i];
    }
    return 0;
}

static int
fake_query_usage(void* ctx, long key_id, const char* model, time_t from, time_t to,
                 usage_row_t* out, int cap, int* n)
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

static void
setup_admin(struct fake_db* db, pg_ops_t* ops, pg_store_t** out_ps,
            aigate_core* out_core, admin_ctx_t* out_adm, char admin_hash[65])
{
    memset(db, 0, sizeof *db);
    db->next_key_id = 1;
    build_fake_ops(db, ops);
    *out_ps = pg_store_open("unused", ops);

    sha256_hex("admin-secret-token", strlen("admin-secret-token"), admin_hash);

    aigate_core_init(out_core, *out_ps, NULL, 5000, 0);

    out_adm->ac = out_core;
    out_adm->ps = *out_ps;
    out_adm->admin_token_hash = admin_hash;
}

static void
teardown_admin(pg_store_t* ps, aigate_core* core, struct fake_db* db)
{
    aigate_core_shutdown(core);
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db->keys[i].in_use) {
            key_rec_free(&db->keys[i].k);
        }
    }
    pg_store_close(ps);
}

TEST_CASE(test_admin_auth)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. Missing bearer */
    int rc = admin_dispatch(&adm, "/admin/v1/keys", "GET", NULL, NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 401, "missing bearer -> 401");
    free(body);

    /* 2. Wrong bearer */
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/keys", "GET", "wrong-secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 401, "wrong bearer -> 401");
    free(body);

    /* 3. Correct bearer */
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/keys", "GET", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "correct bearer -> 200");
    free(body);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_keys_lifecycle)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. Create key */
    const char* req = "{\"name\":\"alice\",\"allowed_models\":[\"gpt-4o\"],\"rate_qps\":5,\"daily_token_quota\":1000}";
    int rc = admin_dispatch(&adm, "/admin/v1/keys", "POST", "admin-secret-token", req, strlen(req), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 201, "create key -> 201");
    TEST_ASSERT(body != NULL, "create key body present");

    json_t* j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(j != NULL, "parsed json");
    json_t* jid = json_object_get(j, "key_id");
    json_t* jplain = json_object_get(j, "plaintext");
    TEST_ASSERT(jid != NULL && json_is_integer(jid), "has key_id");
    TEST_ASSERT(jplain != NULL && json_is_string(jplain), "has plaintext");

    long key_id = json_integer_value(jid);
    char plain[64];
    snprintf(plain, sizeof plain, "%s", json_string_value(jplain));
    TEST_ASSERT(strncmp(plain, "aig_", 4) == 0, "plaintext starts with aig_");
    json_decref(j);

    /* 2. List keys - must not leak plaintext */
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/keys", "GET", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "list keys -> 200");
    j = json_loads(body, 0, NULL);
    free(body);
    json_t* arr = json_object_get(j, "keys");
    TEST_ASSERT(arr != NULL && json_is_array(arr) && json_array_size(arr) == 1, "1 key listed");
    json_t* k0 = json_array_get(arr, 0);
    TEST_ASSERT(json_object_get(k0, "plaintext") == NULL, "plaintext not in list");
    TEST_ASSERT(json_object_get(k0, "key_hash") != NULL, "key_hash present");
    json_decref(j);

    /* 3. Resolve key via core auth cache (populate cache) */
    key_rec_t krec;
    int arc = auth_key_resolve(&core.keys, plain, &krec);
    TEST_ASSERT(arc == 0 && krec.rate_qps == 5, "auth_key_resolve ok");
    key_rec_free(&krec);

    /* 4. Patch key (rate_qps = 20) -> should invalidate cache */
    char patch_uri[64];
    snprintf(patch_uri, sizeof patch_uri, "/admin/v1/keys/%ld", key_id);
    const char* preq = "{\"rate_qps\":20}";
    body = NULL;
    rc = admin_dispatch(&adm, patch_uri, "PATCH", "admin-secret-token", preq, strlen(preq), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "patch key -> 200");
    free(body);

    /* 5. Re-resolve key -> sees updated rate_qps */
    arc = auth_key_resolve(&core.keys, plain, &krec);
    TEST_ASSERT(arc == 0 && krec.rate_qps == 20, "auth_key_resolve sees updated rate");
    key_rec_free(&krec);

    /* 6. Revoke key -> should invalidate cache and return -2 on resolve */
    body = NULL;
    rc = admin_dispatch(&adm, patch_uri, "DELETE", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "revoke key -> 200");
    free(body);

    arc = auth_key_resolve(&core.keys, plain, &krec);
    TEST_ASSERT(arc == -2, "revoked key returns -2");
    key_rec_free(&krec);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_models_lifecycle)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. Create model */
    const char* req = "{\"name\":\"gpt-4o\",\"provider\":\"openai\",\"endpoint\":\"https://api.openai.com/v1\",\"default_params\":{\"temperature\":0.7}}";
    int rc = admin_dispatch(&adm, "/admin/v1/models", "POST", "admin-secret-token", req, strlen(req), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 201, "create model -> 201");
    free(body);

    /* 2. List models */
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/models", "GET", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "list models -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    json_t* arr = json_object_get(j, "models");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 1, "1 model in list");
    json_decref(j);

    /* 3. Patch model */
    const char* preq = "{\"endpoint\":\"https://proxy.openai.com/v1\"}";
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/models/gpt-4o", "PATCH", "admin-secret-token", preq, strlen(preq), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "patch model -> 200");
    free(body);

    model_rec_t m;
    TEST_ASSERT(fake_get_model(&db, "gpt-4o", &m) == 0, "get model ok");
    TEST_ASSERT(strcmp(m.endpoint, "https://proxy.openai.com/v1") == 0, "endpoint updated");

    /* 4. Delete model */
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/models/gpt-4o", "DELETE", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "delete model -> 200");
    free(body);
    TEST_ASSERT(fake_get_model(&db, "gpt-4o", &m) != 0, "model deleted");

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_usage_query)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    /* Seed usage row */
    usage_row_t ur;
    memset(&ur, 0, sizeof ur);
    ur.key_id = 42;
    snprintf(ur.model_name, sizeof ur.model_name, "gpt-4o");
    ur.day = 1726704000; /* 2024-09-19 midnight */
    ur.requests = 100;
    ur.prompt_tokens = 5000;
    ur.completion_tokens = 2000;
    fake_flush_usage(&db, &ur, 1);

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. Missing key param -> 400 */
    int rc = admin_dispatch(&adm, "/admin/v1/usage", "GET", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 400, "missing key param -> 400");
    free(body);

    /* 2. Valid usage query */
    body = NULL;
    rc = admin_dispatch(&adm, "/admin/v1/usage?key=42&from=2024-09-01&to=2024-09-30", "GET", "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "usage query -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(j != NULL, "parsed usage json");
    json_t* uarr = json_object_get(j, "usage");
    TEST_ASSERT(uarr != NULL && json_array_size(uarr) == 1, "1 usage row returned");
    json_t* r0 = json_array_get(uarr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(r0, "requests")) == 100, "requests == 100");
    TEST_ASSERT(json_integer_value(json_object_get(r0, "prompt_tokens")) == 5000, "prompt_tokens == 5000");
    json_decref(j);

    teardown_admin(ps, &core, &db);
}

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

struct fake_provider {
    int            in_use;
    provider_rec_t p;
};

struct fake_db {
    struct fake_key      keys[FAKE_CAP];
    model_rec_t          models[FAKE_CAP];
    int                  n_models;
    int                  fail_create_model; /* when nonzero, create_model fails */
    struct fake_provider providers[FAKE_CAP];
    long                 next_provider_id;
    usage_row_t          usage[FAKE_CAP];
    int                  n_usage;
    long                 next_key_id;
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
deep_copy_provider_models(provider_rec_t* dst, const provider_rec_t* src)
{
    dst->models = NULL;
    dst->n_models = src->n_models;
    if (src->n_models > 0) {
        dst->models = malloc(sizeof(char*) * (size_t)src->n_models);
        if (dst->models == NULL) {
            return -1;
        }
        for (int j = 0; j < src->n_models; j++) {
            dst->models[j] = strdup(src->models[j]);
            if (dst->models[j] == NULL) {
                provider_rec_free(dst);
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
    return 1;
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
    int             count = db->n_models < cap ? db->n_models : cap;
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
    if (db->fail_create_model) {
        return -1;
    }
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
                memcpy(db->models[i].targets, m->targets, sizeof m->targets);
                db->models[i].n_targets = m->n_targets;
            }
            if (mask & MMASK_LB_POLICY) {
                snprintf(
                    db->models[i].lb_policy, sizeof db->models[i].lb_policy, "%s", m->lb_policy);
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
        if (key_id != 0 && r->key_id != key_id) {
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

static int
fake_list_providers(void* ctx, provider_rec_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < FAKE_CAP && *n < cap; i++) {
        struct fake_provider* fp = &db->providers[i];
        if (!fp->in_use) {
            continue;
        }
        out[*n] = fp->p;
        out[*n].models = NULL;
        out[*n].n_models = 0;
        if (deep_copy_provider_models(&out[*n], &fp->p) != 0) {
            return -1;
        }
        (*n)++;
    }
    return 0;
}

static int
fake_get_provider(void* ctx, long id, provider_rec_t* out)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_provider* fp = &db->providers[i];
        if (fp->in_use && fp->p.id == id) {
            *out = fp->p;
            out->models = NULL;
            out->n_models = 0;
            if (deep_copy_provider_models(out, &fp->p) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static int
fake_create_provider(void* ctx, const provider_rec_t* p, long* out_id)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_provider* fp = &db->providers[i];
        if (!fp->in_use) {
            fp->in_use = 1;
            fp->p = *p;
            fp->p.id = ++db->next_provider_id;
            fp->p.models = NULL;
            fp->p.n_models = 0;
            if (deep_copy_provider_models(&fp->p, p) != 0) {
                fp->in_use = 0;
                return -1;
            }
            *out_id = fp->p.id;
            return 0;
        }
    }
    return -1;
}

static int
fake_update_provider(void* ctx, const provider_rec_t* p, int mask)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_provider* fp = &db->providers[i];
        if (fp->in_use && fp->p.id == p->id) {
            if (mask & PMASK_TYPE) {
                snprintf(fp->p.provider_type, sizeof fp->p.provider_type, "%s", p->provider_type);
            }
            if (mask & PMASK_ENDPOINT) {
                snprintf(fp->p.endpoint, sizeof fp->p.endpoint, "%s", p->endpoint);
            }
            if (mask & PMASK_API_KEY) {
                snprintf(fp->p.api_key, sizeof fp->p.api_key, "%s", p->api_key);
            }
            if (mask & PMASK_ENABLED) {
                fp->p.enabled = p->enabled;
            }
            if (mask & PMASK_MODELS) {
                for (int m = 0; m < fp->p.n_models; m++) {
                    free(fp->p.models[m]);
                }
                free(fp->p.models);
                fp->p.models = NULL;
                fp->p.n_models = 0;
                if (deep_copy_provider_models(&fp->p, p) != 0) {
                    return -1;
                }
            }
            return 0;
        }
    }
    return -1;
}

static int
fake_delete_provider(void* ctx, long id)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_provider* fp = &db->providers[i];
        if (fp->in_use && fp->p.id == id) {
            for (int m = 0; m < fp->p.n_models; m++) {
                free(fp->p.models[m]);
            }
            free(fp->p.models);
            fp->in_use = 0;
            return 0;
        }
    }
    return -1;
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
    ops->list_providers = fake_list_providers;
    ops->get_provider = fake_get_provider;
    ops->create_provider = fake_create_provider;
    ops->update_provider = fake_update_provider;
    ops->delete_provider = fake_delete_provider;
    ops->flush_usage = fake_flush_usage;
    ops->query_usage = fake_query_usage;
}

static void
setup_admin(struct fake_db* db,
            pg_ops_t*       ops,
            pg_store_t**    out_ps,
            aigate_core*    out_core,
            admin_ctx_t*    out_adm,
            char            admin_hash[65])
{
    memset(db, 0, sizeof *db);
    db->next_key_id = 1;
    db->next_provider_id = 0; /* first created provider gets id 1 */
    build_fake_ops(db, ops);
    *out_ps = pg_store_open("unused", ops);

    sha256_hex("admin-secret-token", strlen("admin-secret-token"), admin_hash);

    aigate_core_init(out_core, *out_ps, NULL, 5000, 0);

    out_adm->ac = out_core;
    out_adm->ps = *out_ps;
    out_adm->admin_token_hash = admin_hash;
    out_adm->allow_plaintext_keys = 1;
}

static void
teardown_admin(pg_store_t* ps, aigate_core* core, struct fake_db* db)
{
    aigate_core_shutdown(core);
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db->keys[i].in_use) {
            key_rec_free(&db->keys[i].k);
        }
        if (db->providers[i].in_use) {
            provider_rec_free(&db->providers[i].p);
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

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. Missing bearer */
    int rc =
        admin_dispatch(&adm, "/admin/v1/keys", "GET", NULL, NULL, NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 401, "missing bearer -> 401");
    free(body);

    /* 2. Wrong bearer */
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", NULL, "wrong-secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 401, "wrong bearer -> 401");
    free(body);

    /* 3. Correct bearer */
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
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

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. Create key */
    const char* req = "{\"name\":\"alice\",\"allowed_models\":[\"gpt-4o\"],\"rate_qps\":5,\"daily_"
                      "token_quota\":1000}";
    int         rc = admin_dispatch(&adm,
                                    "/admin/v1/keys",
                                    "POST",
                                    NULL,
                                    "admin-secret-token",
                                    req,
                                    strlen(req),
                                    &status,
                                    &body,
                                    &len);
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
    rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
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
    int       arc = auth_key_resolve(&core.keys, plain, &krec);
    TEST_ASSERT(arc == 0 && krec.rate_qps == 5, "auth_key_resolve ok");
    key_rec_free(&krec);

    /* 4. Patch key (rate_qps = 20) -> should invalidate cache */
    char patch_uri[64];
    snprintf(patch_uri, sizeof patch_uri, "/admin/v1/keys/%ld", key_id);
    const char* preq = "{\"rate_qps\":20}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        patch_uri,
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        preq,
                        strlen(preq),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "patch key -> 200");
    free(body);

    /* 5. Re-resolve key -> sees updated rate_qps */
    arc = auth_key_resolve(&core.keys, plain, &krec);
    TEST_ASSERT(arc == 0 && krec.rate_qps == 20, "auth_key_resolve sees updated rate");
    key_rec_free(&krec);

    /* 6. Revoke key -> should invalidate cache and return -2 on resolve */
    body = NULL;
    rc = admin_dispatch(
        &adm, patch_uri, "DELETE", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
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

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. Create model */
    const char* req = "{\"name\":\"gpt-4o\",\"provider\":\"openai\",\"endpoint\":\"https://"
                      "api.openai.com/v1\",\"default_params\":{\"temperature\":0.7}}";
    int         rc = admin_dispatch(&adm,
                                    "/admin/v1/models",
                                    "POST",
                                    NULL,
                                    "admin-secret-token",
                                    req,
                                    strlen(req),
                                    &status,
                                    &body,
                                    &len);
    TEST_ASSERT(rc == 0 && status == 201, "create model -> 201");
    free(body);

    /* 2. List models */
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/models", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "list models -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    json_t* arr = json_object_get(j, "models");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 1, "1 model in list");
    json_decref(j);

    /* 3. Patch model */
    const char* preq = "{\"endpoint\":\"https://proxy.openai.com/v1\"}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/models/gpt-4o",
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        preq,
                        strlen(preq),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "patch model -> 200");
    free(body);

    model_rec_t m;
    TEST_ASSERT(fake_get_model(&db, "gpt-4o", &m) == 0, "get model ok");
    TEST_ASSERT(strcmp(m.endpoint, "https://proxy.openai.com/v1") == 0, "endpoint updated");

    /* 4. Delete model */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/models/gpt-4o",
                        "DELETE",
                        NULL,
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "delete model -> 200");
    free(body);
    TEST_ASSERT(fake_get_model(&db, "gpt-4o", &m) != 0, "model deleted");

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_default_params_oversize_rejected)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1100-char value: the packed JSON exceeds the 1024-byte column */
    char  big[1101];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    char* req = malloc(sizeof big + 128);
    snprintf(req, sizeof big + 128,
             "{\"name\":\"big-params\",\"provider\":\"openai\",\"endpoint\":\"https://"
             "api.openai.com/v1\",\"default_params\":{\"k\":\"%s\"}}",
             big);

    int rc = admin_dispatch(&adm,
                            "/admin/v1/models",
                            "POST",
                            NULL,
                            "admin-secret-token",
                            req,
                            strlen(req),
                            &status,
                            &body,
                            &len);
    int n_before = db.n_models;
    TEST_ASSERT(rc == 0 && status == 400, "oversize create -> 400, got %d", status);
    free(body);
    model_rec_t probe;
    TEST_ASSERT(fake_get_model(&db, "big-params", &probe) != 0, "absent");
    free(req);

    /* Oversize patch of a fresh model: 400 and stored row untouched */
    const char* preq_small =
        "{\"name\":\"ok-model\",\"provider\":\"openai\",\"endpoint\":\"https://api.openai.com/"
        "v1\",\"default_params\":{\"temperature\":0.2}}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/models",
                        "POST",
                        NULL,
                        "admin-secret-token",
                        preq_small,
                        strlen(preq_small),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 201, "small create -> 201, got %d", status);
    free(body);

    req = malloc(sizeof big + 128);
    snprintf(req,
             sizeof big + 128,
             "{\"default_params\":{\"k\":\"%s\"}}",
             big);
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/models/ok-model",
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        req,
                        strlen(req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 400, "oversize patch -> 400, got %d", status);
    free(body);
    free(req);

    model_rec_t m;
    TEST_ASSERT(fake_get_model(&db, "ok-model", &m) == 0, "get model ok");
    TEST_ASSERT(strcmp(m.default_params_json, "{\"temperature\":0.2}") == 0,
                "stored params untouched after rejected patch");

    /* small patch still lands */
    const char* ok_patch = "{\"default_params\":{\"top_p\":0.9}}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/models/ok-model",
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        ok_patch,
                        strlen(ok_patch),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "small patch -> 200, got %d", status);
    free(body);
    TEST_ASSERT(fake_get_model(&db, "ok-model", &m) == 0, "get model ok (2)");
    TEST_ASSERT(strcmp(m.default_params_json, "{\"top_p\":0.9}") == 0, "params updated");

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_models_multi_target)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. Create model with targets array & lb_policy */
    const char* req = "{\"name\":\"hybrid-model\","
                      "\"lb_policy\":\"weighted_round_robin\","
                      "\"targets\":["
                      "  "
                      "{\"provider\":\"openai\",\"endpoint\":\"http://"
                      "ep1\",\"upstream_key_ref\":\"k1\",\"weight\":2,\"priority\":0},"
                      "  "
                      "{\"provider\":\"azure\",\"endpoint\":\"http://"
                      "ep2\",\"upstream_key_ref\":\"k2\",\"weight\":1,\"priority\":1}"
                      "]}";
    int         rc = admin_dispatch(&adm,
                                    "/admin/v1/models",
                                    "POST",
                                    NULL,
                                    "admin-secret-token",
                                    req,
                                    strlen(req),
                                    &status,
                                    &body,
                                    &len);
    TEST_ASSERT(rc == 0 && status == 201, "create multi-target model -> 201");
    free(body);

    /* 2. Trip circuit breaker for target 0 (http://ep1) */
    cb_record_failure(core.cb, "hybrid-model", "http://ep1", 500);
    cb_record_failure(core.cb, "hybrid-model", "http://ep1", 500);
    cb_record_failure(core.cb, "hybrid-model", "http://ep1", 500);
    TEST_ASSERT(cb_get_state(core.cb, "hybrid-model", "http://ep1") == CB_OPEN,
                "ep1 tripped to open");

    /* 3. List models and inspect targets & cb_state */
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/models", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "list models -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(j != NULL, "parsed models json");
    json_t* arr = json_object_get(j, "models");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 1, "1 model in list");
    json_t* m0 = json_array_get(arr, 0);

    json_t* jpol = json_object_get(m0, "lb_policy");
    TEST_ASSERT(jpol != NULL && strcmp(json_string_value(jpol), "weighted_round_robin") == 0,
                "lb_policy is weighted_round_robin");

    json_t* jtargets = json_object_get(m0, "targets");
    TEST_ASSERT(jtargets != NULL && json_array_size(jtargets) == 2, "2 targets in response");

    json_t* t0 = json_array_get(jtargets, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(t0, "endpoint")), "http://ep1") == 0,
                "target 0 endpoint ep1");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(t0, "cb_state")), "open") == 0,
                "target 0 cb_state is open");
    TEST_ASSERT(json_integer_value(json_object_get(t0, "weight")) == 2, "target 0 weight 2");
    json_t* t0kref = json_object_get(t0, "upstream_key_ref");
    TEST_ASSERT(t0kref != NULL && strcmp(json_string_value(t0kref), "k1") == 0,
                "target 0 upstream_key_ref k1 round-trips");

    json_t* t1 = json_array_get(jtargets, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(t1, "endpoint")), "http://ep2") == 0,
                "target 1 endpoint ep2");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(t1, "cb_state")), "closed") == 0,
                "target 1 cb_state is closed");
    TEST_ASSERT(json_integer_value(json_object_get(t1, "priority")) == 1, "target 1 priority 1");
    json_t* t1kref = json_object_get(t1, "upstream_key_ref");
    TEST_ASSERT(t1kref != NULL && strcmp(json_string_value(t1kref), "k2") == 0,
                "target 1 upstream_key_ref k2 round-trips");

    json_decref(j);

    /* 4. Patch model: update lb_policy to priority via PUT */
    const char* preq = "{\"lb_policy\":\"priority\"}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/models/hybrid-model",
                        "PUT",
                        NULL,
                        "admin-secret-token",
                        preq,
                        strlen(preq),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "put model -> 200");
    free(body);

    model_rec_t updated;
    TEST_ASSERT(fake_get_model(&db, "hybrid-model", &updated) == 0, "get model ok");
    TEST_ASSERT(strcmp(updated.lb_policy, "priority") == 0, "lb_policy updated to priority");

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

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. Missing key/from/to -> 200 with defaults (all keys, last 7 days) */
    int rc = admin_dispatch(
        &adm, "/admin/v1/usage", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "missing params -> 200 with defaults");
    json_t* jd = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(jd != NULL && json_object_get(jd, "usage") != NULL,
                "default query returns usage array");
    json_decref(jd);

    /* 2. Valid usage query */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/usage?key=42&from=2024-09-01&to=2024-09-30",
                        "GET",
                        NULL,
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "usage query -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(j != NULL, "parsed usage json");
    json_t* uarr = json_object_get(j, "usage");
    TEST_ASSERT(uarr != NULL && json_array_size(uarr) == 1, "1 usage row returned");
    json_t* r0 = json_array_get(uarr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(r0, "requests")) == 100, "requests == 100");
    TEST_ASSERT(json_integer_value(json_object_get(r0, "prompt_tokens")) == 5000,
                "prompt_tokens == 5000");
    json_decref(j);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_create_and_list)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    const char* req = "{\"name\":\"deepseek\",\"provider_type\":\"deepseek\",\"endpoint\":\"https:/"
                      "/api.deepseek.com/v1\","
                      "\"api_key\":\"sk-0123456789abcdef\",\"models\":[\"deepseek-chat\","
                      "\"deepseek-reasoner\"],\"enabled\":true}";

    int rc = admin_dispatch(&adm,
                            "/admin/v1/providers",
                            "POST",
                            NULL,
                            "admin-secret-token",
                            req,
                            strlen(req),
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0 && status == 201, "provider create 201");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(res != NULL, "parsed create resp");
    TEST_ASSERT(json_is_true(json_object_get(res, "created")), "created is true");
    long id = (long)json_integer_value(json_object_get(res, "id"));
    TEST_ASSERT(id > 0, "id > 0");
    json_decref(res);

    /* Verify auto-synced models in models table */
    model_rec_t m_chat, m_reasoner;
    TEST_ASSERT(fake_get_model(&db, "deepseek-chat", &m_chat) == 0, "deepseek-chat created");
    TEST_ASSERT(strcmp(m_chat.endpoint, "https://api.deepseek.com/v1") == 0,
                "m_chat endpoint matches");
    TEST_ASSERT(strcmp(m_chat.upstream_key_ref, "sk-0123456789abcdef") == 0, "m_chat key matches");
    TEST_ASSERT(fake_get_model(&db, "deepseek-reasoner", &m_reasoner) == 0,
                "deepseek-reasoner created");

    /* List providers */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers",
                        "GET",
                        NULL,
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "provider list 200");
    res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(res != NULL, "parsed list resp");
    json_t* parr = json_object_get(res, "providers");
    TEST_ASSERT(parr != NULL && json_array_size(parr) == 1, "1 provider in list");
    json_t* p0 = json_array_get(parr, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "name")), "deepseek") == 0,
                "name deepseek");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "endpoint")),
                       "https://api.deepseek.com/v1") == 0,
                "endpoint deepseek");
    /* Masked API key */
    const char* masked = json_string_value(json_object_get(p0, "api_key"));
    TEST_ASSERT(masked != NULL && strstr(masked, "••••") != NULL, "masked api key contains dots");
    json_t* marr = json_object_get(p0, "models");
    TEST_ASSERT(marr != NULL && json_array_size(marr) == 2, "2 models in provider");
    json_decref(res);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_sync_failed_reported)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* create with 2 models; storage fails for both -> sync_failed == 2 */
    db.fail_create_model = 1;
    const char* req = "{\"name\":\"sync-fail\",\"provider_type\":\"openai\",\"endpoint\":\"https://"
                      "api.openai.com/v1\","
                      "\"api_key\":\"sk-test\",\"models\":[\"m-one\",\"m-two\"]}";
    int rc = admin_dispatch(&adm,
                            "/admin/v1/providers",
                            "POST",
                            NULL,
                            "admin-secret-token",
                            req,
                            strlen(req),
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0 && status == 201, "create still 201, got %d", status);
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(res != NULL, "parsed create resp");
    json_t* sf = json_object_get(res, "sync_failed");
    TEST_ASSERT(sf != NULL, "sync_failed field present");
    TEST_ASSERT(json_integer_value(sf) == 2, "sync_failed == 2, got %ld",
                (long)json_integer_value(sf));
    json_decref(res);

    /* storage healthy: patch reports sync_failed == 0 */
    db.fail_create_model = 0;
    const char* patch_req = "{\"enabled\":false}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers/1",
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        patch_req,
                        strlen(patch_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "patch 200, got %d", status);
    res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(res != NULL, "parsed patch resp");
    sf = json_object_get(res, "sync_failed");
    TEST_ASSERT(sf != NULL, "patch sync_failed field present");
    TEST_ASSERT(json_integer_value(sf) == 0, "patch sync_failed == 0, got %ld",
                (long)json_integer_value(sf));
    json_decref(res);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_patch_and_delete)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    const char* req =
        "{\"name\":\"anthropic\",\"provider_type\":\"anthropic\",\"endpoint\":\"https://"
        "api.anthropic.com\","
        "\"api_key\":\"sk-ant-testkey123\",\"models\":[\"claude-3-7-sonnet\"],\"enabled\":true}";

    int rc = admin_dispatch(&adm,
                            "/admin/v1/providers",
                            "POST",
                            NULL,
                            "admin-secret-token",
                            req,
                            strlen(req),
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0 && status == 201, "anthropic created");
    free(body);

    /* PATCH provider 1 */
    const char* patch_req =
        "{\"endpoint\":\"https://api.anthropic.com/"
        "v2\",\"models\":[\"claude-3-7-sonnet\",\"claude-3-5-haiku\"],\"enabled\":false}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers/1",
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        patch_req,
                        strlen(patch_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "provider patched");
    free(body);

    /* Verify updated models in models table */
    model_rec_t m;
    TEST_ASSERT(fake_get_model(&db, "claude-3-7-sonnet", &m) == 0, "claude-3-7-sonnet exists");
    TEST_ASSERT(strcmp(m.endpoint, "https://api.anthropic.com/v2") == 0, "endpoint updated");
    TEST_ASSERT(m.enabled == 0, "enabled updated to 0");
    TEST_ASSERT(fake_get_model(&db, "claude-3-5-haiku", &m) == 0, "claude-3-5-haiku auto-created");

    /* DELETE provider 1 */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers/1",
                        "DELETE",
                        NULL,
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "provider deleted");
    free(body);

    /* List should be empty */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers",
                        "GET",
                        NULL,
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "provider list after delete");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(json_array_size(json_object_get(res, "providers")) == 0, "0 providers");
    json_decref(res);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_plaintext_gate)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);
    /* setup_admin enables the gate; prove both directions */

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* plaintext key rejected when gate is off */
    adm.allow_plaintext_keys = 0;
    const char* req =
        "{\"name\":\"deepseek\",\"provider_type\":\"deepseek\",\"endpoint\":\"https://"
        "api.deepseek.com/v1\","
        "\"api_key\":\"sk-0123456789abcdef\",\"models\":[\"deepseek-chat\"],\"enabled\":true}";
    int rc = admin_dispatch(&adm,
                            "/admin/v1/providers",
                            "POST",
                            NULL,
                            "admin-secret-token",
                            req,
                            strlen(req),
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0 && status == 400, "plaintext key rejected -> 400");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    json_t* err = json_object_get(res, "error");
    TEST_ASSERT(err != NULL && strcmp(json_string_value(json_object_get(err, "type")),
                                      "provider_key_required") == 0,
                "provider_key_required error");
    json_decref(res);

    /* env: ref always accepted */
    const char* env_req =
        "{\"name\":\"envprov\",\"provider_type\":\"openai\",\"endpoint\":\"https://api.openai.com/"
        "v1\","
        "\"api_key\":\"env:OPENAI_API_KEY\",\"models\":[\"gpt-4o\"],\"enabled\":true}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers",
                        "POST",
                        NULL,
                        "admin-secret-token",
                        env_req,
                        strlen(env_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 201, "env: ref accepted -> 201");
    free(body);

    /* plaintext key accepted when gate is on */
    adm.allow_plaintext_keys = 1;
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers",
                        "POST",
                        NULL,
                        "admin-secret-token",
                        req,
                        strlen(req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 201, "plaintext key accepted -> 201");
    free(body);

    /* update path: rejected when gate is off */
    adm.allow_plaintext_keys = 0;
    const char* patch_req = "{\"api_key\":\"sk-new-plaintext\"}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/providers/1",
                        "PATCH",
                        NULL,
                        "admin-secret-token",
                        patch_req,
                        strlen(patch_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 400, "patch plaintext key rejected -> 400");
    free(body);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_lockout)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);
    admin_lockout_reset();

    int         status = 0;
    char*       body = NULL;
    size_t      len = 0;
    const char* ip = "10.1.2.3";

    /* 10 failed attempts with a real client_ip → lockout engages */
    for (int i = 0; i < 10; i++) {
        body = NULL;
        int rc = admin_dispatch(
            &adm, "/admin/v1/keys", "GET", ip, "wrong", NULL, 0, &status, &body, &len);
        TEST_ASSERT(rc == 0 && status == 401, "attempt %d -> 401", i);
        free(body);
    }

    /* even the correct token is locked out now */
    body = NULL;
    int rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", ip, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 429, "locked out -> 429");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    json_t* err = json_object_get(res, "error");
    TEST_ASSERT(err != NULL &&
                    strcmp(json_string_value(json_object_get(err, "type")), "locked_out") == 0,
                "locked_out error type");
    json_decref(res);

    /* a different IP is not affected */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/keys",
                        "GET",
                        "10.1.2.4",
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "other ip -> 200");
    free(body);

    /* reset restores access */
    admin_lockout_reset();
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", ip, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "after reset -> 200");
    free(body);

    /* NULL client_ip disables lockout entirely */
    admin_lockout_reset();
    for (int i = 0; i < 20; i++) {
        body = NULL;
        rc = admin_dispatch(
            &adm, "/admin/v1/keys", "GET", NULL, "wrong", NULL, 0, &status, &body, &len);
        TEST_ASSERT(rc == 0 && status == 401, "null ip attempt %d -> 401", i);
        free(body);
    }
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "null ip never locked -> 200");
    free(body);

    teardown_admin(ps, &core, &db);
    admin_lockout_reset();
}

TEST_CASE(test_admin_lockout_policy_env)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);
    admin_lockout_reset();

    int         status = 0;
    char*       body = NULL;
    size_t      len = 0;
    const char* ip = "10.9.9.9";

    /* Tight policy: 3 fails engages the lockout */
    admin_lockout_set_policy(3, 60);
    for (int i = 0; i < 3; i++) {
        body = NULL;
        int rc = admin_dispatch(
            &adm, "/admin/v1/keys", "GET", ip, "wrong", NULL, 0, &status, &body, &len);
        TEST_ASSERT(rc == 0 && status == 401, "tight attempt %d -> 401", i);
        free(body);
    }
    body = NULL;
    int rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", ip, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 429, "tight policy locks after 3 fails, got %d", status);
    free(body);

    /* Out-of-range calls keep the current policy */
    admin_lockout_set_policy(1, 300); /* fails=1 out of [2..1000]: ignored */
    admin_lockout_set_policy(10, 4);  /* window=4 out of [5..3600]: ignored */
    admin_lockout_reset();
    for (int i = 0; i < 2; i++) {
        body = NULL;
        rc = admin_dispatch(
            &adm, "/admin/v1/keys", "GET", ip, "wrong", NULL, 0, &status, &body, &len);
        TEST_ASSERT(rc == 0 && status == 401, "loose attempt %d -> 401", i);
        free(body);
    }
    body = NULL;
    rc = admin_dispatch(
        &adm, "/admin/v1/keys", "GET", ip, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "policy still 3/60: 2 fails not locked, got %d",
                status);
    free(body);

    /* Restore defaults for later tests */
    admin_lockout_set_policy(10, 300);
    admin_lockout_reset();
    teardown_admin(ps, &core, &db);
}

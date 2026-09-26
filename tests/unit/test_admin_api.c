/** @file test_admin_api.c
 *  @brief Unit tests for /admin/v1 management plane (spec §4.2, Task 10).
 */
#include "run_tests.h"
#include "admin_api.h"
#include "aigate_core.h"
#include "pg_store.h"
#include "sha256.h"
#include "mock_upstream.h"

#include <jansson.h>
#include <math.h>
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

struct fake_group {
    int         in_use;
    group_rec_t g;
};

struct fake_db {
    struct fake_key      keys[FAKE_CAP];
    model_rec_t          models[FAKE_CAP];
    int                  n_models;
    int                  fail_create_model; /* when nonzero, create_model fails */
    struct fake_provider providers[FAKE_CAP];
    long                 next_provider_id;
    struct fake_group    groups[FAKE_CAP];
    long                 next_group_id;
    usage_row_t          usage[FAKE_CAP];
    int                  n_usage;
    long                 next_key_id;
    usage_request_row_t  reqs[FAKE_CAP];
    int                  n_reqs;
    cost_row_t           cost_rows[FAKE_CAP];
    int                  n_cost_rows;
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
    if (k->group_id > 0) {
        int found = 0;
        for (int i = 0; i < FAKE_CAP; i++) {
            if (db->groups[i].in_use && db->groups[i].g.id == k->group_id) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return -2;
        }
    }
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
            if (mask & KMASK_GROUP) {
                if (k->group_id > 0) {
                    int found = 0;
                    for (int g = 0; g < FAKE_CAP; g++) {
                        if (db->groups[g].in_use && db->groups[g].g.id == k->group_id) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        return -2;
                    }
                }
                fk->k.group_id = k->group_id;
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
            if (mask & MMASK_PRICING) {
                snprintf(db->models[i].pricing_json,
                         sizeof db->models[i].pricing_json,
                         "%s",
                         m->pricing_json);
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
fake_flush_requests(void* ctx, const usage_request_row_t* rows, int n)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < n && db->n_reqs < FAKE_CAP; i++) {
        db->reqs[db->n_reqs++] = rows[i];
    }
    return 0;
}

static int
fake_query_requests(void* ctx, long key_id, time_t since, usage_request_row_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < db->n_reqs && *n < cap; i++) {
        usage_request_row_t* r = &db->reqs[i];
        if (key_id != 0 && r->key_id != key_id) {
            continue;
        }
        if (r->ts < since) {
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

static int
fake_create_group(void* ctx, const char* name, long* out_id)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db->groups[i].in_use && strcmp(db->groups[i].g.name, name) == 0) {
            return -2;
        }
    }
    for (int i = 0; i < FAKE_CAP; i++) {
        struct fake_group* fg = &db->groups[i];
        if (!fg->in_use) {
            fg->in_use = 1;
            fg->g.id = db->next_group_id++;
            snprintf(fg->g.name, sizeof fg->g.name, "%s", name);
            fg->g.created_at = time(NULL);
            fg->g.key_count = 0;
            *out_id = fg->g.id;
            return 0;
        }
    }
    return -1;
}

static int
fake_list_groups(void* ctx, group_rec_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < FAKE_CAP && *n < cap; i++) {
        if (db->groups[i].in_use) {
            out[*n] = db->groups[i].g;
            long cnt = 0;
            for (int k = 0; k < FAKE_CAP; k++) {
                if (db->keys[k].in_use && db->keys[k].k.group_id == db->groups[i].g.id) {
                    cnt++;
                }
            }
            out[*n].key_count = cnt;
            (*n)++;
        }
    }
    return 0;
}

static int
fake_patch_group(void* ctx, long id, const char* name)
{
    struct fake_db* db = ctx;
    int             target = -1;
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db->groups[i].in_use && db->groups[i].g.id == id) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        return 1;
    }
    for (int i = 0; i < FAKE_CAP; i++) {
        if (i != target && db->groups[i].in_use && strcmp(db->groups[i].g.name, name) == 0) {
            return -2;
        }
    }
    snprintf(db->groups[target].g.name, sizeof db->groups[target].g.name, "%s", name);
    return 0;
}

static int
fake_delete_group(void* ctx, long id)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db->groups[i].in_use && db->groups[i].g.id == id) {
            db->groups[i].in_use = 0;
            for (int k = 0; k < FAKE_CAP; k++) {
                if (db->keys[k].in_use && db->keys[k].k.group_id == id) {
                    db->keys[k].k.group_id = 0;
                }
            }
            return 0;
        }
    }
    return 1;
}

static int
fake_count_keys_in_group(void* ctx, long group_id, long* n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < FAKE_CAP; i++) {
        if (db->keys[i].in_use && db->keys[i].k.group_id == group_id) {
            (*n)++;
        }
    }
    return 0;
}

static int
fake_query_cost(void* ctx, long since_s, long until_s, cost_row_t* out, int cap, int* n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < db->n_cost_rows && *n < cap; i++) {
        cost_row_t* r = &db->cost_rows[i];
        if (r->bucket_day >= since_s && r->bucket_day <= until_s) {
            out[(*n)++] = *r;
        }
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
    ops->list_providers = fake_list_providers;
    ops->get_provider = fake_get_provider;
    ops->create_provider = fake_create_provider;
    ops->update_provider = fake_update_provider;
    ops->delete_provider = fake_delete_provider;
    ops->flush_usage = fake_flush_usage;
    ops->query_usage = fake_query_usage;
    ops->flush_usage_requests = fake_flush_requests;
    ops->query_usage_requests = fake_query_requests;
    ops->create_group = fake_create_group;
    ops->list_groups = fake_list_groups;
    ops->patch_group = fake_patch_group;
    ops->delete_group = fake_delete_group;
    ops->count_keys_in_group = fake_count_keys_in_group;
    ops->query_cost = fake_query_cost;
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
    db->next_group_id = 1;
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
    char big[1101];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    char* req = malloc(sizeof big + 128);
    snprintf(req,
             sizeof big + 128,
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
    snprintf(req, sizeof big + 128, "{\"default_params\":{\"k\":\"%s\"}}", big);
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

TEST_CASE(test_admin_usage_requests_query)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    usage_request_row_t rr;
    memset(&rr, 0, sizeof rr);
    rr.key_id = 42;
    snprintf(rr.model_name, sizeof rr.model_name, "gpt-4o");
    snprintf(rr.provider, sizeof rr.provider, "openai");
    rr.http_status = 200;
    rr.prompt_tokens = 500;
    rr.completion_tokens = 200;
    rr.cached_prompt_tokens = 10;
    rr.reasoning_tokens = 45;
    rr.latency_ns = 123000000;
    rr.ts = 1726704000; /* 2024-09-19 00:00 UTC */
    fake_flush_requests(&db, &rr, 1);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* keyed + since inside the row's day */
    int rc = admin_dispatch(&adm,
                            "/admin/v1/usage/requests?key_id=42&since=2024-09-01",
                            "GET",
                            "127.0.0.1",
                            "admin-secret-token",
                            NULL,
                            0,
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0 && status == 200, "requests query -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(j != NULL, "parsed json");
    json_t* rarr = json_object_get(j, "requests");
    TEST_ASSERT(rarr != NULL && json_array_size(rarr) == 1, "1 request row");
    json_t* r0 = json_array_get(rarr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(r0, "http_status")) == 200, "http_status 200");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(r0, "model")), "gpt-4o") == 0, "model");
    TEST_ASSERT(json_integer_value(json_object_get(r0, "prompt_tokens")) == 500, "prompt_tokens");
    TEST_ASSERT(json_integer_value(json_object_get(r0, "reasoning_tokens")) == 45, "reasoning_tokens");
    TEST_ASSERT(fabs(json_real_value(json_object_get(r0, "latency_ms")) - 123.0) < 0.01,
                "latency_ms 123");
    json_decref(j);

    /* other key: empty array */
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/usage/requests?key_id=99&since=2024-09-01",
                        "GET",
                        "127.0.0.1",
                        "admin-secret-token",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "empty query -> 200");
    j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(json_object_get(j, "requests") != NULL &&
                    json_array_size(json_object_get(j, "requests")) == 0,
                "zero rows for unknown key");
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
    int         rc = admin_dispatch(&adm,
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
    TEST_ASSERT(
        json_integer_value(sf) == 2, "sync_failed == 2, got %ld", (long)json_integer_value(sf));
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
    TEST_ASSERT(json_integer_value(sf) == 0,
                "patch sync_failed == 0, got %ld",
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
    TEST_ASSERT(rc == 0 && status == 200, "policy still 3/60: 2 fails not locked, got %d", status);
    free(body);

    /* Restore defaults for later tests */
    admin_lockout_set_policy(10, 300);
    admin_lockout_reset();
    teardown_admin(ps, &core, &db);
}

/* ---- P1-4: POST /admin/v1/providers/{id}/test ---- */

/* Create one provider via the admin API with the mock as its endpoint.
 * Returns provider id on success, or -1 on failure (caller asserts). */
static long
admin_create_probe_provider(admin_ctx_t* adm,
                            const char*  api_key,
                            const char*  base_url,
                            const char*  ptype)
{
    char req[1024];
    snprintf(req,
             sizeof req,
             "{\"name\":\"probe\",\"provider_type\":\"%s\",\"endpoint\":\"%s\","
             "\"api_key\":\"%s\",\"models\":[\"m1\"],\"enabled\":true}",
             ptype,
             base_url,
             api_key);
    int    status = 0;
    char*  body = NULL;
    size_t len = 0;
    int    rc = admin_dispatch(adm,
                               "/admin/v1/providers",
                               "POST",
                               NULL,
                               "admin-secret-token",
                               req,
                               strlen(req),
                               &status,
                               &body,
                               &len);
    if (rc != 0 || status != 201) {
        free(body);
        return -1;
    }
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    long id = (long)json_integer_value(json_object_get(res, "id"));
    json_decref(res);
    return id;
}

/* Fire the probe endpoint; returns the admin HTTP status and copies the
 * verdict string into @p verdict_out (before the response JSON is freed).
 * Returns -1 when the dispatch itself failed (caller asserts on the HTTP
 * status, so -1 will not match any expected code). */
static int
admin_run_probe(admin_ctx_t* adm, long provider_id, char* verdict_out, size_t cap)
{
    char uri[128];
    snprintf(uri, sizeof uri, "/admin/v1/providers/%ld/test", provider_id);
    int    status = 0;
    char*  body = NULL;
    size_t len = 0;
    int    rc =
        admin_dispatch(adm, uri, "POST", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    if (rc != 0) {
        free(body);
        snprintf(verdict_out, cap, "dispatch_failed");
        return -1;
    }
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    const json_t* jv = json_object_get(res, "verdict");
    if (json_is_string(jv) && cap > 0) {
        snprintf(verdict_out, cap, "%s", json_string_value(jv));
    } else {
        snprintf(verdict_out, cap, "null");
    }
    json_decref(res);
    return status;
}

TEST_CASE(test_admin_provider_test_ok)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock");
    long id = admin_create_probe_provider(&adm, "sk-test", mock_upstream_base(mu), "openai");
    TEST_ASSERT(id > 0, "provider created");

    char verdict[32] = "";
    TEST_ASSERT(admin_run_probe(&adm, id, verdict, sizeof verdict) == 200, "admin 200");
    TEST_ASSERT(strcmp(verdict, "ok") == 0, "verdict ok");
    mock_upstream_stop(mu);
    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_test_key_invalid)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock");
    mock_upstream_status(mu, 401);
    long id = admin_create_probe_provider(&adm, "sk-bad", mock_upstream_base(mu), "openai");
    TEST_ASSERT(id > 0, "provider created");

    char verdict[32] = "";
    TEST_ASSERT(admin_run_probe(&adm, id, verdict, sizeof verdict) == 200, "admin 200");
    TEST_ASSERT(strcmp(verdict, "key_invalid") == 0, "verdict key_invalid");
    mock_upstream_stop(mu);
    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_test_unverified_404)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock");
    mock_upstream_status(mu, 404);
    long id = admin_create_probe_provider(&adm, "sk-test", mock_upstream_base(mu), "openai");
    TEST_ASSERT(id > 0, "provider created");

    char verdict[32] = "";
    TEST_ASSERT(admin_run_probe(&adm, id, verdict, sizeof verdict) == 200, "admin 200");
    TEST_ASSERT(strcmp(verdict, "endpoint_unverified") == 0, "verdict endpoint_unverified");
    mock_upstream_stop(mu);
    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_test_env_missing)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    char verdict[32] = "";
    long id =
        admin_create_probe_provider(&adm, "env:AIGATE_NOPE_PROBE", "http://127.0.0.1:1", "openai");
    TEST_ASSERT(id > 0, "provider created");
    TEST_ASSERT(admin_run_probe(&adm, id, verdict, sizeof verdict) == 400, "env missing -> 400");
    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_test_unknown_type)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    char verdict[32] = "";
    long id = admin_create_probe_provider(&adm, "k", "http://127.0.0.1:1", "vertex");
    TEST_ASSERT(id > 0, "provider created");
    TEST_ASSERT(admin_run_probe(&adm, id, verdict, sizeof verdict) == 400,
                "probe_unsupported -> 400");
    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_provider_test_not_found)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    char verdict[32] = "";
    TEST_ASSERT(admin_run_probe(&adm, 999, verdict, sizeof verdict) == 404, "not found -> 404");
    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_groups_crud)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status;
    char*  body;
    size_t len;

    /* 1. Empty name -> 400 */
    const char* req_bad = "{\"name\":\"\"}";
    admin_dispatch(&adm,
                   "/admin/v1/groups",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_bad,
                   strlen(req_bad),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 400, "empty group name -> 400");
    free(body);

    /* 2. Create group "engineering" -> 201 */
    const char* req_eng = "{\"name\":\"engineering\"}";
    admin_dispatch(&adm,
                   "/admin/v1/groups",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_eng,
                   strlen(req_eng),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 201, "create engineering -> 201");
    json_error_t jerr;
    json_t*      j = json_loads(body, 0, &jerr);
    TEST_ASSERT(j != NULL, "json");
    TEST_ASSERT(json_integer_value(json_object_get(j, "id")) == 1, "id == 1");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(j, "name")), "engineering") == 0, "name");
    json_decref(j);
    free(body);

    /* 3. Duplicate name "engineering" -> 409 group_exists */
    admin_dispatch(&adm,
                   "/admin/v1/groups",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_eng,
                   strlen(req_eng),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 409, "duplicate group name -> 409");
    j = json_loads(body, 0, &jerr);
    json_t* err = json_object_get(j, "error");
    TEST_ASSERT(err && strcmp(json_string_value(json_object_get(err, "type")), "group_exists") == 0,
                "error == group_exists");
    json_decref(j);
    free(body);

    /* 4. Create second group "marketing" -> 201 */
    const char* req_mkt = "{\"name\":\"marketing\"}";
    admin_dispatch(&adm,
                   "/admin/v1/groups",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_mkt,
                   strlen(req_mkt),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 201, "create marketing -> 201");
    free(body);

    /* 5. List groups -> 200, 2 entries */
    admin_dispatch(
        &adm, "/admin/v1/groups", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(status == 200, "list groups -> 200");
    j = json_loads(body, 0, &jerr);
    json_t* garr = json_object_get(j, "groups");
    TEST_ASSERT(json_is_array(garr) && json_array_size(garr) == 2, "2 groups returned");
    json_decref(j);
    free(body);

    /* 6. Create key with non-existent group_id 999 -> 404 group_not_found */
    const char* req_key_bad = "{\"name\":\"k1\",\"group_id\":999}";
    admin_dispatch(&adm,
                   "/admin/v1/keys",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_key_bad,
                   strlen(req_key_bad),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 404, "group_id 999 -> 404");
    j = json_loads(body, 0, &jerr);
    err = json_object_get(j, "error");
    TEST_ASSERT(err &&
                    strcmp(json_string_value(json_object_get(err, "type")), "group_not_found") == 0,
                "group_not_found error");
    json_decref(j);
    free(body);

    /* 7. Create key with group_id 1 -> 201 */
    const char* req_key_ok = "{\"name\":\"k1\",\"group_id\":1}";
    admin_dispatch(&adm,
                   "/admin/v1/keys",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_key_ok,
                   strlen(req_key_ok),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 201, "key created with group_id 1");
    j = json_loads(body, 0, &jerr);
    long kid = json_integer_value(json_object_get(j, "key_id"));
    TEST_ASSERT(json_integer_value(json_object_get(j, "group_id")) == 1, "key group_id == 1");
    json_decref(j);
    free(body);

    /* 8. Verify list_groups reflects key_count = 1 on group 1 */
    admin_dispatch(
        &adm, "/admin/v1/groups", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    j = json_loads(body, 0, &jerr);
    garr = json_object_get(j, "groups");
    TEST_ASSERT(json_integer_value(json_object_get(json_array_get(garr, 0), "key_count")) == 1,
                "group 1 key_count == 1");
    TEST_ASSERT(json_integer_value(json_object_get(json_array_get(garr, 1), "key_count")) == 0,
                "group 2 key_count == 0");
    json_decref(j);
    free(body);

    /* 9. Delete group 1 while key attached -> 409 group_has_keys */
    admin_dispatch(&adm,
                   "/admin/v1/groups/1",
                   "DELETE",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 409, "delete group with keys -> 409");
    j = json_loads(body, 0, &jerr);
    err = json_object_get(j, "error");
    TEST_ASSERT(err &&
                    strcmp(json_string_value(json_object_get(err, "type")), "group_has_keys") == 0,
                "group_has_keys error");
    json_decref(j);
    free(body);

    /* 10. Patch key 1 to ungrouped (group_id: null) */
    char uri_kpatch[64];
    snprintf(uri_kpatch, sizeof uri_kpatch, "/admin/v1/keys/%ld", kid);
    const char* req_kpatch = "{\"group_id\":null}";
    admin_dispatch(&adm,
                   uri_kpatch,
                   "PATCH",
                   NULL,
                   "admin-secret-token",
                   req_kpatch,
                   strlen(req_kpatch),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "key patch to group null -> 200");
    free(body);

    /* 11. Now delete group 1 -> 200 */
    admin_dispatch(&adm,
                   "/admin/v1/groups/1",
                   "DELETE",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "delete group 1 now succeeds -> 200");
    free(body);

    /* 12. Delete group 999 -> 404 */
    admin_dispatch(&adm,
                   "/admin/v1/groups/999",
                   "DELETE",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 404, "delete non-existent group -> 404");
    free(body);

    /* 13. Patch group 2 name to "sales" -> 200 */
    const char* req_rename = "{\"name\":\"sales\"}";
    admin_dispatch(&adm,
                   "/admin/v1/groups/2",
                   "PATCH",
                   NULL,
                   "admin-secret-token",
                   req_rename,
                   strlen(req_rename),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "patch group 2 -> 200");
    j = json_loads(body, 0, &jerr);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(j, "name")), "sales") == 0,
                "renamed to sales");
    json_decref(j);
    free(body);

    /* 14. Patch group 999 -> 404 */
    admin_dispatch(&adm,
                   "/admin/v1/groups/999",
                   "PATCH",
                   NULL,
                   "admin-secret-token",
                   req_rename,
                   strlen(req_rename),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 404, "patch non-existent group -> 404");
    free(body);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_models_pricing)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    int    status;
    char*  body;
    size_t len;

    /* Create model with pricing */
    const char* req_cm =
        "{\"name\":\"gpt-4o\",\"provider\":\"openai\",\"endpoint\":\"http://127.0.0.1:8080\","
        "\"pricing\":{\"in_mtok\":2.5,\"out_mtok\":10.0,\"cached_mtok_discount\":0.1}}";
    admin_dispatch(&adm,
                   "/admin/v1/models",
                   "POST",
                   NULL,
                   "admin-secret-token",
                   req_cm,
                   strlen(req_cm),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 201, "create model with pricing -> 201");
    free(body);

    /* List models and verify pricing is an object */
    admin_dispatch(
        &adm, "/admin/v1/models", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(status == 200, "list models -> 200");
    json_error_t jerr;
    json_t*      j = json_loads(body, 0, &jerr);
    json_t*      marr = json_object_get(j, "models");
    TEST_ASSERT(json_is_array(marr) && json_array_size(marr) == 1, "1 model");
    json_t* m0 = json_array_get(marr, 0);
    json_t* pricing = json_object_get(m0, "pricing");
    TEST_ASSERT(json_is_object(pricing), "pricing is object");
    TEST_ASSERT(fabs(json_number_value(json_object_get(pricing, "in_mtok")) - 2.5) < 1e-6,
                "in_mtok == 2.5");
    TEST_ASSERT(fabs(json_number_value(json_object_get(pricing, "out_mtok")) - 10.0) < 1e-6,
                "out_mtok == 10.0");
    TEST_ASSERT(fabs(json_number_value(json_object_get(pricing, "cached_mtok_discount")) - 0.1) <
                    1e-6,
                "cached discount == 0.1");
    json_decref(j);
    free(body);

    /* Patch pricing */
    const char* req_pm = "{\"pricing\":{\"in_mtok\":3.0,\"out_mtok\":12.0}}";
    admin_dispatch(&adm,
                   "/admin/v1/models/gpt-4o",
                   "PATCH",
                   NULL,
                   "admin-secret-token",
                   req_pm,
                   strlen(req_pm),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "patch pricing -> 200");
    free(body);

    /* Invalid pricing (not object) -> 400 */
    const char* req_bad = "{\"pricing\":\"invalid\"}";
    admin_dispatch(&adm,
                   "/admin/v1/models/gpt-4o",
                   "PATCH",
                   NULL,
                   "admin-secret-token",
                   req_bad,
                   strlen(req_bad),
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 400, "invalid pricing -> 400");
    free(body);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_cost_from_rows_pure)
{
    /* Model 1: gpt-4o with pricing in_mtok=2.5, out_mtok=10.0, cached_mtok_discount=0.1 */
    /* Model 2: claude-3 with pricing in_mtok=3.0, out_mtok=15.0 (no cached discount -> defaults to 1.0) */
    /* Model 3: unpriced with pricing = "{}" */
    model_rec_t models[3];
    memset(models, 0, sizeof models);
    strcpy(models[0].name, "gpt-4o");
    strcpy(models[0].pricing_json,
           "{\"in_mtok\":2.5,\"out_mtok\":10.0,\"cached_mtok_discount\":0.1}");
    strcpy(models[1].name, "claude-3");
    strcpy(models[1].pricing_json, "{\"in_mtok\":3.0,\"out_mtok\":15.0}");
    strcpy(models[2].name, "unpriced");
    strcpy(models[2].pricing_json, "{}");

    group_rec_t groups[2];
    memset(groups, 0, sizeof groups);
    groups[0].id = 1;
    strcpy(groups[0].name, "engineering");
    groups[1].id = 2;
    strcpy(groups[1].name, "marketing");

    cost_row_t rows[4];
    memset(rows, 0, sizeof rows);
    /* Row 0: day 1, group 1, gpt-4o, prompt=1,000,000, completion=100,000, cached=200,000, requests=10
     * Unhit = 800,000 * 2.5 = 2,000,000
     * Cached = 200,000 * 2.5 * 0.1 = 50,000
     * Completion = 100,000 * 10.0 = 1,000,000
     * Total = 3,050,000 / 1e6 = 3.05 USD = 305 cents
     */
    rows[0].bucket_day = 1700000000;
    rows[0].group_id = 1;
    strcpy(rows[0].model, "gpt-4o");
    rows[0].prompt = 1000000;
    rows[0].completion = 100000;
    rows[0].cached = 200000;
    rows[0].requests = 10;

    /* Row 1: day 2, group 1, gpt-4o, prompt=1,000,000, completion=100,000, cached=200,000, requests=10
     * 305 cents
     */
    rows[1].bucket_day = 1700086400;
    rows[1].group_id = 1;
    strcpy(rows[1].model, "gpt-4o");
    rows[1].prompt = 1000000;
    rows[1].completion = 100000;
    rows[1].cached = 200000;
    rows[1].requests = 10;

    /* Row 2: day 1, group 0 (ungrouped), claude-3, prompt=500,000, completion=50,000, cached=0, reqs=5
     * 500,000 * 3.0 + 50,000 * 15.0 = 1,500,000 + 750,000 = 2,250,000 / 1e6 = 2.25 USD = 225 cents
     */
    rows[2].bucket_day = 1700000000;
    rows[2].group_id = 0;
    strcpy(rows[2].model, "claude-3");
    rows[2].prompt = 500000;
    rows[2].completion = 50000;
    rows[2].cached = 0;
    rows[2].requests = 5;

    /* Row 3: day 1, group 2, unpriced model -> cost_cents should be omitted */
    rows[3].bucket_day = 1700000000;
    rows[3].group_id = 2;
    strcpy(rows[3].model, "unpriced");
    rows[3].prompt = 100000;
    rows[3].completion = 10000;
    rows[3].cached = 0;
    rows[3].requests = 2;

    /* 1. Test by=day (by_model = 0) */
    char* json_day = cost_from_rows(rows, 4, models, 3, groups, 2, -1, 0, 0);
    TEST_ASSERT(json_day != NULL, "json_day not null");
    json_error_t jerr;
    json_t*      jd = json_loads(json_day, 0, &jerr);
    json_t*      r_arr = json_object_get(jd, "rows");
    TEST_ASSERT(json_is_array(r_arr) && json_array_size(r_arr) == 4, "4 rows in day mode");
    json_t* item0 = json_array_get(r_arr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(item0, "cost_cents")) == 305, "row0 305 cents");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item0, "group_name")), "engineering") == 0,
                "engineering");

    json_t* item2 = json_array_get(r_arr, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item2, "group_name")), "(ungrouped)") == 0,
                "(ungrouped)");
    TEST_ASSERT(json_integer_value(json_object_get(item2, "cost_cents")) == 225, "row2 225 cents");

    json_t* item3 = json_array_get(r_arr, 3);
    TEST_ASSERT(json_object_get(item3, "cost_cents") == NULL, "unpriced model has no cost_cents");
    TEST_ASSERT(json_integer_value(json_object_get(item3, "prompt_tokens")) == 100000,
                "prompt tokens preserved");
    json_decref(jd);
    free(json_day);

    /* 2. Test by=model (by_model = 1): should aggregate Row 0 and Row 1 into 1 entry with 610 cents! */
    char* json_model = cost_from_rows(rows, 4, models, 3, groups, 2, -1, 1, 0);
    TEST_ASSERT(json_model != NULL, "json_model not null");
    json_t* jm = json_loads(json_model, 0, &jerr);
    json_t* m_arr = json_object_get(jm, "rows");
    TEST_ASSERT(json_is_array(m_arr) && json_array_size(m_arr) == 3, "3 aggregated model rows");
    json_t* agg0 = json_array_get(m_arr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(agg0, "prompt_tokens")) == 2000000,
                "agg prompt 2M");
    TEST_ASSERT(json_integer_value(json_object_get(agg0, "cost_cents")) == 610,
                "agg cost 610 cents");
    json_decref(jm);
    free(json_model);

    /* 3. Test group filter (group_filter = 1): should only return rows for group 1 */
    char* json_filtered = cost_from_rows(rows, 4, models, 3, groups, 2, 1, 0, 0);
    TEST_ASSERT(json_filtered != NULL, "json_filtered not null");
    json_t* jf = json_loads(json_filtered, 0, &jerr);
    json_t* f_arr = json_object_get(jf, "rows");
    TEST_ASSERT(json_is_array(f_arr) && json_array_size(f_arr) == 2, "2 rows for group 1");
    json_decref(jf);
    free(json_filtered);
}

TEST_CASE(test_admin_cost_endpoint)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];
    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    /* Populate 1 group, 1 model, 1 cost row */
    long gid = 0;
    ops.create_group(ops.ctx, "rnd", &gid);
    model_rec_t m;
    memset(&m, 0, sizeof m);
    strcpy(m.name, "gpt-4o");
    strcpy(m.pricing_json, "{\"in_mtok\":2.0,\"out_mtok\":8.0}");
    ops.create_model(ops.ctx, &m);

    time_t now = time(NULL);
    db.cost_rows[0].bucket_day = now;
    db.cost_rows[0].group_id = gid;
    strcpy(db.cost_rows[0].model, "gpt-4o");
    db.cost_rows[0].prompt = 1000000;
    db.cost_rows[0].completion = 100000;
    db.cost_rows[0].cached = 0;
    db.cost_rows[0].requests = 5;
    db.n_cost_rows = 1;

    int    status;
    char*  body;
    size_t len;

    /* GET /admin/v1/cost default */
    admin_dispatch(
        &adm, "/admin/v1/cost", "GET", NULL, "admin-secret-token", NULL, 0, &status, &body, &len);
    TEST_ASSERT(status == 200, "cost query -> 200");
    json_error_t jerr;
    json_t*      j = json_loads(body, 0, &jerr);
    json_t*      rarr = json_object_get(j, "rows");
    TEST_ASSERT(json_is_array(rarr) && json_array_size(rarr) == 1, "1 row");
    json_decref(j);
    free(body);

    /* GET /admin/v1/cost?by=model */
    admin_dispatch(&adm,
                   "/admin/v1/cost?by=model",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "cost query by=model -> 200");
    free(body);

    /* GET /admin/v1/cost with from > to -> 400 */
    admin_dispatch(&adm,
                   "/admin/v1/cost?from=2026-09-01&to=2026-08-01",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 400, "from > to -> 400");
    free(body);

    /* GET /admin/v1/cost range > 365 days -> 400 */
    admin_dispatch(&adm,
                   "/admin/v1/cost?from=2024-01-01&to=2026-01-01",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 400, "range > 365 days -> 400");
    free(body);

    teardown_admin(ps, &core, &db);
}

TEST_CASE(test_admin_pagination)
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

    /* 1. Groups Pagination */
    long g1 = 0, g2 = 0, g3 = 0;
    ops.create_group(ops.ctx, "grp-1", &g1);
    ops.create_group(ops.ctx, "grp-2", &g2);
    ops.create_group(ops.ctx, "grp-3", &g3);

    /* Page 1, limit 2 */
    admin_dispatch(&adm,
                   "/admin/v1/groups?page=1&limit=2",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "groups page 1 -> 200");
    json_error_t jerr;
    json_t*      j = json_loads(body, 0, &jerr);
    TEST_ASSERT(j != NULL, "parsed groups page 1");
    TEST_ASSERT(json_integer_value(json_object_get(j, "total")) == 3, "groups total 3");
    TEST_ASSERT(json_integer_value(json_object_get(j, "page")) == 1, "groups page 1");
    TEST_ASSERT(json_integer_value(json_object_get(j, "limit")) == 2, "groups limit 2");
    json_t* arr = json_object_get(j, "groups");
    TEST_ASSERT(json_is_array(arr) && json_array_size(arr) == 2, "groups page 1 size 2");
    json_decref(j);
    free(body);

    /* Page 2, limit 2 */
    admin_dispatch(&adm,
                   "/admin/v1/groups?page=2&limit=2",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "groups page 2 -> 200");
    j = json_loads(body, 0, &jerr);
    arr = json_object_get(j, "groups");
    TEST_ASSERT(json_is_array(arr) && json_array_size(arr) == 1, "groups page 2 size 1");
    json_decref(j);
    free(body);

    /* Page 3, limit 2 (out of bounds) */
    admin_dispatch(&adm,
                   "/admin/v1/groups?page=3&limit=2",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "groups page 3 -> 200");
    j = json_loads(body, 0, &jerr);
    arr = json_object_get(j, "groups");
    TEST_ASSERT(json_is_array(arr) && json_array_size(arr) == 0, "groups page 3 size 0");
    json_decref(j);
    free(body);

    /* 2. Keys Pagination */
    key_rec_t k1 = {0}, k2 = {0}, k3 = {0};
    strcpy(k1.name, "k1");
    strcpy(k1.key_hash, "h1");
    strcpy(k2.name, "k2");
    strcpy(k2.key_hash, "h2");
    strcpy(k3.name, "k3");
    strcpy(k3.key_hash, "h3");
    long kid = 0;
    ops.create_key(ops.ctx, &k1, &kid);
    ops.create_key(ops.ctx, &k2, &kid);
    ops.create_key(ops.ctx, &k3, &kid);

    admin_dispatch(&adm,
                   "/admin/v1/keys?page=1&limit=2",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "keys page 1 -> 200");
    j = json_loads(body, 0, &jerr);
    TEST_ASSERT(json_integer_value(json_object_get(j, "total")) == 3, "keys total 3");
    arr = json_object_get(j, "keys");
    TEST_ASSERT(json_is_array(arr) && json_array_size(arr) == 2, "keys page 1 size 2");
    json_decref(j);
    free(body);

    /* 3. Models Pagination */
    model_rec_t m1 = {0}, m2 = {0};
    strcpy(m1.name, "m1");
    strcpy(m1.provider, "openai");
    strcpy(m2.name, "m2");
    strcpy(m2.provider, "openai");
    ops.create_model(ops.ctx, &m1);
    ops.create_model(ops.ctx, &m2);

    admin_dispatch(&adm,
                   "/admin/v1/models?page=1&limit=1",
                   "GET",
                   NULL,
                   "admin-secret-token",
                   NULL,
                   0,
                   &status,
                   &body,
                   &len);
    TEST_ASSERT(status == 200, "models page 1 -> 200");
    j = json_loads(body, 0, &jerr);
    TEST_ASSERT(json_integer_value(json_object_get(j, "total")) == 2, "models total 2");
    arr = json_object_get(j, "models");
    TEST_ASSERT(json_is_array(arr) && json_array_size(arr) == 1, "models page 1 size 1");
    json_decref(j);
    free(body);

    /* 4. Cost from rows paginated pure check */
    cost_row_t crows[4];
    memset(crows, 0, sizeof crows);
    model_rec_t cmodels[1];
    memset(cmodels, 0, sizeof cmodels);
    strcpy(cmodels[0].name, "m1");
    group_rec_t cgroups[1];
    memset(cgroups, 0, sizeof cgroups);
    cgroups[0].id = 1;
    strcpy(cgroups[0].name, "g1");

    char* cost_paged = cost_from_rows_paginated(crows, 4, cmodels, 1, cgroups, 1, -1, 0, 0, 1, 2);
    TEST_ASSERT(cost_paged != NULL, "cost_paged not null");
    j = json_loads(cost_paged, 0, &jerr);
    TEST_ASSERT(json_integer_value(json_object_get(j, "total")) == 4, "cost total 4");
    TEST_ASSERT(json_integer_value(json_object_get(j, "page")) == 1, "cost page 1");
    TEST_ASSERT(json_integer_value(json_object_get(j, "limit")) == 2, "cost limit 2");
    arr = json_object_get(j, "rows");
    TEST_ASSERT(json_is_array(arr) && json_array_size(arr) == 2, "cost page 1 size 2");
    json_decref(j);
    free(cost_paged);

    teardown_admin(ps, &core, &db);
}

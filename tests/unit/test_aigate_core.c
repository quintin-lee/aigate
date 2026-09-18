/** @file test_aigate_core.c
 *  @brief aigate_core pipeline tests: auth → allowlist → rate → route →
 *  provider build → upstream (mock) → usage drain, with fake PG ops.
 *
 *  Scenarios:
 *   1. happy path: 200, upstream usage passed through
 *   2. unknown key → 401
 *   3. key allowlist excludes model → 403
 *   4. unknown model → 404
 *   5. QPS exceeded (3rd of 3 at qps=2) → 429 + Retry-After
 *   6. upstream 500 → 502 + X-Upstream-Provider header
 *  Plus provider_openai_build URL/merge behavior (azure api-version,
 *  default_params merge) and drain semantics (key×model accumulation).
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "mock_upstream.h"
#include "pg_store.h"
#include "provider_openai.h"
#include "sha256.h"
#include "usage_meter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- fake ops layer */

#define FKEYS 4
#define FMODELS 4
#define FUSAGE 64

struct fkey {
    int       in_use;
    key_rec_t k;
};

struct fdb {
    struct fkey keys[FKEYS];
    model_rec_t models[FMODELS];
    int         n_models;
    usage_row_t usage[FUSAGE];
    int         n_usage;
    int         flush_calls;
};

static int
fdeep_allowlist(key_rec_t* dst, const key_rec_t* src)
{
    if (src->n_allowed <= 0 || src->allowed_models == NULL) {
        return 0;
    }
    char** vec = malloc(sizeof(char*) * (size_t)src->n_allowed);
    if (vec == NULL) {
        return -1;
    }
    for (int i = 0; i < src->n_allowed; i++) {
        vec[i] = strdup(src->allowed_models[i]);
        if (vec[i] == NULL) {
            for (int j = 0; j < i; j++) {
                free(vec[j]);
            }
            free(vec);
            return -1;
        }
    }
    dst->allowed_models = vec;
    dst->n_allowed = src->n_allowed;
    return 0;
}

static int
fget_key(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct fdb* db = ctx;
    for (int i = 0; i < FKEYS; i++) {
        struct fkey* fk = &db->keys[i];
        if (!fk->in_use) {
            continue;
        }
        if (strcmp(fk->k.key_hash, key_hash) == 0) {
            *out = fk->k; /* value copy; allowlist re-allocated below */
            out->allowed_models = NULL;
            out->n_allowed = 0;
            if (fdeep_allowlist(out, &fk->k) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static int
fget_model(void* ctx, const char* name, model_rec_t* out)
{
    struct fdb* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            *out = db->models[i];
            return 0;
        }
    }
    return -1;
}

static int
f_flush_rows(void* ctx, const usage_row_t* rows, int n)
{
    struct fdb* db = ctx;
    db->flush_calls++;
    for (int i = 0; i < n && db->n_usage < FUSAGE; i++) {
        db->usage[db->n_usage++] = rows[i];
    }
    return 0;
}

static void
fbuild_ops(struct fdb* db, pg_ops_t* ops)
{
    memset(ops, 0, sizeof *ops);
    ops->ctx = db;
    ops->get_key_by_hash = fget_key;
    ops->get_model = fget_model;
    ops->flush_usage = f_flush_rows;
}

/** @brief Insert a key (bearer is SHA-256-hashed into key_hash). */
static void
fkey_add(struct fdb* db,
         int         slot,
         long        key_id,
         const char* bearer,
         int         qps,
         long        quota,
         const char* allowlist)
{
    struct fkey* fk = &db->keys[slot];
    memset(fk, 0, sizeof *fk);
    fk->in_use = 1;
    fk->k.key_id = key_id;
    if (sha256_hex(bearer, strlen(bearer), fk->k.key_hash) != 0) {
        memset(fk->k.key_hash, 0, sizeof fk->k.key_hash);
    }
    fk->k.rate_qps = qps;
    fk->k.daily_token_quota = quota;
    if (allowlist != NULL && allowlist[0] != '\0') {
        fk->k.allowed_models = malloc(sizeof(char*) * 2);
        fk->k.allowed_models[0] = strdup(allowlist);
        fk->k.allowed_models[1] = NULL;
        fk->k.n_allowed = 1;
    }
}

/* ------------------------------------------------- response ctx capture */

struct cap {
    char   hdrs[2048];
    char   body[16384];
    size_t blen;
    int    status;
};

static int
cap_set_header(void* impl, const char* name, const char* value)
{
    struct cap* c = impl;
    snprintf(c->hdrs + strlen(c->hdrs), sizeof c->hdrs - strlen(c->hdrs), "%s: %s\n", name, value);
    return 0;
}

static int
cap_write(void* impl, const void* buf, size_t len, bool fin)
{
    struct cap* c = impl;
    (void)fin;
    if (c->blen + len < sizeof c->body) {
        memcpy(c->body + c->blen, buf, len);
        c->blen += len;
    }
    c->body[c->blen] = '\0';
    return 0;
}

static aigate_response_ctx
cap_rc(struct cap* c)
{
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof rc);
    rc.impl = c;
    rc.set_header = cap_set_header;
    rc.write = cap_write;
    return rc;
}

static int
cap_has_header(const struct cap* c, const char* needle)
{
    return strstr(c->hdrs, needle) != NULL;
}

/** @brief Run one chat request through the pipeline. */
static const char*
run(aigate_core* ac, const char* bearer, const char* model, struct cap* out)
{
    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST";
    rq.path = "/v1/chat/completions";
    rq.bearer = bearer;
    rq.client_ip = "127.0.0.1";
    static char full_body[512];
    snprintf(full_body,
             sizeof full_body,
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
             model);
    rq.body = full_body;
    rq.body_len = strlen(full_body);

    aigate_response_ctx rcc = cap_rc(out);
    aigate_handle_request(ac, &rq, &rcc);
    out->status = rcc.status;
    return out->body;
}

/** @brief Find a drained row by key_id + model; -1 when absent. */
static int
find_row(const usage_row_t* rows, int n, long key_id, const char* model)
{
    for (int i = 0; i < n; i++) {
        if (rows[i].key_id == key_id && strcmp(rows[i].model_name, model) == 0) {
            return i;
        }
    }
    return -1;
}

TEST_CASE(test_core_pipeline)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");

    struct fdb db;
    memset(&db, 0, sizeof db);
    /* key 1: happy path — all models, unlimited */
    fkey_add(&db, 0, 1, "good-key", 0, 0, NULL);
    /* key 2: qps=2 (rate limit scenario) */
    fkey_add(&db, 1, 2, "rate-key", 2, 0, NULL);
    /* key 3: allowlist excludes claude-x */
    fkey_add(&db, 2, 3, "allow-key", 0, 0, "gpt-4o");

    snprintf(db.models[0].name, sizeof db.models[0].name, "%s", "gpt-4o");
    snprintf(db.models[0].provider, sizeof db.models[0].provider, "%s", "openai");
    snprintf(db.models[0].endpoint, sizeof db.models[0].endpoint, "%s", mock_upstream_base(mu));
    db.models[0].enabled = 1;
    db.n_models = 1;

    pg_ops_t ops;
    fbuild_ops(&db, &ops);
    pg_store_t* ps = pg_store_open(NULL, &ops);
    TEST_ASSERT(ps != NULL, "fake store");

    aigate_core ac;
    TEST_ASSERT(aigate_core_init(&ac, ps, NULL, 5000, 0) == 0, "core init");

    /* 1. happy path: 200 + upstream usage passed through */
    struct cap c1;
    memset(&c1, 0, sizeof c1);
    const char* b1 = run(&ac, "good-key", "gpt-4o", &c1);
    TEST_ASSERT(c1.status == 200, "status 200, got %d", c1.status);
    TEST_ASSERT(
        strstr(b1, "\"prompt_tokens\":7") != NULL, "upstream usage passed through: %.80s", b1);
    TEST_ASSERT(mock_upstream_request_count(mu) == 1, "one upstream request");
    TEST_ASSERT(strstr(mock_upstream_last_path(mu), "/chat/completions") != NULL,
                "upstream path /chat/completions");

    /* 2. unknown key → 401 */
    struct cap c2;
    memset(&c2, 0, sizeof c2);
    run(&ac, "nope", "gpt-4o", &c2);
    TEST_ASSERT(c2.status == 401, "401 for unknown key, got %d", c2.status);
    TEST_ASSERT(strstr(c2.body, "invalid api key") != NULL, "401 body");

    /* 3. allowlist excludes claude-x → 403 */
    struct cap c3;
    memset(&c3, 0, sizeof c3);
    run(&ac, "allow-key", "claude-x", &c3);
    TEST_ASSERT(c3.status == 403, "403 for excluded model, got %d", c3.status);
    TEST_ASSERT(strstr(c3.body, "model not allowed") != NULL, "403 body");

    /* 4. unknown model → 404 */
    struct cap c4;
    memset(&c4, 0, sizeof c4);
    run(&ac, "good-key", "nope-model", &c4);
    TEST_ASSERT(c4.status == 404, "404 for unknown model, got %d", c4.status);
    TEST_ASSERT(strstr(c4.body, "model not found") != NULL, "404 body");

    /* 5. rate limit: qps=2 → first two admitted, third denied 429 */
    struct cap r1, r2, r3;
    memset(&r1, 0, sizeof r1);
    memset(&r2, 0, sizeof r2);
    memset(&r3, 0, sizeof r3);
    run(&ac, "rate-key", "gpt-4o", &r1);
    TEST_ASSERT(r1.status == 200, "first admitted, got %d", r1.status);
    run(&ac, "rate-key", "gpt-4o", &r2);
    TEST_ASSERT(r2.status == 200, "second admitted, got %d", r2.status);
    run(&ac, "rate-key", "gpt-4o", &r3);
    TEST_ASSERT(r3.status == 429, "third denied, got %d", r3.status);
    TEST_ASSERT(cap_has_header(&r3, "Retry-After:"), "Retry-After header");
    TEST_ASSERT(strstr(r3.body, "rate limit exceeded") != NULL, "429 body");

    /* 6. upstream 500 → 502 + provider header */
    mock_upstream_fail_all(mu, 1);
    struct cap c6;
    memset(&c6, 0, sizeof c6);
    run(&ac, "good-key", "gpt-4o", &c6);
    TEST_ASSERT(c6.status == 502, "502 on upstream 500, got %d", c6.status);
    TEST_ASSERT(cap_has_header(&c6, "X-Upstream-Provider: openai"), "X-Upstream-Provider header");
    TEST_ASSERT(strstr(c6.body, "upstream request failed") != NULL, "502 body");

    /* drain: accumulators merge per key×model×day */
    usage_row_t rows[FUSAGE];
    int         n = 0;
    TEST_ASSERT(um_drain(ac.um, rows, FUSAGE, &n) == 0, "drain ok");
    TEST_ASSERT(db.flush_calls >= 1, "flush called, got %d", db.flush_calls);

    /* good-key: 1 success (7/11 tokens) + 1 upstream error → same row */
    int ig = find_row(rows, n, 1, "gpt-4o");
    TEST_ASSERT(ig >= 0, "good-key gpt-4o row");
    if (ig >= 0) {
        TEST_ASSERT(rows[ig].requests == 2, "2 requests, got %ld", rows[ig].requests);
        TEST_ASSERT(rows[ig].errors == 1, "1 error, got %ld", rows[ig].errors);
        TEST_ASSERT(rows[ig].prompt_tokens == 7, "prompt 7, got %ld", rows[ig].prompt_tokens);
        TEST_ASSERT(
            rows[ig].completion_tokens == 11, "completion 11, got %ld", rows[ig].completion_tokens);
    }
    /* rate-key: 2 successes merged (14/22 tokens) */
    int ir = find_row(rows, n, 2, "gpt-4o");
    TEST_ASSERT(ir >= 0, "rate-key gpt-4o row");
    if (ir >= 0) {
        TEST_ASSERT(rows[ir].requests == 2, "2 requests, got %ld", rows[ir].requests);
        TEST_ASSERT(rows[ir].prompt_tokens == 14, "prompt 14, got %ld", rows[ir].prompt_tokens);
        TEST_ASSERT(
            rows[ir].completion_tokens == 22, "completion 22, got %ld", rows[ir].completion_tokens);
        TEST_ASSERT(rows[ir].errors == 0, "no errors, got %ld", rows[ir].errors);
    }

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

TEST_CASE(test_provider_azure_build)
{
    /* azure: api-version (default_params) is lifted into the URL query */
    model_rec_t m;
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "%s", "gpt-4o");
    snprintf(m.provider, sizeof m.provider, "%s", "azure");
    snprintf(m.endpoint, sizeof m.endpoint, "%s", "https://res.openai.azure.com");
    snprintf(m.default_params_json,
             sizeof m.default_params_json,
             "%s",
             "{\"api-version\":\"2024-06-01\",\"temperature\":0.5}");

    char   url[1024];
    char*  merged = NULL;
    size_t mlen = 0;
    int    rc = provider_openai_build(
        &m, "/chat/completions", "{\"model\":\"gpt-4o\"}", url, sizeof url, &merged, &mlen);
    TEST_ASSERT(rc == 0, "build ok, rc=%d", rc);
    TEST_ASSERT(strstr(url,
                       "https://res.openai.azure.com/chat/completions"
                       "?api-version=2024-06-01") != NULL,
                "azure URL with api-version: %s",
                url);
    TEST_ASSERT(merged != NULL, "merged body");
    json_t* j = json_loads(merged, 0, NULL);
    TEST_ASSERT(j != NULL, "merged is JSON");
    TEST_ASSERT(json_object_get(j, "temperature") != NULL, "default temperature merged in");
    TEST_ASSERT(json_object_get(j, "model") != NULL, "request model kept");
    TEST_ASSERT(json_object_get(j, "api-version") == NULL, "api-version stripped from body");
    json_decref(j);
    free(merged);
}

TEST_CASE(test_provider_default_params_merge)
{
    /* request fields win over defaults; defaults fill gaps */
    model_rec_t m;
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "%s", "gpt-4o");
    snprintf(m.provider, sizeof m.provider, "%s", "openai");
    snprintf(m.endpoint, sizeof m.endpoint, "%s", "http://up");
    snprintf(m.default_params_json,
             sizeof m.default_params_json,
             "%s",
             "{\"temperature\":0.2,\"top_p\":0.9}");

    char   url[256];
    char*  merged = NULL;
    size_t mlen = 0;
    int    rc = provider_openai_build(
        &m, "/chat/completions", "{\"temperature\":1.0}", url, sizeof url, &merged, &mlen);
    TEST_ASSERT(rc == 0, "build ok");
    json_t* j = json_loads(merged, 0, NULL);
    TEST_ASSERT(j != NULL, "merged JSON");
    TEST_ASSERT(json_is_number(json_object_get(j, "temperature")) &&
                    json_real_value(json_object_get(j, "temperature")) == 1.0,
                "request temperature wins over default");
    TEST_ASSERT(json_is_number(json_object_get(j, "top_p")) &&
                    json_real_value(json_object_get(j, "top_p")) == 0.9,
                "default top_p filled in");
    json_decref(j);
    free(merged);
}

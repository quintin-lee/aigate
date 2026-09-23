/** @file test_stream_pipeline.c
 *  @brief Unit tests for the streaming pipeline in aigate_core.c (Plan 2, Task 2).
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "mock_upstream.h"
#include "pg_store.h"
#include "sha256.h"
#include "usage_meter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
fget_key(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct fdb* db = ctx;
    for (int i = 0; i < FKEYS; i++) {
        struct fkey* fk = &db->keys[i];
        if (!fk->in_use) {
            continue;
        }
        if (strcmp(fk->k.key_hash, key_hash) == 0) {
            *out = fk->k;
            out->allowed_models = NULL;
            out->n_allowed = 0;
            return 0;
        }
    }
    return 1;
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

static int
f_req_stub(void* ctx, ...)
{
    (void)ctx;
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
    ops->flush_usage_requests =
        (int (*)(void*, const usage_request_row_t*, int))f_req_stub;
    ops->query_usage_requests =
        (int (*)(void*, long, time_t, usage_request_row_t*, int, int*))f_req_stub;
}

static void
fkey_add(struct fdb* db, int slot, long key_id, const char* bearer, int qps, long quota)
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
    fk->k.allowed_models = NULL;
    fk->k.n_allowed = 0;
}

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

TEST_CASE(test_stream_pipeline_normal)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");

    struct fdb db;
    memset(&db, 0, sizeof db);
    fkey_add(&db, 0, 1, "stream-key", 0, 1000);

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

    struct cap c;
    memset(&c, 0, sizeof c);

    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST";
    rq.path = "/v1/chat/completions";
    rq.bearer = "stream-key";
    rq.client_ip = "127.0.0.1";
    const char* req_body = "{\"model\":\"gpt-4o\",\"stream\":true,\"messages\":[{\"role\":\"user\","
                           "\"content\":\"hi\"}]}";
    rq.body = req_body;
    rq.body_len = strlen(req_body);

    aigate_response_ctx rc = cap_rc(&c);
    int                 rv = aigate_handle_request(&ac, &rq, &rc);
    TEST_ASSERT(rv == 0, "handle_request returned 0");
    TEST_ASSERT(rc.status == 200, "status == 200, got %d", rc.status);
    TEST_ASSERT(cap_has_header(&c, "Content-Type: text/event-stream; charset=utf-8"),
                "has text/event-stream header");
    TEST_ASSERT(cap_has_header(&c, "Cache-Control: no-cache"), "has no-cache header");
    TEST_ASSERT(cap_has_header(&c, "Connection: keep-alive"), "has keep-alive header");

    TEST_ASSERT(strstr(c.body, "hello") != NULL, "body contains hello");
    TEST_ASSERT(strstr(c.body, "world") != NULL, "body contains world");
    TEST_ASSERT(strstr(c.body, "[DONE]") != NULL, "body contains [DONE]");

    /* Verify usage extraction */
    usage_row_t rows[FUSAGE];
    int         n = 0;
    TEST_ASSERT(um_drain(ac.um, rows, FUSAGE, &n) == 0, "drain ok");
    int ir = find_row(rows, n, 1, "gpt-4o");
    TEST_ASSERT(ir >= 0, "stream-key gpt-4o row found");
    if (ir >= 0) {
        TEST_ASSERT(rows[ir].requests == 1, "1 request, got %ld", rows[ir].requests);
        TEST_ASSERT(rows[ir].errors == 0, "0 errors, got %ld", rows[ir].errors);
        TEST_ASSERT(rows[ir].prompt_tokens == 5, "prompt 5, got %ld", rows[ir].prompt_tokens);
        TEST_ASSERT(
            rows[ir].completion_tokens == 7, "completion 7, got %ld", rows[ir].completion_tokens);
    }

    /* Verify daily token quota reservation: 1000 - (5+7) = 988 */
    long rem = rl_remaining_daily(ac.rl, 1, 1000);
    TEST_ASSERT(rem == 988, "remaining daily tokens == 988, got %ld", rem);

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

TEST_CASE(test_stream_pipeline_early_error)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    mock_upstream_fail_all(mu, 1);

    struct fdb db;
    memset(&db, 0, sizeof db);
    fkey_add(&db, 0, 1, "stream-key", 0, 1000);

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

    struct cap c;
    memset(&c, 0, sizeof c);

    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST";
    rq.path = "/v1/chat/completions";
    rq.bearer = "stream-key";
    rq.client_ip = "127.0.0.1";
    const char* req_body = "{\"model\":\"gpt-4o\",\"stream\":true,\"messages\":[{\"role\":\"user\","
                           "\"content\":\"hi\"}]}";
    rq.body = req_body;
    rq.body_len = strlen(req_body);

    aigate_response_ctx rc = cap_rc(&c);
    int                 rv = aigate_handle_request(&ac, &rq, &rc);
    TEST_ASSERT(rv == 0, "handle_request returned 0");
    TEST_ASSERT(rc.status == 502, "status == 502, got %d", rc.status);
    TEST_ASSERT(cap_has_header(&c, "Content-Type: application/json"),
                "has application/json header");
    TEST_ASSERT(cap_has_header(&c, "X-Upstream-Provider: openai"), "has provider header");
    TEST_ASSERT(strstr(c.body, "upstream request failed") != NULL, "body has error message");

    usage_row_t rows[FUSAGE];
    int         n = 0;
    TEST_ASSERT(um_drain(ac.um, rows, FUSAGE, &n) == 0, "drain ok");
    int ir = find_row(rows, n, 1, "gpt-4o");
    TEST_ASSERT(ir >= 0, "row found");
    if (ir >= 0) {
        TEST_ASSERT(rows[ir].requests == 1, "1 request");
        TEST_ASSERT(rows[ir].errors == 1, "1 error");
        TEST_ASSERT(rows[ir].prompt_tokens == 0, "0 prompt tokens");
    }

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

TEST_CASE(test_stream_pipeline_4xx_passthrough)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    /* Upstream answers 400 with its own JSON error body, not SSE */
    mock_upstream_fail_all(mu, 400);

    struct fdb db;
    memset(&db, 0, sizeof db);
    fkey_add(&db, 0, 1, "stream-key", 0, 1000);

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

    struct cap c;
    memset(&c, 0, sizeof c);

    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST";
    rq.path = "/v1/chat/completions";
    rq.bearer = "stream-key";
    rq.client_ip = "127.0.0.1";
    const char* req_body = "{\"model\":\"gpt-4o\",\"stream\":true,\"messages\":[{\"role\":\"user\","
                           "\"content\":\"hi\"}]}";
    rq.body = req_body;
    rq.body_len = strlen(req_body);

    aigate_response_ctx rc = cap_rc(&c);
    int                 rv = aigate_handle_request(&ac, &rq, &rc);
    TEST_ASSERT(rv == 0, "handle_request returned 0");
    /* Before the fix this path fell through to the generic 502 */
    TEST_ASSERT(rc.status == 400, "status == 400, got %d", rc.status);
    TEST_ASSERT(strstr(c.body, "boom") != NULL, "upstream error body passed through");
    TEST_ASSERT(strstr(c.body, "upstream request failed") == NULL,
                "no generic gateway error");

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

TEST_CASE(test_stream_pipeline_silence_timeout)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");

    struct fdb db;
    memset(&db, 0, sizeof db);
    fkey_add(&db, 0, 1, "stream-key", 0, 1000);

    snprintf(db.models[0].name, sizeof db.models[0].name, "%s", "gpt-4o");
    snprintf(db.models[0].provider, sizeof db.models[0].provider, "%s", "openai");
    snprintf(db.models[0].endpoint, sizeof db.models[0].endpoint, "%s", mock_upstream_base(mu));
    db.models[0].enabled = 1;
    db.n_models = 1;

    pg_ops_t ops;
    fbuild_ops(&db, &ops);
    pg_store_t* ps = pg_store_open(NULL, &ops);
    TEST_ASSERT(ps != NULL, "fake store");

    /* silence timeout = 200ms */
    aigate_core ac;
    TEST_ASSERT(aigate_core_init(&ac, ps, NULL, 200, 0) == 0, "core init");

    struct cap c;
    memset(&c, 0, sizeof c);

    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST";
    rq.path = "/v1/chat/completions";
    rq.bearer = "stream-key";
    rq.client_ip = "127.0.0.1";
    /* stream-slow triggers 1.2s sleep between chunks in mock_upstream */
    const char* req_body = "{\"model\":\"gpt-4o\",\"stream\":true,\"messages\":[{\"role\":\"user\","
                           "\"content\":\"stream-slow\"}]}";
    rq.body = req_body;
    rq.body_len = strlen(req_body);

    aigate_response_ctx rc = cap_rc(&c);
    int                 rv = aigate_handle_request(&ac, &rq, &rc);
    TEST_ASSERT(rv == 0, "handle_request returned 0");
    TEST_ASSERT(rc.status == 200, "initial status was 200, got %d", rc.status);
    TEST_ASSERT(strstr(c.body, "start") != NULL, "received chunk 1");
    TEST_ASSERT(strstr(c.body, "silence timeout") != NULL, "emitted silence timeout error event");
    TEST_ASSERT(strstr(c.body, "[DONE]") != NULL, "emitted [DONE]");

    usage_row_t rows[FUSAGE];
    int         n = 0;
    TEST_ASSERT(um_drain(ac.um, rows, FUSAGE, &n) == 0, "drain ok");
    int ir = find_row(rows, n, 1, "gpt-4o");
    TEST_ASSERT(ir >= 0, "row found");
    if (ir >= 0) {
        TEST_ASSERT(rows[ir].requests == 1, "1 request");
        TEST_ASSERT(rows[ir].errors == 1, "1 error");
    }

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

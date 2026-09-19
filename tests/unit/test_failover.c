/** @file test_failover.c
 *  @brief Unit tests for multi-upstream failover execution & circuit breaker integration.
 */
#include "aigate_core.h"
#include "mock_upstream.h"
#include "pg_store.h"
#include "run_tests.h"
#include "sha256.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct failover_resp {
    int   status;
    char  body[4096];
    int   body_len;
    char  header_provider[64];
    bool  headers_sent;
};

static int
fresp_set_header(void* impl, const char* name, const char* value)
{
    struct failover_resp* r = impl;
    if (strcmp(name, "X-Upstream-Provider") == 0) {
        snprintf(r->header_provider, sizeof(r->header_provider), "%s", value);
    }
    return 0;
}

static int
fresp_write(void* impl, const void* buf, size_t len, bool fin)
{
    (void)fin;
    struct failover_resp* r = impl;
    if ((size_t)r->body_len + len < sizeof(r->body)) {
        memcpy(r->body + r->body_len, buf, len);
        r->body_len += (int)len;
        r->body[r->body_len] = '\0';
    }
    return 0;
}

struct failover_db {
    key_rec_t   key;
    model_rec_t model;
};

static int
fo_get_key(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct failover_db* db = ctx;
    if (strcmp(db->key.key_hash, key_hash) == 0) {
        *out = db->key;
        out->allowed_models = NULL;
        out->n_allowed = 0;
        return 0;
    }
    return -1;
}

static int
fo_get_model(void* ctx, const char* name, model_rec_t* out)
{
    struct failover_db* db = ctx;
    if (strcmp(db->model.name, name) == 0) {
        *out = db->model;
        return 0;
    }
    return -1;
}

static int
fo_noop(void* ctx, ...)
{
    (void)ctx;
    return 0;
}

static pg_ops_t
build_failover_ops(struct failover_db* db)
{
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.ctx = db;
    ops.get_key_by_hash = fo_get_key;
    ops.get_model = fo_get_model;
    ops.flush_usage = (int (*)(void*, const usage_row_t*, int))fo_noop;
    ops.query_usage = (int (*)(void*, long, const char*, time_t, time_t, usage_row_t*, int, int*))fo_noop;
    return ops;
}

static void
setup_failover_env(struct failover_db* db,
                   aigate_core*        ac,
                   pg_store_t**        out_ps,
                   const char*         u1_base,
                   const char*         u2_base)
{
    memset(db, 0, sizeof *db);

    /* Setup API key "client-key" */
    char h[65];
    sha256_hex("client-key", strlen("client-key"), h);
    strcpy(db->key.key_hash, h);
    strcpy(db->key.name, "tester");
    db->key.key_id = 1;
    db->key.rate_qps = 100;
    db->key.daily_token_quota = 100000;

    /* Setup model with Target 1 (primary) and Target 2 (backup) */
    strcpy(db->model.name, "failover-chat");
    strcpy(db->model.lb_policy, "priority");
    db->model.enabled = 1;
    db->model.n_targets = 2;

    /* Target 1: Priority 0 (primary tier) */
    strcpy(db->model.targets[0].provider, "openai");
    snprintf(db->model.targets[0].endpoint, sizeof(db->model.targets[0].endpoint), "%s/v1", u1_base);
    strcpy(db->model.targets[0].upstream_key, "key1");
    db->model.targets[0].weight = 1;
    db->model.targets[0].priority = 0;

    /* Target 2: Priority 1 (backup tier) */
    strcpy(db->model.targets[1].provider, "openai");
    snprintf(db->model.targets[1].endpoint, sizeof(db->model.targets[1].endpoint), "%s/v1", u2_base);
    strcpy(db->model.targets[1].upstream_key, "key2");
    db->model.targets[1].weight = 1;
    db->model.targets[1].priority = 1;

    pg_ops_t ops = build_failover_ops(db);
    *out_ps = pg_store_open("unused", &ops);
    aigate_core_init(ac, *out_ps, NULL, 5000, 60);
}

TEST_CASE(test_failover_on_500_to_backup)
{
    mock_upstream_t* u1 = mock_upstream_start();
    mock_upstream_t* u2 = mock_upstream_start();
    TEST_ASSERT(u1 != NULL && u2 != NULL, "upstreams started");

    /* Target 1 fails with 500; Target 2 is healthy */
    mock_upstream_fail_all(u1, 500);

    struct failover_db db;
    aigate_core        ac;
    pg_store_t*        ps = NULL;
    setup_failover_env(&db, &ac, &ps, mock_upstream_base(u1), mock_upstream_base(u2));

    const char* req_json = "{\"model\":\"failover-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    aigate_request_ctx rq = {
        .method = "POST",
        .path = "/v1/chat/completions",
        .bearer = "client-key",
        .client_ip = "127.0.0.1",
        .body = req_json,
        .body_len = strlen(req_json),
    };
    struct failover_resp fr;
    memset(&fr, 0, sizeof fr);
    aigate_response_ctx rc = {
        .impl = &fr,
        .set_header = fresp_set_header,
        .write = fresp_write,
    };

    int hrc = aigate_handle_request(&ac, &rq, &rc);
    TEST_ASSERT(hrc == 0, "request handled");
    TEST_ASSERT(rc.status == 200, "succeeded via failover with 200");
    TEST_ASSERT(strstr(fr.body, "\"choices\"") != NULL, "valid response body");
    TEST_ASSERT(mock_upstream_request_count(u1) == 1, "target 1 was attempted once");
    TEST_ASSERT(mock_upstream_request_count(u2) == 1, "target 2 served the request");

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(u1);
    mock_upstream_stop(u2);
}

TEST_CASE(test_failover_on_429_to_backup)
{
    mock_upstream_t* u1 = mock_upstream_start();
    mock_upstream_t* u2 = mock_upstream_start();
    TEST_ASSERT(u1 != NULL && u2 != NULL, "upstreams started");

    /* Target 1 fails with 429 Too Many Requests */
    mock_upstream_fail_all(u1, 429);

    struct failover_db db;
    aigate_core        ac;
    pg_store_t*        ps = NULL;
    setup_failover_env(&db, &ac, &ps, mock_upstream_base(u1), mock_upstream_base(u2));

    const char* req_json = "{\"model\":\"failover-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    aigate_request_ctx rq = {
        .method = "POST",
        .path = "/v1/chat/completions",
        .bearer = "client-key",
        .client_ip = "127.0.0.1",
        .body = req_json,
        .body_len = strlen(req_json),
    };
    struct failover_resp fr;
    memset(&fr, 0, sizeof fr);
    aigate_response_ctx rc = {
        .impl = &fr,
        .set_header = fresp_set_header,
        .write = fresp_write,
    };

    int hrc = aigate_handle_request(&ac, &rq, &rc);
    TEST_ASSERT(hrc == 0, "request handled");
    TEST_ASSERT(rc.status == 200, "succeeded on 429 failover");
    TEST_ASSERT(mock_upstream_request_count(u1) == 1, "u1 tried");
    TEST_ASSERT(mock_upstream_request_count(u2) == 1, "u2 succeeded");

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(u1);
    mock_upstream_stop(u2);
}

TEST_CASE(test_failover_circuit_breaker_tripping)
{
    mock_upstream_t* u1 = mock_upstream_start();
    mock_upstream_t* u2 = mock_upstream_start();
    TEST_ASSERT(u1 != NULL && u2 != NULL, "upstreams started");

    mock_upstream_fail_all(u1, 500);

    struct failover_db db;
    aigate_core        ac;
    pg_store_t*        ps = NULL;
    setup_failover_env(&db, &ac, &ps, mock_upstream_base(u1), mock_upstream_base(u2));

    const char* req_json = "{\"model\":\"failover-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    aigate_request_ctx rq = {
        .method = "POST",
        .path = "/v1/chat/completions",
        .bearer = "client-key",
        .client_ip = "127.0.0.1",
        .body = req_json,
        .body_len = strlen(req_json),
    };

    /* Send 3 requests: u1 fails each time and fails over to u2 */
    for (int i = 0; i < 3; i++) {
        struct failover_resp fr;
        memset(&fr, 0, sizeof fr);
        aigate_response_ctx rc = {
            .impl = &fr,
            .set_header = fresp_set_header,
            .write = fresp_write,
        };
        TEST_ASSERT(aigate_handle_request(&ac, &rq, &rc) == 0, "request handled");
        TEST_ASSERT(rc.status == 200, "200 OK via u2");
    }

    TEST_ASSERT(mock_upstream_request_count(u1) == 3, "u1 failed 3 times");
    TEST_ASSERT(mock_upstream_request_count(u2) == 3, "u2 succeeded 3 times");

    /* Target 1 circuit breaker should now be OPEN */
    cb_state_t st = cb_get_state(ac.cb, "failover-chat", db.model.targets[0].endpoint);
    TEST_ASSERT(st == CB_OPEN, "target 1 circuit breaker is OPEN");

    /* 4th request: Router directly skips Target 1 and routes straight to Target 2! */
    struct failover_resp fr4;
    memset(&fr4, 0, sizeof fr4);
    aigate_response_ctx rc4 = {
        .impl = &fr4,
        .set_header = fresp_set_header,
        .write = fresp_write,
    };
    TEST_ASSERT(aigate_handle_request(&ac, &rq, &rc4) == 0, "request 4 handled");
    TEST_ASSERT(rc4.status == 200, "200 OK directly from u2");

    /* u1 request count remains 3 (NOT contacted)! */
    TEST_ASSERT(mock_upstream_request_count(u1) == 3, "u1 bypassed by circuit breaker");
    TEST_ASSERT(mock_upstream_request_count(u2) == 4, "u2 handled request 4");

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(u1);
    mock_upstream_stop(u2);
}

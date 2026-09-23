/** @file test_embeddings.c
 *  @brief Unit tests for /v1/embeddings dual-mode pipeline (Plan 3, Task 5).
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "mock_upstream.h"
#include "pg_store.h"
#include "provider_gemini.h"
#include "provider_openai.h"
#include "ratelimit.h"
#include "sha256.h"
#include "usage_meter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
test_openai_embeddings_build(void)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "text-embedding-3-small");
    snprintf(route.provider, sizeof route.provider, "openai");
    snprintf(route.endpoint, sizeof route.endpoint, "https://api.openai.com/v1");

    const char* in_req = "{\"model\":\"text-embedding-3-small\",\"input\":\"Hello world\"}";
    char        url[1024] = {0};
    const char* extra_hdrs[4][2] = {{0}};
    int         n_extra = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = provider_openai_build_embeddings(
        &route, in_req, url, sizeof url, extra_hdrs, &n_extra, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "openai_build_embeddings returns 0");
    TEST_ASSERT(strcmp(url, "https://api.openai.com/v1/embeddings") == 0, "url is /v1/embeddings");
    TEST_ASSERT(out_body != NULL && strstr(out_body, "Hello world") != NULL, "body has input");
    free(out_body);

    /* Test endpoint without /v1 */
    snprintf(route.endpoint, sizeof route.endpoint, "http://localhost:11434");
    rc = provider_openai_build_embeddings(
        &route, in_req, url, sizeof url, extra_hdrs, &n_extra, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "openai_build_embeddings without v1 in endpoint");
    TEST_ASSERT(strcmp(url, "http://localhost:11434/v1/embeddings") == 0,
                "injected /v1/embeddings");
    free(out_body);
}

void
test_openai_embeddings_parse(void)
{
    const char* raw = "{\"object\":\"list\",\"data\":[{\"object\":\"embedding\",\"index\":0,"
                      "\"embedding\":[0.1,0.2]}],"
                      "\"model\":\"text-embedding-3-small\",\"usage\":{\"prompt_tokens\":14,"
                      "\"total_tokens\":14}}";
    int         status = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;
    long        ptok = 0;

    int rc = provider_openai_parse_embeddings(
        raw, strlen(raw), "text-embedding-3-small", &status, &out_body, &out_len, &ptok);
    TEST_ASSERT(rc == 0, "parse returns 0");
    TEST_ASSERT(status == 200, "status is 200");
    TEST_ASSERT(ptok == 14, "prompt_tokens is 14");
    TEST_ASSERT(out_body != NULL && strcmp(out_body, raw) == 0, "body passed through");
    free(out_body);
}

void
test_gemini_embeddings_build_single(void)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "text-embedding-004");
    snprintf(route.provider, sizeof route.provider, "gemini");
    snprintf(route.endpoint, sizeof route.endpoint, "https://generativelanguage.googleapis.com");
    snprintf(route.upstream_key, sizeof route.upstream_key, "AIzaSyTest123");

    const char* in_req =
        "{\"model\":\"text-embedding-004\",\"input\":\"search query\",\"dimensions\":256}";
    char        url[1024] = {0};
    const char* extra_hdrs[4][2] = {{0}};
    int         n_extra = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = provider_gemini_build_embeddings(
        &route, in_req, url, sizeof url, extra_hdrs, &n_extra, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "gemini build embeddings single returns 0");
    TEST_ASSERT(strcmp(url,
                       "https://generativelanguage.googleapis.com/v1beta/models/"
                       "text-embedding-004:embedContent") == 0,
                "gemini single embed url");
    TEST_ASSERT(n_extra == 1, "has x-goog-api-key header");
    TEST_ASSERT(strcmp(extra_hdrs[0][0], "x-goog-api-key") == 0, "header key");
    TEST_ASSERT(strcmp(extra_hdrs[0][1], "AIzaSyTest123") == 0, "header value");

    json_t* root = json_loads(out_body, 0, NULL);
    TEST_ASSERT(root != NULL, "valid json out");
    json_t* jcontent = json_object_get(root, "content");
    TEST_ASSERT(jcontent != NULL, "has content object");
    json_t* jparts = json_object_get(jcontent, "parts");
    TEST_ASSERT(jparts != NULL && json_array_size(jparts) == 1, "parts has 1 item");
    json_t* jp0 = json_array_get(jparts, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jp0, "text")), "search query") == 0,
                "text matches");
    TEST_ASSERT(json_integer_value(json_object_get(root, "outputDimensionality")) == 256,
                "dim matches");

    json_decref(root);
    free(out_body);
}

void
test_gemini_embeddings_build_batch(void)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "text-embedding-004");
    snprintf(route.provider, sizeof route.provider, "gemini");
    snprintf(route.endpoint, sizeof route.endpoint, "https://generativelanguage.googleapis.com");
    snprintf(route.upstream_key, sizeof route.upstream_key, "AIzaSyTest123");

    const char* in_req = "{\"model\":\"text-embedding-004\",\"input\":[\"hello\",\"world\"]}";
    char        url[1024] = {0};
    const char* extra_hdrs[4][2] = {{0}};
    int         n_extra = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = provider_gemini_build_embeddings(
        &route, in_req, url, sizeof url, extra_hdrs, &n_extra, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "gemini build embeddings batch returns 0");
    TEST_ASSERT(strcmp(url,
                       "https://generativelanguage.googleapis.com/v1beta/models/"
                       "text-embedding-004:batchEmbedContents") == 0,
                "gemini batch embed url");

    json_t* root = json_loads(out_body, 0, NULL);
    TEST_ASSERT(root != NULL, "valid batch json out");
    json_t* jreqs = json_object_get(root, "requests");
    TEST_ASSERT(jreqs != NULL && json_array_size(jreqs) == 2, "batch requests has 2 items");

    json_t* r0 = json_array_get(jreqs, 0);
    TEST_ASSERT(
        strcmp(json_string_value(json_object_get(r0, "model")), "models/text-embedding-004") == 0,
        "model path prefixed");
    json_t* c0 = json_object_get(r0, "content");
    json_t* p0 = json_array_get(json_object_get(c0, "parts"), 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "text")), "hello") == 0,
                "text0 matches");

    json_t* r1 = json_array_get(jreqs, 1);
    json_t* c1 = json_object_get(r1, "content");
    json_t* p1 = json_array_get(json_object_get(c1, "parts"), 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p1, "text")), "world") == 0,
                "text1 matches");

    json_decref(root);
    free(out_body);
}

void
test_gemini_embeddings_parse_single(void)
{
    const char* raw =
        "{\"embedding\":{\"values\":[0.05,-0.02,0.18]},\"usageMetadata\":{\"promptTokenCount\":7}}";
    int    status = 0;
    char*  out_body = NULL;
    size_t out_len = 0;
    long   ptok = 0;

    int rc = provider_gemini_parse_embeddings(
        raw, strlen(raw), "text-embedding-004", &status, &out_body, &out_len, &ptok);
    TEST_ASSERT(rc == 0, "gemini parse single returns 0");
    TEST_ASSERT(status == 200, "status 200");
    TEST_ASSERT(ptok == 7, "prompt tokens 7");

    json_t* root = json_loads(out_body, 0, NULL);
    TEST_ASSERT(root != NULL, "valid json out");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(root, "object")), "list") == 0,
                "object list");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(root, "model")), "text-embedding-004") ==
                    0,
                "model name");

    json_t* data = json_object_get(root, "data");
    TEST_ASSERT(data != NULL && json_array_size(data) == 1, "data length 1");
    json_t* d0 = json_array_get(data, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(d0, "object")), "embedding") == 0,
                "embedding object");
    TEST_ASSERT(json_integer_value(json_object_get(d0, "index")) == 0, "index 0");
    json_t* vals = json_object_get(d0, "embedding");
    TEST_ASSERT(vals != NULL && json_array_size(vals) == 3, "3 values");

    json_t* usage = json_object_get(root, "usage");
    TEST_ASSERT(json_integer_value(json_object_get(usage, "prompt_tokens")) == 7,
                "usage prompt_tokens");
    TEST_ASSERT(json_integer_value(json_object_get(usage, "total_tokens")) == 7,
                "usage total_tokens");

    json_decref(root);
    free(out_body);
}

void
test_gemini_embeddings_parse_batch(void)
{
    const char* raw = "{\"embeddings\":[{\"values\":[0.1,0.2]},{\"values\":[0.3,0.4]}],"
                      "\"usageMetadata\":{\"promptTokenCount\":15}}";
    int         status = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;
    long        ptok = 0;

    int rc = provider_gemini_parse_embeddings(
        raw, strlen(raw), "text-embedding-004", &status, &out_body, &out_len, &ptok);
    TEST_ASSERT(rc == 0, "gemini parse batch returns 0");
    TEST_ASSERT(status == 200, "status 200");
    TEST_ASSERT(ptok == 15, "prompt tokens 15");

    json_t* root = json_loads(out_body, 0, NULL);
    TEST_ASSERT(root != NULL, "valid json out");
    json_t* data = json_object_get(root, "data");
    TEST_ASSERT(data != NULL && json_array_size(data) == 2, "data length 2");

    json_t* d0 = json_array_get(data, 0);
    TEST_ASSERT(json_integer_value(json_object_get(d0, "index")) == 0, "d0 index 0");
    json_t* d1 = json_array_get(data, 1);
    TEST_ASSERT(json_integer_value(json_object_get(d1, "index")) == 1, "d1 index 1");

    json_decref(root);
    free(out_body);
}

void
test_gemini_embeddings_parse_error(void)
{
    const char* raw = "{\"error\":{\"code\":400,\"message\":\"Invalid "
                      "argument\",\"status\":\"INVALID_ARGUMENT\"}}";
    int         status = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;
    long        ptok = 0;

    int rc = provider_gemini_parse_embeddings(
        raw, strlen(raw), "text-embedding-004", &status, &out_body, &out_len, &ptok);
    TEST_ASSERT(rc == 0, "gemini parse error returns 0");
    TEST_ASSERT(status == 400, "status 400");

    json_t* root = json_loads(out_body, 0, NULL);
    TEST_ASSERT(root != NULL, "valid json out");
    json_t* jerr = json_object_get(root, "error");
    TEST_ASSERT(jerr != NULL, "has error object");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jerr, "message")), "Invalid argument") ==
                    0,
                "error msg matches");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jerr, "type")), "upstream_error") == 0,
                "error type matches");

    json_decref(root);
    free(out_body);
}

/* -------------------------------- fake ops for e2e pipeline test */

#define FKEYS 4
#define FMODELS 4
#define FUSAGE 64

struct test_fdb {
    struct {
        int       in_use;
        key_rec_t k;
    } keys[FKEYS];
    model_rec_t models[FMODELS];
    int         n_models;
    usage_row_t usage[FUSAGE];
    int         n_usage;
    int         flush_calls;
};

static int
fget_key_cb(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct test_fdb* db = ctx;
    for (int i = 0; i < FKEYS; i++) {
        if (db->keys[i].in_use && strcmp(db->keys[i].k.key_hash, key_hash) == 0) {
            *out = db->keys[i].k;
            out->allowed_models = NULL;
            out->n_allowed = 0;
            return 0;
        }
    }
    return 1;
}

static int
fget_model_cb(void* ctx, const char* name, model_rec_t* out)
{
    struct test_fdb* db = ctx;
    for (int i = 0; i < db->n_models; i++) {
        if (strcmp(db->models[i].name, name) == 0) {
            *out = db->models[i];
            return 0;
        }
    }
    return -1;
}

static int
fflush_cb(void* ctx, const usage_row_t* rows, int n)
{
    struct test_fdb* db = ctx;
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

struct test_resp_cap {
    char   hdrs[2048];
    char   body[16384];
    size_t blen;
    int    status;
};

static int
cap_hdr_cb(void* impl, const char* name, const char* value)
{
    struct test_resp_cap* c = impl;
    snprintf(c->hdrs + strlen(c->hdrs), sizeof c->hdrs - strlen(c->hdrs), "%s: %s\n", name, value);
    return 0;
}

static int
cap_write_cb(void* impl, const void* buf, size_t len, bool fin)
{
    struct test_resp_cap* c = impl;
    (void)fin;
    if (c->blen + len < sizeof c->body) {
        memcpy(c->body + c->blen, buf, len);
        c->blen += len;
    }
    c->body[c->blen] = '\0';
    return 0;
}

void
test_embeddings_pipeline_e2e(void)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock upstream starts");

    struct test_fdb db = {0};
    db.keys[0].in_use = 1;
    db.keys[0].k.key_id = 1;
    sha256_hex("test-key", 8, db.keys[0].k.key_hash);
    db.keys[0].k.daily_token_quota = 10000;

    /* Model 0: OpenAI embedding model */
    snprintf(db.models[0].name, sizeof db.models[0].name, "text-embedding-3-small");
    snprintf(db.models[0].provider, sizeof db.models[0].provider, "openai");
    snprintf(db.models[0].endpoint, sizeof db.models[0].endpoint, "%s/v1", mock_upstream_base(mu));
    db.models[0].enabled = 1;

    /* Model 1: Gemini embedding model */
    snprintf(db.models[1].name, sizeof db.models[1].name, "text-embedding-004");
    snprintf(db.models[1].provider, sizeof db.models[1].provider, "gemini");
    snprintf(db.models[1].endpoint, sizeof db.models[1].endpoint, "%s", mock_upstream_base(mu));
    db.models[1].enabled = 1;

    /* Model 2: Anthropic model (no embedding support) */
    snprintf(db.models[2].name, sizeof db.models[2].name, "claude-3-haiku");
    snprintf(db.models[2].provider, sizeof db.models[2].provider, "anthropic");
    snprintf(db.models[2].endpoint, sizeof db.models[2].endpoint, "%s", mock_upstream_base(mu));
    db.models[2].enabled = 1;

    db.n_models = 3;

    pg_ops_t ops = {0};
    ops.ctx = &db;
    ops.get_key_by_hash = fget_key_cb;
    ops.get_model = fget_model_cb;
    ops.flush_usage = fflush_cb;
    ops.flush_usage_requests =
        (int (*)(void*, const usage_request_row_t*, int))f_req_stub;
    ops.query_usage_requests =
        (int (*)(void*, long, time_t, usage_request_row_t*, int, int*))f_req_stub;
    pg_store_t* ps = pg_store_open(NULL, &ops);

    aigate_core ac;
    TEST_ASSERT(aigate_core_init(&ac, ps, NULL, 5000, 0) == 0, "core init");

    /* 1. Request to OpenAI embedding model */
    {
        struct test_resp_cap c = {0};
        aigate_response_ctx  rc = {0};
        rc.impl = &c;
        rc.set_header = cap_hdr_cb;
        rc.write = cap_write_cb;

        aigate_request_ctx rq = {0};
        rq.method = "POST";
        rq.path = "/v1/embeddings";
        rq.bearer = "test-key";
        rq.client_ip = "127.0.0.1";
        const char* b = "{\"model\":\"text-embedding-3-small\",\"input\":\"Embed me\"}";
        rq.body = (const unsigned char*)b;
        rq.body_len = strlen(b);

        int code = aigate_handle_request(&ac, &rq, &rc);
        TEST_ASSERT(code == 0, "openai embedding pipeline returns 0");
        TEST_ASSERT(rc.status == 200, "openai embedding rc.status 200");
        TEST_ASSERT(strstr(c.body, "\"embedding\"") != NULL, "response has embedding");
        TEST_ASSERT(strstr(c.body, "\"object\":\"list\"") != NULL, "response has object list");

        /* Verify token reservation in rate limiter */
        long consumed = 10000 - rl_remaining_daily(ac.rl, 1, 10000);
        TEST_ASSERT(consumed == 8, "8 prompt tokens reserved in rate limiter");
    }

    /* 2. Request to Gemini embedding model */
    {
        struct test_resp_cap c = {0};
        aigate_response_ctx  rc = {0};
        rc.impl = &c;
        rc.set_header = cap_hdr_cb;
        rc.write = cap_write_cb;

        aigate_request_ctx rq = {0};
        rq.method = "POST";
        rq.path = "/v1/embeddings";
        rq.bearer = "test-key";
        rq.client_ip = "127.0.0.1";
        const char* b = "{\"model\":\"text-embedding-004\",\"input\":\"Gemini embed\"}";
        rq.body = (const unsigned char*)b;
        rq.body_len = strlen(b);

        int code = aigate_handle_request(&ac, &rq, &rc);
        TEST_ASSERT(code == 0, "gemini embedding pipeline returns 0");
        TEST_ASSERT(rc.status == 200, "gemini embedding rc.status 200");
        TEST_ASSERT(strstr(c.body, "\"embedding\"") != NULL, "gemini response has embedding");
        TEST_ASSERT(strstr(c.body, "\"object\":\"list\"") != NULL,
                    "gemini response has object list");

        long consumed = 10000 - rl_remaining_daily(ac.rl, 1, 10000);
        TEST_ASSERT(consumed == 8 + 6, "14 total tokens consumed (8 + 6)");
    }

    /* 3. Request to Anthropic model on /v1/embeddings -> 400 */
    {
        struct test_resp_cap c = {0};
        aigate_response_ctx  rc = {0};
        rc.impl = &c;
        rc.set_header = cap_hdr_cb;
        rc.write = cap_write_cb;

        aigate_request_ctx rq = {0};
        rq.method = "POST";
        rq.path = "/v1/embeddings";
        rq.bearer = "test-key";
        rq.client_ip = "127.0.0.1";
        const char* b = "{\"model\":\"claude-3-haiku\",\"input\":\"Unsupported embed\"}";
        rq.body = (const unsigned char*)b;
        rq.body_len = strlen(b);

        int code = aigate_handle_request(&ac, &rq, &rc);
        TEST_ASSERT(code == 0, "returns 0 for handled error");
        TEST_ASSERT(rc.status == 400, "status 400 for unsupported embeddings");
        TEST_ASSERT(strstr(c.body, "unsupported_endpoint") != NULL,
                    "error code unsupported_endpoint");
    }

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

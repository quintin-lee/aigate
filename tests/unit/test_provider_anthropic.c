/** @file test_provider_anthropic.c
 *  @brief Unit tests for Anthropic provider adapter and SSE bridge (Plan 2, Tasks 4 & 5).
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "mock_upstream.h"
#include "pg_store.h"
#include "provider_anthropic.h"
#include "sha256.h"
#include "usage_meter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_anthropic_build_system_and_defaults)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "claude-3-5-sonnet-20241022");
    snprintf(route.provider, sizeof route.provider, "anthropic");
    snprintf(route.endpoint, sizeof route.endpoint, "http://127.0.0.1:8080");
    snprintf(route.upstream_key, sizeof route.upstream_key, "sk-ant-testkey");

    const char* in_req = "{\"model\":\"claude-3-5-sonnet-20241022\",\"messages\":["
                         "{\"role\":\"system\",\"content\":\"System Rule 1\"},"
                         "{\"role\":\"user\",\"content\":\"Hello Claude\"},"
                         "{\"role\":\"system\",\"content\":\"System Rule 2\"}"
                         "]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n_hdrs = 0;
    char*       body = NULL;
    size_t      body_len = 0;

    int rc =
        provider_anthropic_build(&route, in_req, url, sizeof url, hdrs, &n_hdrs, &body, &body_len);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(
        strcmp(url, "http://127.0.0.1:8080/v1/messages") == 0, "url is /v1/messages, got %s", url);
    TEST_ASSERT(n_hdrs == 2, "2 extra headers");
    TEST_ASSERT(strcmp(hdrs[0][0], "x-api-key") == 0 && strcmp(hdrs[0][1], "sk-ant-testkey") == 0,
                "x-api-key");
    TEST_ASSERT(strcmp(hdrs[1][0], "anthropic-version") == 0 &&
                    strcmp(hdrs[1][1], "2023-06-01") == 0,
                "anthropic-version");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "parsed output json");
    if (out != NULL) {
        json_t* jsys = json_object_get(out, "system");
        TEST_ASSERT(jsys != NULL && json_is_string(jsys), "system string exists");
        if (jsys != NULL && json_is_string(jsys)) {
            TEST_ASSERT(strcmp(json_string_value(jsys), "System Rule 1\n\nSystem Rule 2") == 0,
                        "system prompt concatenated");
        }

        json_t* jmsgs = json_object_get(out, "messages");
        TEST_ASSERT(jmsgs != NULL && json_is_array(jmsgs), "messages is array");
        if (jmsgs != NULL && json_is_array(jmsgs)) {
            TEST_ASSERT(json_array_size(jmsgs) == 1, "only 1 non-system message");
            json_t* m0 = json_array_get(jmsgs, 0);
            json_t* r0 = json_object_get(m0, "role");
            json_t* c0 = json_object_get(m0, "content");
            TEST_ASSERT(strcmp(json_string_value(r0), "user") == 0, "role user");
            TEST_ASSERT(strcmp(json_string_value(c0), "Hello Claude") == 0, "content user");
        }

        json_t* jmt = json_object_get(out, "max_tokens");
        TEST_ASSERT(jmt != NULL && json_integer_value(jmt) == 4096, "max_tokens default 4096");

        json_decref(out);
    }
    free(body);
}

TEST_CASE(test_anthropic_build_params)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "claude-3-5-sonnet-20241022");
    snprintf(route.provider, sizeof route.provider, "anthropic");
    snprintf(route.endpoint, sizeof route.endpoint, "http://127.0.0.1:8080/v1");

    const char* in_req =
        "{\"model\":\"claude-3-5-sonnet-20241022\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"Hi\"}"
        "],\"max_tokens\":1000,\"temperature\":0.7,\"stop\":[\"STOP\"],\"stream\":true}";

    char        url[512];
    const char* hdrs[4][2];
    int         n_hdrs = 0;
    char*       body = NULL;
    size_t      body_len = 0;

    int rc =
        provider_anthropic_build(&route, in_req, url, sizeof url, hdrs, &n_hdrs, &body, &body_len);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(strcmp(url, "http://127.0.0.1:8080/v1/messages") == 0,
                "endpoint /v1 -> /v1/messages");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "parsed output json");
    if (out != NULL) {
        json_t* jmt = json_object_get(out, "max_tokens");
        TEST_ASSERT(jmt != NULL && json_integer_value(jmt) == 1000, "max_tokens copied");

        json_t* jtemp = json_object_get(out, "temperature");
        TEST_ASSERT(jtemp != NULL && json_number_value(jtemp) > 0.69, "temperature copied");

        json_t* jstop = json_object_get(out, "stop_sequences");
        TEST_ASSERT(jstop != NULL && json_is_array(jstop), "stop converted to stop_sequences");

        json_t* jstr = json_object_get(out, "stream");
        TEST_ASSERT(jstr != NULL && json_is_true(jstr), "stream true");

        json_decref(out);
    }
    free(body);
}

TEST_CASE(test_anthropic_resp_translation)
{
    const char* ant_resp =
        "{\"id\":\"msg_013Zva2CMHLNnxPQCdQUqGsE\",\"type\":\"message\",\"role\":\"assistant\","
        "\"model\":\"claude-3-5-sonnet-20241022\",\"content\":[{\"type\":\"text\",\"text\":\"Hello "
        "world!\"}],"
        "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":12,\"output_tokens\":8}}";

    char*  oai_resp = NULL;
    size_t oai_len = 0;
    long   ptok = 0, ctok = 0;

    int rc = provider_anthropic_resp_to_openai(
        ant_resp, "claude-3-5-sonnet", &oai_resp, &oai_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "resp translation ok");
    TEST_ASSERT(ptok == 12, "input_tokens 12");
    TEST_ASSERT(ctok == 8, "output_tokens 8");

    json_t* out = json_loads(oai_resp, 0, NULL);
    TEST_ASSERT(out != NULL, "parsed translated json");
    if (out != NULL) {
        json_t* jid = json_object_get(out, "id");
        TEST_ASSERT(jid != NULL && strcmp(json_string_value(jid),
                                          "chatcmpl-msg_013Zva2CMHLNnxPQCdQUqGsE") == 0,
                    "id prefixed with chatcmpl-");

        json_t* jobj = json_object_get(out, "object");
        TEST_ASSERT(jobj != NULL && strcmp(json_string_value(jobj), "chat.completion") == 0,
                    "object is chat.completion");

        json_t* jchoices = json_object_get(out, "choices");
        TEST_ASSERT(jchoices != NULL && json_array_size(jchoices) == 1, "1 choice");
        json_t* c0 = json_array_get(jchoices, 0);
        json_t* msg = json_object_get(c0, "message");
        json_t* cnt = json_object_get(msg, "content");
        TEST_ASSERT(cnt != NULL && strcmp(json_string_value(cnt), "Hello world!") == 0,
                    "content translated");
        json_t* fr = json_object_get(c0, "finish_reason");
        TEST_ASSERT(fr != NULL && strcmp(json_string_value(fr), "stop") == 0, "finish_reason stop");

        json_t* jusg = json_object_get(out, "usage");
        json_t* jp = json_object_get(jusg, "prompt_tokens");
        json_t* jc = json_object_get(jusg, "completion_tokens");
        json_t* jt = json_object_get(jusg, "total_tokens");
        TEST_ASSERT(json_integer_value(jp) == 12, "prompt_tokens 12");
        TEST_ASSERT(json_integer_value(jc) == 8, "completion_tokens 8");
        TEST_ASSERT(json_integer_value(jt) == 20, "total_tokens 20");

        json_decref(out);
    }
    free(oai_resp);
}

struct test_cap {
    char   hdrs[2048];
    char   body[16384];
    size_t blen;
    int    status;
};

static int
tcap_set_header(void* impl, const char* name, const char* value)
{
    struct test_cap* c = impl;
    snprintf(c->hdrs + strlen(c->hdrs), sizeof c->hdrs - strlen(c->hdrs), "%s: %s\n", name, value);
    return 0;
}

static int
tcap_write(void* impl, const void* buf, size_t len, bool fin)
{
    struct test_cap* c = impl;
    (void)fin;
    if (c->blen + len < sizeof c->body) {
        memcpy(c->body + c->blen, buf, len);
        c->blen += len;
    }
    c->body[c->blen] = '\0';
    return 0;
}

TEST_CASE(test_anthropic_bridge_streaming)
{
    struct test_cap     c = {0};
    aigate_response_ctx rc = {0};
    rc.impl = &c;
    rc.set_header = tcap_set_header;
    rc.write = tcap_write;

    anthropic_bridge_t bridge;
    anthropic_bridge_init(&bridge, &rc);

    const char* chunk1 = "event: message_start\r\n"
                         "data: "
                         "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_stream_test\","
                         "\"model\":\"claude-3-5\",\"usage\":{\"input_tokens\":10}}}\r\n\r\n";
    anthropic_bridge_feed(&bridge, chunk1, strlen(chunk1));

    const char* chunk2 = "event: content_block_delta\r\n"
                         "data: "
                         "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_"
                         "delta\",\"text\":\"Hello \"}}\r\n\r\n";
    anthropic_bridge_feed(&bridge, chunk2, strlen(chunk2));

    const char* chunk3 = "event: content_block_delta\r\n"
                         "data: "
                         "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_"
                         "delta\",\"text\":\"world!\"}}\r\n\r\n";
    anthropic_bridge_feed(&bridge, chunk3, strlen(chunk3));

    const char* chunk4 = "event: message_delta\r\n"
                         "data: "
                         "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
                         "\"usage\":{\"output_tokens\":20}}\r\n\r\n"
                         "event: message_stop\r\n"
                         "data: {\"type\":\"message_stop\"}\r\n\r\n";
    anthropic_bridge_feed(&bridge, chunk4, strlen(chunk4));

    anthropic_bridge_finish(&bridge);

    TEST_ASSERT(strstr(c.hdrs, "Content-Type: text/event-stream; charset=utf-8") != NULL,
                "header text/event-stream");
    TEST_ASSERT(strstr(c.body, "Hello ") != NULL, "body contains Hello ");
    TEST_ASSERT(strstr(c.body, "world!") != NULL, "body contains world!");
    TEST_ASSERT(strstr(c.body, "\"finish_reason\":\"stop\"") != NULL,
                "body contains stop finish_reason");
    TEST_ASSERT(strstr(c.body, "\"prompt_tokens\":10") != NULL, "usage prompt_tokens 10");
    TEST_ASSERT(strstr(c.body, "\"completion_tokens\":20") != NULL, "usage completion_tokens 20");
    TEST_ASSERT(strstr(c.body, "[DONE]") != NULL, "body contains [DONE]");
    TEST_ASSERT(bridge.input_tokens == 10, "bridge input_tokens == 10");
    TEST_ASSERT(bridge.output_tokens == 20, "bridge output_tokens == 20");
}

static int
tcap_write_fail(void* impl, const void* buf, size_t len, bool fin)
{
    (void)impl;
    (void)buf;
    (void)len;
    (void)fin;
    return -1; /* simulate a client disconnect */
}

TEST_CASE(test_anthropic_bridge_client_abort)
{
    struct test_cap        c = {0};
    aigate_response_ctx    rc = {0};
    rc.impl = &c;
    rc.set_header = tcap_set_header;
    rc.write = tcap_write_fail;

    anthropic_bridge_t bridge;
    anthropic_bridge_init(&bridge, &rc);

    /* feed a content delta: the underlying write fails, so feed must report -1 */
    const char* chunk = "event: content_block_delta\r\n"
                        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
                        "{\"type\":\"text_delta\",\"text\":\"hi\"}}\r\n\r\n";
    int fr = anthropic_bridge_feed(&bridge, chunk, strlen(chunk));
    TEST_ASSERT(fr == -1, "feed returns -1 after client abort");
    TEST_ASSERT(bridge.aborted, "bridge.aborted set");
}

/* -------------------------------- fake ops for pipeline test */

#define FKEYS 4
#define FMODELS 4
#define FUSAGE 64

struct fdb {
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
fget_key(void* ctx, const char* key_hash, key_rec_t* out)
{
    struct fdb* db = ctx;
    for (int i = 0; i < FKEYS; i++) {
        if (db->keys[i].in_use && strcmp(db->keys[i].k.key_hash, key_hash) == 0) {
            *out = db->keys[i].k;
            out->allowed_models = NULL;
            out->n_allowed = 0;
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
f_flush(void* ctx, const usage_row_t* rows, int n)
{
    struct fdb* db = ctx;
    db->flush_calls++;
    for (int i = 0; i < n && db->n_usage < FUSAGE; i++) {
        db->usage[db->n_usage++] = rows[i];
    }
    return 0;
}

TEST_CASE(test_anthropic_pipeline_end_to_end)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock start");

    struct fdb db = {0};
    db.keys[0].in_use = 1;
    db.keys[0].k.key_id = 1;
    sha256_hex("claude-key", 10, db.keys[0].k.key_hash);
    db.keys[0].k.daily_token_quota = 5000;

    snprintf(db.models[0].name, sizeof db.models[0].name, "claude-3-5-sonnet");
    snprintf(db.models[0].provider, sizeof db.models[0].provider, "anthropic");
    snprintf(db.models[0].endpoint, sizeof db.models[0].endpoint, "%s", mock_upstream_base(mu));
    snprintf(db.models[0].upstream_key, sizeof db.models[0].upstream_key, "sk-ant-upstream");
    db.models[0].enabled = 1;
    db.n_models = 1;

    pg_ops_t ops = {0};
    ops.ctx = &db;
    ops.get_key_by_hash = fget_key;
    ops.get_model = fget_model;
    ops.flush_usage = f_flush;
    pg_store_t* ps = pg_store_open(NULL, &ops);

    aigate_core ac;
    TEST_ASSERT(aigate_core_init(&ac, ps, NULL, 5000, 0) == 0, "core init");

    /* 1. Non-streaming call to Claude */
    {
        struct test_cap     c = {0};
        aigate_response_ctx rc = {0};
        rc.impl = &c;
        rc.set_header = tcap_set_header;
        rc.write = tcap_write;

        aigate_request_ctx rq = {0};
        rq.method = "POST";
        rq.path = "/v1/chat/completions";
        rq.bearer = "claude-key";
        rq.client_ip = "127.0.0.1";
        const char* b = "{\"model\":\"claude-3-5-sonnet\",\"messages\":[{\"role\":\"user\","
                        "\"content\":\"hi\"}]}";
        rq.body = b;
        rq.body_len = strlen(b);

        int rv = aigate_handle_request(&ac, &rq, &rc);
        TEST_ASSERT(rv == 0, "handle request 0");
        TEST_ASSERT(rc.status == 200, "status 200");
        TEST_ASSERT(strstr(c.body, "Hello from Claude non-stream") != NULL,
                    "got claude non-stream text");

        usage_row_t rows[FUSAGE];
        int         n = 0;
        um_drain(ac.um, rows, FUSAGE, &n);
        TEST_ASSERT(n == 1, "1 usage row");
        if (n == 1) {
            TEST_ASSERT(rows[0].prompt_tokens == 12, "ptok 12");
            TEST_ASSERT(rows[0].completion_tokens == 18, "ctok 18");
        }
    }

    /* 2. Streaming call to Claude */
    {
        struct test_cap     c = {0};
        aigate_response_ctx rc = {0};
        rc.impl = &c;
        rc.set_header = tcap_set_header;
        rc.write = tcap_write;

        aigate_request_ctx rq = {0};
        rq.method = "POST";
        rq.path = "/v1/chat/completions";
        rq.bearer = "claude-key";
        rq.client_ip = "127.0.0.1";
        const char* b = "{\"model\":\"claude-3-5-sonnet\",\"stream\":true,\"messages\":[{\"role\":"
                        "\"user\",\"content\":\"hi\"}]}";
        rq.body = b;
        rq.body_len = strlen(b);

        int rv = aigate_handle_request(&ac, &rq, &rc);
        TEST_ASSERT(rv == 0, "handle streaming request 0");
        TEST_ASSERT(rc.status == 200, "status 200");
        TEST_ASSERT(strstr(c.hdrs, "text/event-stream") != NULL, "header text/event-stream");
        TEST_ASSERT(strstr(c.body, "Hello ") != NULL, "stream has Hello ");
        TEST_ASSERT(strstr(c.body, "from Claude") != NULL, "stream has from Claude");
        TEST_ASSERT(strstr(c.body, "[DONE]") != NULL, "stream has [DONE]");

        usage_row_t rows[FUSAGE];
        int         n = 0;
        um_drain(ac.um, rows, FUSAGE, &n);
        TEST_ASSERT(n == 1, "1 usage row");
        if (n == 1) {
            TEST_ASSERT(rows[0].prompt_tokens == 12, "stream ptok 12");
            TEST_ASSERT(rows[0].completion_tokens == 18, "stream ctok 18");
        }
    }

    aigate_core_shutdown(&ac);
    pg_store_close(ps);
    mock_upstream_stop(mu);
}

/** @file test_provider_anthropic_tools.c
 *  @brief Unit tests for Anthropic Tool Calling protocol translation.
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "provider_anthropic.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */
static model_rec_t
make_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "claude-3-5-sonnet-20241022");
    snprintf(r.provider, sizeof r.provider, "anthropic");
    snprintf(r.endpoint, sizeof r.endpoint, "http://127.0.0.1:8080");
    snprintf(r.upstream_key, sizeof r.upstream_key, "sk-ant-test");
    return r;
}

/* ----------------------------------------------------- Test 1: tools build */
TEST_CASE(test_anthropic_tools_request_build)
{
    model_rec_t route = make_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"What's the weather?\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{"
        "\"name\":\"get_weather\","
        "\"description\":\"Get weather for a location\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}}],"
        "\"tool_choice\":\"auto\"}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "output is valid json");

    json_t* jtools = json_object_get(out, "tools");
    TEST_ASSERT(jtools && json_is_array(jtools) && json_array_size(jtools) == 1,
                "tools array has 1 entry");
    json_t* t0 = json_array_get(jtools, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(t0, "name")), "get_weather") == 0,
                "tool name");
    TEST_ASSERT(json_object_get(t0, "input_schema") != NULL, "input_schema present");

    json_t* jtc = json_object_get(out, "tool_choice");
    TEST_ASSERT(jtc && json_is_object(jtc), "tool_choice is object");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jtc, "type")), "auto") == 0,
                "tool_choice type=auto");

    json_decref(out);
    free(body);
}

/* --------------------------------------------- Test 2: tool_use response */
TEST_CASE(test_anthropic_tool_use_response_parse)
{
    const char* ant_resp =
        "{\"id\":\"msg_01\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\","
        "\"stop_reason\":\"tool_use\","
        "\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_01\","
        "\"name\":\"get_weather\",\"input\":{\"location\":\"Beijing\"}}],"
        "\"usage\":{\"input_tokens\":30,\"output_tokens\":10}}";

    char*  oai = NULL;
    size_t olen = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_anthropic_resp_to_openai(
        ant_resp, "claude-3-5-sonnet-20241022", &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    TEST_ASSERT(oai != NULL, "output not null");

    json_t* out = json_loads(oai, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");
    json_t* choices = json_object_get(out, "choices");
    json_t* c0 = json_array_get(choices, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(c0, "finish_reason")), "tool_calls") == 0,
                "finish_reason=tool_calls");
    json_t* msg = json_object_get(c0, "message");
    json_t* tc_arr = json_object_get(msg, "tool_calls");
    TEST_ASSERT(tc_arr && json_is_array(tc_arr) && json_array_size(tc_arr) == 1,
                "tool_calls array has 1 entry");
    json_t* tc0 = json_array_get(tc_arr, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tc0, "id")), "toolu_01") == 0, "id");
    json_t* fn = json_object_get(tc0, "function");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fn, "name")), "get_weather") == 0, "name");
    const char* args = json_string_value(json_object_get(fn, "arguments"));
    json_t*     parsed_args = json_loads(args, 0, NULL);
    TEST_ASSERT(parsed_args != NULL, "arguments is valid json");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(parsed_args, "location")), "Beijing") == 0,
                "location arg");

    json_decref(parsed_args);
    json_decref(out);
    free(oai);
}

/* ----------------------------------------- Test 3: mixed text + tool_use */
TEST_CASE(test_anthropic_mixed_text_and_tool_use)
{
    const char* ant_resp =
        "{\"id\":\"msg_02\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\","
        "\"stop_reason\":\"tool_use\","
        "\"content\":["
        "{\"type\":\"text\",\"text\":\"Let me check the weather.\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_02\",\"name\":\"get_weather\","
        "\"input\":{\"location\":\"Shanghai\"}}],"
        "\"usage\":{\"input_tokens\":40,\"output_tokens\":15}}";

    char*  oai = NULL;
    size_t olen = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_anthropic_resp_to_openai(
        ant_resp, "claude-3-5-sonnet-20241022", &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    json_t* out = json_loads(oai, 0, NULL);
    json_t* msg = json_object_get(json_array_get(json_object_get(out, "choices"), 0), "message");
    json_t* jcontent = json_object_get(msg, "content");
    TEST_ASSERT(jcontent && json_is_string(jcontent) &&
                    strcmp(json_string_value(jcontent), "Let me check the weather.") == 0,
                "text content preserved");
    TEST_ASSERT(json_array_size(json_object_get(msg, "tool_calls")) == 1, "tool_calls has 1 entry");

    json_decref(out);
    free(oai);
}

/* --------------------------------------- Test 4: tool result message build */
TEST_CASE(test_anthropic_tool_result_message_build)
{
    model_rec_t route = make_route();
    /* Conversation: user → assistant(tool_calls) → tool(result) */
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":\"What's the weather in Beijing?\"},"
        "{\"role\":\"assistant\",\"content\":null,"
        "\"tool_calls\":[{\"id\":\"toolu_01\",\"type\":\"function\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{"
        "\\\"location\\\":\\\"Beijing\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"toolu_01\","
        "\"content\":\"Sunny, 25C\"}"
        "]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    json_t* msgs = json_object_get(out, "messages");
    TEST_ASSERT(json_is_array(msgs) && json_array_size(msgs) == 3, "3 messages");

    /* Check assistant message has tool_use content */
    json_t* asst = json_array_get(msgs, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(asst, "role")), "assistant") == 0,
                "assistant role");
    json_t* asst_content = json_object_get(asst, "content");
    TEST_ASSERT(json_is_array(asst_content), "assistant content is array");
    json_t* tu = json_array_get(asst_content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tu, "type")), "tool_use") == 0,
                "tool_use type");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tu, "id")), "toolu_01") == 0, "id");

    /* Check tool result message */
    json_t* tr_msg = json_array_get(msgs, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr_msg, "role")), "user") == 0,
                "tool result role=user");
    json_t* tr_content = json_object_get(tr_msg, "content");
    TEST_ASSERT(json_is_array(tr_content) && json_array_size(tr_content) == 1, "content array");
    json_t* tr = json_array_get(tr_content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr, "type")), "tool_result") == 0,
                "type=tool_result");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr, "tool_use_id")), "toolu_01") == 0,
                "tool_use_id");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr, "content")), "Sunny, 25C") == 0,
                "content value");

    json_decref(out);
    free(body);
}

/* ------------------------------------------- Test 5: SSE tool_call stream */
static char   g_ant_sse_buf[65536];
static size_t g_ant_sse_len;

static int
ant_mock_write(void* impl, const void* data, size_t len, bool fin)
{
    (void)impl;
    (void)fin;
    if (g_ant_sse_len + len < sizeof g_ant_sse_buf) {
        memcpy(g_ant_sse_buf + g_ant_sse_len, data, len);
        g_ant_sse_len += len;
        g_ant_sse_buf[g_ant_sse_len] = '\0';
    }
    return 0;
}

static int
ant_mock_set_header(void* impl, const char* k, const char* v)
{
    (void)impl;
    (void)k;
    (void)v;
    return 0;
}

TEST_CASE(test_anthropic_sse_tool_call_stream)
{
    g_ant_sse_len = 0;
    g_ant_sse_buf[0] = '\0';

    aigate_response_ctx rc_ctx;
    memset(&rc_ctx, 0, sizeof rc_ctx);
    rc_ctx.write = ant_mock_write;
    rc_ctx.set_header = ant_mock_set_header;

    anthropic_bridge_t b;
    anthropic_bridge_init(&b, &rc_ctx);
    snprintf(b.model, sizeof b.model, "claude-3-5-sonnet-20241022");
    snprintf(b.msg_id, sizeof b.msg_id, "msg_01");

    /* message_start */
    const char* s1 =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01\","
        "\"type\":\"message\",\"role\":\"assistant\","
        "\"model\":\"claude-3-5-sonnet-20241022\","
        "\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n";
    anthropic_bridge_feed(&b, s1, strlen(s1));

    /* content_block_start (tool_use) */
    const char* s2 =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01\","
        "\"name\":\"get_weather\",\"input\":{}}}\n\n";
    anthropic_bridge_feed(&b, s2, strlen(s2));

    /* input_json_delta: two partial JSON chunks */
    const char* s3 =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"loc\"}}\n\n";
    anthropic_bridge_feed(&b, s3, strlen(s3));

    const char* s4 =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"ation\\\":"
        "\\\"Beijing\\\"\"}}\n\n";
    anthropic_bridge_feed(&b, s4, strlen(s4));

    /* content_block_stop — should emit tool_calls SSE chunk */
    const char* s5 =
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    anthropic_bridge_feed(&b, s5, strlen(s5));

    /* message_delta stop_reason=tool_use */
    const char* s6 =
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},"
        "\"usage\":{\"output_tokens\":20}}\n\n";
    anthropic_bridge_feed(&b, s6, strlen(s6));

    /* message_stop */
    const char* s7 = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    anthropic_bridge_feed(&b, s7, strlen(s7));
    anthropic_bridge_finish(&b);

    free(b.tool_args_buf);

    TEST_ASSERT(strstr(g_ant_sse_buf, "tool_calls") != NULL, "output contains tool_calls");
    TEST_ASSERT(strstr(g_ant_sse_buf, "get_weather") != NULL, "output contains get_weather");
    TEST_ASSERT(strstr(g_ant_sse_buf, "toolu_01") != NULL, "output contains toolu_01");
}

/* ----------------------------- Test 6: tool_choice required→any mapping */
TEST_CASE(test_anthropic_tool_choice_required_mapping)
{
    model_rec_t route = make_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"Call a tool\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{"
        "\"name\":\"fn\",\"description\":\"d\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}],"
        "\"tool_choice\":\"required\"}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");
    json_t* out = json_loads(body, 0, NULL);
    json_t* jtc = json_object_get(out, "tool_choice");
    TEST_ASSERT(jtc && strcmp(json_string_value(json_object_get(jtc, "type")), "any") == 0,
                "required maps to any");

    json_decref(out);
    free(body);
}

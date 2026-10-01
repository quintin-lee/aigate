/** @file test_provider_gemini_tools.c
 *  @brief Unit tests for Gemini Tool Calling protocol translation.
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "provider_gemini.h"
#include "provider_adapter.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static model_rec_t
make_gemini_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "gemini-1.5-pro");
    snprintf(r.provider, sizeof r.provider, "gemini");
    snprintf(r.endpoint, sizeof r.endpoint, "https://generativelanguage.googleapis.com");
    snprintf(r.upstream_key, sizeof r.upstream_key, "AIzaSyTest");
    return r;
}

/* ------------------------------------------------ Test 1: tools build */
TEST_CASE(test_gemini_tools_request_build)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"What is the weather?\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{"
        "\"name\":\"get_weather\",\"description\":\"Get weather\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}}],"
        "\"tool_choice\":\"auto\"}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* jtools = json_object_get(out, "tools");
    TEST_ASSERT(jtools && json_is_array(jtools) && json_array_size(jtools) == 1, "tools array");
    json_t* t0 = json_array_get(jtools, 0);
    json_t* fn_decls = json_object_get(t0, "functionDeclarations");
    TEST_ASSERT(fn_decls && json_is_array(fn_decls) && json_array_size(fn_decls) == 1,
                "functionDeclarations");
    json_t* fd0 = json_array_get(fn_decls, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd0, "name")), "get_weather") == 0,
                "name");
    TEST_ASSERT(json_object_get(fd0, "parameters") != NULL, "parameters present");

    json_t* jtc = json_object_get(out, "toolConfig");
    json_t* fcc = jtc ? json_object_get(jtc, "functionCallingConfig") : NULL;
    TEST_ASSERT(fcc && strcmp(json_string_value(json_object_get(fcc, "mode")), "AUTO") == 0,
                "toolConfig mode=AUTO");

    json_decref(out);
    free(body);
}

/* ----------------------------------------- Test 2: functionCall response */
TEST_CASE(test_gemini_function_call_response_parse)
{
    const char* gemini_resp =
        "{\"candidates\":[{"
        "\"content\":{\"role\":\"model\",\"parts\":[{"
        "\"functionCall\":{\"name\":\"get_weather\",\"args\":{\"location\":\"Beijing\"}}"
        "}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":20,\"candidatesTokenCount\":10}}";

    char*  oai = NULL;
    size_t olen = 0;
    long   ptok = 0, ctok = 0;
    int    rc =
        provider_gemini_resp_to_openai(gemini_resp, "gemini-1.5-pro", &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    json_t* out = json_loads(oai, 0, NULL);
    json_t* c0 = json_array_get(json_object_get(out, "choices"), 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(c0, "finish_reason")), "tool_calls") == 0,
                "finish_reason=tool_calls");
    json_t* msg = json_object_get(c0, "message");
    json_t* tc_arr = json_object_get(msg, "tool_calls");
    TEST_ASSERT(tc_arr && json_array_size(tc_arr) == 1, "1 tool call");
    json_t* tc0 = json_array_get(tc_arr, 0);
    json_t* fn = json_object_get(tc0, "function");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fn, "name")), "get_weather") == 0, "name");
    const char* args = json_string_value(json_object_get(fn, "arguments"));
    json_t*     pargs = json_loads(args, 0, NULL);
    TEST_ASSERT(
        pargs && strcmp(json_string_value(json_object_get(pargs, "location")), "Beijing") == 0,
        "location arg");

    json_decref(pargs);
    json_decref(out);
    free(oai);
}

/* -------------------------------------- Test 3: tool_result name lookup */
TEST_CASE(test_gemini_tool_result_name_lookup)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":\"Weather in Beijing?\"},"
        "{\"role\":\"assistant\",\"content\":null,"
        "\"tool_calls\":[{\"id\":\"call_001\",\"type\":\"function\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\\\"Beijing\\\"}"
        "\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"call_001\",\"content\":\"Sunny 25C\"}"
        "]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    json_t* contents = json_object_get(out, "contents");
    TEST_ASSERT(json_is_array(contents) && json_array_size(contents) == 3, "3 contents");

    /* Third entry: user role with functionResponse */
    json_t* tr_entry = json_array_get(contents, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr_entry, "role")), "user") == 0, "user");
    json_t* tr_parts = json_object_get(tr_entry, "parts");
    json_t* trp0 = json_array_get(tr_parts, 0);
    json_t* fr = json_object_get(trp0, "functionResponse");
    TEST_ASSERT(fr != NULL, "functionResponse present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fr, "name")), "get_weather") == 0,
                "function name looked up correctly");

    json_decref(out);
    free(body);
}

/* ---------------------------------- Test 4: tool_result name fallback */
TEST_CASE(test_gemini_tool_result_name_missing_fallback)
{
    model_rec_t route = make_gemini_route();
    /* No preceding assistant message — must fall back to tool_call_id */
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":\"Hello\"},"
        "{\"role\":\"tool\",\"tool_call_id\":\"orphan_id\",\"content\":\"result\"}"
        "]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok (no crash on missing id)");

    json_t* out = json_loads(body, 0, NULL);
    json_t* contents = json_object_get(out, "contents");
    json_t* tr_entry = json_array_get(contents, 1);
    json_t* tr_parts = json_object_get(tr_entry, "parts");
    json_t* fr = json_object_get(json_array_get(tr_parts, 0), "functionResponse");
    /* Fallback: name == tool_call_id */
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fr, "name")), "orphan_id") == 0,
                "fallback name = tool_call_id");

    json_decref(out);
    free(body);
}

/* --------------------------------------- Test 5: multi tool_calls response */
TEST_CASE(test_gemini_multi_tool_calls_response)
{
    const char* gemini_resp =
        "{\"candidates\":[{"
        "\"content\":{\"role\":\"model\",\"parts\":["
        "{\"functionCall\":{\"name\":\"fn_a\",\"args\":{\"x\":1}}},"
        "{\"functionCall\":{\"name\":\"fn_b\",\"args\":{\"y\":2}}}"
        "]},"
        "\"finishReason\":\"STOP\"}]}";

    char*  oai = NULL;
    size_t olen = 0;
    long   ptok = 0, ctok = 0;
    int    rc =
        provider_gemini_resp_to_openai(gemini_resp, "gemini-1.5-pro", &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    json_t* out = json_loads(oai, 0, NULL);
    json_t* msg = json_object_get(json_array_get(json_object_get(out, "choices"), 0), "message");
    json_t* tc_arr = json_object_get(msg, "tool_calls");
    TEST_ASSERT(tc_arr && json_array_size(tc_arr) == 2, "2 tool calls");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(
                           json_object_get(json_array_get(tc_arr, 0), "function"), "name")),
                       "fn_a") == 0,
                "first tool=fn_a");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(
                           json_object_get(json_array_get(tc_arr, 1), "function"), "name")),
                       "fn_b") == 0,
                "second tool=fn_b");

    json_decref(out);
    free(oai);
}

/* ---------------------------------- Test 6: SSE functionCall stream */
static char   g_gemini_sse_buf[65536];
static size_t g_gemini_sse_len;

static int
gemini_mock_write(void* impl, const void* data, size_t len, bool fin)
{
    (void)impl;
    (void)fin;
    if (g_gemini_sse_len + len < sizeof g_gemini_sse_buf) {
        memcpy(g_gemini_sse_buf + g_gemini_sse_len, data, len);
        g_gemini_sse_len += len;
        g_gemini_sse_buf[g_gemini_sse_len] = '\0';
    }
    return 0;
}

static int
gemini_mock_set_header(void* impl, const char* k, const char* v)
{
    (void)impl;
    (void)k;
    (void)v;
    return 0;
}

TEST_CASE(test_gemini_sse_function_call_stream)
{
    g_gemini_sse_len = 0;
    g_gemini_sse_buf[0] = '\0';

    aigate_response_ctx rc_ctx;
    memset(&rc_ctx, 0, sizeof rc_ctx);
    rc_ctx.write = gemini_mock_write;
    rc_ctx.set_header = gemini_mock_set_header;

    extern const provider_adapter_t g_provider_gemini;
    stream_bridge_t* bridge = g_provider_gemini.stream_bridge_new(&rc_ctx, "gemini-1.5-pro");
    TEST_ASSERT(bridge != NULL, "bridge created");

    /* Gemini SSE line with functionCall */
    const char* sse_line =
        "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{"
        "\"functionCall\":{\"name\":\"get_weather\","
        "\"args\":{\"location\":\"Shanghai\"}}"
        "}]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":20,\"candidatesTokenCount\":5}}\n\n";

    g_provider_gemini.stream_bridge_feed(bridge, sse_line, strlen(sse_line));
    g_provider_gemini.stream_bridge_finish(bridge);
    g_provider_gemini.stream_bridge_free(bridge);

    TEST_ASSERT(strstr(g_gemini_sse_buf, "tool_calls") != NULL, "SSE output contains tool_calls");
    TEST_ASSERT(strstr(g_gemini_sse_buf, "get_weather") != NULL, "SSE output contains get_weather");
    TEST_ASSERT(strstr(g_gemini_sse_buf, "Shanghai") != NULL, "SSE output contains Shanghai");
}

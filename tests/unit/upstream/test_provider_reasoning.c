/** @file test_provider_reasoning.c
 *  @brief Unit tests for reasoning models and extended thinking budget parsing.
 */
#include "run_tests.h"
#include "provider_adapter.h"
#include <jansson.h>
#include <string.h>

TEST_CASE(test_reasoning_budget_parsing_explicit)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    const char* json_str = "{\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":2048}}";
    json_t* req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int rc = parse_reasoning_config(req, &route, &cfg);
    json_decref(req);

    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.enabled == true, "thinking enabled");
    TEST_ASSERT(cfg.budget_tokens == 2048, "budget_tokens 2048");
}

TEST_CASE(test_reasoning_budget_parsing_effort)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    const char* json_str = "{\"reasoning_effort\":\"medium\"}";
    json_t* req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int rc = parse_reasoning_config(req, &route, &cfg);
    json_decref(req);

    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.enabled == true, "thinking enabled");
    TEST_ASSERT(cfg.budget_tokens == 4096, "medium effort -> 4096 tokens");
    TEST_ASSERT(strcmp(cfg.effort, "medium") == 0, "effort matches medium");
}

TEST_CASE(test_reasoning_budget_parsing_route_fallback)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    route.supports_reasoning = true;
    route.default_thinking_budget = 8192;

    const char* json_str = "{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    json_t* req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int rc = parse_reasoning_config(req, &route, &cfg);
    json_decref(req);

    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.enabled == true, "thinking enabled from route");
    TEST_ASSERT(cfg.budget_tokens == 8192, "fallback to default_thinking_budget");
}

#include "provider_anthropic.h"
#include "provider_gemini.h"

TEST_CASE(test_anthropic_thinking_inbound_max_tokens_autolift)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    snprintf(route.endpoint, sizeof(route.endpoint), "https://api.anthropic.com");
    snprintf(route.upstream_key, sizeof(route.upstream_key), "ant-key-123");

    /* Client sets reasoning_effort: medium (4096 tokens) and max_tokens: 2000 (which is <= 4096) */
    const char* in_json = "{\"model\":\"claude-3-7-sonnet\",\"messages\":[{\"role\":\"user\",\"content\":\"think\"}],\"reasoning_effort\":\"medium\",\"max_tokens\":2000}";
    char url[512];
    const char* hdrs[4][2];
    int nhdrs = 0;
    char* out_body = NULL;
    size_t out_len = 0;

    int rc = provider_anthropic_build(&route, in_json, url, sizeof(url), hdrs, &nhdrs, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "build success");
    TEST_ASSERT(out_body != NULL, "out_body non-null");

    json_t* out = json_loads(out_body, 0, NULL);
    free(out_body);
    TEST_ASSERT(out != NULL, "parsed json valid");

    /* Verify thinking object injected */
    json_t* jth = json_object_get(out, "thinking");
    TEST_ASSERT(jth != NULL && json_is_object(jth), "thinking injected");
    json_t* jbudget = json_object_get(jth, "budget_tokens");
    TEST_ASSERT(jbudget && json_integer_value(jbudget) == 4096, "budget_tokens is 4096");

    /* Verify max_tokens auto-lifted to budget + 4096 = 8192 */
    json_t* jmt = json_object_get(out, "max_tokens");
    TEST_ASSERT(jmt && json_integer_value(jmt) == 8192, "max_tokens autolifted to 8192, got %lld", (long long)json_integer_value(jmt));

    json_decref(out);
}

TEST_CASE(test_gemini_thinking_inbound_budget)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    snprintf(route.endpoint, sizeof(route.endpoint), "https://generativelanguage.googleapis.com");
    snprintf(route.upstream_key, sizeof(route.upstream_key), "gemini-key");

    const char* in_json = "{\"model\":\"gemini-2.0-flash-thinking\",\"messages\":[{\"role\":\"user\",\"content\":\"solve\"}],\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":3000}}";
    char url[512];
    const char* hdrs[4][2];
    int nhdrs = 0;
    char* out_body = NULL;
    size_t out_len = 0;

    int rc = g_provider_gemini.build_chat(&route, in_json, url, sizeof(url), hdrs, &nhdrs, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "gemini build success");
    TEST_ASSERT(out_body != NULL, "out_body non-null");

    json_t* out = json_loads(out_body, 0, NULL);
    free(out_body);
    TEST_ASSERT(out != NULL, "gemini json valid");

    json_t* gcfg = json_object_get(out, "generationConfig");
    TEST_ASSERT(gcfg && json_is_object(gcfg), "generationConfig present");
    json_t* thcfg = json_object_get(gcfg, "thinkingConfig");
    TEST_ASSERT(thcfg && json_is_object(thcfg), "thinkingConfig present");
    json_t* jb = json_object_get(thcfg, "thinkingBudget");
    TEST_ASSERT(jb && json_integer_value(jb) == 3000, "thinkingBudget is 3000");

    json_decref(out);
}

TEST_CASE(test_anthropic_thinking_outbound_sync)
{
    const char* ant_resp =
        "{\"id\":\"msg_12345\","
        "\"type\":\"message\","
        "\"role\":\"assistant\","
        "\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"Step 1: Analyze problem. Step 2: Formulate solution.\"},"
        "{\"type\":\"text\",\"text\":\"The solution is 42.\"}"
        "],"
        "\"stop_reason\":\"end_turn\","
        "\"usage\":{\"input_tokens\":50,\"output_tokens\":120}"
        "}";

    char* out_openai = NULL;
    size_t out_len = 0;
    long ptok = 0, ctok = 0;

    int rc = provider_anthropic_resp_to_openai(ant_resp, "claude-3-7-sonnet", &out_openai, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "translation success");
    TEST_ASSERT(out_openai != NULL, "out_openai non-null");

    json_t* root = json_loads(out_openai, 0, NULL);
    free(out_openai);
    TEST_ASSERT(root != NULL, "json valid");

    json_t* choices = json_object_get(root, "choices");
    TEST_ASSERT(choices && json_is_array(choices), "choices array");
    json_t* c0 = json_array_get(choices, 0);
    json_t* msg = json_object_get(c0, "message");
    TEST_ASSERT(msg != NULL, "message present");

    json_t* jreasoning = json_object_get(msg, "reasoning_content");
    TEST_ASSERT(jreasoning && json_is_string(jreasoning), "reasoning_content present");
    TEST_ASSERT(strcmp(json_string_value(jreasoning), "Step 1: Analyze problem. Step 2: Formulate solution.") == 0,
                "reasoning_content matches thinking text");

    json_t* jcontent = json_object_get(msg, "content");
    TEST_ASSERT(jcontent && json_is_string(jcontent), "content present");
    TEST_ASSERT(strcmp(json_string_value(jcontent), "The solution is 42.") == 0, "content matches text");

    json_decref(root);
}

typedef struct {
    char   accum[8192];
    size_t len;
} mock_stream_sink_t;

static int mock_stream_write(void* ctx, const void* data, size_t len, bool is_final)
{
    (void)is_final;
    mock_stream_sink_t* sink = ctx;
    if (sink->len + len < sizeof(sink->accum)) {
        memcpy(sink->accum + sink->len, data, len);
        sink->len += len;
        sink->accum[sink->len] = '\0';
    }
    return 0;
}

TEST_CASE(test_anthropic_thinking_outbound_stream)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc = {
        .impl = &sink,
        .write = mock_stream_write,
    };

    stream_bridge_t* bridge = g_provider_anthropic.stream_bridge_new(&rc, "claude-3-7-sonnet");
    TEST_ASSERT(bridge != NULL, "bridge created");

    /* 1. Send message_start */
    const char* chunk1 = "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"claude-3-7-sonnet\",\"usage\":{\"input_tokens\":20}}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk1, strlen(chunk1));

    /* 2. Send content_block_start for thinking */
    const char* chunk2 = "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk2, strlen(chunk2));

    /* 3. Send content_block_delta for thinking_delta */
    const char* chunk3 = "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Analyzing query...\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk3, strlen(chunk3));

    /* 4. Send content_block_start for text */
    const char* chunk4 = "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk4, strlen(chunk4));

    /* 5. Send content_block_delta for text_delta */
    const char* chunk5 = "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello world!\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk5, strlen(chunk5));

    g_provider_anthropic.stream_bridge_finish(bridge);
    g_provider_anthropic.stream_bridge_free(bridge);

    /* Verify reasoning_content emitted */
    TEST_ASSERT(strstr(sink.accum, "\"reasoning_content\":\"Analyzing query...\"") != NULL,
                "reasoning_content emitted in SSE chunk");
    /* Verify content emitted */
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Hello world!\"") != NULL,
                "content emitted in SSE chunk");
}

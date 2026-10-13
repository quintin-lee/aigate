/** @file test_provider_reasoning.c
 *  @brief Unit tests for reasoning models and extended thinking budget parsing.
 */
#include "run_tests.h"
#include "provider_adapter.h"
#include "provider_openai.h"
#include "provider_gemini.h"
#include <jansson.h>
#include <string.h>

TEST_CASE(test_reasoning_budget_parsing_explicit)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    const char* json_str = "{\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":2048}}";
    json_t*     req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int                rc = parse_reasoning_config(req, &route, &cfg);
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
    json_t*     req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int                rc = parse_reasoning_config(req, &route, &cfg);
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
    json_t*     req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int                rc = parse_reasoning_config(req, &route, &cfg);
    json_decref(req);

    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.enabled == true, "thinking enabled from route");
    TEST_ASSERT(cfg.budget_tokens == 8192, "fallback to default_thinking_budget");
}

TEST_CASE(test_reasoning_tag_emulation_parsing)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    /* 1. Inbound JSON body contains thinking_mode: "tag" */
    const char* json_str =
        "{\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":2048},\"thinking_mode\":\"tag\"}";
    json_t* req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int                rc = parse_reasoning_config(req, &route, &cfg);
    json_decref(req);

    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.enabled == true, "thinking enabled");
    TEST_ASSERT(cfg.budget_tokens == 2048, "budget 2048");
    TEST_ASSERT(cfg.tag_emulation == true, "tag emulation true");

    /* 2. Inbound JSON body contains thinking_format: "tag" */
    const char* json_str2 = "{\"thinking_format\":\"tag\"}";
    json_t*     req2 = json_loads(json_str2, 0, NULL);
    rc = parse_reasoning_config(req2, &route, &cfg);
    json_decref(req2);
    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.tag_emulation == true, "thinking_format tag detected");
}

TEST_CASE(test_reasoning_tag_emulation_3tier_hierarchy)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    reasoning_config_t cfg;
    json_t*            req_tag = json_pack("{s:s}", "thinking_format", "tag");
    json_t*            req_raw = json_pack("{s:s}", "thinking_format", "raw");
    json_t*            req_none = json_pack("{s:s}", "model", "test-model");

    /* Case 1: Tier 1 (Header "raw") beats Tier 2 (Body "tag") */
    route.tag_emulation_default = true;
    int rc = parse_reasoning_config_ext(req_tag, &route, "raw", &cfg);
    TEST_ASSERT(rc == 0 && cfg.tag_emulation == false, "Header raw overrides body tag");

    /* Case 2: Tier 1 (Header "tag") beats Tier 2 (Body "raw") */
    route.tag_emulation_default = false;
    rc = parse_reasoning_config_ext(req_raw, &route, "tag", &cfg);
    TEST_ASSERT(rc == 0 && cfg.tag_emulation == true, "Header tag overrides body raw");

    /* Case 3: Tier 2 (Body "raw") beats Tier 3 (Route default true) */
    route.tag_emulation_default = true;
    rc = parse_reasoning_config_ext(req_raw, &route, NULL, &cfg);
    TEST_ASSERT(rc == 0 && cfg.tag_emulation == false, "Body raw overrides route true");

    /* Case 4: Tier 2 (Body "tag") beats Tier 3 (Route default false) */
    route.tag_emulation_default = false;
    rc = parse_reasoning_config_ext(req_tag, &route, NULL, &cfg);
    TEST_ASSERT(rc == 0 && cfg.tag_emulation == true, "Body tag overrides route false");

    /* Case 5: Tier 3 fallback when no header and no body format */
    route.tag_emulation_default = true;
    rc = parse_reasoning_config_ext(req_none, &route, NULL, &cfg);
    TEST_ASSERT(rc == 0 && cfg.tag_emulation == true,
                "Route true takes effect when no header/body");

    route.tag_emulation_default = false;
    rc = parse_reasoning_config_ext(req_none, &route, NULL, &cfg);
    TEST_ASSERT(rc == 0 && cfg.tag_emulation == false,
                "Route false takes effect when no header/body");

    json_decref(req_tag);
    json_decref(req_raw);
    json_decref(req_none);
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
    const char* in_json =
        "{\"model\":\"claude-3-7-sonnet\",\"messages\":[{\"role\":\"user\",\"content\":\"think\"}],"
        "\"reasoning_effort\":\"medium\",\"max_tokens\":2000}";
    char        url[512];
    const char* hdrs[4][2];
    int         nhdrs = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = provider_anthropic_build(
        &route, in_json, url, sizeof(url), hdrs, &nhdrs, &out_body, &out_len);
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
    TEST_ASSERT(jmt && json_integer_value(jmt) == 8192,
                "max_tokens autolifted to 8192, got %lld",
                (long long)json_integer_value(jmt));

    json_decref(out);
}

TEST_CASE(test_gemini_thinking_inbound_budget)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    snprintf(route.endpoint, sizeof(route.endpoint), "https://generativelanguage.googleapis.com");
    snprintf(route.upstream_key, sizeof(route.upstream_key), "gemini-key");

    const char* in_json =
        "{\"model\":\"gemini-2.0-flash-thinking\",\"messages\":[{\"role\":\"user\",\"content\":"
        "\"solve\"}],\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":3000}}";
    char        url[512];
    const char* hdrs[4][2];
    int         nhdrs = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = g_provider_gemini.build_chat(
        &route, in_json, url, sizeof(url), hdrs, &nhdrs, &out_body, &out_len);
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
    const char* ant_resp = "{\"id\":\"msg_12345\","
                           "\"type\":\"message\","
                           "\"role\":\"assistant\","
                           "\"content\":["
                           "{\"type\":\"thinking\",\"thinking\":\"Step 1: Analyze problem. Step 2: "
                           "Formulate solution.\"},"
                           "{\"type\":\"text\",\"text\":\"The solution is 42.\"}"
                           "],"
                           "\"stop_reason\":\"end_turn\","
                           "\"usage\":{\"input_tokens\":50,\"output_tokens\":120}"
                           "}";

    char*  out_openai = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0;

    int rc = provider_anthropic_resp_to_openai(
        ant_resp, "claude-3-7-sonnet", &out_openai, &out_len, &ptok, &ctok);
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
    TEST_ASSERT(strcmp(json_string_value(jreasoning),
                       "Step 1: Analyze problem. Step 2: Formulate solution.") == 0,
                "reasoning_content matches thinking text");

    json_t* jcontent = json_object_get(msg, "content");
    TEST_ASSERT(jcontent && json_is_string(jcontent), "content present");
    TEST_ASSERT(strcmp(json_string_value(jcontent), "The solution is 42.") == 0,
                "content matches text");

    json_decref(root);
}

typedef struct {
    char   accum[8192];
    size_t len;
} mock_stream_sink_t;

static int
mock_stream_write(void* ctx, const void* data, size_t len, bool is_final)
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
    const char* chunk1 = "event: message_start\ndata: "
                         "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":"
                         "\"claude-3-7-sonnet\",\"usage\":{\"input_tokens\":20}}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk1, strlen(chunk1));

    /* 2. Send content_block_start for thinking */
    const char* chunk2 = "event: content_block_start\ndata: "
                         "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                         "\"type\":\"thinking\",\"thinking\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk2, strlen(chunk2));

    /* 3. Send content_block_delta for thinking_delta */
    const char* chunk3 = "event: content_block_delta\ndata: "
                         "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                         "\"thinking_delta\",\"thinking\":\"Analyzing query...\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk3, strlen(chunk3));

    /* 4. Send content_block_start for text */
    const char* chunk4 = "event: content_block_start\ndata: "
                         "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{"
                         "\"type\":\"text\",\"text\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, chunk4, strlen(chunk4));

    /* 5. Send content_block_delta for text_delta */
    const char* chunk5 = "event: content_block_delta\ndata: "
                         "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_"
                         "delta\",\"text\":\"Hello world!\"}}\n\n";
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

TEST_CASE(test_anthropic_thinking_stream_tag_emulation)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.write = mock_stream_write;
    rc.impl = &sink;

    stream_bridge_t* bridge = g_provider_anthropic.stream_bridge_new(&rc, "claude-3-7-sonnet");
    TEST_ASSERT(bridge != NULL, "bridge created");
    anthropic_bridge_t* ab = (anthropic_bridge_t*)bridge;
    ab->tag_emulation = true;

    /* Feed content_block_start with thinking */
    const char* c1 = "event: content_block_start\ndata: "
                     "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
                     "\"thinking\",\"thinking\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c1, strlen(c1));

    /* Feed thinking_delta */
    const char* c2 = "event: content_block_delta\ndata: "
                     "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_"
                     "delta\",\"thinking\":\"Reasoning step 1...\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c2, strlen(c2));

    /* Feed content_block_stop */
    const char* c3 =
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c3, strlen(c3));

    /* Feed text content */
    const char* c4 = "event: content_block_start\ndata: "
                     "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
                     "\"text\",\"text\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c4, strlen(c4));
    const char* c5 = "event: content_block_delta\ndata: "
                     "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_"
                     "delta\",\"text\":\"Hello world!\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c5, strlen(c5));

    g_provider_anthropic.stream_bridge_finish(bridge);
    g_provider_anthropic.stream_bridge_free(bridge);

    /* Verify <think> tag emitted in delta.content */
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"<think>\\n\"") != NULL, "<think> open emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Reasoning step 1...\"") != NULL,
                "thinking delta in content");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"\\n</think>\\n\\n\"") != NULL,
                "</think> close emitted");
}

TEST_CASE(test_anthropic_thinking_sync_tag_emulation)
{
    const char* anthropic_resp = "{\"id\":\"msg_tag_1\",\"type\":\"message\",\"role\":"
                                 "\"assistant\",\"model\":\"claude-3-7-sonnet\","
                                 "\"content\":["
                                 "{\"type\":\"thinking\",\"thinking\":\"Step-by-step reasoning.\"},"
                                 "{\"type\":\"text\",\"text\":\"42 is the answer.\"}"
                                 "],"
                                 "\"usage\":{\"input_tokens\":10,\"output_tokens\":20}}";

    char*  out_json = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_anthropic_resp_to_openai_ext(
        anthropic_resp, "claude-3-7-sonnet", true, &out_json, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "translated ok");

    json_t* root = json_loads(out_json, 0, NULL);
    free(out_json);
    TEST_ASSERT(root != NULL, "json valid");

    json_t*     choices = json_object_get(root, "choices");
    json_t*     c0 = json_array_get(choices, 0);
    json_t*     msg = json_object_get(c0, "message");
    const char* content = json_string_value(json_object_get(msg, "content"));
    TEST_ASSERT(
        strstr(content, "<think>\nStep-by-step reasoning.\n</think>\n\n42 is the answer.") != NULL,
        "<think> wrapped content present");

    json_decref(root);
}

TEST_CASE(test_openai_to_anthropic_thinking_sync)
{
    const char* oai_resp =
        "{\"id\":\"chatcmpl-r1\",\"object\":\"chat.completion\",\"model\":\"deepseek-r1\","
        "\"choices\":[{\"index\":0,\"message\":{"
        "\"role\":\"assistant\","
        "\"reasoning_content\":\"Proof of irrationality of sqrt(2).\","
        "\"content\":\"Assume p/q is irreducible...\""
        "},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":100}}";

    char*  ant_resp = NULL;
    size_t ant_len = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_openai_resp_to_anthropic(
        oai_resp, "deepseek-r1", &ant_resp, &ant_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "converted ok");

    json_t* root = json_loads(ant_resp, 0, NULL);
    free(ant_resp);
    TEST_ASSERT(root != NULL, "json valid");

    json_t* content = json_object_get(root, "content");
    TEST_ASSERT(json_is_array(content) && json_array_size(content) == 2, "2 content blocks");

    json_t* b0 = json_array_get(content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b0, "type")), "thinking") == 0,
                "b0 is thinking");
    TEST_ASSERT(strstr(json_string_value(json_object_get(b0, "thinking")),
                       "Proof of irrationality") != NULL,
                "thinking text matches");

    json_t* b1 = json_array_get(content, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b1, "type")), "text") == 0, "b1 is text");
    TEST_ASSERT(strstr(json_string_value(json_object_get(b1, "text")), "Assume p/q") != NULL,
                "answer text matches");

    json_decref(root);
}

TEST_CASE(test_openai_to_anthropic_thinking_stream)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.write = mock_stream_write;
    rc.impl = &sink;

    stream_bridge_t* b = provider_openai_to_anthropic_bridge_new(&rc, "deepseek-r1");
    TEST_ASSERT(b != NULL, "bridge created");

    /* Feed OpenAI chunk with reasoning_content */
    const char* chunk1 =
        "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"model\":\"deepseek-r1\","
        "\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Solving "
        "equation...\"},\"finish_reason\":null}]}\n\n";
    provider_openai_to_anthropic_bridge_feed(b, chunk1, strlen(chunk1));

    /* Feed OpenAI chunk with final content */
    const char* chunk2 =
        "data: {\"id\":\"c2\",\"object\":\"chat.completion.chunk\",\"model\":\"deepseek-r1\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x = "
        "5\"},\"finish_reason\":\"stop\"}]}\n\n";
    provider_openai_to_anthropic_bridge_feed(b, chunk2, strlen(chunk2));

    provider_openai_to_anthropic_bridge_finish(b);
    provider_openai_to_anthropic_bridge_free(b);

    /* Verify Anthropic SSE events */
    TEST_ASSERT(strstr(sink.accum, "event: content_block_start") != NULL, "block start emitted");
    TEST_ASSERT(strstr(sink.accum, "\"type\":\"thinking\"") != NULL, "thinking block start");
    TEST_ASSERT(strstr(sink.accum, "event: content_block_delta") != NULL, "delta emitted");
    TEST_ASSERT(strstr(sink.accum, "\"type\":\"thinking_delta\"") != NULL, "thinking delta");
    TEST_ASSERT(strstr(sink.accum, "Solving equation...") != NULL, "thinking text inside delta");
    TEST_ASSERT(strstr(sink.accum, "\"type\":\"text_delta\"") != NULL, "text delta emitted");
    TEST_ASSERT(strstr(sink.accum, "x = 5") != NULL, "text content emitted");
}

#include "watermark_engine.h"

TEST_CASE(test_thinking_watermark_clean_pass)
{
    /* Construct response with reasoning_content and content */
    const char* resp_json = "{\"id\":\"chatcmpl-wm-test\",\"object\":\"chat.completion\","
                            "\"choices\":[{\"index\":0,\"message\":{"
                            "\"role\":\"assistant\","
                            "\"reasoning_content\":\"Detailed math proof: 2 + 2 = 4.\","
                            "\"content\":\"The answer is four.\""
                            "}}]}";

    json_t* root = json_loads(resp_json, 0, NULL);
    TEST_ASSERT(root != NULL, "json valid");

    /* Watermark payload */
    watermark_payload_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.timestamp = 1728000000;
    wp.key_id = 42;
    wp.short_trace = 0x12345678ULL;

    /* Inject watermark into content */
    json_t*     choices = json_object_get(root, "choices");
    json_t*     c0 = json_array_get(choices, 0);
    json_t*     msg = json_object_get(c0, "message");
    json_t*     content_val = json_object_get(msg, "content");
    const char* raw_txt = json_string_value(content_val);
    size_t      wm_sz = 0;
    char*       wm_txt = watermark_inject(raw_txt, strlen(raw_txt), &wp, &wm_sz);
    TEST_ASSERT(wm_txt != NULL, "watermark injected into content");
    json_object_set_new(msg, "content", json_string(wm_txt));
    free(wm_txt);

    /* Verify reasoning_content remains completely untouched without zero-width marks */
    json_t*     jreasoning = json_object_get(msg, "reasoning_content");
    const char* rtxt = json_string_value(jreasoning);
    TEST_ASSERT(strstr(rtxt, "\xe2\x80\x8b") == NULL, "zero-width space not in reasoning");
    TEST_ASSERT(strstr(rtxt, "\xe2\x80\x8c") == NULL, "zero-width non-joiner not in reasoning");
    TEST_ASSERT(strcmp(rtxt, "Detailed math proof: 2 + 2 = 4.") == 0, "reasoning matches original");

    json_decref(root);
}

TEST_CASE(test_gemini_thinking_inbound_budget_zero)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    snprintf(route.endpoint, sizeof(route.endpoint), "https://generativelanguage.googleapis.com");
    snprintf(route.upstream_key, sizeof(route.upstream_key), "gemini-key");
    route.supports_reasoning = true;

    const char* in_json =
        "{\"model\":\"gemini-2.0-flash\",\"messages\":[{\"role\":\"user\",\"content\":\"solve\"}],"
        "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":0}}";
    char        url[512];
    const char* hdrs[4][2];
    int         nhdrs = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = g_provider_gemini.build_chat(
        &route, in_json, url, sizeof(url), hdrs, &nhdrs, &out_body, &out_len);
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
    TEST_ASSERT(jb && json_integer_value(jb) == 0, "thinkingBudget is 0");

    json_decref(out);
}

TEST_CASE(test_gemini_thinking_sync_tag_emulation)
{
    const char* gemini_resp =
        "{\"candidates\":[{"
        "\"content\":{\"parts\":["
        "{\"text\":\"Solving math problem step-by-step...\",\"thought\":true},"
        "{\"text\":\"The solution is x = 42.\"}"
        "]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":25,"
        "\"thoughtsTokenCount\":15}}";

    char*  out_json = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_gemini_resp_to_openai_ext(
        gemini_resp, "gemini-2.0-flash", true, &out_json, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "gemini resp translated ok");
    TEST_ASSERT(out_json != NULL, "out_json not null");

    json_t* root = json_loads(out_json, 0, NULL);
    free(out_json);
    TEST_ASSERT(root != NULL, "json parsed ok");

    json_t*     choices = json_object_get(root, "choices");
    json_t*     c0 = json_array_get(choices, 0);
    json_t*     msg = json_object_get(c0, "message");
    const char* content = json_string_value(json_object_get(msg, "content"));
    TEST_ASSERT(content != NULL, "content present");
    TEST_ASSERT(strstr(content,
                       "<think>\nSolving math problem step-by-step...\n</think>\n\nThe "
                       "solution is x = 42.") != NULL,
                "<think> wrapped content present in tag emulation");

    /* reasoning_content is also preserved in sync message for dual compatibility */
    json_t* jreasoning = json_object_get(msg, "reasoning_content");
    TEST_ASSERT(jreasoning != NULL && json_is_string(jreasoning),
                "reasoning_content present for dual compatibility");

    /* Check reasoning tokens in usage */
    json_t* usage = json_object_get(root, "usage");
    TEST_ASSERT(usage != NULL, "usage object present");
    json_t* ctd = json_object_get(usage, "completion_tokens_details");
    TEST_ASSERT(ctd != NULL, "completion_tokens_details present");
    json_t* jrtok = json_object_get(ctd, "reasoning_tokens");
    TEST_ASSERT(jrtok && json_integer_value(jrtok) == 15, "reasoning_tokens is 15");

    json_decref(root);
}

TEST_CASE(test_gemini_thinking_raw_reasoning_content)
{
    const char* gemini_resp = "{\"candidates\":[{"
                              "\"content\":{\"parts\":["
                              "{\"text\":\"Evaluating step 1...\",\"thought\":true},"
                              "{\"text\":\"Evaluating step 2...\",\"thought\":true},"
                              "{\"text\":\"Final answer is 100.\"}"
                              "]},\"finishReason\":\"STOP\"}],"
                              "\"usageMetadata\":{\"promptTokenCount\":12,\"candidatesTokenCount\":"
                              "30,\"thoughtsTokenCount\":18}}";

    char*  out_json = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_gemini_resp_to_openai_ext(
        gemini_resp, "gemini-2.0-flash", false, &out_json, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "gemini resp translated ok");
    TEST_ASSERT(out_json != NULL, "out_json not null");

    json_t* root = json_loads(out_json, 0, NULL);
    free(out_json);
    TEST_ASSERT(root != NULL, "json parsed ok");

    json_t*     choices = json_object_get(root, "choices");
    json_t*     c0 = json_array_get(choices, 0);
    json_t*     msg = json_object_get(c0, "message");
    const char* content = json_string_value(json_object_get(msg, "content"));
    const char* rcontent = json_string_value(json_object_get(msg, "reasoning_content"));

    TEST_ASSERT(content != NULL && strcmp(content, "Final answer is 100.") == 0, "content correct");
    TEST_ASSERT(rcontent != NULL &&
                    strcmp(rcontent, "Evaluating step 1...Evaluating step 2...") == 0,
                "reasoning_content correct");

    json_t* usage = json_object_get(root, "usage");
    json_t* ctd = json_object_get(usage, "completion_tokens_details");
    json_t* jrtok = json_object_get(ctd, "reasoning_tokens");
    TEST_ASSERT(jrtok && json_integer_value(jrtok) == 18, "reasoning_tokens is 18");

    json_decref(root);
}

TEST_CASE(test_gemini_thinking_stream_tag_emulation)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.write = mock_stream_write;
    rc.impl = &sink;

    stream_bridge_t* bridge = g_provider_gemini.stream_bridge_new(&rc, "gemini-2.0-flash");
    TEST_ASSERT(bridge != NULL, "bridge created");
    gemini_bridge_t* gb = (gemini_bridge_t*)bridge;
    gb->tag_emulation = true;

    /* Feed thinking chunk 1 */
    const char* c1 = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Deep reasoning "
                     "starts...\",\"thought\":true}]}}]}\n\n";
    g_provider_gemini.stream_bridge_feed(bridge, c1, strlen(c1));

    /* Feed thinking chunk 2 */
    const char* c2 = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\" continuing "
                     "proof.\",\"thought\":true}]}}]}\n\n";
    g_provider_gemini.stream_bridge_feed(bridge, c2, strlen(c2));

    /* Feed answer chunk with usage */
    const char* c3 =
        "data: "
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Conclusion: "
        "Q.E.D.\"}]}}],\"finishReason\":\"STOP\",\"usageMetadata\":{\"promptTokenCount\":10,"
        "\"candidatesTokenCount\":25,\"thoughtsTokenCount\":14}}\n\n";
    g_provider_gemini.stream_bridge_feed(bridge, c3, strlen(c3));

    g_provider_gemini.stream_bridge_finish(bridge);

    long ptok = 0, ctok = 0, cached_tok = 0, rtok = 0;
    provider_gemini_bridge_get_tokens(bridge, &ptok, &ctok, &cached_tok, &rtok);
    g_provider_gemini.stream_bridge_free(bridge);

    TEST_ASSERT(ptok == 10, "prompt tokens 10");
    TEST_ASSERT(ctok == 25, "candidates tokens 25");
    TEST_ASSERT(rtok == 14, "reasoning tokens 14");

    /* Verify <think> tag emitted in delta.content */
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"<think>\\n\"") != NULL, "<think> open emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Deep reasoning starts...\"") != NULL,
                "thinking chunk 1 in content");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\" continuing proof.\"") != NULL,
                "thinking chunk 2 in content");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"\\n</think>\\n\\n\"") != NULL,
                "</think> close emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Conclusion: Q.E.D.\"") != NULL,
                "answer text in content");
    TEST_ASSERT(strstr(sink.accum, "data: [DONE]") != NULL, "[DONE] emitted");
}

TEST_CASE(test_gemini_thinking_stream_raw_reasoning_content)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.write = mock_stream_write;
    rc.impl = &sink;

    stream_bridge_t* bridge = g_provider_gemini.stream_bridge_new(&rc, "gemini-2.0-flash");
    TEST_ASSERT(bridge != NULL, "bridge created");
    gemini_bridge_t* gb = (gemini_bridge_t*)bridge;
    gb->tag_emulation = false;

    /* Feed thinking chunk */
    const char* c1 = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hypothesis "
                     "check...\",\"thought\":true}]}}]}\n\n";
    g_provider_gemini.stream_bridge_feed(bridge, c1, strlen(c1));

    /* Feed answer chunk */
    const char* c2 =
        "data: "
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Valid.\"}]}}],\"finishReason\":"
        "\"STOP\"}\n\n";
    g_provider_gemini.stream_bridge_feed(bridge, c2, strlen(c2));

    g_provider_gemini.stream_bridge_finish(bridge);
    g_provider_gemini.stream_bridge_free(bridge);

    /* Verify reasoning_content and regular content emitted separately without tags */
    TEST_ASSERT(strstr(sink.accum, "\"reasoning_content\":\"Hypothesis check...\"") != NULL,
                "reasoning_content emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Valid.\"") != NULL, "regular content emitted");
    TEST_ASSERT(strstr(sink.accum, "<think>") == NULL, "no <think> tag emitted in raw mode");
    TEST_ASSERT(strstr(sink.accum, "</think>") == NULL, "no </think> tag emitted in raw mode");
}

TEST_CASE(test_claude_37_128k_budget_parsing)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    /* 128k budget */
    const char*        j1 = "{\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":128000}}";
    json_t*            r1 = json_loads(j1, 0, NULL);
    reasoning_config_t cfg1;
    int                rc = parse_reasoning_config(r1, &route, &cfg1);
    json_decref(r1);
    TEST_ASSERT(rc == 0 && cfg1.enabled && cfg1.budget_tokens == 128000, "128k parsed ok");

    /* Clamped over 128k */
    const char*        j2 = "{\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":200000}}";
    json_t*            r2 = json_loads(j2, 0, NULL);
    reasoning_config_t cfg2;
    rc = parse_reasoning_config(r2, &route, &cfg2);
    json_decref(r2);
    TEST_ASSERT(rc == 0 && cfg2.enabled && cfg2.budget_tokens == 128000,
                "over-128k clamped to 128000");

    /* Disabled */
    const char*        j3 = "{\"thinking\":{\"type\":\"disabled\"}}";
    json_t*            r3 = json_loads(j3, 0, NULL);
    reasoning_config_t cfg3;
    rc = parse_reasoning_config(r3, &route, &cfg3);
    json_decref(r3);
    TEST_ASSERT(rc == 0 && !cfg3.enabled && cfg3.budget_tokens == 0, "thinking disabled");

    /* Tag emulation direct boolean in body */
    const char*        j4 = "{\"tag_emulation\":true}";
    json_t*            r4 = json_loads(j4, 0, NULL);
    reasoning_config_t cfg4;
    rc = parse_reasoning_config(r4, &route, &cfg4);
    json_decref(r4);
    TEST_ASSERT(rc == 0 && cfg4.tag_emulation == true, "tag_emulation boolean supported");
}

TEST_CASE(test_claude_37_signature_delta_stream)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc = {
        .impl = &sink,
        .write = mock_stream_write,
    };

    stream_bridge_t* bridge = g_provider_anthropic.stream_bridge_new(&rc, "claude-3-7-sonnet");
    TEST_ASSERT(bridge != NULL, "bridge created");
    anthropic_bridge_t* ab = (anthropic_bridge_t*)bridge;
    ab->tag_emulation = false;

    /* 1. message_start */
    const char* c1 = "event: message_start\ndata: "
                     "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_sig\",\"model\":"
                     "\"claude-3-7-sonnet\",\"usage\":{\"input_tokens\":15}}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c1, strlen(c1));

    /* 2. content_block_start thinking */
    const char* c2 = "event: content_block_start\ndata: "
                     "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
                     "\"thinking\",\"thinking\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c2, strlen(c2));

    /* 3. thinking_delta */
    const char* c3 = "event: content_block_delta\ndata: "
                     "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_"
                     "delta\",\"thinking\":\"Calculating steps...\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c3, strlen(c3));

    /* 4. signature_delta */
    const char* c4 = "event: content_block_delta\ndata: "
                     "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                     "\"signature_delta\",\"signature\":\"sig_claude37_proof\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c4, strlen(c4));

    g_provider_anthropic.stream_bridge_finish(bridge);
    g_provider_anthropic.stream_bridge_free(bridge);

    /* Verify reasoning_content and reasoning_signature emitted */
    TEST_ASSERT(strstr(sink.accum, "\"reasoning_content\":\"Calculating steps...\"") != NULL,
                "reasoning_content emitted");
    TEST_ASSERT(strstr(sink.accum, "\"reasoning_signature\":\"sig_claude37_proof\"") != NULL,
                "reasoning_signature emitted");
}

TEST_CASE(test_claude_37_redacted_thinking)
{
    /* Non-streaming test with redacted_thinking */
    const char* ant_resp = "{\"id\":\"msg_redacted\","
                           "\"type\":\"message\","
                           "\"role\":\"assistant\","
                           "\"content\":["
                           "{\"type\":\"redacted_thinking\",\"data\":\"encrypted_blob\"},"
                           "{\"type\":\"text\",\"text\":\"Safe answer.\"}"
                           "],"
                           "\"usage\":{\"input_tokens\":20,\"output_tokens\":30}"
                           "}";

    char*  out_openai = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0;
    int    rc = provider_anthropic_resp_to_openai(
        ant_resp, "claude-3-7-sonnet", &out_openai, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "translation success");
    TEST_ASSERT(strstr(out_openai, "[redacted thinking]") != NULL, "redacted thinking captured");
    free(out_openai);

    /* Streaming test with redacted_thinking and tag emulation */
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc_stream = {
        .impl = &sink,
        .write = mock_stream_write,
    };
    stream_bridge_t* bridge =
        g_provider_anthropic.stream_bridge_new(&rc_stream, "claude-3-7-sonnet");
    anthropic_bridge_t* ab = (anthropic_bridge_t*)bridge;
    ab->tag_emulation = true;

    const char* c_red = "event: content_block_start\ndata: "
                        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
                        "\"redacted_thinking\",\"data\":\"secret\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c_red, strlen(c_red));
    g_provider_anthropic.stream_bridge_finish(bridge);
    g_provider_anthropic.stream_bridge_free(bridge);

    TEST_ASSERT(strstr(sink.accum, "<think>") != NULL, "<think> emitted for redacted thinking");
    TEST_ASSERT(strstr(sink.accum, "[redacted thinking]") != NULL, "[redacted thinking] emitted");
}

TEST_CASE(test_claude_37_degraded_suppression)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    snprintf(route.endpoint, sizeof(route.endpoint), "https://api.anthropic.com");

    /* Degraded budget (0 tokens) */
    const char* in_json =
        "{\"model\":\"claude-3-7-sonnet\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
        "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":0}}";
    char        url[512];
    const char* hdrs[4][2];
    int         nhdrs = 0;
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = provider_anthropic_build(
        &route, in_json, url, sizeof(url), hdrs, &nhdrs, &out_body, &out_len);
    TEST_ASSERT(rc == 0, "build success");
    TEST_ASSERT(out_body != NULL, "out_body non-null");

    json_t* out = json_loads(out_body, 0, NULL);
    free(out_body);
    TEST_ASSERT(out != NULL, "parsed json valid");

    /* Verify thinking object is suppressed (NULL) */
    json_t* jth = json_object_get(out, "thinking");
    TEST_ASSERT(jth == NULL, "thinking object suppressed when budget is 0 / degraded");

    json_decref(out);
}

TEST_CASE(test_openai_reasoning_effort_mapping)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));
    snprintf(route.endpoint, sizeof(route.endpoint), "https://api.openai.com");
    snprintf(route.name, sizeof(route.name), "o1-mini");
    route.supports_reasoning = true;

    /* 1. Request with reasoning_effort and max_tokens on o1 model */
    const char* in_json = "{\"model\":\"o1-mini\",\"messages\":[{\"role\":\"user\",\"content\":"
                          "\"think\"}],\"reasoning_effort\":\"high\",\"max_tokens\":5000}";
    char        url[512];
    char*       out_body = NULL;
    size_t      out_len = 0;

    int rc = provider_openai_build(
        &route, "/chat/completions", in_json, url, sizeof(url), &out_body, &out_len);
    TEST_ASSERT(rc == 0, "build success");
    TEST_ASSERT(out_body != NULL, "out_body non-null");

    json_t* out = json_loads(out_body, 0, NULL);
    free(out_body);
    TEST_ASSERT(out != NULL, "json valid");

    json_t* jeff = json_object_get(out, "reasoning_effort");
    TEST_ASSERT(jeff && strcmp(json_string_value(jeff), "high") == 0,
                "reasoning_effort high preserved");

    /* max_tokens must be remapped to max_completion_tokens */
    json_t* jmct = json_object_get(out, "max_completion_tokens");
    TEST_ASSERT(jmct && json_integer_value(jmct) == 5000, "max_completion_tokens is 5000");
    TEST_ASSERT(json_object_get(out, "max_tokens") == NULL, "max_tokens remapped and removed");

    json_decref(out);

    /* 2. Degraded request: reasoning_effort should be stripped */
    const char* deg_json =
        "{\"model\":\"o1-mini\",\"messages\":[{\"role\":\"user\",\"content\":\"think\"}],"
        "\"reasoning_effort\":\"high\",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":0}}";
    rc = provider_openai_build(
        &route, "/chat/completions", deg_json, url, sizeof(url), &out_body, &out_len);
    TEST_ASSERT(rc == 0, "build success");
    out = json_loads(out_body, 0, NULL);
    free(out_body);
    TEST_ASSERT(out != NULL, "json valid");
    TEST_ASSERT(json_object_get(out, "reasoning_effort") == NULL,
                "reasoning_effort stripped when degraded");
    json_decref(out);
}

TEST_CASE(test_openai_stream_reasoning_tag_emulation)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc = {
        .impl = &sink,
        .write = mock_stream_write,
    };

    stream_bridge_t* bridge = g_provider_openai.stream_bridge_new(&rc, "o3-mini");
    TEST_ASSERT(bridge != NULL, "bridge created");
    openai_bridge_t* ob = (openai_bridge_t*)bridge;
    ob->tag_emulation = true;

    /* 1. Reasoning chunk */
    const char* c1 = "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Solving "
                     "equation...\"},\"finish_reason\":null}]}\n\n";
    g_provider_openai.stream_bridge_feed(bridge, c1, strlen(c1));

    /* 2. Content chunk */
    const char* c2 = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x = "
                     "5\"},\"finish_reason\":null}]}\n\n";
    g_provider_openai.stream_bridge_feed(bridge, c2, strlen(c2));

    /* 3. Finish chunk */
    const char* c3 =
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    g_provider_openai.stream_bridge_feed(bridge, c3, strlen(c3));

    g_provider_openai.stream_bridge_finish(bridge);
    g_provider_openai.stream_bridge_free(bridge);

    /* Verify <think> wrapper */
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"<think>\\n\"") != NULL, "<think> open emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Solving equation...\"") != NULL,
                "reasoning text in content");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"\\n</think>\\n\\n\"") != NULL,
                "</think> close emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"x = 5\"") != NULL, "answer text in content");
}

TEST_CASE(test_openai_stream_fallback_thought)
{
    mock_stream_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc = {
        .impl = &sink,
        .write = mock_stream_write,
    };

    stream_bridge_t* bridge = g_provider_openai.stream_bridge_new(&rc, "deepseek-r1");
    TEST_ASSERT(bridge != NULL, "bridge created");
    openai_bridge_t* ob = (openai_bridge_t*)bridge;
    ob->tag_emulation = false; /* raw mode */

    /* Chunk with delta.thought instead of delta.reasoning_content */
    const char* c1 = "data: {\"choices\":[{\"index\":0,\"delta\":{\"thought\":\"Internal thought "
                     "process\"},\"finish_reason\":null}]}\n\n";
    g_provider_openai.stream_bridge_feed(bridge, c1, strlen(c1));

    g_provider_openai.stream_bridge_finish(bridge);
    g_provider_openai.stream_bridge_free(bridge);

    /* Verify normalized to delta.reasoning_content */
    TEST_ASSERT(strstr(sink.accum, "\"reasoning_content\":\"Internal thought process\"") != NULL,
                "thought normalized to reasoning_content");
}

TEST_CASE(test_openai_sync_reasoning_tag_emulation)
{
    const char* in_resp =
        "{\"id\":\"chatcmpl-sync-test\",\"object\":\"chat.completion\",\"model\":\"o1-mini\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Result is "
        "100.\",\"reasoning_content\":\"Calculating 10*10=100.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":25,\"total_tokens\":35}}";

    char*  out_body = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0, cached = 0;

    int rc = provider_openai_resp_format_tag_emulation(
        in_resp, strlen(in_resp), "o1-mini", &out_body, &out_len, &ptok, &ctok, &cached);
    TEST_ASSERT(rc == 0, "formatting success");
    TEST_ASSERT(out_body != NULL, "out_body non-null");

    json_t* root = json_loads(out_body, 0, NULL);
    free(out_body);
    TEST_ASSERT(root != NULL, "json valid");

    json_t*     choices = json_object_get(root, "choices");
    json_t*     c0 = json_array_get(choices, 0);
    json_t*     msg = json_object_get(c0, "message");
    const char* content = json_string_value(json_object_get(msg, "content"));

    TEST_ASSERT(strstr(content, "<think>\nCalculating 10*10=100.\n</think>\n\nResult is 100.") !=
                    NULL,
                "content formatted with <think> tag emulation");
    json_decref(root);
}

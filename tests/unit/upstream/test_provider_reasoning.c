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

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

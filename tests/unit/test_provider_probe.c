/** @file test_provider_probe.c
 *  @brief P1-4 provider_probe_plan family mapping + upstream_probe transport. */
#include "run_tests.h"
#include "provider_adapter.h"
#include "upstream_client.h"
#include "mock_upstream.h"

#include <string.h>

/* --- provider_probe_plan: openai family --- */
TEST_CASE(test_probe_plan_openai_family)
{
    static const char* family[] = {"openai", "ollama", "azure",
                                   "deepseek", "siliconflow", "vllm"};
    provider_probe_plan_t plan;
    for (size_t i = 0; i < sizeof family / sizeof family[0]; i++) {
        TEST_ASSERT(provider_probe_plan(family[i], "http://api.openai.com", &plan) == 0,
                    "%s maps", family[i]);
        TEST_ASSERT(strcmp(plan.url, "http://api.openai.com/models") == 0, "url");
        TEST_ASSERT(strcmp(plan.auth_header, "Authorization") == 0, "auth hdr");
        TEST_ASSERT(plan.bearer == 1, "bearer");
        TEST_ASSERT(plan.extra_header[0] == '\0', "no extra hdr");
    }
}

/* --- provider_probe_plan: anthropic /v1 suffix rule mirrors /messages --- */
TEST_CASE(test_probe_plan_anthropic_suffixes)
{
    provider_probe_plan_t plan;

    TEST_ASSERT(provider_probe_plan("anthropic", "http://x", &plan) == 0, "bare");
    TEST_ASSERT(strcmp(plan.url, "http://x/v1/models") == 0, "bare -> /v1/models");
    TEST_ASSERT(strcmp(plan.auth_header, "x-api-key") == 0, "x-api-key");
    TEST_ASSERT(plan.bearer == 0, "no bearer");
    TEST_ASSERT(strcmp(plan.extra_header, "anthropic-version") == 0, "version hdr");

    TEST_ASSERT(provider_probe_plan("anthropic", "http://x/v1", &plan) == 0, "/v1");
    TEST_ASSERT(strcmp(plan.url, "http://x/v1/models") == 0, "/v1 -> /v1/models");

    TEST_ASSERT(provider_probe_plan("anthropic", "http://x/v1/", &plan) == 0, "/v1/");
    TEST_ASSERT(strcmp(plan.url, "http://x/v1/models") == 0, "/v1/ -> /v1/models");
}

/* --- provider_probe_plan: gemini family --- */
TEST_CASE(test_probe_plan_gemini)
{
    provider_probe_plan_t plan;
    for (int i = 0; i < 2; i++) {
        const char* type = i == 0 ? "gemini" : "google";
        TEST_ASSERT(provider_probe_plan(type, "https://generativelanguage.googleapis.com", &plan) == 0,
                    "%s maps", type);
        TEST_ASSERT(strcmp(plan.url, "https://generativelanguage.googleapis.com/v1beta/models") == 0,
                    "url");
        TEST_ASSERT(strcmp(plan.auth_header, "x-goog-api-key") == 0, "auth hdr");
        TEST_ASSERT(plan.bearer == 0, "no bearer");
    }
}

/* --- provider_probe_plan: error paths --- */
TEST_CASE(test_probe_plan_errors)
{
    provider_probe_plan_t plan;
    TEST_ASSERT(provider_probe_plan("vertex", "http://x", &plan) != 0, "unknown type");
    TEST_ASSERT(provider_probe_plan("openai", "", &plan) != 0, "empty endpoint");
    TEST_ASSERT(provider_probe_plan("openai", NULL, &plan) != 0, "NULL endpoint");

    char huge[2000];
    memset(huge, 'a', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    TEST_ASSERT(provider_probe_plan("openai", huge, &plan) != 0, "truncating endpoint");
}

/* --- upstream_probe via the mock (mapping + transport composite) --- */
TEST_CASE(test_probe_transport_ok)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");

    char url[128];
    snprintf(url, sizeof url, "%s/models", mock_upstream_base(mu));

    int  us = 0;
    long lat_ns = -1;
    int  rc = upstream_probe(url, "Authorization", "Bearer sk-test", NULL, NULL,
                             5000L, &us, &lat_ns);
    TEST_ASSERT(rc == 0, "rc %d", rc);
    TEST_ASSERT(us == 200, "status %d", us);
    TEST_ASSERT(lat_ns >= 0, "latency %ld", lat_ns);
    TEST_ASSERT(strcmp(mock_upstream_last_path(mu), "/models") == 0, "GET /models");

    mock_upstream_stop(mu);
}

TEST_CASE(test_probe_transport_key_invalid)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    mock_upstream_status(mu, 401);

    char url[128];
    snprintf(url, sizeof url, "%s/models", mock_upstream_base(mu));

    int us = 0;
    int rc = upstream_probe(url, "x-api-key", "sk-bad", "anthropic-version", "2023-06-01",
                            5000L, &us, NULL);
    TEST_ASSERT(rc == 0, "rc %d", rc);
    TEST_ASSERT(us == 401, "status %d", us); /* maps to verdict key_invalid */

    mock_upstream_stop(mu);
}

TEST_CASE(test_probe_transport_unreachable)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    char base[128];
    snprintf(base, sizeof base, "%s", mock_upstream_base(mu));
    mock_upstream_stop(mu);

    /* closed port -> transport failure, not timeout */
    char url[160];
    snprintf(url, sizeof url, "%s/models", base);
    int us = 42;
    int rc = upstream_probe(url, NULL, NULL, NULL, NULL, 3000L, &us, NULL);
    TEST_ASSERT(rc == -502, "rc %d", rc);
    TEST_ASSERT(us == 0, "status left 0");
}

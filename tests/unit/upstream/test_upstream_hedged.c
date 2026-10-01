/** @file test_upstream_hedged.c
 *  @brief Unit tests for upstream_hedged execution engine.
 */
#include "latency_tracker.h"
#include "mock_upstream.h"
#include "run_tests.h"
#include "upstream_hedged.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_hedged_params_validation(void)
{
    hedged_call_result_t res;
    TEST_ASSERT(upstream_call_hedged(NULL, &res) == -1, "null params should return -1");

    hedged_call_params_t params;
    memset(&params, 0, sizeof(params));
    TEST_ASSERT(upstream_call_hedged(&params, NULL) == -1, "null result should return -1");
}

static void
test_hedged_primary_fast_success(void)
{
    mock_upstream_t* mu1 = mock_upstream_start();
    mock_upstream_t* mu2 = mock_upstream_start();
    TEST_ASSERT(mu1 != NULL && mu2 != NULL, "mock servers started");

    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    hedged_call_params_t params;
    memset(&params, 0, sizeof(params));
    params.model = "test-fast";
    snprintf(params.primary.url,
             sizeof(params.primary.url),
             "%s/v1/chat/completions",
             mock_upstream_base(mu1));
    snprintf(params.secondary.url,
             sizeof(params.secondary.url),
             "%s/v1/chat/completions",
             mock_upstream_base(mu2));
    params.has_secondary = true;
    params.payload = "{\"model\":\"test-fast\",\"messages\":[]}";
    params.payload_len = strlen(params.payload);
    params.delay_ms = 400; /* Primary should finish long before 400ms */
    params.timeout_ms = 2000;
    params.lt = lt;

    hedged_call_result_t res;
    int                  rc = upstream_call_hedged(&params, &res);
    TEST_ASSERT(rc == 0, "upstream_call_hedged rc should be 0");
    TEST_ASSERT(res.status == 200, "status should be 200, got %d", res.status);
    TEST_ASSERT(res.winning_target_idx == 0, "primary should win (idx 0)");
    TEST_ASSERT(res.was_hedged == false, "should not have hedged since primary was fast");
    TEST_ASSERT(res.body != NULL, "body should not be null");

    free(res.body);
    latency_tracker_destroy(lt);
    mock_upstream_stop(mu1);
    mock_upstream_stop(mu2);
}

static void
test_hedged_secondary_wins_when_primary_dead(void)
{
    mock_upstream_t* mu2 = mock_upstream_start();
    TEST_ASSERT(mu2 != NULL, "mock server 2 started");

    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    hedged_call_params_t params;
    memset(&params, 0, sizeof(params));
    params.model = "test-dead-primary";
    /* Primary points to an unreachable port */
    snprintf(
        params.primary.url, sizeof(params.primary.url), "http://127.0.0.1:1/v1/chat/completions");
    snprintf(params.secondary.url,
             sizeof(params.secondary.url),
             "%s/v1/chat/completions",
             mock_upstream_base(mu2));
    params.has_secondary = true;
    params.payload = "{\"model\":\"test-dead-primary\",\"messages\":[]}";
    params.payload_len = strlen(params.payload);
    params.delay_ms = 50; /* 50ms delay to hedge quickly */
    params.timeout_ms = 2000;
    params.lt = lt;

    hedged_call_result_t res;
    int                  rc = upstream_call_hedged(&params, &res);
    TEST_ASSERT(rc == 0, "upstream_call_hedged rc should be 0");
    TEST_ASSERT(res.status == 200, "status should be 200");
    TEST_ASSERT(res.winning_target_idx == 1, "secondary should win (idx 1)");
    TEST_ASSERT(res.body != NULL, "body should not be null");

    free(res.body);
    latency_tracker_destroy(lt);
    mock_upstream_stop(mu2);
}

void
test_upstream_hedged_suite(void)
{
    test_hedged_params_validation();
    test_hedged_primary_fast_success();
    test_hedged_secondary_wins_when_primary_dead();
}

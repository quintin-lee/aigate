/** @file test_model_router_adaptive.c
 *  @brief Unit tests for adaptive latency routing (latency_p95 and dynamic_weighted).
 */
#include "latency_tracker.h"
#include "model_router.h"
#include "run_tests.h"
#include <string.h>

TEST_CASE(adaptive_routing_latency_p95)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    model_rec_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "test-p95-model");
    snprintf(m.lb_policy, sizeof(m.lb_policy), "latency_p95");
    m.n_targets = 2;

    /* Target 0: slow endpoint */
    snprintf(m.targets[0].provider, sizeof(m.targets[0].provider), "openai");
    snprintf(m.targets[0].endpoint, sizeof(m.targets[0].endpoint), "http://slow.local");
    m.targets[0].weight = 1;
    m.targets[0].priority = 0;

    /* Target 1: fast endpoint */
    snprintf(m.targets[1].provider, sizeof(m.targets[1].provider), "openai");
    snprintf(m.targets[1].endpoint, sizeof(m.targets[1].endpoint), "http://fast.local");
    m.targets[1].weight = 1;
    m.targets[1].priority = 0;

    /* Feed latencies: slow gets 800ms, fast gets 60ms */
    for (int i = 0; i < 20; i++) {
        latency_tracker_record(lt, m.name, m.targets[0].endpoint, 800 * 1000000ULL);
        latency_tracker_record(lt, m.name, m.targets[1].endpoint, 60 * 1000000ULL);
    }

    upstream_target_t cands[4];
    int               count = 0;
    int               rc = model_router_select_candidates(NULL, lt, &m, cands, 4, &count);
    TEST_ASSERT(rc == 0, "select_candidates return 0");
    TEST_ASSERT(count == 2, "selected 2 candidates");

    /* Fast endpoint must be selected first! */
    TEST_ASSERT(strcmp(cands[0].endpoint, "http://fast.local") == 0,
                "expected fast.local first, got %s",
                cands[0].endpoint);
    TEST_ASSERT(strcmp(cands[1].endpoint, "http://slow.local") == 0,
                "expected slow.local second, got %s",
                cands[1].endpoint);

    latency_tracker_destroy(lt);
}

TEST_CASE(adaptive_routing_dynamic_weighted)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    model_rec_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "test-dyn-model");
    snprintf(m.lb_policy, sizeof(m.lb_policy), "dynamic_weighted");
    m.n_targets = 2;

    snprintf(m.targets[0].provider, sizeof(m.targets[0].provider), "openai");
    snprintf(m.targets[0].endpoint, sizeof(m.targets[0].endpoint), "http://slow.local");
    m.targets[0].weight = 1;
    m.targets[0].priority = 0;

    snprintf(m.targets[1].provider, sizeof(m.targets[1].provider), "openai");
    snprintf(m.targets[1].endpoint, sizeof(m.targets[1].endpoint), "http://fast.local");
    m.targets[1].weight = 1;
    m.targets[1].priority = 0;

    /* Feed latencies: slow = 1000ms (weight ~1), fast = 10ms (weight ~50) */
    for (int i = 0; i < 20; i++) {
        latency_tracker_record(lt, m.name, m.targets[0].endpoint, 1000 * 1000000ULL);
        latency_tracker_record(lt, m.name, m.targets[1].endpoint, 10 * 1000000ULL);
    }

    int fast_first_count = 0;
    for (int i = 0; i < 50; i++) {
        upstream_target_t cands[4];
        int               count = 0;
        model_router_select_candidates(NULL, lt, &m, cands, 4, &count);
        if (strcmp(cands[0].endpoint, "http://fast.local") == 0) {
            fast_first_count++;
        }
    }

    /* Fast should win vast majority of selections (e.g. > 35 out of 50) */
    TEST_ASSERT(
        fast_first_count >= 35, "expected fast_first_count >= 35, got %d", fast_first_count);

    latency_tracker_destroy(lt);
}

TEST_CASE(adaptive_routing_null_tracker_fallback)
{
    model_rec_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "test-null-lt");
    snprintf(m.lb_policy, sizeof(m.lb_policy), "latency_p95");
    m.n_targets = 2;

    snprintf(m.targets[0].endpoint, sizeof(m.targets[0].endpoint), "http://ep1");
    snprintf(m.targets[1].endpoint, sizeof(m.targets[1].endpoint), "http://ep2");

    upstream_target_t cands[4];
    int               count = 0;
    int               rc = model_router_select_candidates(NULL, NULL, &m, cands, 4, &count);
    TEST_ASSERT(rc == 0, "select candidates should succeed with null lt");
    TEST_ASSERT(count == 2, "count == 2");
    TEST_ASSERT(strcmp(cands[0].endpoint, "http://ep1") == 0, "fallback to default order");
}

void
test_model_router_adaptive_suite(void)
{
    adaptive_routing_latency_p95();
    adaptive_routing_dynamic_weighted();
    adaptive_routing_null_tracker_fallback();
}

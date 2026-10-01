/** @file test_latency_tracker.c
 *  @brief Unit tests for latency tracker (P95, EWMA, and Hedge Budget).
 */
#include "latency_tracker.h"
#include "run_tests.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void
test_latency_tracker_cold_start(void)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    uint32_t p95 = latency_tracker_get_p95_ms(lt, "gpt-4o", "http://ep1");
    TEST_ASSERT(p95 == 1000, "expected default p95=1000, got %u", p95);

    uint32_t ewma = latency_tracker_get_ewma_ms(lt, "gpt-4o", "http://ep1");
    TEST_ASSERT(ewma == 1000, "expected default ewma=1000, got %u", ewma);

    latency_tracker_destroy(lt);
}

static void
test_latency_tracker_p95_calculation(void)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    /* Record samples: 10ms, 20ms, 30ms, ... 640ms (64 samples) */
    for (int i = 1; i <= 64; i++) {
        uint64_t ns = (uint64_t)i * 10 * 1000000ULL;
        latency_tracker_record(lt, "claude-3-5-sonnet", "https://api.anthropic.com", ns);
    }

    uint32_t p95 = latency_tracker_get_p95_ms(lt, "claude-3-5-sonnet", "https://api.anthropic.com");
    /* 64 * 0.95 = 60.8 -> sample ~60 or 61 -> 600ms or 610ms */
    TEST_ASSERT(p95 >= 590 && p95 <= 620, "expected p95 in [590, 620], got %u", p95);

    latency_tracker_destroy(lt);
}

static void
test_latency_tracker_ewma_smoothing(void)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    /* First sample: 100ms -> EWMA = 100ms */
    latency_tracker_record(lt, "model-x", "http://fast.ai", 100 * 1000000ULL);
    uint32_t ewma1 = latency_tracker_get_ewma_ms(lt, "model-x", "http://fast.ai");
    TEST_ASSERT(ewma1 == 100, "expected ewma1=100, got %u", ewma1);

    /* Second sample: 200ms -> EWMA = 0.2 * 200 + 0.8 * 100 = 120ms */
    latency_tracker_record(lt, "model-x", "http://fast.ai", 200 * 1000000ULL);
    uint32_t ewma2 = latency_tracker_get_ewma_ms(lt, "model-x", "http://fast.ai");
    TEST_ASSERT(ewma2 == 120, "expected ewma2=120, got %u", ewma2);

    latency_tracker_destroy(lt);
}

static void
test_latency_tracker_hedge_budget(void)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    /* Simulate 10 normal requests */
    for (int i = 0; i < 10; i++) {
        latency_tracker_record_request(lt, "model-hedge");
    }

    /* Budget 15%: 0/11 = 0% -> allowed */
    TEST_ASSERT(latency_tracker_hedge_admitted(lt, "model-hedge", 15), "0/11 should be admitted");

    /* 1 hedged request: 1 * 100 / 11 = 9% <= 15% -> allowed */
    latency_tracker_record_hedge(lt, "model-hedge");
    TEST_ASSERT(latency_tracker_hedge_admitted(lt, "model-hedge", 15), "1/11 should be admitted");

    /* 2nd hedged request: 2 * 100 / 11 = 18% > 15% -> rejected */
    latency_tracker_record_hedge(lt, "model-hedge");
    TEST_ASSERT(!latency_tracker_hedge_admitted(lt, "model-hedge", 15), "2/11 should be rejected");

    /* Budget 0% -> always rejected */
    TEST_ASSERT(!latency_tracker_hedge_admitted(lt, "model-hedge", 0), "budget 0 should reject");

    latency_tracker_destroy(lt);
}

struct thread_arg {
    latency_tracker_t* lt;
    const char*        model;
    const char*        endpoint;
};

static void*
worker_func(void* arg)
{
    struct thread_arg* ta = (struct thread_arg*)arg;
    for (int i = 0; i < 200; i++) {
        latency_tracker_record(ta->lt, ta->model, ta->endpoint, (10 + (i % 50)) * 1000000ULL);
        latency_tracker_get_p95_ms(ta->lt, ta->model, ta->endpoint);
        latency_tracker_get_ewma_ms(ta->lt, ta->model, ta->endpoint);
    }
    return NULL;
}

static void
test_latency_tracker_concurrency(void)
{
    latency_tracker_t* lt = latency_tracker_create();
    TEST_ASSERT(lt != NULL, "lt created");

    pthread_t         threads[4];
    struct thread_arg args[4] = {
        {lt, "m1", "ep1"},
        {lt, "m1", "ep2"},
        {lt, "m2", "ep1"},
        {lt, "m2", "ep2"},
    };

    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker_func, &args[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    uint32_t p95 = latency_tracker_get_p95_ms(lt, "m1", "ep1");
    TEST_ASSERT(p95 > 0, "p95 should be > 0");

    latency_tracker_destroy(lt);
}

void
test_latency_tracker_suite(void)
{
    test_latency_tracker_cold_start();
    test_latency_tracker_p95_calculation();
    test_latency_tracker_ewma_smoothing();
    test_latency_tracker_hedge_budget();
    test_latency_tracker_concurrency();
}

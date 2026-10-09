/** @file test_sla_circuit_breaker.c
 *  @brief Unit tests for SLA soft degradation and state machine in circuit breaker.
 */
#include "circuit_breaker.h"
#include "run_tests.h"
#include <string.h>

TEST_CASE(test_sla_degradation_state_machine)
{
    circuit_breaker_t* cb = cb_create();
    TEST_ASSERT(cb != NULL, "cb_create should succeed");

    const char* model = "deepseek-r1";
    const char* endpoint = "https://api.deepseek.com/v1";

    /* SLA: TTFT max 3000ms, latency max 6000ms, window 5, violation ratio 40%, fallback qwen-max */
    cb_configure_sla(cb, model, 3000, 6000, 5, 0.40f, "qwen-max");

    /* Initial state should be CLOSED */
    char       fallback[64] = {0};
    cb_state_t st = cb_get_sla_state(cb, model, endpoint, fallback, sizeof(fallback));
    TEST_ASSERT(st == CB_CLOSED, "initial state should be CLOSED, got %d", st);

    /* Record 3 normal calls (TTFT < 3000) */
    cb_record_sla_sample(cb, model, endpoint, 500, 1000);
    cb_record_sla_sample(cb, model, endpoint, 600, 1100);
    cb_record_sla_sample(cb, model, endpoint, 700, 1200);

    st = cb_get_sla_state(cb, model, endpoint, fallback, sizeof(fallback));
    TEST_ASSERT(st == CB_CLOSED, "state after 3 normal samples should be CLOSED, got %d", st);

    /* Record 2 violation calls (TTFT = 3500, 4000) -> 2 of 5 = 40% violation ratio */
    cb_record_sla_sample(cb, model, endpoint, 3500, 5000);
    cb_record_sla_sample(cb, model, endpoint, 4000, 5500);

    /* Transition to CB_SLA_DEGRADED and report fallback model */
    st = cb_get_sla_state(cb, model, endpoint, fallback, sizeof(fallback));
    TEST_ASSERT(st == CB_SLA_DEGRADED, "state should degrade to CB_SLA_DEGRADED, got %d", st);
    TEST_ASSERT(strcmp(fallback, "qwen-max") == 0, "fallback should be qwen-max, got %s", fallback);

    /* Recovery probe: 1 normal sample (needs 2 consecutive) */
    cb_record_sla_sample(cb, model, endpoint, 800, 1200);
    st = cb_get_sla_state(cb, model, endpoint, fallback, sizeof(fallback));
    TEST_ASSERT(
        st == CB_SLA_DEGRADED, "state should still be CB_SLA_DEGRADED after 1 probe, got %d", st);

    /* 2nd consecutive normal sample -> recovery to CLOSED */
    cb_record_sla_sample(cb, model, endpoint, 850, 1300);
    st = cb_get_sla_state(cb, model, endpoint, fallback, sizeof(fallback));
    TEST_ASSERT(st == CB_CLOSED, "state should recover to CB_CLOSED after 2 probes, got %d", st);

    /* Admin manual override */
    bool ok = cb_override_state(cb, model, endpoint, CB_SLA_DEGRADED);
    TEST_ASSERT(ok, "cb_override_state should succeed");
    st = cb_get_sla_state(cb, model, endpoint, fallback, sizeof(fallback));
    TEST_ASSERT(
        st == CB_SLA_DEGRADED, "state should be CB_SLA_DEGRADED after override, got %d", st);

    /* Check string representation */
    TEST_ASSERT(strcmp(cb_state_to_str(CB_SLA_DEGRADED), "sla_degraded") == 0,
                "state_to_str for SLA_DEGRADED");

    cb_destroy(cb);
}

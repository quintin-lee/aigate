/** @file test_model_router_sla.c
 *  @brief Unit tests for SLA degraded model router fallback redirection.
 */
#include "model_router.h"
#include "circuit_breaker.h"
#include "run_tests.h"
#include <string.h>

TEST_CASE(test_model_router_sla_fallback_redirection)
{
    circuit_breaker_t* cb = cb_create();
    TEST_ASSERT(cb != NULL, "cb_create should succeed");

    cb_configure_sla(cb, "deepseek-r1", 3000, 6000, 5, 0.40f, "qwen-max");

    /* Set deepseek-r1 to CB_SLA_DEGRADED */
    cb_override_state(cb, "deepseek-r1", NULL, CB_SLA_DEGRADED);

    char routed_model[64] = {0};
    char fallback_reason[32] = {0};
    bool is_fallback = false;

    int rc = model_router_resolve_with_sla(cb,
                                           "deepseek-r1",
                                           routed_model,
                                           sizeof(routed_model),
                                           &is_fallback,
                                           fallback_reason,
                                           sizeof(fallback_reason));
    TEST_ASSERT(rc == 0, "model_router_resolve_with_sla should return 0, got %d", rc);
    TEST_ASSERT(is_fallback, "is_fallback should be true");
    TEST_ASSERT(strcmp(routed_model, "qwen-max") == 0,
                "routed_model should be qwen-max, got %s",
                routed_model);
    TEST_ASSERT(strcmp(fallback_reason, "SLA_TTFT_EXCEEDED") == 0,
                "fallback_reason should be SLA_TTFT_EXCEEDED, got %s",
                fallback_reason);

    /* Normal model should not fallback */
    memset(routed_model, 0, sizeof(routed_model));
    is_fallback = false;
    rc = model_router_resolve_with_sla(cb,
                                       "gpt-4o",
                                       routed_model,
                                       sizeof(routed_model),
                                       &is_fallback,
                                       fallback_reason,
                                       sizeof(fallback_reason));
    TEST_ASSERT(rc == 0, "resolve normal model should return 0");
    TEST_ASSERT(!is_fallback, "normal model should not be fallback");
    TEST_ASSERT(strcmp(routed_model, "gpt-4o") == 0,
                "routed_model should remain gpt-4o, got %s",
                routed_model);

    cb_destroy(cb);
}

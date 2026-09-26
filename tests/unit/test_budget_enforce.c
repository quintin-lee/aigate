/** @file test_budget_enforce.c
 *  @brief Unit tests for monthly budget enforcer (API key & Group limits).
 */
#include "run_tests.h"
#include "budget_enforce.h"
#include <string.h>

TEST_CASE(test_budget_enforce_unlimited)
{
    budget_enforce_mgr_t* mgr = budget_enforce_create(NULL, NULL);
    TEST_ASSERT(mgr != NULL, "budget_enforce_create");

    char err_msg[128] = {0};

    /* 0 means unlimited */
    TEST_ASSERT(budget_enforce_check(mgr, 1, 0, 0.0, 0, 0.0, err_msg, sizeof err_msg) == 0,
                "unlimited check passes");

    /* Record large usage */
    budget_enforce_record(mgr, 1, 0, 10000.0, 50000000);

    /* Should still pass */
    TEST_ASSERT(budget_enforce_check(mgr, 1, 0, 0.0, 0, 0.0, err_msg, sizeof err_msg) == 0,
                "unlimited still passes after large usage");

    budget_enforce_destroy(mgr);
}

TEST_CASE(test_budget_enforce_key_cost_limit)
{
    budget_enforce_mgr_t* mgr = budget_enforce_create(NULL, NULL);
    TEST_ASSERT(mgr != NULL, "budget_enforce_create");

    char err_msg[128] = {0};

    /* Check with $10.00 budget */
    TEST_ASSERT(budget_enforce_check(mgr, 10, 0, 10.0, 0, 0.0, err_msg, sizeof err_msg) == 0,
                "under budget passes");

    /* Record $6.50 */
    budget_enforce_record(mgr, 10, 0, 6.50, 100);
    TEST_ASSERT(budget_enforce_check(mgr, 10, 0, 10.0, 0, 0.0, err_msg, sizeof err_msg) == 0,
                "6.50 of 10.0 passes");

    /* Record another $4.00 (total $10.50) */
    budget_enforce_record(mgr, 10, 0, 4.00, 100);
    int rc = budget_enforce_check(mgr, 10, 0, 10.0, 0, 0.0, err_msg, sizeof err_msg);
    TEST_ASSERT(rc == -1, "10.50 of 10.0 must fail");
    TEST_ASSERT(strstr(err_msg, "Monthly cost budget") != NULL, "error message specifies cost budget");
    TEST_ASSERT(strstr(err_msg, "10.00") != NULL, "error message specifies limit");

    /* Other key unaffected */
    TEST_ASSERT(budget_enforce_check(mgr, 11, 0, 10.0, 0, 0.0, err_msg, sizeof err_msg) == 0,
                "key 11 unaffected");

    budget_enforce_destroy(mgr);
}

TEST_CASE(test_budget_enforce_key_token_limit)
{
    budget_enforce_mgr_t* mgr = budget_enforce_create(NULL, NULL);
    TEST_ASSERT(mgr != NULL, "budget_enforce_create");

    char err_msg[128] = {0};

    /* Limit 100,000 tokens */
    TEST_ASSERT(budget_enforce_check(mgr, 20, 0, 0.0, 100000, 0.0, err_msg, sizeof err_msg) == 0,
                "token under limit passes");

    budget_enforce_record(mgr, 20, 0, 0.50, 80000);
    TEST_ASSERT(budget_enforce_check(mgr, 20, 0, 0.0, 100000, 0.0, err_msg, sizeof err_msg) == 0,
                "80k of 100k passes");

    budget_enforce_record(mgr, 20, 0, 0.50, 20500);
    int rc = budget_enforce_check(mgr, 20, 0, 0.0, 100000, 0.0, err_msg, sizeof err_msg);
    TEST_ASSERT(rc == -1, "100.5k of 100k must fail");
    TEST_ASSERT(strstr(err_msg, "Monthly token budget") != NULL, "error message specifies token budget");

    budget_enforce_destroy(mgr);
}

TEST_CASE(test_budget_enforce_group_cost_limit)
{
    budget_enforce_mgr_t* mgr = budget_enforce_create(NULL, NULL);
    TEST_ASSERT(mgr != NULL, "budget_enforce_create");

    char err_msg[128] = {0};

    /* Group 5 limit: $50.00; key has no individual limit */
    TEST_ASSERT(budget_enforce_check(mgr, 30, 5, 0.0, 0, 50.0, err_msg, sizeof err_msg) == 0,
                "under group budget passes");

    /* Key 30 spends $30 for Group 5 */
    budget_enforce_record(mgr, 30, 5, 30.0, 1000);
    TEST_ASSERT(budget_enforce_check(mgr, 30, 5, 0.0, 0, 50.0, err_msg, sizeof err_msg) == 0,
                "30 of 50 passes");

    /* Key 31 in same Group 5 spends $25 (group total $55) */
    budget_enforce_record(mgr, 31, 5, 25.0, 1000);

    /* Now both keys in Group 5 should be blocked */
    int rc = budget_enforce_check(mgr, 30, 5, 0.0, 0, 50.0, err_msg, sizeof err_msg);
    TEST_ASSERT(rc == -1, "group budget exceeded for key 30");
    TEST_ASSERT(strstr(err_msg, "group 5") != NULL, "error specifies group 5");

    rc = budget_enforce_check(mgr, 31, 5, 0.0, 0, 50.0, err_msg, sizeof err_msg);
    TEST_ASSERT(rc == -1, "group budget exceeded for key 31");

    /* Key in Group 6 unaffected */
    TEST_ASSERT(budget_enforce_check(mgr, 32, 6, 0.0, 0, 50.0, err_msg, sizeof err_msg) == 0,
                "group 6 unaffected");

    budget_enforce_destroy(mgr);
}

TEST_CASE(test_budget_enforce_rollover_and_reset)
{
    budget_enforce_mgr_t* mgr = budget_enforce_create(NULL, NULL);
    TEST_ASSERT(mgr != NULL, "budget_enforce_create");

    char err_msg[128] = {0};

    budget_enforce_record(mgr, 40, 0, 15.0, 1000);
    TEST_ASSERT(budget_enforce_check(mgr, 40, 0, 10.0, 0, 0.0, err_msg, sizeof err_msg) == -1,
                "over budget fails");

    /* Reset / rollover clears counters */
    budget_enforce_reset(mgr);
    TEST_ASSERT(budget_enforce_check(mgr, 40, 0, 10.0, 0, 0.0, err_msg, sizeof err_msg) == 0,
                "after reset passes");

    budget_enforce_destroy(mgr);
}

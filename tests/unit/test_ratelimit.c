/** @file test_ratelimit.c
 *  @brief Token-bucket QPS + daily quota tests, incl. concurrency. */
#include "run_tests.h"
#include "ratelimit.h"
#include <pthread.h>
#include <unistd.h>

TEST_CASE(test_rl_qps_boundary)
{
    ratelimit_t* rl = ratelimit_new();
    TEST_ASSERT(rl != NULL, "new");
    long retry = 0;
    int  admitted = 0;
    for (int i = 0; i < 10; i++) {
        if (rl_allow_request(rl, 1, 10, &retry) == 0) {
            admitted++;
        }
    }
    TEST_ASSERT(admitted == 10, "burst admitted, got %d", admitted);

    int rc = rl_allow_request(rl, 1, 10, &retry);
    TEST_ASSERT(rc == -1, "11th denied");
    TEST_ASSERT(retry >= 1 && retry <= 200, "retry_ms in range, got %ld", retry);

    usleep(120000); /* let ~1-2 tokens refill */
    int admitted2 = 0;
    for (int i = 0; i < 5; i++) {
        if (rl_allow_request(rl, 1, 10, &retry) == 0) {
            admitted2++;
        }
    }
    TEST_ASSERT(admitted2 >= 1 && admitted2 <= 4, "refill admits 1-4, got %d", admitted2);
    ratelimit_free(rl);
}

TEST_CASE(test_rl_unlimited)
{
    ratelimit_t* rl = ratelimit_new();
    TEST_ASSERT(rl != NULL, "new");
    int admitted = 0;
    for (int i = 0; i < 1000; i++) {
        if (rl_allow_request(rl, 2, 0, NULL) == 0) {
            admitted++;
        }
    }
    TEST_ASSERT(admitted == 1000, "unlimited admits all, got %d", admitted);
    ratelimit_free(rl);
}

TEST_CASE(test_rl_daily_quota)
{
    ratelimit_t* rl = ratelimit_new();
    TEST_ASSERT(rl != NULL, "new");
    TEST_ASSERT(rl_reserve_tokens(rl, 3, 100, 60) == 0, "reserve 60");
    TEST_ASSERT(rl_remaining_daily(rl, 3, 100) == 40, "40 remaining");
    TEST_ASSERT(rl_reserve_tokens(rl, 3, 100, 50) == -1, "reserve 50 over quota");
    TEST_ASSERT(rl_remaining_daily(rl, 3, 100) == -10, "110 used, -10 remaining");
    TEST_ASSERT(rl_remaining_daily(rl, 3, 0) == LONG_MAX, "unlimited quota");
    ratelimit_free(rl);
}

TEST_CASE(test_rl_daily_quota_record_first)
{
    ratelimit_t* rl = ratelimit_new();
    TEST_ASSERT(rl != NULL, "new");
    /* record-first: even a denied reservation is accounted */
    TEST_ASSERT(rl_reserve_tokens(rl, 7, 100, 60) == 0, "reserve 60 ok");
    TEST_ASSERT(rl_remaining_daily(rl, 7, 100) == 40, "40 remaining");
    TEST_ASSERT(rl_reserve_tokens(rl, 7, 100, 50) == -1, "reserve 50 over quota");
    TEST_ASSERT(rl_remaining_daily(rl, 7, 100) == -10, "110 used, -10 remaining");
    ratelimit_free(rl);
}

TEST_CASE(test_rl_reset_day)
{
    ratelimit_t* rl = ratelimit_new();
    TEST_ASSERT(rl != NULL, "new");
    TEST_ASSERT(rl_reserve_tokens(rl, 4, 50, 40) == 0, "use 40/50");
    TEST_ASSERT(rl_remaining_daily(rl, 4, 50) == 10, "10 remaining");
    rl_reset_day(rl, 1234567890); /* any time: forces a new UTC midnight */
    TEST_ASSERT(rl_remaining_daily(rl, 4, 50) == 50, "reset to full quota");
    ratelimit_free(rl);
}

struct cc_arg {
    ratelimit_t* rl;
    int          iters;
};
static void*
cc_worker(void* arg)
{
    struct cc_arg* a = arg;
    long           admitted = 0;
    for (int i = 0; i < a->iters; i++) {
        long retry;
        if (rl_allow_request(a->rl, 1, 1000, &retry) == 0) {
            admitted++;
        }
    }
    /* per-thread admitted not summed globally here; smoke check only */
    (void)admitted;
    return NULL;
}

TEST_CASE(test_rl_concurrent_smoke)
{
    ratelimit_t* rl = ratelimit_new();
    TEST_ASSERT(rl != NULL, "new");
    pthread_t     th[8];
    struct cc_arg a = {rl, 50000};
    for (unsigned i = 0; i < 8; i++) {
        TEST_ASSERT(pthread_create(&th[i], NULL, cc_worker, &a) == 0, "pthread_create");
    }
    for (unsigned i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
    }
    /* no crash + buckets consistent: remaining daily is LONG_MAX (no quota) */
    TEST_ASSERT(rl_remaining_daily(rl, 1, 0) == LONG_MAX, "daily untouched");
    ratelimit_free(rl);
}

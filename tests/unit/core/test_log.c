/** @file test_log.c
 *  @brief smoke tests for the stderr logger: formatting + concurrent safety. */
#include "run_tests.h"
#include "aigate_log.h"
#include <pthread.h>

TEST_CASE(test_log_smoke)
{
    AIGATE_LOG_INFO("log smoke test %d", 42);
    AIGATE_LOG_ERROR("sample error %s", "x");
}

static void*
log_worker(void* arg)
{
    int n = *(int*)arg;
    for (int i = 0; i < n; i++) {
        AIGATE_LOG_WARN("concurrent warn %d", i);
    }
    return NULL;
}

TEST_CASE(test_log_concurrent)
{
    pthread_t th[4];
    int       n = 500;
    for (unsigned i = 0; i < 4; i++) {
        TEST_ASSERT(pthread_create(&th[i], NULL, log_worker, &n) == 0, "pthread_create");
    }
    for (unsigned i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
}

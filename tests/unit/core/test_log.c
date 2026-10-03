/** @file test_log.c
 *  @brief smoke tests for the stderr logger: formatting + concurrent safety. */
#include "run_tests.h"
#include "aigate_log.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

TEST_CASE(test_log_smoke)
{
    AIGATE_LOG_INFO("log smoke test %d", 42);
    AIGATE_LOG_ERROR("sample error %s", "x");
}

/** @brief Log concurrent worker thread entry. */
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

TEST_CASE(test_log_json_and_level_filtering)
{
    /* Save stderr */
    int saved_stderr = dup(STDERR_FILENO);
    int pfd[2];
    TEST_ASSERT(pipe(pfd) == 0, "pipe created");
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);

    /* Test 1: Level filtering (WARN level: DEBUG and INFO should be dropped) */
    aigate_log_init("text", "warn");
    AIGATE_LOG_DEBUG("should_be_dropped_debug");
    AIGATE_LOG_INFO("should_be_dropped_info");
    AIGATE_LOG_WARN("should_appear_warn");

    /* Restore stderr */
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    char buf[2048];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);

    TEST_ASSERT(n > 0, "log output read");
    TEST_ASSERT(strstr(buf, "should_be_dropped_debug") == NULL, "debug dropped");
    TEST_ASSERT(strstr(buf, "should_be_dropped_info") == NULL, "info dropped");
    TEST_ASSERT(strstr(buf, "should_appear_warn") != NULL, "warn emitted");

    /* Test 2: JSON formatting */
    saved_stderr = dup(STDERR_FILENO);
    TEST_ASSERT(pipe(pfd) == 0, "pipe created");
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);

    aigate_log_init("json", "info");
    AIGATE_LOG_INFO("hello \"world\" test\nsecond line");

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    memset(buf, 0, sizeof(buf));
    n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);

    TEST_ASSERT(n > 0, "json output read");
    TEST_ASSERT(strstr(buf, "{\"ts\":\"") != NULL, "json ts key present");
    TEST_ASSERT(strstr(buf, "\"level\":\"INFO\"") != NULL, "json level key present");
    TEST_ASSERT(strstr(buf, "\"file\":\"") != NULL, "json file key present");
    TEST_ASSERT(strstr(buf, "\"line\":") != NULL, "json line key present");
    TEST_ASSERT(strstr(buf, "\"msg\":\"hello \\\"world\\\" test\\nsecond line\"") != NULL,
                "json msg escaped properly");

    /* Reset logger back to text / info default for subsequent tests */
    aigate_log_init("text", "info");
}

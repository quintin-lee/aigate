/** @file test_config.c
 *  @brief config loader tests: defaults, required vars, validation. */
#include "run_tests.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

/** @brief Clear test-related environment variables. */
static void
clear_env(void)
{
    const char* vars[] = {"AIGATE_LISTEN",
                          "AIGATE_PG_DSN",
                          "AIGATE_ADMIN_TOKEN",
                          "AIGATE_MASTER_KEY",
                          "AIGATE_UPSTREAM_TIMEOUT_MS",
                          "AIGATE_METRICS_ACL",
                          "AIGATE_WORKER_THREADS",
                          "AIGATE_REQUEST_TIMEOUT_MS",
                          "AIGATE_TRUSTED_PROXIES",
                          "AIGATE_DRAIN_TIMEOUT_S",
                          "AIGATE_REDIS_FAIL_OPEN",
                          "AIGATE_CORS_ALLOW_ORIGIN",
                          "AIGATE_LOG_FORMAT",
                          "AIGATE_LOG_LEVEL"};
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        unsetenv(vars[i]);
    }
}

TEST_CASE(test_config_defaults)
{
    aigate_config c;
    clear_env();
    setenv("AIGATE_PG_DSN", "dbname=aigate", 1);
    setenv("AIGATE_ADMIN_TOKEN", "s3cr3t", 1);

    TEST_ASSERT(aigate_config_load(&c) == 0, "load with required vars");
    TEST_ASSERT(strcmp(c.listen, ":8080") == 0, "default listen");
    TEST_ASSERT(c.upstream_timeout_ms == 60000, "default timeout");
    TEST_ASSERT(strcmp(c.metrics_acl, "127.0.0.1") == 0, "default acl");
    TEST_ASSERT(c.master_key[0] == '\0', "no master key");
    TEST_ASSERT(strlen(c.admin_token_hash) == 64, "admin token hashed");
    TEST_ASSERT(c.worker_threads == 64, "default worker threads 64");
    TEST_ASSERT(c.request_timeout_ms == 300000, "default request timeout 300000ms");
    TEST_ASSERT(strcmp(c.trusted_proxies, "127.0.0.1") == 0, "default trusted proxies");
    TEST_ASSERT(c.drain_timeout_s == 15, "default drain timeout 15s");
    TEST_ASSERT(c.redis_fail_open == 1, "default redis fail open 1");
    TEST_ASSERT(strcmp(c.cors_allow_origin, "*") == 0, "default cors allow origin *");
    TEST_ASSERT(strcmp(c.log_format, "text") == 0, "default log format text");
    TEST_ASSERT(strcmp(c.log_level, "info") == 0, "default log level info");
    clear_env();
}

TEST_CASE(test_config_missing_required)
{
    aigate_config c;
    clear_env();
    TEST_ASSERT(aigate_config_load(&c) == -1, "missing PG DSN rejected");
    setenv("AIGATE_PG_DSN", "dbname=x", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "missing admin token rejected");
    clear_env();
}

TEST_CASE(test_config_bad_master_key)
{
    aigate_config c;
    clear_env();
    setenv("AIGATE_PG_DSN", "dbname=x", 1);
    setenv("AIGATE_ADMIN_TOKEN", "t", 1);
    setenv("AIGATE_MASTER_KEY", "short", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "short master key rejected");
    setenv(
        "AIGATE_MASTER_KEY", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    TEST_ASSERT(aigate_config_load(&c) == 0, "64-hex master key accepted");
    TEST_ASSERT(c.upstream_timeout_ms == 60000, "defaults still applied");
    setenv("AIGATE_UPSTREAM_TIMEOUT_MS", "0", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "timeout 0 rejected");
    clear_env();
}

TEST_CASE(test_config_worker_threads_and_p0)
{
    aigate_config c;
    clear_env();
    setenv("AIGATE_PG_DSN", "dbname=x", 1);
    setenv("AIGATE_ADMIN_TOKEN", "t", 1);

    setenv("AIGATE_WORKER_THREADS", "128", 1);
    setenv("AIGATE_REQUEST_TIMEOUT_MS", "600000", 1);
    setenv("AIGATE_TRUSTED_PROXIES", "10.0.0.0/8,192.168.1.1", 1);
    setenv("AIGATE_DRAIN_TIMEOUT_S", "30", 1);
    setenv("AIGATE_REDIS_FAIL_OPEN", "0", 1);

    TEST_ASSERT(aigate_config_load(&c) == 0, "load custom P0 configs");
    TEST_ASSERT(c.worker_threads == 128, "custom worker threads 128");
    TEST_ASSERT(c.request_timeout_ms == 600000, "custom request timeout 600000ms");
    TEST_ASSERT(strcmp(c.trusted_proxies, "10.0.0.0/8,192.168.1.1") == 0, "custom trusted proxies");
    TEST_ASSERT(c.drain_timeout_s == 30, "custom drain timeout 30s");
    TEST_ASSERT(c.redis_fail_open == 0, "custom redis fail open 0");

    /* Test out-of-range validations */
    setenv("AIGATE_WORKER_THREADS", "2", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "worker threads < 4 rejected");

    setenv("AIGATE_WORKER_THREADS", "5000", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "worker threads > 4096 rejected");

    setenv("AIGATE_WORKER_THREADS", "64", 1);
    setenv("AIGATE_REQUEST_TIMEOUT_MS", "500", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "request timeout < 1000 rejected");

    setenv("AIGATE_REQUEST_TIMEOUT_MS", "300000", 1);
    setenv("AIGATE_DRAIN_TIMEOUT_S", "200", 1);
    TEST_ASSERT(aigate_config_load(&c) == -1, "drain timeout > 120 rejected");

    clear_env();
}

TEST_CASE(test_config_p1_features)
{
    aigate_config c;
    clear_env();
    setenv("AIGATE_PG_DSN", "dbname=x", 1);
    setenv("AIGATE_ADMIN_TOKEN", "t", 1);

    setenv("AIGATE_CORS_ALLOW_ORIGIN", "https://app.example.com", 1);
    setenv("AIGATE_LOG_FORMAT", "json", 1);
    setenv("AIGATE_LOG_LEVEL", "debug", 1);

    TEST_ASSERT(aigate_config_load(&c) == 0, "load custom P1 configs");
    TEST_ASSERT(strcmp(c.cors_allow_origin, "https://app.example.com") == 0, "custom cors origin");
    TEST_ASSERT(strcmp(c.log_format, "json") == 0, "custom log format json");
    TEST_ASSERT(strcmp(c.log_level, "debug") == 0, "custom log level debug");

    /* Invalid log format and log level fallback to defaults with warning */
    setenv("AIGATE_LOG_FORMAT", "xml", 1);
    setenv("AIGATE_LOG_LEVEL", "verbose", 1);
    TEST_ASSERT(aigate_config_load(&c) == 0, "load with fallback for invalid log format/level");
    TEST_ASSERT(strcmp(c.log_format, "text") == 0, "fallback log format text");
    TEST_ASSERT(strcmp(c.log_level, "info") == 0, "fallback log level info");

    clear_env();
}

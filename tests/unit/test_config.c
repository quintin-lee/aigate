/** @file test_config.c
 *  @brief config loader tests: defaults, required vars, validation. */
#include "run_tests.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

static void clear_env(void)
{
  const char *vars[] = {
    "AIGATE_LISTEN", "AIGATE_PG_DSN", "AIGATE_ADMIN_TOKEN",
    "AIGATE_MASTER_KEY", "AIGATE_UPSTREAM_TIMEOUT_MS", "AIGATE_METRICS_ACL"
  };
  for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++)
    unsetenv(vars[i]);
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
  setenv("AIGATE_MASTER_KEY",
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
  TEST_ASSERT(aigate_config_load(&c) == 0, "64-hex master key accepted");
  TEST_ASSERT(c.upstream_timeout_ms == 60000, "defaults still applied");
  setenv("AIGATE_UPSTREAM_TIMEOUT_MS", "0", 1);
  TEST_ASSERT(aigate_config_load(&c) == -1, "timeout 0 rejected");
  clear_env();
}

/** @file run_tests.c
 *  @brief assert-based unit test runner (no framework; spec section 6).
 *
 *  Each test file defines plain (non-static) test functions using TEST_CASE
 *  from run_tests.h; this file keeps the registry. Exit code = number of
 *  failed test functions.
 */
#include "run_tests.h"
#include <stdio.h>

int g_failures = 0;

typedef void (*test_fn)(void);
static struct { const char *name; test_fn fn; } g_tests[256];
static int g_n_tests = 0;

void test_register(const char *name, test_fn fn)
{
  if (g_n_tests < (int)(sizeof g_tests / sizeof g_tests[0])) {
    g_tests[g_n_tests].name = name;
    g_tests[g_n_tests].fn = fn;
    g_n_tests++;
  }
}

int main(void)
{
  extern void test_log_smoke(void);
  extern void test_log_concurrent(void);
  extern void test_sha256_kat(void);
  extern void test_sha256_equal(void);
  extern void test_config_defaults(void);
  extern void test_config_missing_required(void);
  extern void test_config_bad_master_key(void);
  test_register("log_smoke", test_log_smoke);
  test_register("log_concurrent", test_log_concurrent);
  test_register("sha256_kat", test_sha256_kat);
  test_register("sha256_equal", test_sha256_equal);
  test_register("config_defaults", test_config_defaults);
  test_register("config_missing_required", test_config_missing_required);
  test_register("config_bad_master_key", test_config_bad_master_key);

  int failed = 0;
  for (int i = 0; i < g_n_tests; i++) {
    g_failures = 0;
    g_tests[i].fn();
    if (g_failures > 0) {
      failed++;
      fprintf(stderr, "FAILED: %s\n", g_tests[i].name);
    }
  }
  printf("PASS: %d/%d test(s), %d failure(s)\n", g_n_tests - failed, g_n_tests, failed);
  return failed;
}

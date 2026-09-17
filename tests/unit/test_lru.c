/** @file test_lru.c
 *  @brief LRU behavior tests: eviction order, recency refresh, invalidate, concurrency. */
#include "run_tests.h"
#include "lru.h"
#include <pthread.h>

static int g_evictions = 0;
static void count_evict(void *val)
{
  (void)val;
  g_evictions++;
}

TEST_CASE(test_lru_eviction_order)
{
  lru_t *lr = lru_new(2, count_evict);
  TEST_ASSERT(lr != NULL, "lru_new");
  g_evictions = 0;
  lru_put(lr, "a", (void *)0x1);
  lru_put(lr, "b", (void *)0x2);
  lru_put(lr, "c", (void *)0x3);
  TEST_ASSERT(lru_size(lr) == 2, "size after evict");
  TEST_ASSERT(lru_get(lr, "a") == NULL, "a evicted");
  TEST_ASSERT(lru_get(lr, "b") == (void *)0x2, "b kept");
  TEST_ASSERT(lru_get(lr, "c") == (void *)0x3, "c kept");
  TEST_ASSERT(g_evictions == 1, "one eviction");
  lru_free(lr);
}

TEST_CASE(test_lru_recency_refresh)
{
  lru_t *lr = lru_new(2, count_evict);
  g_evictions = 0;
  lru_put(lr, "a", (void *)0x1);
  lru_put(lr, "b", (void *)0x2);
  TEST_ASSERT(lru_get(lr, "a") == (void *)0x1, "get a refreshes");
  lru_put(lr, "c", (void *)0x3);
  TEST_ASSERT(lru_get(lr, "b") == NULL, "b evicted, not a");
  TEST_ASSERT(lru_get(lr, "a") == (void *)0x1, "a still present");
  TEST_ASSERT(g_evictions == 1, "single eviction");
  lru_free(lr);
}

TEST_CASE(test_lru_replace_and_invalidate)
{
  lru_t *lr = lru_new(4, count_evict);
  lru_put(lr, "k", (void *)0x1);
  g_evictions = 0;
  lru_put(lr, "k", (void *)0x2);
  TEST_ASSERT(lru_size(lr) == 1, "replace does not grow size");
  TEST_ASSERT(lru_get(lr, "k") == (void *)0x2, "replaced value");
  TEST_ASSERT(g_evictions == 1, "replacement evicted the old value");
  g_evictions = 0;
  TEST_ASSERT(lru_invalidate(lr, "k") == 1, "invalidate present");
  TEST_ASSERT(g_evictions == 1, "invalidate evicted the value");
  TEST_ASSERT(lru_invalidate(lr, "k") == 0, "invalidate absent");
  TEST_ASSERT(lru_size(lr) == 0, "empty after invalidate");
  lru_put(lr, "j", (void *)0x9);
  g_evictions = 0;
  lru_free(lr); /* remaining value evicted on free */
  TEST_ASSERT(g_evictions == 1, "free evicts remaining value");
}

struct cc_arg { lru_t *lr; int ops_per_thread; };
static void *cc_worker(void *arg)
{
  struct cc_arg *a = arg;
  char key[16];
  for (int i = 0; i < a->ops_per_thread; i++) {
    snprintf(key, sizeof key, "k%d", i % 32);
    if (i % 3 == 0)
      lru_put(a->lr, key, (void *)(long)(i + 1));
    else if (i % 3 == 1)
      lru_get(a->lr, key);
    else
      lru_invalidate(a->lr, key);
  }
  return NULL;
}

TEST_CASE(test_lru_concurrent_smoke)
{
  lru_t *lr = lru_new(16, NULL);
  struct cc_arg a = { lr, 100000 };
  pthread_t th[8];
  for (unsigned i = 0; i < 8; i++)
    TEST_ASSERT(pthread_create(&th[i], NULL, cc_worker, &a) == 0, "pthread_create");
  for (unsigned i = 0; i < 8; i++)
    pthread_join(th[i], NULL);
  TEST_ASSERT(lru_size(lr) <= 16, "size within capacity, got %zu", lru_size(lr));
  lru_free(lr);
}

/** @file test_usage_meter.c
 *  @brief usage_meter counters/drain/HDR + metrics render/ACL tests. */
#include "run_tests.h"
#include "usage_meter.h"
#include "metrics.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

struct um_db {
  int flush_calls;
  usage_row_t rows[64];
  int n_rows;
};

static int um_flush(void *ctx, const usage_row_t *rows, int n)
{
  struct um_db *db = ctx;
  db->flush_calls++;
  for (int i = 0; i < n && db->n_rows < (int)(sizeof db->rows / sizeof db->rows[0]); i++)
    db->rows[db->n_rows++] = rows[i];
  return 0;
}

static int um_fail(void *ctx, ...)
{
  (void)ctx;
  return -1;
}

static pg_store_t *open_um_store(struct um_db *db)
{
  pg_ops_t ops;
  memset(&ops, 0, sizeof ops);
  ops.ctx = db;
  ops.get_key_by_hash = (int (*)(void *, const char *, key_rec_t *))um_fail;
  ops.list_models = (int (*)(void *, model_rec_t *, int, int *))um_fail;
  ops.get_model = (int (*)(void *, const char *, model_rec_t *))um_fail;
  ops.create_key = (int (*)(void *, const key_rec_t *, long *))um_fail;
  ops.update_key = (int (*)(void *, const key_rec_t *, int))um_fail;
  ops.revoke_key = (int (*)(void *, long))um_fail;
  ops.create_model = (int (*)(void *, const model_rec_t *))um_fail;
  ops.update_model = (int (*)(void *, const model_rec_t *, int))um_fail;
  ops.delete_model = (int (*)(void *, const char *))um_fail;
  ops.flush_usage = um_flush;
  ops.query_usage =
      (int (*)(void *, long, const char *, time_t, time_t, usage_row_t *, int,
               int *))um_fail;
  return pg_store_open("unused", &ops);
}

TEST_CASE(test_um_counters_and_drain)
{
  struct um_db db;
  memset(&db, 0, sizeof db);
  pg_store_t *ps = open_um_store(&db);
  TEST_ASSERT(ps != NULL, "store open");
  usage_meter_t *um = usage_meter_new(ps, 0); /* no worker: manual drain */
  TEST_ASSERT(um != NULL, "meter new");

  um_record(um, 1, "gpt-4o", 200, 7, 11, 50000000, "openai");
  um_record(um, 1, "gpt-4o", 500, 0, 0, 120000000, "openai");
  um_record(um, 2, "claude-3", 200, 3, 4, 90000000, "openai");

  TEST_ASSERT(um_total_requests(um) == 3, "3 requests");
  TEST_ASSERT(um_total_errors(um) == 1, "1 error");
  TEST_ASSERT(um_total_tokens(um) == 25, "25 tokens");

  usage_row_t rows[16];
  int n = 0;
  TEST_ASSERT(um_drain(um, rows, 16, &n) == 0, "drain ok");
  TEST_ASSERT(n == 2, "2 accumulated rows, got %d", n);
  TEST_ASSERT(db.flush_calls == 1, "flush called once");
  /* key 1 gpt-4o: 2 requests, 7+11 tokens? no — 7,11 then 0,0 → 7 prompt 11
   * completion, 1 error */
  int found1 = 0, found2 = 0;
  for (int i = 0; i < db.n_rows; i++) {
    if (db.rows[i].key_id == 1 && strcmp(db.rows[i].model_name, "gpt-4o") == 0) {
      TEST_ASSERT(db.rows[i].requests == 2, "k1 reqs 2");
      TEST_ASSERT(db.rows[i].prompt_tokens == 7, "k1 prompt 7");
      TEST_ASSERT(db.rows[i].completion_tokens == 11, "k1 compl 11");
      TEST_ASSERT(db.rows[i].errors == 1, "k1 errors 1");
      found1 = 1;
    }
    if (db.rows[i].key_id == 2 && strcmp(db.rows[i].model_name, "claude-3") == 0) {
      TEST_ASSERT(db.rows[i].requests == 1, "k2 reqs 1");
      found2 = 1;
    }
  }
  TEST_ASSERT(found1 && found2, "both rows flushed");

  /* HDR sanity: openai recorded 3 samples (50ms, 120ms, 90ms) */
  TEST_ASSERT(um_provider_sampled(um, "openai") == 3, "openai 3 samples");
  long p50 = um_provider_percentile_ns(um, "openai", 50.0);
  TEST_ASSERT(p50 >= 50000000 && p50 <= 120000000, "p50 in range, got %ld", p50);
  TEST_ASSERT(um_provider_count_below_ns(um, "openai", 100000000) == 2,
              "2 samples <= 100ms");

  /* metrics text contains expected prefixes */
  char buf[16384];
  TEST_ASSERT(metrics_render(um, buf, sizeof buf) == 0, "render ok");
  TEST_ASSERT(strstr(buf, "aigate_requests_total 3") != NULL, "requests line");
  TEST_ASSERT(strstr(buf, "aigate_errors_total 1") != NULL, "errors line");
  TEST_ASSERT(strstr(buf, "aigate_tokens_total 25") != NULL, "tokens line");
  TEST_ASSERT(strstr(buf, "aigate_upstream_requests_total{provider=\"openai\"} 3") != NULL,
              "per-provider counter");
  TEST_ASSERT(strstr(buf, "aigate_upstream_latency_ns_ns_bucket{provider=\"openai\",le=\"50000000\"} 1") != NULL,
              "50ms bucket == 1");

  usage_meter_free(um);
  pg_store_close(ps);
}

TEST_CASE(test_metrics_acl)
{
  TEST_ASSERT(metrics_acl_allows("127.0.0.1", "") == 1, "empty acl allows");
  TEST_ASSERT(metrics_acl_allows("127.0.0.1", "127.0.0.1") == 1, "exact");
  TEST_ASSERT(metrics_acl_allows("127.0.0.1", "127.0.0.1,10.0.0.0/8") == 1,
              "list hit");
  TEST_ASSERT(metrics_acl_allows("8.8.8.8", "127.0.0.1,10.0.0.0/8") == 0,
              "outside denied");
  TEST_ASSERT(metrics_acl_allows("10.1.2.3", "10.0.0.0/8") == 1, "/8 match");
  TEST_ASSERT(metrics_acl_allows("9.1.2.3", "10.0.0.0/8") == 0, "/8 miss");
}

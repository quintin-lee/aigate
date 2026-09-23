/** @file test_usage_meter.c
 *  @brief usage_meter counters/drain/HDR + metrics render/ACL tests. */
#include "run_tests.h"
#include "usage_meter.h"
#include "metrics.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

struct um_db {
    int                 flush_calls;
    int                 fail_flush;
    usage_row_t         rows[64];
    int                 n_rows;
    usage_request_row_t reqs[64];
    int                 n_reqs;
    int                 fail_req_flush;
    int                 req_flush_calls;
};

static int
um_flush(void* ctx, const usage_row_t* rows, int n)
{
    struct um_db* db = ctx;
    db->flush_calls++;
    if (db->fail_flush) {
        return -1;
    }
    for (int i = 0; i < n && db->n_rows < (int)(sizeof db->rows / sizeof db->rows[0]); i++) {
        db->rows[db->n_rows++] = rows[i];
    }
    return 0;
}

static int
um_flush_reqs(void* ctx, const usage_request_row_t* rows, int n)
{
    struct um_db* db = ctx;
    if (db->fail_req_flush) {
        return -1;
    }
    db->req_flush_calls++;
    for (int i = 0; i < n && db->n_reqs < (int)(sizeof db->reqs / sizeof db->reqs[0]); i++) {
        db->reqs[db->n_reqs++] = rows[i];
    }
    return 0;
}

static int
um_fail(void* ctx, ...)
{
    (void)ctx;
    return -1;
}

static pg_store_t*
open_um_store(struct um_db* db)
{
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.ctx = db;
    ops.get_key_by_hash = (int (*)(void*, const char*, key_rec_t*))um_fail;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))um_fail;
    ops.get_model = (int (*)(void*, const char*, model_rec_t*))um_fail;
    ops.create_key = (int (*)(void*, const key_rec_t*, long*))um_fail;
    ops.update_key = (int (*)(void*, const key_rec_t*, int))um_fail;
    ops.revoke_key = (int (*)(void*, long))um_fail;
    ops.create_model = (int (*)(void*, const model_rec_t*))um_fail;
    ops.update_model = (int (*)(void*, const model_rec_t*, int))um_fail;
    ops.delete_model = (int (*)(void*, const char*))um_fail;
    ops.flush_usage = um_flush;
    ops.query_usage =
        (int (*)(void*, long, const char*, time_t, time_t, usage_row_t*, int, int*))um_fail;
    ops.flush_usage_requests = um_flush_reqs;
    ops.query_usage_requests =
        (int (*)(void*, long, time_t, usage_request_row_t*, int, int*))um_fail;
    return pg_store_open("unused", &ops);
}

TEST_CASE(test_um_counters_and_drain)
{
    struct um_db db;
    memset(&db, 0, sizeof db);
    pg_store_t* ps = open_um_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    usage_meter_t* um = usage_meter_new(ps, NULL, 0); /* no worker: manual drain */
    TEST_ASSERT(um != NULL, "meter new");

    um_record(um, 1, "gpt-4o", 200, 7, 11, 5, 50000000, "openai");
    um_record(um, 1, "gpt-4o", 500, 0, 0, 0, 120000000, "openai");
    um_record(um, 2, "claude-3", 200, 3, 4, 2, 90000000, "openai");

    TEST_ASSERT(um_total_requests(um) == 3, "3 requests");
    TEST_ASSERT(um_total_errors(um) == 1, "1 error");
    TEST_ASSERT(um_total_tokens(um) == 25, "25 tokens");
    TEST_ASSERT(um_total_cached_tokens(um) == 7, "7 cached tokens");

    usage_row_t rows[16];
    int         n = 0;
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
            TEST_ASSERT(db.rows[i].cached_prompt_tokens == 5, "k1 cached 5");
            TEST_ASSERT(db.rows[i].errors == 1, "k1 errors 1");
            found1 = 1;
        }
        if (db.rows[i].key_id == 2 && strcmp(db.rows[i].model_name, "claude-3") == 0) {
            TEST_ASSERT(db.rows[i].requests == 1, "k2 reqs 1");
            TEST_ASSERT(db.rows[i].cached_prompt_tokens == 2, "k2 cached 2");
            found2 = 1;
        }
    }
    TEST_ASSERT(found1 && found2, "both rows flushed");

    /* HDR sanity: openai recorded 3 samples (50ms, 120ms, 90ms) */
    TEST_ASSERT(um_provider_sampled(um, "openai") == 3, "openai 3 samples");
    long p50 = um_provider_percentile_ns(um, "openai", 50.0);
    TEST_ASSERT(p50 >= 50000000 && p50 <= 120000000, "p50 in range, got %ld", p50);
    TEST_ASSERT(um_provider_count_below_ns(um, "openai", 100000000) == 2, "2 samples <= 100ms");

    /* metrics text contains expected prefixes */
    char buf[16384];
    TEST_ASSERT(metrics_render(um, buf, sizeof buf) == 0, "render ok");
    TEST_ASSERT(strstr(buf, "aigate_requests_total 3") != NULL, "requests line");
    TEST_ASSERT(strstr(buf, "aigate_errors_total 1") != NULL, "errors line");
    TEST_ASSERT(strstr(buf, "aigate_tokens_total 25") != NULL, "tokens line");
    TEST_ASSERT(strstr(buf, "aigate_tokens_cached_total 7") != NULL, "cached tokens line");
    TEST_ASSERT(strstr(buf, "aigate_upstream_requests_total{provider=\"openai\"} 3") != NULL,
                "per-provider counter");
    TEST_ASSERT(
        strstr(buf,
               "aigate_upstream_latency_ns_ns_bucket{provider=\"openai\",le=\"50000000\"} 1") !=
            NULL,
        "50ms bucket == 1");

    usage_meter_free(um);
    pg_store_close(ps);
}

TEST_CASE(test_um_drain_fail_requeue)
{
    struct um_db db;
    memset(&db, 0, sizeof db);
    pg_store_t* ps = open_um_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    usage_meter_t* um = usage_meter_new(ps, NULL, 0);
    TEST_ASSERT(um != NULL, "meter new");

    um_record(um, 1, "gpt-4o", 200, 7, 11, 5, 50000000, "openai");
    um_record(um, 2, "claude-3", 200, 3, 4, 2, 90000000, "anthropic");

    /* failed flush: rows must be re-queued, counters preserved */
    db.fail_flush = 1;
    usage_row_t rows[16];
    int         n = 0;
    TEST_ASSERT(um_drain(um, rows, 16, &n) == -1, "drain flush fails");
    TEST_ASSERT(n == 2, "2 rows drained before failure");
    TEST_ASSERT(um_total_requests(um) == 2, "requests still 2");
    TEST_ASSERT(um_total_tokens(um) == 25, "tokens still 25");

    /* unflush and drain again with flush restored: values must re-appear */
    TEST_ASSERT(um_unflush(um, rows, n) == 0, "unflush ok");
    db.fail_flush = 0;
    TEST_ASSERT(um_drain(um, rows, 16, &n) == 0, "second drain ok");
    TEST_ASSERT(n == 2, "2 rows flushed on retry");
    int found1 = 0, found2 = 0;
    for (int i = db.n_rows - 2; i < db.n_rows; i++) {
        if (db.rows[i].key_id == 1 && strcmp(db.rows[i].model_name, "gpt-4o") == 0) {
            TEST_ASSERT(db.rows[i].requests == 1, "k1 reqs 1");
            TEST_ASSERT(db.rows[i].prompt_tokens == 7, "k1 prompt 7");
            found1 = 1;
        }
        if (db.rows[i].key_id == 2 && strcmp(db.rows[i].model_name, "claude-3") == 0) {
            found2 = 1;
        }
    }
    TEST_ASSERT(found1 && found2, "both rows re-flushed");

    /* no accumulation left */
    TEST_ASSERT(um_drain(um, rows, 16, &n) == 0, "final drain ok");
    TEST_ASSERT(n == 0, "accumulator empty");

    usage_meter_free(um);
    pg_store_close(ps);
}

TEST_CASE(test_um_provider_metering)
{
    struct um_db db;
    memset(&db, 0, sizeof db);
    pg_store_t* ps = open_um_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    usage_meter_t* um = usage_meter_new(ps, NULL, 0); /* no worker: manual drain */
    TEST_ASSERT(um != NULL, "meter new");

    /* Every label the adapter registry can route must be metered; anything
     * else is skipped. 9 labels vs 16 slots: no overflow. */
    const char* all_labels[] = {"openai",
                                "ollama",
                                "azure",
                                "deepseek",
                                "siliconflow",
                                "vllm",
                                "anthropic",
                                "gemini",
                                "google"};
    for (size_t i = 0; i < (sizeof all_labels) / (sizeof all_labels[0]); i++) {
        um_record(um, 1, "m", 200, 0, 0, 0, 1000000, all_labels[i]);
    }
    um_record(um, 1, "m", 200, 0, 0, 0, 1000000, "nonexistent");
    um_record(um, 1, "m", 200, 0, 0, 0, 1000000, NULL);

    char names[16][32];
    int  n = um_provider_names(um, names, 16);
    TEST_ASSERT(n == 9, "9 providers metered, got %d", n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT(um_provider_sampled(um, names[i]) == 1, "provider %s sampled once", names[i]);
    }
    TEST_ASSERT(um_provider_sampled(um, "vllm") == 1, "vllm metered (regression)");
    TEST_ASSERT(um_provider_sampled(um, "google") == 1, "google metered (regression)");
    TEST_ASSERT(um_provider_sampled(um, "nonexistent") == 0, "unknown not metered");
    usage_meter_free(um);
    pg_store_close(ps);
}

TEST_CASE(test_metrics_acl)
{
    TEST_ASSERT(metrics_acl_allows("127.0.0.1", "") == 1, "empty acl allows");
    TEST_ASSERT(metrics_acl_allows("127.0.0.1", "127.0.0.1") == 1, "exact");
    TEST_ASSERT(metrics_acl_allows("127.0.0.1", "127.0.0.1,10.0.0.0/8") == 1, "list hit");
    TEST_ASSERT(metrics_acl_allows("8.8.8.8", "127.0.0.1,10.0.0.0/8") == 0, "outside denied");
    TEST_ASSERT(metrics_acl_allows("10.1.2.3", "10.0.0.0/8") == 1, "/8 match");
    TEST_ASSERT(metrics_acl_allows("9.1.2.3", "10.0.0.0/8") == 0, "/8 miss");
}

TEST_CASE(test_metrics_failover)
{
    metrics_reset_failovers();
    TEST_ASSERT(metrics_total_failovers() == 0, "initially 0 failovers");

    metrics_inc_failover("gpt-4o", "openai", "azure");
    metrics_inc_failover("gpt-4o", "openai", "azure");
    metrics_inc_failover("claude-3-5", "anthropic", "aws-bedrock");

    TEST_ASSERT(metrics_total_failovers() == 3, "total failovers == 3");
    TEST_ASSERT(metrics_get_failover("gpt-4o", "openai", "azure") == 2,
                "gpt-4o openai->azure == 2");
    TEST_ASSERT(metrics_get_failover("claude-3-5", "anthropic", "aws-bedrock") == 1,
                "claude-3-5 anthropic->aws == 1");
    TEST_ASSERT(metrics_get_failover("gpt-4o", "azure", "openai") == 0, "reverse direction == 0");

    char buf[4096];
    TEST_ASSERT(metrics_render(NULL, buf, sizeof buf) == 0, "render ok without um");
    TEST_ASSERT(strstr(buf,
                       "aigate_failover_total{model=\"gpt-4o\",from_provider=\"openai\",to_"
                       "provider=\"azure\"} 2") != NULL,
                "contains gpt-4o failover metric");
    TEST_ASSERT(strstr(buf,
                       "aigate_failover_total{model=\"claude-3-5\",from_provider=\"anthropic\",to_"
                       "provider=\"aws-bedrock\"} 1") != NULL,
                "contains claude failover metric");

    metrics_reset_failovers();
    TEST_ASSERT(metrics_total_failovers() == 0, "reset ok");
}

TEST_CASE(test_um_request_ring)
{
    struct um_db db;
    memset(&db, 0, sizeof db);
    pg_store_t* ps = open_um_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    usage_meter_t* um = usage_meter_new(ps, NULL, 0); /* no worker: manual drain */
    TEST_ASSERT(um != NULL, "meter new");

    um_record(um, 1, "gpt-4o", 200, 7, 11, 5, 50000000, "openai");
    um_record(um, 2, "claude-3", 500, 3, 4, 2, 90000000, "anthropic");

    usage_request_row_t buf[16];
    int                 n = 0;

    /* drain = copy + advance head; no flush call */
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain reqs");
    TEST_ASSERT(n == 2, "2 audit rows, got %d", n);
    TEST_ASSERT(db.n_reqs == 0, "drain does not flush");
    TEST_ASSERT(buf[0].key_id == 1 && strcmp(buf[0].provider, "openai") == 0 &&
                    buf[0].http_status == 200 && buf[0].prompt_tokens == 7 &&
                    buf[0].latency_ns == 50000000,
                "row 0 fields");
    TEST_ASSERT(buf[1].key_id == 2 && buf[1].http_status == 500, "row 1 fields");
    TEST_ASSERT(um_requests_dropped(um) == 0, "nothing dropped");

    /* manual flush of the drained rows, then release (no-op confirm) */
    TEST_ASSERT(um_flush_reqs(&db, buf, n) == 0, "manual flush");
    TEST_ASSERT(um_release_requests(um, n) == 0, "release no-op confirm");
    TEST_ASSERT(db.n_reqs == 2, "fake now holds 2");
    TEST_ASSERT(db.req_flush_calls == 1, "one flush call");

    /* simulate a worker tick: record, drain, flush fails -> re-queue */
    db.fail_req_flush = 1;
    um_record(um, 3, "gpt-4o", 200, 1, 1, 0, 1000000, "openai");
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain reqs 2");
    TEST_ASSERT(n == 1, "1 pending audit row, got %d", n);
    TEST_ASSERT(um_flush_reqs(&db, buf, n) == -1, "flush fails");
    TEST_ASSERT(um_requeue_requests(um, n) == 0, "re-queue after failure");
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain again");
    TEST_ASSERT(n == 1, "row survives re-queue");
    db.fail_req_flush = 0;
    TEST_ASSERT(um_flush_reqs(&db, buf, n) == 0, "flush succeeds on retry");
    TEST_ASSERT(um_release_requests(um, n) == 0, "release after success");
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain empty");
    TEST_ASSERT(n == 0, "ring empty after release");

    usage_meter_free(um);
    pg_store_close(ps);
}

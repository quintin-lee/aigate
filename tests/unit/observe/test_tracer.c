/** @file test_tracer.c
 *  @brief Unit tests for OpenTelemetry distributed tracing and W3C TraceContext.
 */
#include "observe/tracer.h"
#include "run_tests.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

TEST_CASE(test_w3c_traceparent_parsing)
{
    trace_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Valid W3C traceparent */
    const char* valid_header = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, valid_header) == true,
                "valid traceparent should parse");
    TEST_ASSERT(strcmp(ctx.trace_id, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "trace_id matches");
    TEST_ASSERT(strcmp(ctx.inbound_parent_id, "00f067aa0ba902b7") == 0, "parent_id matches");
    TEST_ASSERT(ctx.is_sampled == true, "flags 01 means sampled");
    TEST_ASSERT(ctx.trace_flags == 0x01, "flags byte is 0x01");

    /* Valid traceparent with sampled=00 */
    memset(&ctx, 0, sizeof(ctx));
    const char* unsampled_header = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, unsampled_header) == true,
                "unsampled traceparent should parse");
    TEST_ASSERT(ctx.is_sampled == false, "flags 00 means unsampled");
    TEST_ASSERT(ctx.trace_flags == 0x00, "flags byte is 0x00");

    /* Uppercase hex parsing normalizes to lowercase */
    memset(&ctx, 0, sizeof(ctx));
    const char* upper_header = "00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, upper_header) == true,
                "uppercase traceparent parses");
    TEST_ASSERT(strcmp(ctx.trace_id, "4bf92f3577b34da6a3ce929d0e0e4736") == 0,
                "trace_id normalized to lowercase");
    TEST_ASSERT(strcmp(ctx.inbound_parent_id, "00f067aa0ba902b7") == 0,
                "parent_id normalized to lowercase");

    /* Invalid formats */
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT(tracer_parse_traceparent(&ctx, "invalid-header") == false,
                "invalid format should fail");
    TEST_ASSERT(tracer_parse_traceparent(&ctx, "00-short-00f067aa0ba902b7-01") == false,
                "short trace id fails");
    TEST_ASSERT(tracer_parse_traceparent(&ctx, NULL) == false, "null header fails");
    TEST_ASSERT(tracer_parse_traceparent(NULL, valid_header) == false, "null ctx fails");

    /* All-zero trace_id fails */
    const char* zero_trace = "00-00000000000000000000000000000000-00f067aa0ba902b7-01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, zero_trace) == false, "all zero trace id fails");

    /* All-zero parent_id fails */
    const char* zero_parent = "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, zero_parent) == false, "all zero parent id fails");

    /* Invalid version fails */
    const char* bad_ver = "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, bad_ver) == false, "version ff fails");

    /* Invalid delimiter fails */
    const char* bad_delim = "00_4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7_01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, bad_delim) == false, "bad delimiters fail");

    /* Invalid hex flag fails */
    const char* bad_flag = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-zz";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, bad_flag) == false, "bad hex flags fail");

    /* Trailing chars fail */
    const char* too_long = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-extra";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, too_long) == false, "too long header fails");

    /* Outbound format injection */
    tracer_parse_traceparent(&ctx, valid_header);
    char out_buf[128];
    tracer_format_traceparent(&ctx, "1122334455667788", out_buf, sizeof(out_buf));
    TEST_ASSERT(strncmp(out_buf, "00-4bf92f3577b34da6a3ce929d0e0e4736-1122334455667788-", 53) == 0,
                "formatted traceparent has valid prefix");
    TEST_ASSERT(strcmp(out_buf, "00-4bf92f3577b34da6a3ce929d0e0e4736-1122334455667788-01") == 0,
                "formatted traceparent matches exact string");

    /* Fallback to root_span_id when span_id is NULL */
    strncpy(ctx.root_span_id, "aabbccddeeff0011", sizeof(ctx.root_span_id));
    tracer_format_traceparent(&ctx, NULL, out_buf, sizeof(out_buf));
    TEST_ASSERT(strcmp(out_buf, "00-4bf92f3577b34da6a3ce929d0e0e4736-aabbccddeeff0011-01") == 0,
                "formatted traceparent uses root_span_id fallback");
}

TEST_CASE(test_span_lifecycle_and_timing)
{
    trace_context_t ctx;
    tracer_context_init(&ctx, NULL, true);

    TEST_ASSERT(strlen(ctx.trace_id) == 32, "auto generated trace_id is 32 hex");
    TEST_ASSERT(strlen(ctx.root_span_id) == 16, "auto generated root_span_id is 16 hex");
    TEST_ASSERT(ctx.is_sampled == true, "default sampled is true");
    TEST_ASSERT(ctx.span_count == 0, "initial span count is 0");
    TEST_ASSERT(ctx.req_start_realtime_us > 0, "realtime start timestamp recorded");

    int span_idx =
        tracer_span_start(&ctx, "guardrails_inbound", SPAN_KIND_INTERNAL, ctx.root_span_id);
    TEST_ASSERT(span_idx >= 0, "span started successfully");
    TEST_ASSERT(ctx.span_count == 1, "span count is 1");
    TEST_ASSERT(strlen(ctx.spans[span_idx].span_id) == 16, "span_id is 16 hex chars");
    TEST_ASSERT(strcmp(ctx.spans[span_idx].parent_span_id, ctx.root_span_id) == 0,
                "parent matches root_span_id");
    TEST_ASSERT(ctx.spans[span_idx].start_time_ns > 0, "start_time_ns is non-zero");

    tracer_span_set_attr(&ctx, "guardrails_inbound", "aigate.guardrails.pii_masked", "2");
    tracer_span_set_attr_int(&ctx, "guardrails_inbound", "gen_ai.usage.prompt_tokens", 128);

    TEST_ASSERT(ctx.spans[span_idx].attr_count == 2, "2 attributes stored");
    TEST_ASSERT(strcmp(ctx.spans[span_idx].attributes[0].key, "aigate.guardrails.pii_masked") == 0,
                "key 0 matches");
    TEST_ASSERT(strcmp(ctx.spans[span_idx].attributes[0].value, "2") == 0, "val 0 matches");
    TEST_ASSERT(strcmp(ctx.spans[span_idx].attributes[1].key, "gen_ai.usage.prompt_tokens") == 0,
                "key 1 matches");
    TEST_ASSERT(strcmp(ctx.spans[span_idx].attributes[1].value, "128") == 0, "val 1 matches");

    /* Overwrite existing attribute key */
    tracer_span_set_attr(&ctx, "guardrails_inbound", "aigate.guardrails.pii_masked", "5");
    TEST_ASSERT(ctx.spans[span_idx].attr_count == 2, "attr_count unchanged on key overwrite");
    TEST_ASSERT(strcmp(ctx.spans[span_idx].attributes[0].value, "5") == 0,
                "value updated on overwrite");

    tracer_span_end(&ctx, "guardrails_inbound", SPAN_STATUS_OK, NULL);
    TEST_ASSERT(ctx.spans[span_idx].status == SPAN_STATUS_OK, "span status is OK");
    TEST_ASSERT(ctx.spans[span_idx].end_time_ns >= ctx.spans[span_idx].start_time_ns,
                "monotonic end >= start");

    /* Test error span with status description */
    int err_idx = tracer_span_start(&ctx, "upstream_ttft", SPAN_KIND_CLIENT, NULL);
    TEST_ASSERT(err_idx >= 0, "error span started");
    tracer_span_end(&ctx, "upstream_ttft", SPAN_STATUS_ERROR, "connection timeout");
    TEST_ASSERT(ctx.spans[err_idx].status == SPAN_STATUS_ERROR, "status is ERROR");
    TEST_ASSERT(strcmp(ctx.spans[err_idx].status_desc, "connection timeout") == 0,
                "status_desc recorded");

    /* Test span capacity overflow */
    while (ctx.span_count < TRACE_MAX_SPANS) {
        int idx = tracer_span_start(&ctx, "dummy_span", SPAN_KIND_INTERNAL, NULL);
        TEST_ASSERT(idx >= 0, "dummy span allocated");
    }
    TEST_ASSERT(ctx.span_count == TRACE_MAX_SPANS, "span count reached max");
    int overflow_idx = tracer_span_start(&ctx, "overflow", SPAN_KIND_INTERNAL, NULL);
    TEST_ASSERT(overflow_idx == -1, "overflow span rejected");

    /* Test attribute capacity overflow */
    for (int i = 0; i < TRACE_MAX_ATTRS + 4; i++) {
        char k[32];
        snprintf(k, sizeof(k), "k_%d", i);
        tracer_span_set_attr(&ctx, "upstream_ttft", k, "v");
    }
    TEST_ASSERT(ctx.spans[err_idx].attr_count == TRACE_MAX_ATTRS,
                "attributes capped at TRACE_MAX_ATTRS");

    /* Test cleanup */
    tracer_context_cleanup(&ctx);
    TEST_ASSERT(ctx.trace_id[0] == '\0', "context zeroed out on cleanup");
    TEST_ASSERT(ctx.span_count == 0, "span_count reset to 0");
}

TEST_CASE(test_trace_tail_sampling_decision)
{
    tracer_config_t cfg = {
        .enabled = true,
        .sample_rate = 0.0, /* 0% regular sampling */
        .slow_threshold_ms = 1000,
        .otlp_endpoint = "http://localhost:4318/v1/traces",
    };

    trace_context_t ctx;
    tracer_context_init(&ctx, NULL, false);
    ctx.is_sampled = false;

    /* Disabled tracer should never sample */
    tracer_config_t disabled_cfg = cfg;
    disabled_cfg.enabled = false;
    TEST_ASSERT(tracer_should_sample(&ctx, &disabled_cfg, 500, 5000) == false,
                "disabled tracer never samples even on error and slow");
    TEST_ASSERT(tracer_should_sample(&ctx, NULL, 500, 5000) == false, "null config never samples");

    /* Regular 200 OK within threshold should NOT be sampled */
    bool decision1 = tracer_should_sample(&ctx, &cfg, 200, 200);
    TEST_ASSERT(decision1 == false, "normal fast 200 is unsampled when rate is 0");

    /* HTTP 500 error MUST be tail-sampled */
    bool decision2 = tracer_should_sample(&ctx, &cfg, 500, 200);
    TEST_ASSERT(decision2 == true, "error 500 is tail-sampled");

    /* Slow query (> 1000ms) MUST be tail-sampled */
    bool decision3 = tracer_should_sample(&ctx, &cfg, 200, 1500);
    TEST_ASSERT(decision3 == true, "slow query > threshold is tail-sampled");

    /* Guardrail blocked (400) MUST be tail-sampled */
    bool decision4 = tracer_should_sample(&ctx, &cfg, 400, 50);
    TEST_ASSERT(decision4 == true, "blocked 400 is tail-sampled");

    /* Already-sampled context stays sampled */
    ctx.is_sampled = true;
    bool decision5 = tracer_should_sample(&ctx, &cfg, 200, 100);
    TEST_ASSERT(decision5 == true, "already-sampled ctx stays sampled");

    /* Sample rate 1.0 always samples */
    tracer_config_t full_cfg = cfg;
    full_cfg.sample_rate = 1.0;
    ctx.is_sampled = false;
    TEST_ASSERT(tracer_should_sample(&ctx, &full_cfg, 200, 100) == true,
                "sample rate 1.0 always samples");
}

struct concurrency_worker_arg {
    trace_ring_buffer_t* rb;
    int                  push_count;
    int                  worker_id;
};

static void*
ring_buffer_writer_worker(void* arg)
{
    struct concurrency_worker_arg* warg = (struct concurrency_worker_arg*)arg;
    for (int i = 0; i < warg->push_count; i++) {
        trace_context_t ctx;
        tracer_context_init(&ctx, NULL, true);
        snprintf(ctx.trace_id,
                 sizeof(ctx.trace_id),
                 "%08x%08x%08x%08x",
                 (unsigned int)warg->worker_id,
                 (unsigned int)i,
                 (unsigned int)warg->worker_id,
                 (unsigned int)i);
        trace_ring_buffer_push(warg->rb, &ctx);
        usleep(100);
    }
    return NULL;
}

static volatile bool g_concurrency_done = false;
static int           g_reader_popped = 0;

static void*
ring_buffer_reader_worker(void* arg)
{
    trace_ring_buffer_t* rb = (trace_ring_buffer_t*)arg;
    trace_context_t      out_ctx;
    while (!g_concurrency_done || trace_ring_buffer_count(rb) > 0) {
        if (trace_ring_buffer_pop(rb, &out_ctx, 10)) {
            g_reader_popped++;
        }
    }
    return NULL;
}

TEST_CASE(test_trace_ring_buffer_operations)
{
    /* 1. Basic create, push up to capacity and beyond (non-blocking overwrite & dropped counter) */
    trace_ring_buffer_t* rb = trace_ring_buffer_create(8);
    TEST_ASSERT(rb != NULL, "ring buffer created");
    TEST_ASSERT(trace_ring_buffer_count(rb) == 0, "initial count is 0");
    TEST_ASSERT(trace_ring_buffer_dropped(rb) == 0, "initial dropped is 0");

    trace_context_t sample_ctx;
    tracer_context_init(&sample_ctx, NULL, true);

    /* Push until full and beyond */
    for (int i = 0; i < 12; i++) {
        bool pushed = trace_ring_buffer_push(rb, &sample_ctx);
        TEST_ASSERT(pushed == true, "push returns true");
    }

    TEST_ASSERT(trace_ring_buffer_count(rb) == 8, "count is capped at capacity 8");
    TEST_ASSERT(trace_ring_buffer_dropped(rb) == 4, "4 overflow traces dropped without blocking");

    trace_context_t out_ctx;
    TEST_ASSERT(trace_ring_buffer_pop(rb, &out_ctx, 10) == true, "pop succeeds");
    TEST_ASSERT(trace_ring_buffer_count(rb) == 7, "count decremented to 7");

    trace_ring_buffer_destroy(rb);

    /* 2. FIFO order and overwrite correctness verification */
    trace_ring_buffer_t* fifo_rb = trace_ring_buffer_create(3);
    TEST_ASSERT(fifo_rb != NULL, "fifo ring buffer created");

    trace_context_t ctx1, ctx2, ctx3, ctx4, ctx5;
    tracer_context_init(&ctx1, NULL, true);
    strncpy(ctx1.trace_id, "00000000000000000000000000000001", sizeof(ctx1.trace_id));
    tracer_context_init(&ctx2, NULL, true);
    strncpy(ctx2.trace_id, "00000000000000000000000000000002", sizeof(ctx2.trace_id));
    tracer_context_init(&ctx3, NULL, true);
    strncpy(ctx3.trace_id, "00000000000000000000000000000003", sizeof(ctx3.trace_id));
    tracer_context_init(&ctx4, NULL, true);
    strncpy(ctx4.trace_id, "00000000000000000000000000000004", sizeof(ctx4.trace_id));
    tracer_context_init(&ctx5, NULL, true);
    strncpy(ctx5.trace_id, "00000000000000000000000000000005", sizeof(ctx5.trace_id));

    trace_ring_buffer_push(fifo_rb, &ctx1);
    trace_ring_buffer_push(fifo_rb, &ctx2);
    trace_ring_buffer_push(fifo_rb, &ctx3);
    TEST_ASSERT(trace_ring_buffer_count(fifo_rb) == 3, "fifo buffer full (count 3)");
    TEST_ASSERT(trace_ring_buffer_dropped(fifo_rb) == 0, "fifo buffer dropped 0");

    /* Pushing 4 and 5 overwrites 1 and 2 */
    trace_ring_buffer_push(fifo_rb, &ctx4);
    trace_ring_buffer_push(fifo_rb, &ctx5);
    TEST_ASSERT(trace_ring_buffer_count(fifo_rb) == 3, "count remains 3");
    TEST_ASSERT(trace_ring_buffer_dropped(fifo_rb) == 2, "dropped count is 2");

    /* Popping should return ctx3, ctx4, ctx5 in FIFO order */
    trace_context_t pop_res;
    TEST_ASSERT(trace_ring_buffer_pop(fifo_rb, &pop_res, 0) == true, "pop ctx3");
    TEST_ASSERT(strcmp(pop_res.trace_id, "00000000000000000000000000000003") == 0,
                "first popped is ctx3 (oldest remaining)");

    TEST_ASSERT(trace_ring_buffer_pop(fifo_rb, &pop_res, 0) == true, "pop ctx4");
    TEST_ASSERT(strcmp(pop_res.trace_id, "00000000000000000000000000000004") == 0,
                "second popped is ctx4");

    TEST_ASSERT(trace_ring_buffer_pop(fifo_rb, &pop_res, 0) == true, "pop ctx5");
    TEST_ASSERT(strcmp(pop_res.trace_id, "00000000000000000000000000000005") == 0,
                "third popped is ctx5");

    TEST_ASSERT(trace_ring_buffer_count(fifo_rb) == 0, "buffer is now empty");

    /* 3. Pop timeout on empty buffer */
    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    bool timed_pop = trace_ring_buffer_pop(fifo_rb, &pop_res, 50);
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    TEST_ASSERT(timed_pop == false, "pop on empty buffer times out and returns false");
    uint64_t wait_ms = (uint64_t)(end_ts.tv_sec - start_ts.tv_sec) * 1000ULL +
                       (uint64_t)(end_ts.tv_nsec - start_ts.tv_nsec) / 1000000ULL;
    TEST_ASSERT(wait_ms >= 35, "waited approximately 50ms on timeout");
    trace_ring_buffer_destroy(fifo_rb);

    /* 4. Concurrency with reader/writer threads */
    trace_ring_buffer_t* concurrent_rb = trace_ring_buffer_create(8);
    TEST_ASSERT(concurrent_rb != NULL, "concurrent ring buffer created");

    pthread_t                     writer_threads[4];
    pthread_t                     reader_thread;
    struct concurrency_worker_arg wargs[4];

    g_concurrency_done = false;
    g_reader_popped = 0;

    for (int i = 0; i < 4; i++) {
        wargs[i].rb = concurrent_rb;
        wargs[i].push_count = 50;
        wargs[i].worker_id = i;
        pthread_create(&writer_threads[i], NULL, ring_buffer_writer_worker, &wargs[i]);
    }
    pthread_create(&reader_thread, NULL, ring_buffer_reader_worker, concurrent_rb);

    for (int i = 0; i < 4; i++) {
        pthread_join(writer_threads[i], NULL);
    }
    g_concurrency_done = true;
    pthread_join(reader_thread, NULL);

    /* Verify ring buffer invariants after concurrent stress */
    size_t   final_count = trace_ring_buffer_count(concurrent_rb);
    uint64_t final_dropped = trace_ring_buffer_dropped(concurrent_rb);

    TEST_ASSERT((size_t)(g_reader_popped + final_count + final_dropped) == 200,
                "conservation invariant: popped + count + dropped == pushed");
    TEST_ASSERT(final_count == 0, "reader drained buffer completely");
    TEST_ASSERT(g_reader_popped > 0, "reader popped items concurrently");
    TEST_ASSERT(g_reader_popped + final_dropped == 200,
                "all pushed items were either popped or dropped");

    trace_ring_buffer_destroy(concurrent_rb);
}

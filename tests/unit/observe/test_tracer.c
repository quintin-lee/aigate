/** @file test_tracer.c
 *  @brief Unit tests for OpenTelemetry distributed tracing and W3C TraceContext.
 */
#include "observe/tracer.h"
#include "run_tests.h"
#include <string.h>

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

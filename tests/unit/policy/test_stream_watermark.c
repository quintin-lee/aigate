#include "run_tests.h"
#include "policy/watermark_engine.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_stream_watermark_openai_sse_injection)
{
    watermark_payload_t in_p = {
        .timestamp = 1791554000,
        .key_id = 5566,
        .short_trace = 0x1122334455667788ULL,
        .crc_valid = false,
    };

    stream_watermark_state_t state;
    watermark_stream_init(&state, &in_p, "openai", 50);

    const char* chunk1 =
        "data: {\"choices\":[{\"delta\":{\"content\":\"This is confidential AI generated text.\"}}]}\n\n";
    const char* chunk2 = "data: [DONE]\n\n";

    char out1[1024];
    size_t out1_len = 0;
    TEST_ASSERT(watermark_stream_feed(&state, chunk1, strlen(chunk1), false, out1, sizeof(out1), &out1_len) == 0,
                "feed chunk1");
    out1[out1_len] = '\0';

    char out2[1024];
    size_t out2_len = 0;
    TEST_ASSERT(watermark_stream_feed(&state, chunk2, strlen(chunk2), true, out2, sizeof(out2), &out2_len) == 0,
                "feed chunk2");
    out2[out2_len] = '\0';

    /* Concatenate all chunks and decode */
    char full_stream[2048];
    snprintf(full_stream, sizeof(full_stream), "%s%s", out1, out2);

    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(full_stream, strlen(full_stream), &out_p) == 0,
                "decode from full openai sse stream");
    TEST_ASSERT(out_p.key_id == 5566, "key_id match: %u", out_p.key_id);
    TEST_ASSERT(out_p.short_trace == 0x1122334455667788ULL, "short_trace match");
}

TEST_CASE(test_stream_watermark_anthropic_sse_injection)
{
    watermark_payload_t in_p = {
        .timestamp = 1791554500,
        .key_id = 7711,
        .short_trace = 0x8877665544332211ULL,
        .crc_valid = false,
    };

    stream_watermark_state_t state;
    watermark_stream_init(&state, &in_p, "anthropic", 50);

    const char* chunk1 =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Internal enterprise strategy report.\"}}\n\n";
    const char* chunk2 =
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    char out1[1024];
    size_t out1_len = 0;
    TEST_ASSERT(watermark_stream_feed(&state, chunk1, strlen(chunk1), false, out1, sizeof(out1), &out1_len) == 0,
                "feed anthropic chunk1");
    out1[out1_len] = '\0';

    char out2[1024];
    size_t out2_len = 0;
    TEST_ASSERT(watermark_stream_feed(&state, chunk2, strlen(chunk2), true, out2, sizeof(out2), &out2_len) == 0,
                "feed anthropic chunk2");
    out2[out2_len] = '\0';

    char full_stream[2048];
    snprintf(full_stream, sizeof(full_stream), "%s%s", out1, out2);

    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(full_stream, strlen(full_stream), &out_p) == 0,
                "decode from anthropic sse stream");
    TEST_ASSERT(out_p.key_id == 7711, "key_id match: %u", out_p.key_id);
}

TEST_CASE(test_stream_watermark_no_punctuation_fallback)
{
    watermark_payload_t in_p = {
        .timestamp = 1791555000,
        .key_id = 3344,
        .short_trace = 0xA1B2C3D4E5F60718ULL,
        .crc_valid = false,
    };

    stream_watermark_state_t state;
    watermark_stream_init(&state, &in_p, "openai", 100);

    /* Single word with no punctuation */
    const char* chunk1 =
        "data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n\n";
    const char* chunk2 = "data: [DONE]\n\n";

    char out1[1024];
    size_t out1_len = 0;
    watermark_stream_feed(&state, chunk1, strlen(chunk1), false, out1, sizeof(out1), &out1_len);
    out1[out1_len] = '\0';

    char out2[1024];
    size_t out2_len = 0;
    watermark_stream_feed(&state, chunk2, strlen(chunk2), true, out2, sizeof(out2), &out2_len);
    out2[out2_len] = '\0';

    char full_stream[2048];
    snprintf(full_stream, sizeof(full_stream), "%s%s", out1, out2);

    /* Ensure fallback delta chunk was generated before [DONE] */
    TEST_ASSERT(strstr(out2, "data: [DONE]") != NULL, "must preserve [DONE]");
    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(full_stream, strlen(full_stream), &out_p) == 0,
                "decode fallback watermark from short response");
    TEST_ASSERT(out_p.key_id == 3344, "key_id match: %u", out_p.key_id);
}

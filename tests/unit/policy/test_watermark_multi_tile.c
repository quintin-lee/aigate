#include "run_tests.h"
#include "policy/watermark_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_watermark_multi_tile_injection_and_sliding_decode)
{
    watermark_payload_t in_p = {
        .timestamp = 1791553000,
        .key_id = 9901,
        .short_trace = 0xFEDCBA9876543210ULL,
        .crc_valid = false,
    };

    const char* doc =
        "Artificial intelligence models process natural language with precision. "
        "This technology revolutionizes automated reasoning across many domains. "
        "Security watermarks protect against intellectual property theft and unauthorized leaks. "
        "Confidential document verification requires robust forensic traceability.";

    size_t out_len = 0;
    char* watermarked = watermark_inject_multi_tile(doc, strlen(doc), &in_p, 70, &out_len);
    TEST_ASSERT(watermarked != NULL, "watermark_inject_multi_tile should succeed");
    TEST_ASSERT(out_len >= strlen(doc) + (2 * WATERMARK_UTF8_BYTES),
                "Should have at least 2 watermark tiles injected, got len %zu vs orig %zu",
                out_len,
                strlen(doc));

    /* 1. Full document decode */
    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(watermarked, out_len, &out_p) == 0, "full decode should succeed");
    TEST_ASSERT(out_p.key_id == 9901, "key_id match: %u", out_p.key_id);
    TEST_ASSERT(out_p.short_trace == 0xFEDCBA9876543210ULL, "short_trace match");

    /* 2. Decode all tiles */
    watermark_payload_t all_tiles[8];
    int n_tiles = 0;
    TEST_ASSERT(watermark_decode_all(watermarked, out_len, all_tiles, 8, &n_tiles) == 0,
                "decode_all should succeed");
    TEST_ASSERT(n_tiles >= 2, "Expected at least 2 valid tiles decoded, got %d", n_tiles);
    TEST_ASSERT(all_tiles[0].key_id == 9901 && all_tiles[1].key_id == 9901,
                "all tiles match key_id");

    /* 3. Tail truncation resilience: keep only the first 40% of the document */
    size_t front_len = out_len * 4 / 10;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(watermarked, front_len, &out_p) == 0,
                "tail truncation (first 40%%) should still recover early watermark tile");
    TEST_ASSERT(out_p.key_id == 9901, "recovered key_id after tail truncation: %u", out_p.key_id);

    /* 4. Head truncation resilience: keep only the last 45% of the document */
    size_t back_offset = out_len * 55 / 100;
    const char* back_slice = watermarked + back_offset;
    size_t back_len = out_len - back_offset;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(back_slice, back_len, &out_p) == 0,
                "head truncation (last 45%%) should still recover late watermark tile");
    TEST_ASSERT(out_p.key_id == 9901, "recovered key_id after head truncation: %u", out_p.key_id);

    free(watermarked);
}

TEST_CASE(test_watermark_chinese_punctuation_anchoring)
{
    watermark_payload_t in_p = {
        .timestamp = 1791553500,
        .key_id = 8822,
        .short_trace = 0x1234567890ABCDEFULL,
        .crc_valid = false,
    };

    const char* doc =
        "人工智能正在深刻改变软件工程。"
        "模型网关为企业级应用提供了安全屏障与审计追踪。"
        "多段零宽水印技术能够在文本被截断或局部复制时，依然保持追溯能力！";

    size_t out_len = 0;
    char* watermarked = watermark_inject_multi_tile(doc, strlen(doc), &in_p, 40, &out_len);
    TEST_ASSERT(watermarked != NULL, "watermark_inject_multi_tile failed for chinese doc");
    TEST_ASSERT(out_len >= strlen(doc) + (2 * WATERMARK_UTF8_BYTES),
                "Should have at least 2 tiles for chinese text");

    /* 1. Full decode */
    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(watermarked, out_len, &out_p) == 0, "chinese full decode");
    TEST_ASSERT(out_p.key_id == 8822, "key_id match");

    /* 2. Decode after tail truncation (remove last sentence) */
    size_t half_len = out_len / 2;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(watermarked, half_len, &out_p) == 0,
                "chinese tail truncation decode");
    TEST_ASSERT(out_p.key_id == 8822, "key_id match after tail truncation");

    /* 3. Decode after head truncation (remove first sentence) */
    size_t head_skip = out_len * 4 / 10;
    memset(&out_p, 0, sizeof(out_p));
    TEST_ASSERT(watermark_decode(watermarked + head_skip, out_len - head_skip, &out_p) == 0,
                "chinese head truncation decode");
    TEST_ASSERT(out_p.key_id == 8822, "key_id match after head truncation");

    free(watermarked);
}

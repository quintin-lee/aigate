#include "watermark_engine.h"
#include "run_tests.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

TEST_CASE(test_watermark_encode_decode_roundtrip)
{
    watermark_payload_t in_p;
    in_p.timestamp = 1791552000;
    in_p.key_id = 4096;
    in_p.short_trace = 0x1122334455667788ULL;
    in_p.crc_valid = false;

    const char* origin = "Hello, this is a confidential AI generated response. Thank you.";
    size_t      out_len = 0;
    char*       watermarked = watermark_inject(origin, strlen(origin), &in_p, &out_len);
    TEST_ASSERT(watermarked != NULL, "watermark_inject should not return NULL");
    TEST_ASSERT(out_len > strlen(origin), "Length should increase due to zero-width UTF-8 bytes");

    /* 逆向解码验证 */
    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof out_p);
    int rc = watermark_decode(watermarked, out_len, &out_p);
    TEST_ASSERT(rc == 0, "watermark_decode failed, rc=%d", rc);
    TEST_ASSERT(out_p.crc_valid == true, "CRC should be valid");
    TEST_ASSERT(
        out_p.key_id == in_p.key_id, "key_id mismatch: %u vs %u", out_p.key_id, in_p.key_id);
    TEST_ASSERT(out_p.timestamp == in_p.timestamp,
                "timestamp mismatch: %u vs %u",
                out_p.timestamp,
                in_p.timestamp);
    TEST_ASSERT(out_p.short_trace == in_p.short_trace, "short_trace mismatch");

    free(watermarked);
}

TEST_CASE(test_watermark_mixed_chinese_and_truncation)
{
    watermark_payload_t in_p;
    in_p.timestamp = 1791552500;
    in_p.key_id = 8888;
    in_p.short_trace = 0xAABBCCDDEEFF0011ULL;
    in_p.crc_valid = false;

    const char* origin = "你好，这是企业内部核心模型输出的敏感分析结果。请妥善保管！";
    size_t      out_len = 0;
    char*       watermarked = watermark_inject(origin, strlen(origin), &in_p, &out_len);
    TEST_ASSERT(watermarked != NULL, "watermark_inject failed for chinese text");

    /* 完整解码验证 */
    watermark_payload_t out_p;
    memset(&out_p, 0, sizeof out_p);
    int rc = watermark_decode(watermarked, out_len, &out_p);
    TEST_ASSERT(rc == 0 && out_p.crc_valid, "decode failed for chinese text");
    TEST_ASSERT(out_p.key_id == 8888, "key_id mismatch");

    /* 截断测试：破坏后半部分零宽字符，应安全返回 -1 且不越界 */
    watermark_payload_t broken_p;
    memset(&broken_p, 0, sizeof broken_p);
    int rc_broken = watermark_decode(watermarked, strlen(origin) + 20, &broken_p);
    TEST_ASSERT(rc_broken != 0, "Truncated watermark should fail CRC or framing");

    /* 无水印测试 */
    watermark_payload_t clean_p;
    memset(&clean_p, 0, sizeof clean_p);
    int rc_clean = watermark_decode(origin, strlen(origin), &clean_p);
    TEST_ASSERT(rc_clean != 0, "Clean text without watermark should return non-zero");

    free(watermarked);
}

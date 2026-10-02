/**
 * @file test_prompt_compressor.c
 * @brief Unit tests for prompt compression and token pruning engine.
 */

#include "policy/prompt_compressor.h"
#include "run_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_compressor_fast_token_estimate)
{
    const char* text_en = "Hello world, this is a test prompt for token estimation.";
    uint32_t    tokens_en = compressor_estimate_tokens(text_en, strlen(text_en));
    TEST_ASSERT(tokens_en >= 10 && tokens_en <= 20, "English token estimate out of bounds");

    const char* text_zh = "你好世界，这是一个中文提示词测试。";
    uint32_t    tokens_zh = compressor_estimate_tokens(text_zh, strlen(text_zh));
    TEST_ASSERT(tokens_zh >= 10 && tokens_zh <= 35, "Chinese token estimate out of bounds");
}

TEST_CASE(test_compressor_whitespace_sanitization)
{
    const char* raw = "Line 1\n\n\n\nLine 2   with   extra   spaces\n```python\ndef foo():\n    # "
                      "indented code\n\n\n    return 42\n```\nAfter code\n\n\nEnd";
    char   out_buf[1024];
    size_t out_len =
        compressor_sanitize_whitespace(raw, strlen(raw), out_buf, sizeof(out_buf), true);
    TEST_ASSERT(out_len > 0, "Sanitization failed");

    /* Verifies that non-code consecutive newlines were collapsed */
    TEST_ASSERT(strstr(out_buf, "Line 1\n\nLine 2") != NULL,
                "Failed to collapse newlines outside code");
    /* Verifies that inside ```python, indentation and newlines are preserved */
    TEST_ASSERT(strstr(out_buf, "    # indented code\n\n\n    return 42") != NULL,
                "Code block corrupted");
    TEST_ASSERT(strstr(out_buf, "After code\n\nEnd") != NULL, "Trailing newlines not collapsed");
}

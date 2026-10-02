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
    char        out_buf[1024];
    size_t      out_len =
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

TEST_CASE(test_compressor_history_windowing_and_safety)
{
    const char* mock_payload =
        "{\n"
        "  \"model\": \"gpt-4o\",\n"
        "  \"messages\": [\n"
        "    {\"role\": \"system\", \"content\": \"You are a helpful assistant. Output in JSON "
        "format.\"},\n"
        "    {\"role\": \"user\", \"content\": \"My email is {{PII_EMAIL_1}} and turn 1.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"Turn 1 answer.\"},\n"
        "    {\"role\": \"user\", \"content\": \"Turn 2 question.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"Turn 2 answer.\"},\n"
        "    {\"role\": \"user\", \"content\": \"Turn 3 question.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"Turn 3 answer.\"},\n"
        "    {\"role\": \"user\", \"content\": \"Turn 4 question: show me python code.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"```python\\nprint('hello')\\n```\"},\n"
        "    {\"role\": \"user\", \"content\": \"Final user question: what is my email?\"}\n"
        "  ]\n"
        "}";

    compressor_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.enabled = true;
    rule.level = COMPRESS_LEVEL_MODERATE;
    rule.min_tokens = 10;
    rule.max_history_turns = 2; /* Only keep last 2 turns + system + final user */
    rule.preserve_system = true;
    rule.preserve_code = true;
    rule.preserve_tools = true;

    compressor_result_t res;
    memset(&res, 0, sizeof(res));
    bool ok = prompt_compressor_process_payload(mock_payload, strlen(mock_payload), &rule, &res);
    TEST_ASSERT(ok, "compressor process payload failed");
    TEST_ASSERT(res.compressed, "expected compression to occur");
    TEST_ASSERT(res.compressed_payload != NULL, "compressed payload is null");
    TEST_ASSERT(res.saved_tokens > 0, "expected tokens saved");

    /* System prompt preserved */
    TEST_ASSERT(strstr(res.compressed_payload, "Output in JSON format") != NULL,
                "System prompt lost");
    /* Final user question strictly preserved */
    TEST_ASSERT(strstr(res.compressed_payload, "Final user question: what is my email?") != NULL,
                "Final question lost");
    /* Python code block preserved */
    TEST_ASSERT(strstr(res.compressed_payload, "```python") != NULL, "Code block lost");
    /* Older turn 1 question dropped or folded */
    TEST_ASSERT(strstr(res.compressed_payload, "turn 1.") == NULL, "Turn 1 should be pruned");

    prompt_compressor_result_cleanup(&res);
}

TEST_CASE(test_compressor_sentence_density_pruning_and_cache)
{
    /* Text with polite fillers and low entropy sentences */
    const char* text_with_filler =
        "As an AI assistant, I would be pleased to assist you with this comprehensive request. "
        "The server port is configured to 8080 and bind to 127.0.0.1. "
        "Please feel free to ask if you have any further questions or inquiries.";

    char   pruned[512];
    size_t pruned_len = compressor_prune_sentence_density(
        text_with_filler, strlen(text_with_filler), 0.60, pruned, sizeof(pruned));
    TEST_ASSERT(pruned_len > 0, "Prune failed");
    /* Core config sentence preserved */
    TEST_ASSERT(strstr(pruned, "port is configured to 8080") != NULL, "Core sentence was pruned");

    /* Test Snapshot Cache & Stats */
    compressor_cache_t* cache = compressor_cache_create(200);
    TEST_ASSERT(cache != NULL, "cache create failed");

    compressor_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.req_id, sizeof(snap.req_id), "req-12345");
    snprintf(snap.model, sizeof(snap.model), "gpt-4o");
    snap.original_tokens = 3000;
    snap.compressed_tokens = 1500;
    snap.saved_tokens = 1500;
    snap.compression_ratio = 0.50;
    snap.elapsed_us = 210;

    compressor_cache_record(cache, &snap);

    compressor_stats_t stats;
    compressor_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.total_evaluated == 1, "evaluated mismatch");
    TEST_ASSERT(stats.total_saved_tokens == 1500, "saved mismatch");

    compressor_cache_destroy(cache);
}

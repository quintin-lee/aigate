/**
 * @file prompt_compressor.c
 * @brief Implementation of prompt compression, token estimation, and whitespace sanitization.
 */

#include "policy/prompt_compressor.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t
compressor_estimate_tokens(const char* text, size_t len)
{
    if (!text || len == 0) {
        return 0;
    }

    uint32_t words = 0;
    uint32_t multibyte_chars = 0;
    bool     in_word = false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) {
            /* UTF-8 multi-byte sequence leading byte check */
            if ((c & 0xC0) != 0x80) {
                multibyte_chars++;
            }
            if (in_word) {
                in_word = false;
            }
        } else if (isspace(c) || ispunct(c)) {
            if (in_word) {
                words++;
                in_word = false;
            }
            if (ispunct(c)) {
                /* Common punctuation counts as separate token in BPE */
                words++;
            }
        } else {
            in_word = true;
        }
    }
    if (in_word) {
        words++;
    }

    /* English tokens: ~1.25 to 1.3 per word; CJK tokens: ~1.0 to 1.5 per char */
    uint32_t est = (uint32_t)((double)words * 1.3) + multibyte_chars;
    return est > 0 ? est : 1;
}

size_t
compressor_sanitize_whitespace(
    const char* src, size_t src_len, char* dst, size_t dst_cap, bool preserve_code)
{
    if (!src || src_len == 0 || !dst || dst_cap == 0) {
        return 0;
    }

    bool   in_code_block = false;
    size_t r = 0;
    size_t w = 0;
    int    consecutive_newlines = 0;
    bool   in_whitespace_run = false;

    while (r < src_len && w + 1 < dst_cap) {
        /* Check for code block delimiter ``` at start of line or position */
        if (preserve_code && (r == 0 || src[r - 1] == '\n') && (r + 2 < src_len) && src[r] == '`' &&
            src[r + 1] == '`' && src[r + 2] == '`') {
            in_code_block = !in_code_block;
            consecutive_newlines = 0;
            in_whitespace_run = false;
            /* Copy the ``` */
            for (int k = 0; k < 3 && r < src_len && w + 1 < dst_cap; k++) {
                dst[w++] = src[r++];
            }
            continue;
        }

        char c = src[r];

        if (in_code_block) {
            /* Inside code block: preserve every character verbatim */
            dst[w++] = c;
            r++;
            continue;
        }

        /* Outside code block: collapse excess whitespace and newlines */
        if (c == '\r') {
            /* Ignore CR for canonical LF */
            r++;
            continue;
        }

        if (c == '\n') {
            consecutive_newlines++;
            in_whitespace_run = false;
            if (consecutive_newlines <= 2) {
                dst[w++] = '\n';
            }
            r++;
            continue;
        }

        consecutive_newlines = 0;

        if (c == ' ' || c == '\t') {
            if (!in_whitespace_run) {
                dst[w++] = ' ';
                in_whitespace_run = true;
            }
            r++;
            continue;
        }

        in_whitespace_run = false;
        dst[w++] = c;
        r++;
    }

    dst[w] = '\0';
    return w;
}

bool
prompt_compressor_process_payload(const char*              payload,
                                  size_t                   payload_len,
                                  const compressor_rule_t* rule,
                                  compressor_result_t*     out_result)
{
    if (!payload || payload_len == 0 || !rule || !out_result) {
        return false;
    }

    memset(out_result, 0, sizeof(*out_result));

    if (!rule->enabled || rule->level == COMPRESS_LEVEL_OFF) {
        return true;
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    uint32_t orig_tokens = compressor_estimate_tokens(payload, payload_len);
    out_result->original_tokens = orig_tokens;

    if (orig_tokens < rule->min_tokens) {
        return true;
    }

    json_error_t jerr;
    json_t*      root = json_loads(payload, 0, &jerr);
    if (!root || !json_is_object(root)) {
        if (root) {
            json_decref(root);
        }
        return false;
    }

    json_t* messages = json_object_get(root, "messages");
    if (!messages || !json_is_array(messages)) {
        json_decref(root);
        return true;
    }

    size_t num_msgs = json_array_size(messages);
    if (num_msgs == 0) {
        json_decref(root);
        return true;
    }

    /* Locate the index of the final user message */
    ssize_t last_user_idx = -1;
    for (ssize_t i = (ssize_t)num_msgs - 1; i >= 0; i--) {
        json_t*     msg = json_array_get(messages, (size_t)i);
        const char* role = json_string_value(json_object_get(msg, "role"));
        if (role && strcmp(role, "user") == 0) {
            last_user_idx = i;
            break;
        }
    }

    bool* keep = (bool*)calloc(num_msgs, sizeof(bool));
    if (!keep) {
        json_decref(root);
        return false;
    }

    /* 1. Final user question is strictly preserved 100% */
    if (last_user_idx >= 0) {
        keep[last_user_idx] = true;
    }

    /* 2. System and developer prompts at the beginning are preserved */
    for (size_t i = 0; i < num_msgs; i++) {
        json_t*     msg = json_array_get(messages, i);
        const char* role = json_string_value(json_object_get(msg, "role"));
        if (role && (strcmp(role, "system") == 0 || strcmp(role, "developer") == 0)) {
            if (rule->preserve_system) {
                keep[i] = true;
            }
        } else {
            break;
        }
    }

    /* 3. Multi-turn history windowing: scan backward before the last user question */
    uint32_t turns_kept = 0;
    ssize_t  scan_start = (last_user_idx >= 0) ? (last_user_idx - 1) : ((ssize_t)num_msgs - 1);
    for (ssize_t i = scan_start; i >= 0; i--) {
        if (keep[i]) {
            continue; /* already preserved (e.g. system prompt) */
        }
        json_t*     msg = json_array_get(messages, (size_t)i);
        const char* role = json_string_value(json_object_get(msg, "role"));
        if (role && (strcmp(role, "assistant") == 0 || strcmp(role, "user") == 0 ||
                     strcmp(role, "tool") == 0)) {
            if (turns_kept < rule->max_history_turns) {
                keep[i] = true;
                if (strcmp(role, "user") == 0) {
                    turns_kept++;
                }
            }
        }
    }

    /* 4. Tool calls preservation */
    if (rule->preserve_tools) {
        for (size_t i = 0; i < num_msgs; i++) {
            if (keep[i]) {
                json_t* msg = json_array_get(messages, i);
                if (json_object_get(msg, "tool_calls") != NULL) {
                    /* If an assistant with tool_calls is kept, keep the next tool response */
                    if (i + 1 < num_msgs) {
                        json_t*     next_msg = json_array_get(messages, i + 1);
                        const char* next_role =
                            json_string_value(json_object_get(next_msg, "role"));
                        if (next_role && strcmp(next_role, "tool") == 0) {
                            keep[i + 1] = true;
                        }
                    }
                }
            }
        }
    }

    /* 5. Sanitize content on kept messages */
    for (size_t i = 0; i < num_msgs; i++) {
        if (!keep[i]) {
            continue;
        }
        json_t* msg = json_array_get(messages, i);
        json_t* content_val = json_object_get(msg, "content");
        if (content_val && json_is_string(content_val)) {
            const char* text = json_string_value(content_val);
            size_t      text_len = strlen(text);
            char*       sanitized = (char*)malloc(text_len + 1);
            if (sanitized) {
                size_t s_len = compressor_sanitize_whitespace(
                    text, text_len, sanitized, text_len + 1, rule->preserve_code);
                json_object_set_new(msg, "content", json_stringn(sanitized, s_len));
                free(sanitized);
            }
        }
    }

    /* 6. Construct new messages array */
    json_t* new_messages = json_array();
    for (size_t i = 0; i < num_msgs; i++) {
        if (keep[i]) {
            json_array_append(new_messages, json_array_get(messages, i));
        }
    }
    free(keep);

    json_object_set(root, "messages", new_messages);
    json_decref(new_messages);

    /* 7. Dump new payload and calculate metrics */
    char* dumped = json_dumps(root, JSON_COMPACT);
    json_decref(root);

    if (dumped) {
        uint32_t comp_tokens = compressor_estimate_tokens(dumped, strlen(dumped));
        if (comp_tokens < orig_tokens) {
            out_result->compressed = true;
            out_result->original_tokens = orig_tokens;
            out_result->compressed_tokens = comp_tokens;
            out_result->saved_tokens = orig_tokens - comp_tokens;
            out_result->compression_ratio = (double)comp_tokens / (double)orig_tokens;
            out_result->compressed_payload = dumped;
            out_result->compressed_len = strlen(dumped);
        } else {
            free(dumped);
            out_result->compressed = false;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    uint64_t elapsed_us = (uint64_t)(ts_end.tv_sec - ts_start.tv_sec) * 1000000ULL +
                          (uint64_t)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000ULL;
    out_result->elapsed_us = elapsed_us;

    return true;
}

void
prompt_compressor_result_cleanup(compressor_result_t* res)
{
    if (res && res->compressed_payload) {
        free(res->compressed_payload);
        res->compressed_payload = NULL;
    }
}

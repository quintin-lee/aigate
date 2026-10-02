/**
 * @file prompt_compressor.c
 * @brief Implementation of prompt compression, token estimation, whitespace sanitization,
 *        sentence density pruning, and circular snapshot cache.
 */

#include "policy/prompt_compressor.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief Internal representation of prompt compression circular cache.
 */
struct compressor_cache {
    compressor_snapshot_t* items;    /**< Dynamic array of snapshots */
    size_t                 capacity; /**< Maximum capacity */
    size_t                 head;     /**< Next write index in circular ring */
    size_t                 count;    /**< Current stored snapshot count */
    compressor_stats_t     stats;    /**< Aggregated cumulative statistics */
    pthread_mutex_t        lock;     /**< Mutex protecting cache accesses */
};

/** @brief Maximum number of sentences processed in sentence density pruner */
#define MAX_SENTENCES 128

static bool
str_has_substr(const char* haystack, size_t hlen, const char* needle)
{
    if (!haystack || !needle) {
        return false;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) {
        return false;
    }
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncmp(haystack + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

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

size_t
compressor_prune_sentence_density(
    const char* src, size_t src_len, double target_ratio, char* dst, size_t dst_cap)
{
    if (!src || src_len == 0 || !dst || dst_cap == 0) {
        return 0;
    }
    if (target_ratio <= 0.0 || target_ratio >= 1.0) {
        size_t copy_len = (src_len < dst_cap - 1) ? src_len : dst_cap - 1;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
        return copy_len;
    }

    typedef struct {
        size_t start;
        size_t len;
        bool   is_protected;
        bool   keep;
        double score;
    } sentence_t;

    sentence_t sents[MAX_SENTENCES];
    size_t     count = 0;

    size_t cur_start = 0;
    while (cur_start < src_len && count < MAX_SENTENCES) {
        while (cur_start < src_len && isspace((unsigned char)src[cur_start])) {
            cur_start++;
        }
        if (cur_start >= src_len) {
            break;
        }

        size_t cur_end = cur_start;
        while (cur_end < src_len) {
            char c = src[cur_end];
            if (c == '.' || c == '!' || c == '?' || c == '\n') {
                cur_end++;
                break;
            }
            /* CJK fullwidth punctuation checking */
            if ((unsigned char)c >= 0x80 && cur_end + 2 < src_len) {
                if ((unsigned char)c == 0xE3 && (unsigned char)src[cur_end + 1] == 0x80 &&
                    (unsigned char)src[cur_end + 2] == 0x82) { /* '。' */
                    cur_end += 3;
                    break;
                }
                if ((unsigned char)c == 0xEF && (unsigned char)src[cur_end + 1] == 0xBC &&
                    ((unsigned char)src[cur_end + 2] == 0x81 ||
                     (unsigned char)src[cur_end + 2] == 0x9F)) { /* '！' or '？' */
                    cur_end += 3;
                    break;
                }
            }
            cur_end++;
        }

        sents[count].start = cur_start;
        sents[count].len = cur_end - cur_start;
        sents[count].is_protected = false;
        sents[count].keep = false;
        sents[count].score = 1.0;

        const char* s_ptr = src + cur_start;
        size_t      s_len = sents[count].len;

        /* PII placeholder check */
        if (str_has_substr(s_ptr, s_len, "{{PII_")) {
            sents[count].is_protected = true;
        }

        /* Essential instructions keywords */
        if (str_has_substr(s_ptr, s_len, "must") || str_has_substr(s_ptr, s_len, "json") ||
            str_has_substr(s_ptr, s_len, "schema") || str_has_substr(s_ptr, s_len, "format") ||
            str_has_substr(s_ptr, s_len, "必须") || str_has_substr(s_ptr, s_len, "禁止")) {
            sents[count].is_protected = true;
        }

        /* Check common polite filler phrases */
        if (str_has_substr(s_ptr, s_len, "As an AI") || str_has_substr(s_ptr, s_len, "as an ai") ||
            str_has_substr(s_ptr, s_len, "pleased to assist") ||
            str_has_substr(s_ptr, s_len, "feel free to ask") ||
            str_has_substr(s_ptr, s_len, "很高兴为您") || str_has_substr(s_ptr, s_len, "作为AI")) {
            sents[count].score = 0.1;
        }

        count++;
        cur_start = cur_end;
    }

    if (count == 0) {
        dst[0] = '\0';
        return 0;
    }

    /* Protect first and last sentences if count > 2 and they are not fillers */
    if (count > 2) {
        if (sents[0].score > 0.5) {
            sents[0].is_protected = true;
        }
        if (sents[count - 1].score > 0.5) {
            sents[count - 1].is_protected = true;
        }
    }

    size_t target_len = (size_t)((double)src_len * target_ratio);
    if (target_len == 0) {
        target_len = 1;
    }

    size_t acc_len = 0;
    for (size_t i = 0; i < count; i++) {
        if (sents[i].is_protected) {
            sents[i].keep = true;
            acc_len += sents[i].len;
        }
    }

    /* Greedy selection of remaining sentences by score */
    while (acc_len < target_len) {
        ssize_t best_idx = -1;
        double  best_score = -1.0;
        for (size_t i = 0; i < count; i++) {
            if (!sents[i].keep && sents[i].score > best_score) {
                best_score = sents[i].score;
                best_idx = (ssize_t)i;
            }
        }
        if (best_idx < 0) {
            break;
        }
        sents[best_idx].keep = true;
        acc_len += sents[best_idx].len;
    }

    /* Assemble kept sentences */
    size_t w = 0;
    for (size_t i = 0; i < count; i++) {
        if (sents[i].keep) {
            if (w > 0 && w + 1 < dst_cap && dst[w - 1] != '\n' && dst[w - 1] != ' ') {
                dst[w++] = ' ';
            }
            size_t copy_bytes = (sents[i].len < dst_cap - 1 - w) ? sents[i].len : dst_cap - 1 - w;
            memcpy(dst + w, src + sents[i].start, copy_bytes);
            w += copy_bytes;
        }
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

    /* 5. Sanitize content and apply sentence density pruning in aggressive mode */
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
                sanitized[s_len] = '\0';

                const char* role = json_string_value(json_object_get(msg, "role"));
                if (rule->level == COMPRESS_LEVEL_AGGRESSIVE && role &&
                    strcmp(role, "assistant") == 0 && s_len > 120) {
                    char* pruned = (char*)malloc(s_len + 1);
                    if (pruned) {
                        size_t p_len = compressor_prune_sentence_density(
                            sanitized, s_len, rule->target_ratio, pruned, s_len + 1);
                        json_object_set_new(msg, "content", json_stringn(pruned, p_len));
                        free(pruned);
                    } else {
                        json_object_set_new(msg, "content", json_stringn(sanitized, s_len));
                    }
                } else {
                    json_object_set_new(msg, "content", json_stringn(sanitized, s_len));
                }
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

compressor_cache_t*
compressor_cache_create(size_t capacity)
{
    if (capacity == 0) {
        capacity = 200;
    }
    compressor_cache_t* cache = (compressor_cache_t*)calloc(1, sizeof(compressor_cache_t));
    if (!cache) {
        return NULL;
    }
    cache->capacity = capacity;
    cache->items = (compressor_snapshot_t*)calloc(capacity, sizeof(compressor_snapshot_t));
    if (!cache->items) {
        free(cache);
        return NULL;
    }
    pthread_mutex_init(&cache->lock, NULL);
    return cache;
}

void
compressor_cache_destroy(compressor_cache_t* cache)
{
    if (!cache) {
        return;
    }
    pthread_mutex_destroy(&cache->lock);
    free(cache->items);
    free(cache);
}

void
compressor_cache_record(compressor_cache_t* cache, const compressor_snapshot_t* snapshot)
{
    if (!cache || !snapshot) {
        return;
    }
    pthread_mutex_lock(&cache->lock);
    cache->items[cache->head] = *snapshot;
    cache->head = (cache->head + 1) % cache->capacity;
    if (cache->count < cache->capacity) {
        cache->count++;
    }
    cache->stats.total_evaluated++;
    if (snapshot->saved_tokens > 0) {
        cache->stats.total_compressed++;
    }
    cache->stats.total_orig_tokens += snapshot->original_tokens;
    cache->stats.total_comp_tokens += snapshot->compressed_tokens;
    cache->stats.total_saved_tokens += snapshot->saved_tokens;
    cache->stats.total_duration_us += snapshot->elapsed_us;
    cache->stats.estimated_cost_saved += (double)snapshot->saved_tokens * 0.000005;
    pthread_mutex_unlock(&cache->lock);
}

void
compressor_cache_get_stats(compressor_cache_t* cache, compressor_stats_t* out_stats)
{
    if (!cache || !out_stats) {
        return;
    }
    pthread_mutex_lock(&cache->lock);
    *out_stats = cache->stats;
    pthread_mutex_unlock(&cache->lock);
}

size_t
compressor_cache_get_snapshots(compressor_cache_t*    cache,
                               compressor_snapshot_t* out_snapshots,
                               size_t                 max_count)
{
    if (!cache || !out_snapshots || max_count == 0) {
        return 0;
    }
    pthread_mutex_lock(&cache->lock);
    size_t to_copy = (cache->count < max_count) ? cache->count : max_count;
    for (size_t i = 0; i < to_copy; i++) {
        size_t idx = (cache->head + cache->capacity - 1 - i) % cache->capacity;
        out_snapshots[i] = cache->items[idx];
    }
    pthread_mutex_unlock(&cache->lock);
    return to_copy;
}

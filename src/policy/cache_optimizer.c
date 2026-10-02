/**
 * @file cache_optimizer.c
 * @brief Implementation of prompt cache prefix alignment and optimization engine.
 */

#include "policy/cache_optimizer.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Helper structure used for sorting tool and function definitions.
 */
typedef struct {
    json_t*     elem; /**< Underlying JSON object pointer */
    const char* name; /**< Extracted tool or function name */
} tool_entry_t;

/**
 * @brief Comparator for sorting tool entries alphabetically by name.
 */
static int
tool_entry_cmp(const void* a, const void* b)
{
    const tool_entry_t* ea = (const tool_entry_t*)a;
    const tool_entry_t* eb = (const tool_entry_t*)b;
    return strcmp(ea->name, eb->name);
}

bool
cache_optimizer_sort_tools(json_t* root)
{
    if (!root || !json_is_object(root)) {
        return false;
    }

    json_t* tools = json_object_get(root, "tools");
    if (!tools || !json_is_array(tools)) {
        tools = json_object_get(root, "functions");
        if (!tools || !json_is_array(tools)) {
            return false;
        }
    }

    size_t count = json_array_size(tools);
    if (count <= 1) {
        return false;
    }

    tool_entry_t* entries = (tool_entry_t*)malloc(sizeof(tool_entry_t) * count);
    if (!entries) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        json_t*     item = json_array_get(tools, i);
        const char* name = NULL;

        if (json_is_object(item)) {
            json_t* fn = json_object_get(item, "function");
            if (json_is_object(fn)) {
                json_t* fn_name = json_object_get(fn, "name");
                if (json_is_string(fn_name)) {
                    name = json_string_value(fn_name);
                }
            }
            if (!name) {
                json_t* item_name = json_object_get(item, "name");
                if (json_is_string(item_name)) {
                    name = json_string_value(item_name);
                }
            }
        }
        entries[i].elem = item;
        entries[i].name = name ? name : "";
    }

    /* Check if already sorted */
    bool already_sorted = true;
    for (size_t i = 1; i < count; i++) {
        if (strcmp(entries[i - 1].name, entries[i].name) > 0) {
            already_sorted = false;
            break;
        }
    }

    if (already_sorted) {
        free(entries);
        return false;
    }

    /* Sort entries stably */
    qsort(entries, count, sizeof(tool_entry_t), tool_entry_cmp);

    /* Rebuild array in sorted order */
    for (size_t i = 0; i < count; i++) {
        json_incref(entries[i].elem);
    }
    json_array_clear(tools);
    for (size_t i = 0; i < count; i++) {
        json_array_append_new(tools, entries[i].elem);
    }

    free(entries);
    return true;
}

size_t
cache_optimizer_normalize_whitespace(const char* in, size_t in_len, char* out, size_t out_sz)
{
    if (!in || in_len == 0 || !out || out_sz == 0) {
        if (out && out_sz > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    size_t r = 0;
    size_t w = 0;

    /* Skip leading whitespace */
    while (r < in_len && (in[r] == ' ' || in[r] == '\t' || in[r] == '\r')) {
        r++;
    }

    int consecutive_newlines = 0;
    int consecutive_spaces = 0;

    while (r < in_len && w + 2 < out_sz) {
        char c = in[r++];

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            /* Trim trailing whitespace on current line before newline */
            while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
                w--;
            }
            consecutive_spaces = 0;
            if (consecutive_newlines < 2) {
                out[w++] = '\n';
                consecutive_newlines++;
            }
            continue;
        }

        if (c == ' ' || c == '\t') {
            if (consecutive_newlines > 0) {
                /* Skip spaces right after newlines */
                continue;
            }
            if (consecutive_spaces == 0) {
                out[w++] = ' ';
                consecutive_spaces++;
            }
            continue;
        }

        /* Regular non-whitespace character */
        consecutive_newlines = 0;
        consecutive_spaces = 0;
        out[w++] = c;
    }

    /* Trim trailing spaces before newlines or end */
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
        w--;
    }

    out[w] = '\0';
    return w;
}

/**
 * @brief Checks if a string prefix matches common volatile date/time or session patterns.
 */
static bool
is_dynamic_header_prefix(const char* p, size_t len)
{
    if (len < 8) {
        return false;
    }

    if (strncasecmp(p, "today is", 8) == 0) {
        return true;
    }
    if (strncasecmp(p, "current date", 12) == 0 || strncasecmp(p, "current time", 12) == 0) {
        return true;
    }
    if (strncasecmp(p, "session id:", 11) == 0 || strncasecmp(p, "session_id:", 11) == 0 ||
        strncasecmp(p, "session:", 8) == 0) {
        return true;
    }
    if (strncasecmp(p, "request id:", 11) == 0 || strncasecmp(p, "request_id:", 11) == 0) {
        return true;
    }

    /* Check for leading ISO date: YYYY-MM-DD or YYYY/MM/DD */
    if (len >= 10 && isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
        isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
        (p[4] == '-' || p[4] == '/') && isdigit((unsigned char)p[5]) &&
        isdigit((unsigned char)p[6]) && (p[7] == '-' || p[7] == '/') &&
        isdigit((unsigned char)p[8]) && isdigit((unsigned char)p[9])) {
        return true;
    }

    return false;
}

bool
cache_optimizer_sink_dynamic_system(const char* in, size_t in_len, char* out, size_t out_sz)
{
    if (!in || in_len == 0 || !out || out_sz == 0) {
        return false;
    }

    /* Skip leading whitespace */
    size_t start = 0;
    while (start < in_len && isspace((unsigned char)in[start])) {
        start++;
    }

    if (!is_dynamic_header_prefix(in + start, in_len - start)) {
        return false;
    }

    /* Find end of the first dynamic sentence or line */
    size_t end = start;
    while (end < in_len) {
        if (in[end] == '\n') {
            break;
        }
        if (in[end] == '.' && (end + 1 >= in_len || isspace((unsigned char)in[end + 1]))) {
            end++; /* include the period */
            break;
        }
        end++;
    }

    /* Extract dynamic slice, trimming trailing period and whitespace */
    size_t dyn_start = start;
    size_t dyn_end = end;
    while (dyn_end > dyn_start &&
           (in[dyn_end - 1] == '.' || isspace((unsigned char)in[dyn_end - 1]))) {
        dyn_end--;
    }
    size_t dyn_len = dyn_end - dyn_start;
    if (dyn_len == 0) {
        return false;
    }

    /* Find start of remaining static content */
    size_t static_start = end;
    while (static_start < in_len && isspace((unsigned char)in[static_start])) {
        static_start++;
    }

    if (static_start >= in_len) {
        /* No static content following the dynamic header */
        return false;
    }

    size_t static_len = in_len - static_start;

    /* Format into out: static content first, then [Runtime Context: <dyn>]\n */
    int written = snprintf(out,
                           out_sz,
                           "%.*s\n\n[Runtime Context: %.*s]\n",
                           (int)static_len,
                           in + static_start,
                           (int)dyn_len,
                           in + dyn_start);

    return (written > 0 && (size_t)written < out_sz);
}

/**
 * @brief Helper to create an ephemeral cache_control JSON object.
 */
static json_t*
create_ephemeral_cache_control(void)
{
    json_t* obj = json_object();
    if (obj) {
        json_object_set_new(obj, "type", json_string("ephemeral"));
    }
    return obj;
}

/**
 * @brief Helper to estimate token count from a character length.
 */
static uint32_t
estimate_tokens_len(size_t char_len)
{
    /* ~4 characters per token average */
    return (uint32_t)((char_len + 3) / 4);
}

int
cache_optimizer_inject_anthropic_breakpoints(json_t* root, uint32_t min_tokens)
{
    if (!root || !json_is_object(root)) {
        return 0;
    }

    int breakpoints = 0;

    /* 1. Check tools array */
    json_t* tools = json_object_get(root, "tools");
    if (tools && json_is_array(tools)) {
        size_t count = json_array_size(tools);
        if (count > 0 && breakpoints < ANTHROPIC_MAX_BREAKPOINTS) {
            char* tools_dump = json_dumps(tools, JSON_COMPACT);
            if (tools_dump) {
                uint32_t est = estimate_tokens_len(strlen(tools_dump));
                free(tools_dump);
                if (est >= min_tokens) {
                    json_t* last_tool = json_array_get(tools, count - 1);
                    if (json_is_object(last_tool)) {
                        json_object_set_new(
                            last_tool, "cache_control", create_ephemeral_cache_control());
                        breakpoints++;
                    }
                }
            }
        }
    }

    /* 2. Check top-level "system" object or string */
    json_t* sys_item = json_object_get(root, "system");
    if (sys_item && breakpoints < ANTHROPIC_MAX_BREAKPOINTS) {
        if (json_is_string(sys_item)) {
            const char* sys_str = json_string_value(sys_item);
            if (sys_str && estimate_tokens_len(strlen(sys_str)) >= min_tokens) {
                json_t* arr = json_array();
                json_t* blk = json_object();
                json_object_set_new(blk, "type", json_string("text"));
                json_object_set_new(blk, "text", json_string(sys_str));
                json_object_set_new(blk, "cache_control", create_ephemeral_cache_control());
                json_array_append_new(arr, blk);
                json_object_set_new(root, "system", arr);
                breakpoints++;
            }
        } else if (json_is_array(sys_item)) {
            size_t sys_count = json_array_size(sys_item);
            if (sys_count > 0) {
                json_t* last_blk = json_array_get(sys_item, sys_count - 1);
                if (json_is_object(last_blk)) {
                    json_object_set_new(
                        last_blk, "cache_control", create_ephemeral_cache_control());
                    breakpoints++;
                }
            }
        }
    }

    /* 3. Check messages array */
    json_t* messages = json_object_get(root, "messages");
    if (messages && json_is_array(messages)) {
        size_t n_msgs = json_array_size(messages);

        /* 3a. System message inside messages array */
        for (size_t i = 0; i < n_msgs && breakpoints < ANTHROPIC_MAX_BREAKPOINTS; i++) {
            json_t*     msg = json_array_get(messages, i);
            const char* role = json_string_value(json_object_get(msg, "role"));
            if (role && strcmp(role, "system") == 0) {
                json_t* content = json_object_get(msg, "content");
                if (json_is_string(content)) {
                    const char* text = json_string_value(content);
                    if (text && estimate_tokens_len(strlen(text)) >= min_tokens) {
                        json_t* arr = json_array();
                        json_t* blk = json_object();
                        json_object_set_new(blk, "type", json_string("text"));
                        json_object_set_new(blk, "text", json_string(text));
                        json_object_set_new(blk, "cache_control", create_ephemeral_cache_control());
                        json_array_append_new(arr, blk);
                        json_object_set_new(msg, "content", arr);
                        breakpoints++;
                        break;
                    }
                } else if (json_is_array(content)) {
                    size_t c_count = json_array_size(content);
                    if (c_count > 0) {
                        json_t* last_blk = json_array_get(content, c_count - 1);
                        if (json_is_object(last_blk)) {
                            json_object_set_new(
                                last_blk, "cache_control", create_ephemeral_cache_control());
                            breakpoints++;
                            break;
                        }
                    }
                }
            }
        }

        /* 3b. Turn N-2 assistant turn for multi-turn chat caching */
        if (n_msgs >= 3 && breakpoints < ANTHROPIC_MAX_BREAKPOINTS) {
            for (size_t i = n_msgs - 2; i > 0 && breakpoints < ANTHROPIC_MAX_BREAKPOINTS; i--) {
                json_t*     msg = json_array_get(messages, i);
                const char* role = json_string_value(json_object_get(msg, "role"));
                if (role && strcmp(role, "assistant") == 0) {
                    json_t* content = json_object_get(msg, "content");
                    if (json_is_string(content)) {
                        const char* text = json_string_value(content);
                        if (text && estimate_tokens_len(strlen(text)) >= min_tokens) {
                            json_t* arr = json_array();
                            json_t* blk = json_object();
                            json_object_set_new(blk, "type", json_string("text"));
                            json_object_set_new(blk, "text", json_string(text));
                            json_object_set_new(
                                blk, "cache_control", create_ephemeral_cache_control());
                            json_array_append_new(arr, blk);
                            json_object_set_new(msg, "content", arr);
                            breakpoints++;
                            break;
                        }
                    } else if (json_is_array(content)) {
                        size_t c_count = json_array_size(content);
                        if (c_count > 0) {
                            json_t* last_blk = json_array_get(content, c_count - 1);
                            if (json_is_object(last_blk)) {
                                json_object_set_new(
                                    last_blk, "cache_control", create_ephemeral_cache_control());
                                breakpoints++;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    return breakpoints;
}

bool
cache_optimizer_rule_matches(const cache_optimizer_rule_t* rule, const char* model)
{
    if (!rule || !model) {
        return false;
    }
    if (!rule->enabled) {
        return false;
    }
    if (strcmp(rule->model_pattern, "*") == 0) {
        return true;
    }

    size_t pat_len = strlen(rule->model_pattern);
    if (pat_len > 0 && rule->model_pattern[pat_len - 1] == '*') {
        return strncmp(model, rule->model_pattern, pat_len - 1) == 0;
    }

    return strcmp(rule->model_pattern, model) == 0;
}

/**
 * @brief Internal circular evaluation cache structure.
 */
struct cache_optimizer_cache {
    cache_optimizer_snapshot_t* items;    /**< Dynamic array of request snapshots */
    size_t                      capacity; /**< Maximum capacity of ring buffer */
    size_t                      head;     /**< Next write index */
    size_t                      count;    /**< Current stored snapshot count */
    cache_optimizer_stats_t     stats;    /**< Cumulative aggregated statistics */
    pthread_mutex_t             lock;     /**< Mutex protecting cache access */
};


cache_optimizer_cache_t*
cache_optimizer_cache_create(size_t capacity)
{
    if (capacity == 0) {
        capacity = CACHE_OPTIMIZER_MAX_SNAPSHOTS;
    }
    cache_optimizer_cache_t* cache =
        (cache_optimizer_cache_t*)calloc(1, sizeof(cache_optimizer_cache_t));
    if (!cache) {
        return NULL;
    }
    cache->capacity = capacity;
    cache->items =
        (cache_optimizer_snapshot_t*)calloc(capacity, sizeof(cache_optimizer_snapshot_t));
    if (!cache->items) {
        free(cache);
        return NULL;
    }
    pthread_mutex_init(&cache->lock, NULL);
    return cache;
}

void
cache_optimizer_cache_destroy(cache_optimizer_cache_t* cache)
{
    if (!cache) {
        return;
    }
    pthread_mutex_destroy(&cache->lock);
    free(cache->items);
    free(cache);
}

void
cache_optimizer_cache_record(cache_optimizer_cache_t* cache, const cache_optimizer_snapshot_t* snap)
{
    if (!cache || !snap) {
        return;
    }
    pthread_mutex_lock(&cache->lock);

    cache->items[cache->head] = *snap;
    cache->head = (cache->head + 1) % cache->capacity;
    if (cache->count < cache->capacity) {
        cache->count++;
    }

    cache->stats.total_optimized_requests++;
    if (snap->upstream_cache_hit) {
        cache->stats.upstream_cache_hit_requests++;
    }
    cache->stats.total_prompt_tokens += snap->prompt_tokens;
    cache->stats.total_cached_tokens += snap->cached_tokens;
    cache->stats.total_savings_usd += snap->cost_savings_usd;

    if (cache->stats.total_optimized_requests > 0) {
        cache->stats.avg_latency_us =
            (cache->stats.avg_latency_us * (cache->stats.total_optimized_requests - 1) +
             snap->latency_us) /
            cache->stats.total_optimized_requests;
    }

    pthread_mutex_unlock(&cache->lock);
}

void
cache_optimizer_cache_get_stats(cache_optimizer_cache_t* cache, cache_optimizer_stats_t* out_stats)
{
    if (!cache || !out_stats) {
        return;
    }
    pthread_mutex_lock(&cache->lock);
    *out_stats = cache->stats;
    pthread_mutex_unlock(&cache->lock);
}

size_t
cache_optimizer_cache_get_snapshots(cache_optimizer_cache_t*    cache,
                                    cache_optimizer_snapshot_t* out_snaps,
                                    size_t                      max_snaps)
{
    if (!cache || !out_snaps || max_snaps == 0) {
        return 0;
    }
    pthread_mutex_lock(&cache->lock);

    size_t copied = 0;
    for (size_t i = 0; i < cache->count && copied < max_snaps; i++) {
        size_t idx = (cache->head + cache->capacity - 1 - i) % cache->capacity;
        out_snaps[copied++] = cache->items[idx];
    }

    pthread_mutex_unlock(&cache->lock);
    return copied;
}

void
cache_optimizer_result_cleanup(cache_optimizer_result_t* res)
{
    if (res && res->optimized_payload) {
        free(res->optimized_payload);
        res->optimized_payload = NULL;
    }
}

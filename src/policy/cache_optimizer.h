/**
 * @file cache_optimizer.h
 * @brief Prompt cache prefix alignment and optimization engine.
 *
 * Implements deterministic tools dictionary sorting, volatile timestamp/UUID
 * sinking, and Anthropic ephemeral breakpoint injection to maximize upstream
 * KV/Prompt Cache hits across OpenAI, Anthropic, and vLLM/SGLang.
 */

#ifndef AIGATE_CACHE_OPTIMIZER_H
#define AIGATE_CACHE_OPTIMIZER_H

#include <jansson.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of snapshots preserved in the circular evaluation cache. */
#define CACHE_OPTIMIZER_MAX_SNAPSHOTS 200

/** Minimum estimated tokens required for Anthropic prompt caching eligibility. */
#define ANTHROPIC_CACHE_MIN_TOKENS 1024

/** Maximum number of ephemeral cache breakpoints allowed by Anthropic Claude. */
#define ANTHROPIC_MAX_BREAKPOINTS 4

/**
 * @brief Cache optimizer rule configuration.
 */
typedef struct {
    char     id[37];                       /**< Unique rule UUID (36 chars + \0) */
    char     model_pattern[64];            /**< Model match pattern (e.g. "claude-*" or "*") */
    bool     enabled;                      /**< Whether this rule is active */
    bool     sort_tools;                   /**< Enable deterministic dictionary sorting of tools */
    bool     sink_dynamic_system;          /**< Enable sinking of volatile timestamps and UUIDs */
    bool     inject_anthropic_breakpoints; /**< Enable injection of Anthropic cache_control */
    uint32_t min_tokens_threshold;         /**< Minimum estimated tokens required to optimize */
    int64_t  created_at;                   /**< Rule creation timestamp */
    int64_t  updated_at;                   /**< Last update timestamp */
} cache_optimizer_rule_t;

/**
 * @brief Result of a single cache optimization pass.
 */
typedef struct {
    bool     optimized;            /**< Whether the payload was modified */
    bool     tools_sorted;         /**< Whether tools were reordered */
    bool     dynamic_sunk;         /**< Whether volatile dynamic context was sunk */
    int      breakpoints_injected; /**< Number of cache_control breakpoints injected */
    char*    optimized_payload;    /**< Newly allocated optimized JSON string (caller frees) */
    size_t   optimized_len;        /**< Length of optimized payload in bytes */
    uint32_t latency_us;           /**< Optimization execution time in microseconds */
} cache_optimizer_result_t;

/**
 * @brief Request execution snapshot entry for observability and audit.
 */
typedef struct {
    char     req_id[37];         /**< Unique request ID */
    char     model[64];          /**< Model name */
    int64_t  timestamp;          /**< Request timestamp */
    bool     upstream_cache_hit; /**< Whether upstream reported cached prompt tokens */
    uint32_t prompt_tokens;      /**< Total prompt tokens */
    uint32_t cached_tokens;      /**< Cached prompt tokens read */
    double   cost_savings_usd;   /**< Estimated cost savings in USD */
    uint32_t latency_us;         /**< Optimizer execution latency in us */
    int      breakpoints_count;  /**< Breakpoints injected count */
    bool     dynamic_sunk;       /**< Whether dynamic content was sunk */
    bool     tools_sorted;       /**< Whether tools array was sorted */
} cache_optimizer_snapshot_t;

/**
 * @brief Aggregated cache optimizer efficiency metrics.
 */
typedef struct {
    uint64_t total_optimized_requests;    /**< Total requests processed by optimizer */
    uint64_t upstream_cache_hit_requests; /**< Total requests that hit upstream cache */
    uint64_t total_prompt_tokens;         /**< Cumulative prompt tokens */
    uint64_t total_cached_tokens;         /**< Cumulative cached tokens */
    double   total_savings_usd;           /**< Cumulative estimated dollar savings */
    uint64_t avg_latency_us;              /**< Average execution latency in us */
} cache_optimizer_stats_t;

/**
 * @brief Opaque type for circular snapshot cache.
 */
typedef struct cache_optimizer_cache cache_optimizer_cache_t;

/**
 * @brief Sorts the tools or functions array in a chat completion JSON payload by name in ASCII order.
 *
 * @param root Parsed jansson root object representing the request body.
 * @return true if tools were present and their order was modified, false otherwise.
 */
bool cache_optimizer_sort_tools(json_t* root);

/**
 * @brief Normalizes redundant whitespace and blank lines to improve token prefix consistency.
 *
 * @param in Input raw string.
 * @param in_len Length of input string.
 * @param out Output buffer to write normalized string.
 * @param out_sz Capacity of output buffer.
 * @return Length of output string written.
 */
size_t
cache_optimizer_normalize_whitespace(const char* in, size_t in_len, char* out, size_t out_sz);

/**
 * @brief Detects volatile timestamps, dates, or UUID/Session IDs in the header of system prompt,
 *        and relocates them to the end in a runtime context block, preserving exact prefix alignment.
 *
 * @param in Input system prompt string.
 * @param in_len Length of input string.
 * @param out Output buffer to write transformed system prompt.
 * @param out_sz Capacity of output buffer.
 * @return true if volatile dynamic context was detected and relocated, false if unchanged.
 */
bool cache_optimizer_sink_dynamic_system(const char* in, size_t in_len, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_CACHE_OPTIMIZER_H */

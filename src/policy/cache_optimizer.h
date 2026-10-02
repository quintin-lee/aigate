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

/**
 * @brief Injects ephemeral cache_control breakpoints into Claude/Anthropic payloads.
 *
 * Places up to 4 cache breakpoints on qualifying elements (Tools, System, conversation turns)
 * exceeding the min_tokens threshold.
 *
 * @param root Parsed request JSON object.
 * @param min_tokens Minimum token length threshold for eligibility (default 1024).
 * @return Number of breakpoints successfully injected (0 to 4).
 */
int cache_optimizer_inject_anthropic_breakpoints(json_t* root, uint32_t min_tokens);

/**
 * @brief Matches a candidate model against a rule wildcard pattern.
 *
 * @param rule Pointer to rule configuration.
 * @param model Model name string to test.
 * @return true if model matches rule pattern, false otherwise.
 */
bool cache_optimizer_rule_matches(const cache_optimizer_rule_t* rule, const char* model);

/**
 * @brief Creates a thread-safe circular evaluation cache for request snapshots.
 *
 * @param capacity Maximum number of entries (default 200).
 * @return Pointer to newly allocated cache, or NULL on failure.
 */
cache_optimizer_cache_t* cache_optimizer_cache_create(size_t capacity);

/**
 * @brief Frees all memory associated with a circular evaluation cache.
 *
 * @param cache Pointer to cache.
 */
void cache_optimizer_cache_destroy(cache_optimizer_cache_t* cache);

/**
 * @brief Records a request evaluation snapshot into the circular cache.
 *
 * @param cache Pointer to cache.
 * @param snap Snapshot data to record.
 */
void cache_optimizer_cache_record(cache_optimizer_cache_t*          cache,
                                  const cache_optimizer_snapshot_t* snap);

/**
 * @brief Retrieves cumulative efficiency statistics from the cache.
 *
 * @param cache Pointer to cache.
 * @param out_stats Buffer to populate with statistics.
 */
void cache_optimizer_cache_get_stats(cache_optimizer_cache_t* cache,
                                     cache_optimizer_stats_t* out_stats);

/**
 * @brief Copies the most recent snapshots from the circular cache.
 *
 * @param cache Pointer to cache.
 * @param out_snaps Array to populate with snapshots.
 * @param max_snaps Maximum number of snapshots to copy.
 * @return Number of snapshots copied.
 */
size_t cache_optimizer_cache_get_snapshots(cache_optimizer_cache_t*    cache,
                                           cache_optimizer_snapshot_t* out_snaps,
                                           size_t                      max_snaps);

/**
 * @brief Releases any allocated buffers inside an optimization result.
 *
 * @param res Pointer to result to clean up.
 */
void cache_optimizer_result_cleanup(cache_optimizer_result_t* res);

/**
 * @brief Processes and optimizes a chat completion request payload for maximum cache prefix alignment.
 *
 * Performs deterministic tools sorting, volatile header sinking, and Anthropic ephemeral breakpoint injection.
 *
 * @param payload Raw JSON request string.
 * @param payload_len Length of raw JSON string.
 * @param rule Active cache optimizer rule.
 * @param out_result Output result struct populated with metrics and optimized payload.
 * @return true on success (even if no change was necessary), false on parse failure.
 */
bool cache_optimizer_process_payload(const char*                   payload,
                                     size_t                        payload_len,
                                     const cache_optimizer_rule_t* rule,
                                     cache_optimizer_result_t*     out_result);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_CACHE_OPTIMIZER_H */

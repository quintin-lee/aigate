/**
 * @file prompt_compressor.h
 * @brief High-performance prompt compression and token pruning engine.
 */

#ifndef AIGATE_PROMPT_COMPRESSOR_H
#define AIGATE_PROMPT_COMPRESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prompt compression intensity level.
 */
typedef enum {
    COMPRESS_LEVEL_OFF = 0, /**< Compression disabled */
    COMPRESS_LEVEL_MODERATE =
        1, /**< Moderate: whitespace sanitization and multi-turn history windowing */
    COMPRESS_LEVEL_AGGRESSIVE = 2 /**< Aggressive: moderate + sentence importance density pruning */
} compressor_level_t;

/**
 * @brief Prompt compression rule configuration.
 */
typedef struct {
    char id[37];                /**< Unique rule UUID (36 chars + \0) */
    char model_pattern[64];     /**< Target model matching wildcard, e.g. "gpt-4o*", "*" */
    bool enabled;               /**< Whether this rule is enabled */
    compressor_level_t level;   /**< Compression intensity level */
    uint32_t min_tokens;        /**< Trigger threshold: prompt tokens >= min_tokens to compress */
    uint32_t max_history_turns; /**< Maximum multi-turn history turns to retain */
    double   target_ratio;      /**< Target retention ratio (0.10 ~ 0.90, default 0.60) */
    bool     preserve_system;   /**< Strictly preserve system prompt instructions */
    bool     preserve_code;     /**< Strictly preserve code blocks and indentation */
    bool     preserve_tools;    /**< Strictly preserve tool calls and function schema */
    int64_t  created_at;        /**< Creation timestamp (unix seconds) */
    int64_t  updated_at;        /**< Last updated timestamp (unix seconds) */
} compressor_rule_t;

/**
 * @brief Per-request prompt compression execution metrics and payload.
 */
typedef struct {
    bool     compressed;         /**< Whether prompt was actually pruned/compressed */
    uint32_t original_tokens;    /**< Estimated original prompt token count */
    uint32_t compressed_tokens;  /**< Compressed prompt token count */
    uint32_t saved_tokens;       /**< Saved token count (original - compressed) */
    double   compression_ratio;  /**< Actual retention ratio (compressed / original) */
    uint64_t elapsed_us;         /**< Compression execution latency in microseconds */
    char*    compressed_payload; /**< Newly allocated compressed JSON payload (caller frees) */
    size_t   compressed_len;     /**< Length of compressed JSON payload in bytes */
} compressor_result_t;

/**
 * @brief In-memory snapshot item for Web Console Diff modal and auditing.
 */
typedef struct {
    char     req_id[37];          /**< Request / Trace ID */
    char     model[64];           /**< Target model name */
    int64_t  timestamp;           /**< Event timestamp (unix seconds) */
    uint32_t original_tokens;     /**< Original prompt tokens */
    uint32_t compressed_tokens;   /**< Compressed prompt tokens */
    uint32_t saved_tokens;        /**< Saved prompt tokens */
    double   compression_ratio;   /**< Retention ratio */
    uint32_t elapsed_us;          /**< Pruning execution time in microseconds */
    char     prompt_preview[128]; /**< Short preview of the prompt */
    char     orig_preview[512];   /**< Snapshot of original messages snippet */
    char     comp_preview[512];   /**< Snapshot of compressed messages snippet */
} compressor_snapshot_t;

/**
 * @brief Global aggregated compression benefit statistics.
 */
typedef struct {
    uint64_t total_evaluated;      /**< Total evaluated requests */
    uint64_t total_compressed;     /**< Total requests that underwent compression */
    uint64_t total_orig_tokens;    /**< Cumulative original tokens */
    uint64_t total_comp_tokens;    /**< Cumulative compressed tokens */
    uint64_t total_saved_tokens;   /**< Cumulative saved tokens */
    uint64_t total_duration_us;    /**< Cumulative execution time in microseconds */
    double   estimated_cost_saved; /**< Estimated dollar savings */
} compressor_stats_t;

/**
 * @brief Fast approximation of token count based on words and UTF-8 multi-byte characters.
 *
 * @param text Pointer to input string.
 * @param len Byte length of input string.
 * @return uint32_t Estimated token count.
 */
uint32_t compressor_estimate_tokens(const char* text, size_t len);

/**
 * @brief Sanitizes excessive whitespace, multiple newlines, and trailing spaces while protecting code blocks.
 *
 * @param src Source text buffer.
 * @param src_len Byte length of source text.
 * @param dst Destination buffer for sanitized output.
 * @param dst_cap Capacity of destination buffer.
 * @param preserve_code Whether to leave code blocks (```...```) untouched.
 * @return size_t Length of sanitized output written to dst, or 0 on error/truncation.
 */
size_t compressor_sanitize_whitespace(
    const char* src, size_t src_len, char* dst, size_t dst_cap, bool preserve_code);

/**
 * @brief Processes an incoming chat completion JSON payload through the multi-tier compression pipeline.
 *
 * @param payload Raw JSON request string.
 * @param payload_len Byte length of raw JSON string.
 * @param rule Active compression rule configuration.
 * @param out_result Output struct populated with execution metrics and compressed payload.
 * @return true if processing succeeded (regardless of whether compression was applied), false on parsing error.
 */
bool prompt_compressor_process_payload(const char*              payload,
                                       size_t                   payload_len,
                                       const compressor_rule_t* rule,
                                       compressor_result_t*     out_result);

/**
 * @brief Releases heap memory allocated inside a compressor_result_t.
 *
 * @param res Pointer to result struct.
 */
void prompt_compressor_result_cleanup(compressor_result_t* res);

/**
 * @brief Opaque in-memory circular cache for recent compression snapshots.
 */
typedef struct compressor_cache compressor_cache_t;

/**
 * @brief Prunes low-information/polite filler sentences based on sentence density, while preserving
 *        PII placeholders, constraint terms, and topic sentences.
 *
 * @param src Input text.
 * @param src_len Byte length of input text.
 * @param target_ratio Target length ratio (e.g. 0.60).
 * @param dst Destination buffer.
 * @param dst_cap Destination buffer capacity.
 * @return size_t Length written to dst.
 */
size_t compressor_prune_sentence_density(
    const char* src, size_t src_len, double target_ratio, char* dst, size_t dst_cap);

/**
 * @brief Creates a circular cache for prompt compression snapshots.
 *
 * @param capacity Maximum number of snapshots to retain (e.g. 200).
 * @return compressor_cache_t* Pointer to newly allocated cache, or NULL on error.
 */
compressor_cache_t* compressor_cache_create(size_t capacity);

/**
 * @brief Destroys the circular cache and frees internal resources.
 *
 * @param cache Pointer to cache.
 */
void compressor_cache_destroy(compressor_cache_t* cache);

/**
 * @brief Records a compression snapshot into the circular cache and updates global statistics.
 *
 * @param cache Pointer to cache.
 * @param snapshot Pointer to snapshot to copy.
 */
void compressor_cache_record(compressor_cache_t* cache, const compressor_snapshot_t* snapshot);

/**
 * @brief Retrieves cumulative statistics from the compression cache.
 *
 * @param cache Pointer to cache.
 * @param out_stats Output statistics struct.
 */
void compressor_cache_get_stats(compressor_cache_t* cache, compressor_stats_t* out_stats);

/**
 * @brief Retrieves up to max_count recent snapshots in descending chronological order.
 *
 * @param cache Pointer to cache.
 * @param out_snapshots Array to populate with snapshots.
 * @param max_count Maximum number of snapshots to retrieve.
 * @return size_t Actual number of snapshots populated.
 */
size_t compressor_cache_get_snapshots(compressor_cache_t*    cache,
                                      compressor_snapshot_t* out_snapshots,
                                      size_t                 max_count);

/**
 * @brief Checks if a rule matches the specified model name and estimated token count.
 *
 * @param rule Pointer to rule.
 * @param model Model name string.
 * @param estimated_tokens Estimated token count.
 * @return true if the rule matches and is enabled, false otherwise.
 */
bool
compressor_rule_match(const compressor_rule_t* rule, const char* model, uint32_t estimated_tokens);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_PROMPT_COMPRESSOR_H */

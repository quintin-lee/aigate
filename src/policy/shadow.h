/** @file shadow.h
 *  @ingroup group_policy
 *  @brief Traffic shadowing and canary A/B testing evaluation engine.
 */
#ifndef AIGATE_SHADOW_H
#define AIGATE_SHADOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pg_store;

/** @brief Default capacity for the non-blocking shadow task queue. */
#define SHADOW_QUEUE_DEFAULT_CAPACITY 256
/** @brief Default capacity for the recent side-by-side evaluation circular cache. */
#define SHADOW_EVAL_DEFAULT_CAPACITY 200

/**
 * @brief Traffic routing evaluation mode.
 */
typedef enum {
    /** @brief Asynchronous shadow mirroring: primary request untouched, clone called asynchronously. */
    TRAFFIC_MODE_SHADOW = 0,
    /** @brief Canary live split: progressive percentage of production traffic routed to candidate. */
    TRAFFIC_MODE_CANARY = 1
} traffic_mode_t;

/**
 * @brief Traffic shadowing or canary split rule configuration.
 */
typedef struct {
    long           id;                  /**< Rule primary key ID */
    char           source_model[64];    /**< Source model identifier (e.g., "gpt-4o") */
    char           target_model[64];    /**< Target candidate or shadow model */
    char           target_provider[32]; /**< Target provider override or empty string */
    traffic_mode_t mode;                /**< Evaluation mode (SHADOW or CANARY) */
    double         sample_rate;         /**< Sampling rate or canary split ratio (0.0 to 1.0) */
    char           header_match[64];    /**< Optional header filter (e.g., "x-env: test") */
    bool           enabled;             /**< Whether this rule is currently enabled */
    uint32_t       timeout_ms;          /**< Shadow request timeout in milliseconds */
} shadow_rule_t;

/**
 * @brief Single request dual-track evaluation snapshot item.
 */
typedef struct {
    char           eval_id[33];      /**< Unique evaluation hex identifier */
    char           trace_id[33];     /**< W3C Trace ID */
    char           source_model[64]; /**< Primary source model */
    char           target_model[64]; /**< Target shadow or canary model */
    traffic_mode_t mode;             /**< Traffic mode */

    double primary_latency_ms;       /**< Primary response total latency in ms */
    double shadow_latency_ms;        /**< Shadow response total latency in ms */
    double primary_ttft_ms;          /**< Primary time-to-first-token in ms */
    double shadow_ttft_ms;           /**< Shadow time-to-first-token in ms */

    int    primary_http_status;      /**< Primary HTTP status code */
    int    shadow_http_status;       /**< Shadow HTTP status code */
    long   primary_tokens;           /**< Primary prompt + completion tokens */
    long   shadow_tokens;            /**< Shadow prompt + completion tokens */
    double primary_cost_usd;         /**< Primary estimated cost in USD */
    double shadow_cost_usd;          /**< Shadow estimated cost in USD */

    char prompt_preview[256];        /**< Truncated user prompt preview */
    char primary_resp_snippet[512];  /**< Truncated primary response snippet */
    char shadow_resp_snippet[512];   /**< Truncated shadow response snippet */

    uint64_t timestamp_us;           /**< Monotonic timestamp in microseconds */
} shadow_eval_item_t;

/**
 * @brief Cloned task delivered to the background shadow queue.
 */
typedef struct {
    char          eval_id[33];         /**< Unique evaluation identifier */
    char          trace_id[33];        /**< W3C Trace ID */
    shadow_rule_t rule;                /**< Shadow rule copy */
    char*         body_copy;           /**< Cloned HTTP request body (heap allocated) */
    size_t        body_len;            /**< Cloned HTTP request body length */
    char          prompt_preview[256]; /**< Truncated prompt preview */
    uint64_t      timestamp_us;        /**< Monotonic timestamp */
} shadow_task_t;

/**
 * @brief Cumulative metrics and evaluation statistics.
 */
typedef struct {
    uint64_t total_evaluated;         /**< Total number of evaluated requests */
    uint64_t successful_shadow;       /**< Successful shadow executions */
    uint64_t failed_shadow;           /**< Failed shadow executions */
    double   primary_cost_usd;        /**< Total cost of primary requests */
    double   shadow_cost_usd;         /**< Total cost of shadow requests */
    double   cost_saved_usd;          /**< Estimated USD saved by shadow alternative */
    double   avg_primary_lat_ms;      /**< Average primary latency in ms */
    double   avg_shadow_lat_ms;       /**< Average shadow latency in ms */
    uint64_t dropped_shadow_requests; /**< Number of shadow tasks dropped due to full queue */
} shadow_stats_t;

/** @brief Opaque non-blocking shadow task queue. */
typedef struct shadow_queue shadow_queue_t;
/** @brief Opaque circular evaluation snapshot cache. */
typedef struct shadow_eval_cache shadow_eval_cache_t;
/** @brief Opaque shadow and canary engine instance. */
typedef struct shadow_engine shadow_engine_t;

/**
 * @brief Check if a shadow/canary rule matches the given model and header string.
 * @param rule       Rule to test.
 * @param model      Request model name.
 * @param header_str Request header to match against (can be NULL).
 * @return true if rule matches and is enabled; false otherwise.
 */
bool shadow_rule_matches(const shadow_rule_t* rule, const char* model, const char* header_str);

/**
 * @brief Determine if a rule should sample based on its sample rate.
 * @param rule Rule containing sample_rate.
 * @return true if sampled; false otherwise.
 */
bool shadow_rule_should_sample(const shadow_rule_t* rule);

/**
 * @brief Create a thread-safe shadow task queue.
 * @param capacity Maximum number of queued tasks.
 * @return Newly allocated queue, or NULL on failure.
 */
shadow_queue_t* shadow_queue_create(size_t capacity);

/**
 * @brief Destroy a shadow task queue and free remaining tasks.
 * @param q Queue to destroy.
 */
void shadow_queue_destroy(shadow_queue_t* q);

/**
 * @brief Push a task to the queue non-blockingly.
 * @param q    Target queue.
 * @param task Task data to push.
 * @return true if enqueued; false if queue is full (task dropped).
 */
bool shadow_queue_push(shadow_queue_t* q, const shadow_task_t* task);

/**
 * @brief Pop a task from the queue with timeout.
 * @param q          Target queue.
 * @param out_task   Buffer to receive popped task.
 * @param timeout_ms Timeout in milliseconds.
 * @return true if task popped; false on timeout.
 */
bool shadow_queue_pop(shadow_queue_t* q, shadow_task_t* out_task, uint32_t timeout_ms);

/**
 * @brief Return current number of tasks waiting in queue.
 * @param q Target queue.
 * @return Count of queued items.
 */
size_t shadow_queue_count(shadow_queue_t* q);

/**
 * @brief Return total number of tasks dropped due to queue saturation.
 * @param q Target queue.
 * @return Dropped task count.
 */
uint64_t shadow_queue_dropped(shadow_queue_t* q);

/**
 * @brief Free heap allocations inside a shadow task.
 * @param task Task to clean up.
 */
void shadow_task_free(shadow_task_t* task);

/**
 * @brief Create a circular evaluation snapshot cache.
 * @param capacity Maximum number of snapshots preserved in memory.
 * @return Allocated cache, or NULL on failure.
 */
shadow_eval_cache_t* shadow_eval_cache_create(size_t capacity);

/**
 * @brief Destroy an evaluation cache.
 * @param c Cache to destroy.
 */
void shadow_eval_cache_destroy(shadow_eval_cache_t* c);

/**
 * @brief Record a completed evaluation item into the circular cache.
 * @param c    Target cache.
 * @param item Evaluation item to record.
 */
void shadow_eval_cache_record(shadow_eval_cache_t* c, const shadow_eval_item_t* item);

/**
 * @brief Retrieve recent evaluation snapshots (most recent first).
 * @param c         Target cache.
 * @param out_items Output array.
 * @param max_items Maximum items out_items can hold.
 * @return Number of items written to out_items.
 */
int
shadow_eval_cache_get_recent(shadow_eval_cache_t* c, shadow_eval_item_t* out_items, int max_items);

/**
 * @brief Retrieve current cumulative evaluation statistics.
 * @param c         Target cache.
 * @param out_stats Output statistics buffer.
 */
void shadow_eval_cache_get_stats(shadow_eval_cache_t* c, shadow_stats_t* out_stats);

/**
 * @brief Return count of items currently stored in cache.
 * @param c Target cache.
 * @return Stored item count.
 */
size_t shadow_eval_cache_count(shadow_eval_cache_t* c);

/* --- Engine & Dual-Track Pairing APIs --- */

/**
 * @brief Create a global shadow engine instance.
 * @param ps        Backing PostgreSQL store (borrowed, can be NULL).
 * @param queue_cap Maximum queue capacity (e.g. 256).
 * @param eval_cap  Maximum recent evaluation snapshots capacity (e.g. 200).
 * @return Engine instance, or NULL on failure.
 */
shadow_engine_t* shadow_engine_create(struct pg_store* ps, size_t queue_cap, size_t eval_cap);

/**
 * @brief Destroy a shadow engine instance.
 * @param eng Engine to destroy.
 */
void shadow_engine_destroy(shadow_engine_t* eng);

/**
 * @brief Start the shadow engine background worker thread(s).
 * @param eng Target engine.
 * @return 0 on success, negative on error.
 */
int shadow_engine_start(shadow_engine_t* eng);

/**
 * @brief Stop the shadow engine worker thread(s).
 * @param eng Target engine.
 */
void shadow_engine_stop(shadow_engine_t* eng);

/**
 * @brief Initialize a pairing slot for a dual-track request.
 * @param eng            Target engine.
 * @param eval_id        Unique evaluation identifier.
 * @param trace_id       W3C Trace ID.
 * @param source_model   Primary model name.
 * @param target_model   Target shadow or canary model name.
 * @param mode           Traffic mode (SHADOW or CANARY).
 * @param prompt_preview Truncated prompt preview.
 * @return true if pairing slot allocated; false on error.
 */
bool shadow_engine_start_pairing(shadow_engine_t* eng,
                                 const char*      eval_id,
                                 const char*      trace_id,
                                 const char*      source_model,
                                 const char*      target_model,
                                 traffic_mode_t   mode,
                                 const char*      prompt_preview);

/**
 * @brief Record completion of the primary request side in a pairing slot.
 * @param eng          Target engine.
 * @param eval_id      Evaluation ID.
 * @param latency_ms   Primary latency in ms.
 * @param ttft_ms      Primary TTFT in ms.
 * @param http_status  Primary HTTP status code.
 * @param tokens       Primary prompt + completion tokens.
 * @param cost_usd     Primary estimated cost in USD.
 * @param resp_snippet Primary response snippet.
 * @return true if recorded, false if slot not found.
 */
bool shadow_engine_record_primary(shadow_engine_t* eng,
                                  const char*      eval_id,
                                  double           latency_ms,
                                  double           ttft_ms,
                                  int              http_status,
                                  long             tokens,
                                  double           cost_usd,
                                  const char*      resp_snippet);

/**
 * @brief Record completion of the shadow/canary candidate side in a pairing slot.
 * @param eng          Target engine.
 * @param eval_id      Evaluation ID.
 * @param latency_ms   Shadow latency in ms.
 * @param ttft_ms      Shadow TTFT in ms.
 * @param http_status  Shadow HTTP status code.
 * @param tokens       Shadow prompt + completion tokens.
 * @param cost_usd     Shadow estimated cost in USD.
 * @param resp_snippet Shadow response snippet.
 * @return true if recorded, false if slot not found.
 */
bool shadow_engine_record_shadow(shadow_engine_t* eng,
                                 const char*      eval_id,
                                 double           latency_ms,
                                 double           ttft_ms,
                                 int              http_status,
                                 long             tokens,
                                 double           cost_usd,
                                 const char*      resp_snippet);

/**
 * @brief Return count of completed evaluations in engine cache.
 * @param eng Target engine.
 * @return Count of items in cache.
 */
size_t shadow_engine_eval_count(shadow_engine_t* eng);

/**
 * @brief Retrieve recent evaluation snapshots from engine.
 * @param eng       Target engine.
 * @param out_items Output items array.
 * @param max_items Maximum items to retrieve.
 * @return Count of retrieved items.
 */
int
shadow_engine_get_recent_evals(shadow_engine_t* eng, shadow_eval_item_t* out_items, int max_items);

/**
 * @brief Retrieve aggregate evaluation metrics from engine.
 * @param eng       Target engine.
 * @param out_stats Output stats buffer.
 */
void shadow_engine_get_stats(shadow_engine_t* eng, shadow_stats_t* out_stats);

/**
 * @brief Submit a cloned shadow task to the engine queue.
 * @param eng  Target engine.
 * @param task Cloned task to submit.
 * @return true if enqueued; false if queue full or error.
 */
bool shadow_engine_submit_task(shadow_engine_t* eng, const shadow_task_t* task);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_SHADOW_H */

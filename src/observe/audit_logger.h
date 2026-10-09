/** @file audit_logger.h
 *  @ingroup group_observe
 *  @brief Audit log streaming, compliance data models, and dual-channel pipeline.
 */
#ifndef AIGATE_AUDIT_LOGGER_H
#define AIGATE_AUDIT_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Audit severity classification levels.
 */
typedef enum {
    AUDIT_SEV_INFO = 0,      /**< Normal successful requests (HTTP 200). */
    AUDIT_SEV_WARN = 1,      /**< 429 rate limit, concurrency limits, failovers. */
    AUDIT_SEV_VIOLATION = 2, /**< Guardrail blocks, PII leakage, prompt injections. */
    AUDIT_SEV_ERROR = 3      /**< 5xx errors, all-upstream outages, critical failures. */
} audit_severity_t;

/**
 * @brief Webhook message formatting adapters.
 */
typedef enum {
    AUDIT_HOOK_STANDARD = 0,   /**< Standard JSON payload for SIEM. */
    AUDIT_HOOK_FEISHU = 1,     /**< Feishu interactive card. */
    AUDIT_HOOK_DINGTALK = 2,   /**< DingTalk markdown robot message. */
    AUDIT_HOOK_WECHAT_WORK = 3 /**< WeChat Work markdown robot message. */
} audit_webhook_format_t;

/**
 * @brief Structured audit event representation.
 */
typedef struct audit_event {
    /* 1. Request metadata */
    char     trace_id[64];      /**< W3C Trace ID or unique request ID. */
    int64_t  timestamp_ms;      /**< Milliseconds epoch timestamp. */
    int64_t  key_id;            /**< Authenticated API key identifier. */
    char     client_ip[48];     /**< Client real IP address. */
    char     model[64];         /**< Model name requested. */
    char     provider[32];      /**< Actual upstream provider used. */
    int      http_status;       /**< Final HTTP status code returned. */
    uint32_t prompt_tokens;     /**< Inbound prompt token count. */
    uint32_t completion_tokens; /**< Outbound completion token count. */
    uint64_t latency_ns;        /**< Total round-trip latency in nanoseconds. */
    uint64_t ttft_ns;           /**< Time to first token in nanoseconds (streaming). */

    /* 2. Audit classification & violation details */
    audit_severity_t severity;           /**< Severity classification. */
    char             violation_type[32]; /**< Rule tag: "guardrail_block", "pii_leak", etc. */
    char             rule_detail[128];   /**< Matched rule description or regex name. */

    /* 3. Evidence prompt snapshot */
    char*  prompt_snapshot;     /**< Allocated truncated snapshot (VIOLATION/ERROR only). */
    size_t prompt_snapshot_len; /**< Length in bytes of prompt_snapshot. */
} audit_event_t;

/**
 * @brief In-memory live audit event snapshot for admin live inspection.
 */
typedef struct {
    uint64_t         seq_id;                   /**< Monotonic sequence identifier. */
    char             trace_id[64];             /**< Distributed trace ID. */
    char             tenant_id[64];            /**< Tenant identifier. */
    char             client_ip[48];            /**< Client IP address. */
    char             model[64];                /**< Model requested. */
    char             routed_model[64];         /**< Actual model routed. */
    char             provider[32];             /**< Upstream provider. */
    int              http_status;              /**< HTTP status returned. */
    uint32_t         prompt_tokens;            /**< Prompt tokens. */
    uint32_t         completion_tokens;        /**< Completion tokens. */
    uint32_t         ttft_ms;                  /**< Time to first token in ms. */
    uint32_t         total_latency_ms;         /**< Total latency in ms. */
    audit_severity_t severity;                 /**< Severity classification. */
    char             violation_type[32];       /**< Rule tag / violation reason. */
    char             rule_detail[128];         /**< Matched rule detail. */
    char             fallback_reason[32];      /**< Fallback trigger reason. */
    char             prompt_snippet[1024];     /**< Truncated prompt snapshot. */
    char             completion_snippet[1024]; /**< Truncated completion snapshot. */
    int64_t          timestamp_ms;             /**< Milliseconds epoch timestamp. */
} audit_live_event_t;

/**
 * @brief Initialize an audit event structure to zero/defaults.
 * @param[out] ev Pointer to audit event.
 */
void audit_event_init(audit_event_t* ev);

/**
 * @brief Free any dynamically allocated memory inside an audit event.
 * @param[in,out] ev Pointer to audit event.
 */
void audit_event_cleanup(audit_event_t* ev);

/**
 * @brief Perform a deep copy of an audit event from src to dst.
 * @param[out] dst Destination audit event.
 * @param[in]  src Source audit event.
 * @return 0 on success, -1 on allocation failure.
 */
int audit_event_copy(audit_event_t* dst, const audit_event_t* src);

/**
 * @brief Safely copy and truncate prompt text into the audit event snapshot.
 * @param[in,out] ev Audit event to update.
 * @param[in]     prompt Inbound prompt text (may be NULL).
 * @param[in]     max_len Maximum allowed characters/bytes to retain.
 * @return 0 on success, -1 on allocation failure.
 */
int audit_event_set_prompt(audit_event_t* ev, const char* prompt, size_t max_len);

/**
 * @brief Convert audit event to a single-line NDJSON string.
 * @param[in] ev Audit event to format.
 * @return Heap-allocated NUL-terminated JSON string (caller must free), or NULL on error.
 */
char* audit_event_to_ndjson(const audit_event_t* ev);

/**
 * @brief Convert audit event to a target webhook payload string.
 * @param[in] ev Audit event to format.
 * @param[in] fmt Target webhook adapter format.
 * @return Heap-allocated NUL-terminated JSON string (caller must free), or NULL on error.
 */
char* audit_event_to_webhook_payload(const audit_event_t* ev, audit_webhook_format_t fmt);

/* --- High-Performance Non-blocking Ring Buffer --- */

/**
 * @brief Thread-safe ring buffer for audit events.
 */
typedef struct audit_ring audit_ring_t;

/**
 * @brief Create a new audit ring buffer with fixed capacity.
 * @param capacity Maximum number of items in the buffer (min 4).
 * @return Newly allocated ring buffer or NULL on OOM.
 */
audit_ring_t* audit_ring_create(size_t capacity);

/**
 * @brief Destroy ring buffer and free all pending events.
 * @param ring Ring buffer to destroy (safe with NULL).
 */
void audit_ring_destroy(audit_ring_t* ring);

/**
 * @brief Non-blocking push of an audit event into the ring buffer.
 *
 * If the ring buffer is full, the oldest event at the head is freed and overwritten,
 * and the dropped counter is incremented. Never blocks the caller.
 *
 * @param ring Ring buffer instance.
 * @param ev Audit event to deep-copy into the buffer.
 * @return true on success, false on invalid arguments or OOM.
 */
bool audit_ring_push(audit_ring_t* ring, const audit_event_t* ev);

/**
 * @brief Pop a batch of audit events from the ring buffer.
 *
 * Waits up to timeout_ms if buffer is currently empty.
 * Ownership of popped audit events is transferred to caller (caller must audit_event_cleanup).
 *
 * @param ring Ring buffer instance.
 * @param out_batch Destination array for popped events.
 * @param max_count Maximum number of events to pop in this batch.
 * @param timeout_ms Maximum time to wait in milliseconds if buffer is empty (0 for non-blocking).
 * @return Number of events popped into out_batch.
 */
size_t audit_ring_pop_batch(audit_ring_t*  ring,
                            audit_event_t* out_batch,
                            size_t         max_count,
                            uint32_t       timeout_ms);

/**
 * @brief Get the current number of queued events in the ring buffer.
 * @param ring Ring buffer instance.
 * @return Queued item count.
 */
size_t audit_ring_count(audit_ring_t* ring);

/**
 * @brief Get the lifetime total dropped events due to buffer saturation.
 * @param ring Ring buffer instance.
 * @return Total dropped events.
 */
uint64_t audit_ring_dropped(audit_ring_t* ring);

/* --- Dual-Channel Audit Logger Engine --- */

/**
 * @brief Audit logger configuration parameters.
 */
typedef struct {
    char log_file[512];    /**< Destination NDJSON file path (empty = disabled). */
    int  max_size_mb;      /**< Single file maximum size before rotation (MB). */
    int  max_backups;      /**< Maximum number of rolled backup files retained. */
    char webhook_url[512]; /**< Target Webhook alert endpoint URL (empty = disabled). */
    audit_webhook_format_t webhook_format; /**< Payload template adapter format. */
    int                    max_prompt_len; /**< Maximum characters retained in prompt snapshot. */
    double sample_rate; /**< Probabilistic sample rate (0.0 to 1.0) for INFO events. */
} audit_config_t;

/**
 * @brief Opaque handle to audit logger service.
 */
typedef struct audit_logger audit_logger_t;

/**
 * @brief Create a new audit logger instance with configuration.
 * @param cfg Pointer to audit configuration.
 * @return Newly allocated audit logger or NULL on OOM.
 */
audit_logger_t* audit_logger_create(const audit_config_t* cfg);

/**
 * @brief Start background worker threads for Channel A and Channel B.
 * @param al Audit logger instance.
 * @return 0 on success, -1 on thread creation failure.
 */
int audit_logger_start(audit_logger_t* al);

/**
 * @brief Stop background workers and flush pending queued events.
 * @param al Audit logger instance.
 */
void audit_logger_stop(audit_logger_t* al);

/**
 * @brief Signal Channel A file worker to reload and reopen the log file (SIGHUP support).
 * @param al Audit logger instance.
 */
void audit_logger_reload(audit_logger_t* al);

/**
 * @brief Non-blocking record of an audit event into the dual-channel pipeline (< 1µs).
 * @param al Audit logger instance (safe if NULL).
 * @param ev Audit event to record.
 */
void audit_logger_record(audit_logger_t* al, const audit_event_t* ev);

/**
 * @brief Destroy audit logger and free all associated resources.
 * @param al Audit logger instance (safe if NULL).
 */
void audit_logger_destroy(audit_logger_t* al);

/**
 * @brief Get total dropped events across all internal buffers.
 * @param al Audit logger instance.
 * @return Dropped events count.
 */
uint64_t audit_logger_get_dropped_total(audit_logger_t* al);

/**
 * @brief Get total successful webhook dispatches.
 * @param al Audit logger instance.
 * @return Success count.
 */
uint64_t audit_logger_get_webhook_success_total(audit_logger_t* al);

/**
 * @brief Get total failed webhook dispatches (after exhausting retries).
 * @param al Audit logger instance.
 * @return Failure count.
 */
uint64_t audit_logger_get_webhook_failures_total(audit_logger_t* al);

/**
 * @brief Get maximum allowed prompt length configured for this logger.
 * @param al Audit logger instance (safe if NULL).
 * @return Configured max prompt length, or default 4096.
 */
int audit_logger_get_max_prompt_len(const audit_logger_t* al);

/**
 * @brief Query recent audit events from the in-memory live ring buffer.
 * @param[in]  al Audit logger handle.
 * @param[out] out_events Destination array for returned live events.
 * @param[in]  max_count Maximum events to return in out_events.
 * @param[in]  after_seq Return events with seq_id > after_seq (0 returns oldest available up to max_count).
 * @param[out] out_missed Outputs count of events overwritten/missed since after_seq.
 * @return Number of events populated in out_events.
 */
size_t audit_logger_query_recent(audit_logger_t*     al,
                                 audit_live_event_t* out_events,
                                 size_t              max_count,
                                 uint64_t            after_seq,
                                 size_t*             out_missed);

#endif /* AIGATE_AUDIT_LOGGER_H */

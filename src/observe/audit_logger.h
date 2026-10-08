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

#endif /* AIGATE_AUDIT_LOGGER_H */

/** @file aigate_core.h
 *  @brief Transport-agnostic gateway pipeline (THE SEAM, spec §2.2).
 *
 *  Fill aigate_request_ctx from any transport and call
 *  aigate_handle_request: auth → allowlist → rate limit → route →
 *  provider build → upstream (single retry on 5xx) → usage parse →
 *  meter → response write. The response ctx write callbacks carry the
 *  answer back to the transport.
 */

/**
 * @defgroup group_core Core layer
 * @brief Core: gateway context, config, secrets, logging.
 */
#ifndef AIGATE_CORE_H
#define AIGATE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "auth_key.h"
#include "circuit_breaker.h"
#include "guardrails.h"
#include "budget_enforce.h"
#include "model_router.h"
#include "ratelimit.h"
#include "usage_meter.h"
#include "observe/tracer.h"

/** @brief Normalized inbound request (transport fills, pipeline reads). */
typedef struct aigate_request_ctx {
    const char* method;        /**< "POST" */
    const char* path;          /**< "/v1/chat/completions" */
    const char* bearer;        /**< client key, raw */
    const char* client_ip;     /**< Peer IP (for rate limiting/auditing, may be NULL) */
    const void* body;          /**< Request body (borrowed, not owned) */
    size_t      body_len;      /**< Request body length in bytes */
    const char* cache_control; /**< Client Cache-Control header, may be NULL */
    const char*
        target_provider; /**< Client X-Aigate-Target-Provider header for channel debugging, may be NULL */
    const char* traceparent; /**< Inbound W3C traceparent header, may be NULL */
} aigate_request_ctx;

/** @brief Outbound response sink (transport implements callbacks). */
typedef struct aigate_response_ctx {
    int status;       /**< HTTP status to write */
    bool
        headers_sent; /**< Headers already flushed (triggered by first write), prevent duplicates */
    void* impl;       /**< Transport private handle (borrowed) */
    int (*set_header)(void*       impl,
                      const char* name,
                      const char* value); /**< Append response header before flush */
    int (*write)(void*       impl,
                 const void* buf,
                 size_t      len,
                 bool        fin); /**< Chunked write after status+headers flushed */
} aigate_response_ctx;

struct health_prober;
struct event_bus;
struct response_cache;
struct latency_tracker;

/** @brief Gateway pipeline state: policy handles plus config (THE SEAM owner). */
typedef struct aigate_core {
    auth_key_cache          keys;               /**< API key cache (includes negative cache) */
    ratelimit_t*            rl;                 /**< Rate limiter, may be NULL (disabled) */
    model_router_t*         router;             /**< Model routing table */
    usage_meter_t*          um;                 /**< Usage meter, may be NULL (disabled) */
    circuit_breaker_t*      cb;                 /**< Circuit breaker, may be NULL (disabled) */
    pg_store_t*             ps;                 /**< Backing store (borrowed, not owned) */
    int                     default_timeout_ms; /**< Upstream default timeout, ms */
    guardrails_ctx_t*       gr;                 /**< Guardrails context, may be NULL (disabled) */
    budget_enforce_mgr_t*   be;                 /**< Budget enforcement, may be NULL (disabled) */
    struct health_prober*   hp;                 /**< Health prober, may be NULL */
    struct event_bus*       eb;                 /**< Event bus, may be NULL */
    struct response_cache*  rc;                 /**< Response cache, may be NULL (disabled) */
    struct latency_tracker* lt;                 /**< Latency tracker & hedge budget, may be NULL */
    tracer_config_t         tracer_cfg; /**< OpenTelemetry distributed tracing configuration */
    trace_ring_buffer_t*    trace_rb;   /**< Trace export ring buffer, may be NULL (disabled) */
} aigate_core;

/** @brief Type alias for gateway pipeline context. */
typedef struct aigate_core aigate_ctx;
/** @brief Type alias for gateway pipeline context with _t suffix. */
typedef struct aigate_core aigate_ctx_t;

/** @brief Initialize the pipeline state. @return 0 ok, -1 on alloc failure. */
int aigate_core_init(aigate_core*   ac,
                     pg_store_t*    ps,
                     const uint8_t* master32,
                     int            default_timeout_ms,
                     int            flush_interval_s);

/** @brief Release the pipeline (flushes usage). */
void aigate_core_shutdown(aigate_core* ac);

/** @brief Reload guardrails rules from DB into memory. */
int aigate_core_reload_guardrails(aigate_core* ac);

/** @brief Run the full pipeline. @return 0 when a response body (success
 *  or error) has been written. */
int aigate_handle_request(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/** @brief Write a JSON body with a status. Allocates the HTTP header
 *  block (Content-Type/Length) and one fin write. */
int aigate_write_json(aigate_response_ctx* rc, int status, const char* body, size_t len);

/** @brief Write an OpenAI-shaped error body:
 *  {"error":{"message":...,"type":...,"code":HTTP status}}. */
int
aigate_write_error(aigate_response_ctx* rc, int http_status, const char* type, const char* message);

/** @brief Write Anthropic error JSON ({"type": "error", "error": {"type": ..., "message": ...}}). */
int aigate_write_anthropic_error(aigate_response_ctx* rc,
                                 int                  http_status,
                                 const char*          type,
                                 const char*          message);

/** @brief Write Gemini error JSON ({"error": {"code": ..., "message": ..., "status": ...}}). */
int aigate_write_gemini_error(aigate_response_ctx* rc,
                              int                  http_status,
                              const char*          status_str,
                              const char*          message);

#endif /* AIGATE_CORE_H */

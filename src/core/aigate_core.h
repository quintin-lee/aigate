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
 * @defgroup group_core 核心层
 * @brief 核心：网关上下文、配置、密钥、日志。
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

/** @brief Normalized inbound request (transport fills, pipeline reads). */
typedef struct aigate_request_ctx {
    const char* method; /**< "POST" */
    const char* path;   /**< "/v1/chat/completions" */
    const char* bearer; /**< client key, raw */
    const char* client_ip;    /**< 对端 IP（限流/审计用，可为 NULL） */
    const void* body;         /**< 请求体（借用，不拥有） */
    size_t      body_len;     /**< 请求体字节数 */
    const char* cache_control; /**< 客户端 Cache-Control 头，可为 NULL */
} aigate_request_ctx;

/** @brief Outbound response sink (transport implements callbacks). */
typedef struct aigate_response_ctx {
    int   status;       /**< 待写 HTTP 状态码 */
    bool  headers_sent; /**< 头已刷出（首写触发），防重复 */
    void* impl;         /**< 传输私有句柄（借用） */
    int (*set_header)(void* impl, const char* name, const char* value); /**< 未发出前追加响应头 */
    int (*write)(void* impl, const void* buf, size_t len, bool fin);    /**< 首写刷状态行+头后分片写 */
} aigate_response_ctx;

struct health_prober;
struct event_bus;
struct response_cache;

/** @brief Gateway pipeline state: policy handles plus config (THE SEAM owner). */
typedef struct aigate_core {
    auth_key_cache         keys;              /**< API key 缓存（含否定缓存） */
    ratelimit_t*           rl;                /**< 限流器，可为 NULL（禁用） */
    model_router_t*        router;            /**< 模型路由表 */
    usage_meter_t*         um;                /**< 用量计量，可为 NULL（禁用） */
    circuit_breaker_t*     cb;                /**< 熔断器，可为 NULL（禁用） */
    pg_store_t*            ps;                /**< 后备存储（借用，不拥有） */
    int                    default_timeout_ms; /**< 上游默认超时，毫秒 */
    guardrails_ctx_t*      gr;                /**< 护栏上下文，可为 NULL（禁用） */
    budget_enforce_mgr_t*  be;                /**< 预算强制，可为 NULL（禁用） */
    struct health_prober*  hp;                /**< 健康探针，可为 NULL */
    struct event_bus*      eb;                /**< 事件总线，可为 NULL */
    struct response_cache* rc;                /**< 响应缓存，可为 NULL（禁用） */
} aigate_core;

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
 *  {"error":{"message":...,"type":...,"code":HTTP 状态码}}. */
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

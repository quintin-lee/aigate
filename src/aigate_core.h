/** @file aigate_core.h
 *  @brief Transport-agnostic gateway pipeline (THE SEAM, spec §2.2).
 *
 *  Fill aigate_request_ctx from any transport and call
 *  aigate_handle_request: auth → allowlist → rate limit → route →
 *  provider build → upstream (single retry on 5xx) → usage parse →
 *  meter → response write. The response ctx write callbacks carry the
 *  answer back to the transport.
 */
#ifndef AIGATE_CORE_H
#define AIGATE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "auth_key.h"
#include "model_router.h"
#include "ratelimit.h"
#include "usage_meter.h"

typedef struct aigate_request_ctx {
    const char* method; /* "POST" */
    const char* path;   /* "/v1/chat/completions" */
    const char* bearer; /* client key, raw */
    const char* client_ip;
    const void* body;
    size_t      body_len;
} aigate_request_ctx;

typedef struct aigate_response_ctx {
    int   status;
    bool  headers_sent;
    void* impl;
    int (*set_header)(void* impl, const char* name, const char* value);
    int (*write)(void* impl, const void* buf, size_t len, bool fin);
} aigate_response_ctx;

typedef struct aigate_core {
    auth_key_cache  keys;
    ratelimit_t*    rl;
    model_router_t* router;
    usage_meter_t*  um;
    int             default_timeout_ms;
} aigate_core;

/** @brief Initialize the pipeline state. @return 0 ok, -1 on alloc failure. */
int aigate_core_init(aigate_core*   ac,
                     pg_store_t*    ps,
                     const uint8_t* master32,
                     int            default_timeout_ms,
                     int            flush_interval_s);

/** @brief Release the pipeline (flushes usage). */
void aigate_core_shutdown(aigate_core* ac);

/** @brief Run the full pipeline. @return 0 when a response body (success
 *  or error) has been written. */
int aigate_handle_request(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/** @brief Write a JSON body with a status. Allocates the HTTP header
 *  block (Content-Type/Length) and one fin write. */
int aigate_write_json(aigate_response_ctx* rc, int status, const char* body, size_t len);

/** @brief Write an OpenAI-shaped error body:
 *  {"error":{"message":...,"type":...,"code":<http_status>}}. */
int
aigate_write_error(aigate_response_ctx* rc, int http_status, const char* type, const char* message);

#endif /* AIGATE_CORE_H */

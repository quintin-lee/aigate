/** @file provider_adapter.h
 *  @brief VTable interface and registry for upstream LLM providers (Plan 3, Task 1).
 */
#ifndef AIGATE_PROVIDER_ADAPTER_H
#define AIGATE_PROVIDER_ADAPTER_H

#include "aigate_core.h"
#include "pg_store.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct stream_bridge stream_bridge_t;

typedef struct provider_adapter {
    const char* name;

    /** @brief Check if this adapter handles the given provider label. */
    bool (*supports)(const char* provider);

    /** @brief Build upstream request URL, headers, and body for chat completions. */
    int (*build_chat)(const model_rec_t* route,
                      const char*        in_body,
                      char*              url_out,
                      size_t             url_cap,
                      const char*        extra_headers[4][2],
                      int*               n_extra_headers,
                      char**             out_body,
                      size_t*            out_body_len);

    /** @brief Parse non-streaming chat response; extracts token metrics & translates body. */
    int (*parse_chat_response)(const char* raw_body,
                               size_t      raw_len,
                               const char* model,
                               int*        http_status,
                               char**      out_body,
                               size_t*     out_len,
                               long*       out_ptok,
                               long*       out_ctok,
                               long*       out_cached_tok);

    /** @brief Allocate and initialize a streaming bridge state machine. */
    stream_bridge_t* (*stream_bridge_new)(aigate_response_ctx* rc, const char* model);

    /** @brief Feed an upstream chunk to the bridge; returns 0 or -1 on abort. */
    int (*stream_bridge_feed)(void* bridge, const void* chunk, size_t len);

    /** @brief Finalize the streaming bridge (e.g. emit final chunk and [DONE]). */
    int (*stream_bridge_finish)(stream_bridge_t* b);

    /** @brief Check if the bridge has sent HTTP response headers to client. */
    bool (*stream_bridge_headers_sent)(stream_bridge_t* b);

    /** @brief Extract accumulated token counts from the bridge. */
    void (*stream_bridge_get_tokens)(stream_bridge_t* b,
                                     long*            out_ptok,
                                     long*            out_ctok,
                                     long*            out_cached_tok);

    /** @brief Free bridge state machine allocations. */
    void (*stream_bridge_free)(stream_bridge_t* b);

    /** @brief Build request for /v1/embeddings (NULL if unsupported). */
    int (*build_embeddings)(const model_rec_t* route,
                            const char*        in_body,
                            char*              url_out,
                            size_t             url_cap,
                            const char*        extra_headers[4][2],
                            int*               n_extra_headers,
                            char**             out_body,
                            size_t*            out_body_len);

    /** @brief Parse non-streaming embeddings response (NULL if unsupported). */
    int (*parse_embeddings_response)(const char* raw_body,
                                     size_t      raw_len,
                                     const char* model,
                                     int*        http_status,
                                     char**      out_body,
                                     size_t*     out_len,
                                     long*       out_ptok);
} provider_adapter_t;

/** @brief Look up the provider adapter by provider name; returns NULL if unsupported. */
const provider_adapter_t* provider_find(const char* provider);

/** @brief GET /models probe plan for a provider family (P1-4).
 * Family is decided by the adapter registry (supports()); URL/header
 * conventions mirror the adapters' own build_chat rules.
 * @p out->extra_header holds "anthropic-version" for the anthropic
 * family (fixed value "2023-06-01"); empty string otherwise. */
typedef struct {
    char url[1024];
    char auth_header[32];
    int  bearer; /* 1 = prefix the key value with "Bearer " */
    char extra_header[32];
} provider_probe_plan_t;

/** @brief Map a provider_type + endpoint to a GET /models probe plan.
 * @return 0 ok; -1 when no adapter supports @p provider_type, @p endpoint
 *         is empty, or the URL would exceed 1024 chars. */
int
provider_probe_plan(const char* provider_type, const char* endpoint, provider_probe_plan_t* out);

#endif /* AIGATE_PROVIDER_ADAPTER_H */

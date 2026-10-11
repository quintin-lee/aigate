/** @file provider_gemini.h
 *  @ingroup group_upstream
 *  @brief Google Gemini provider adapter (Plan 3, Task 3 & 4).
 */
#ifndef AIGATE_PROVIDER_GEMINI_H
#define AIGATE_PROVIDER_GEMINI_H

#include "aigate_core.h"
#include "pg_store.h"
#include "provider_adapter.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief Gemini provider vtable instance (see the provider_adapter vtable). */
extern const provider_adapter_t g_provider_gemini;

/** @brief 1 when provider is "gemini" or "google". */
int provider_gemini_supports(const char* provider);

/** @brief Build Google Gemini generateContent request from OpenAI chat completion request. */
int provider_gemini_build(const model_rec_t* route,
                          const char*        in_body,
                          char*              url_out,
                          size_t             url_cap,
                          const char*        extra_headers[4][2],
                          int*               n_extra_headers,
                          char**             out_body,
                          size_t*            out_body_len);

/** @brief Translate Gemini generateContent response JSON to OpenAI chat completion format. */
int provider_gemini_resp_to_openai(const char* gemini_resp,
                                   const char* req_model,
                                   char**      out_openai,
                                   size_t*     out_openai_len,
                                   long*       out_ptok,
                                   long*       out_ctok);

/** @brief Translate Gemini generateContent response JSON to OpenAI format with optional tag emulation. */
int provider_gemini_resp_to_openai_ext(const char* gemini_resp,
                                       const char* req_model,
                                       bool        tag_emulation,
                                       char**      out_openai,
                                       size_t*     out_openai_len,
                                       long*       out_ptok,
                                       long*       out_ctok);

/** @brief Gemini→OpenAI streaming translation bridge state machine. */
typedef struct gemini_bridge {
    aigate_response_ctx* rc;                /**< Downstream response context (borrowed). */
    bool                 headers_sent;      /**< Downstream headers already sent. */
    bool                 aborted;           /**< Downstream aborted. */
    char                 line_buf[8192];    /**< SSE line buffer. */
    size_t               line_len;          /**< Line buffer bytes used. */
    char                 model[64];         /**< Model name. */
    char                 msg_id[64];        /**< Upstream message id. */
    char                 finish_reason[32]; /**< Upstream finishReason. */
    long                 prompt_tokens;     /**< Accumulated prompt tokens. */
    long                 completion_tokens; /**< Accumulated completion tokens. */
    bool                 done_emitted;      /**< [DONE] already emitted. */
    bool                 tag_emulation;     /**< True to output <think>...</think> in content */
    bool                 tag_opened;        /**< True if <think> open tag already sent */
    bool                 tag_closed;        /**< True if </think> close tag already sent */
    bool                 in_thought;        /**< True while emitting thought deltas */
    long                 reasoning_tokens;  /**< Accumulated reasoning tokens */
} gemini_bridge_t;

/** @brief Create a Gemini→OpenAI streaming translation bridge. */
stream_bridge_t* provider_gemini_bridge_new(aigate_response_ctx* rc, const char* model);

/** @brief Get the bridge's accumulated prompt, completion, cached, and reasoning tokens. */
void provider_gemini_bridge_get_tokens(stream_bridge_t* bridge,
                                       long*            out_ptok,
                                       long*            out_ctok,
                                       long*            out_cached_tok,
                                       long*            out_reasoning_tok);

/** @brief Build Gemini request for embeddings (single -> embedContent, array -> batchEmbedContents). */
int provider_gemini_build_embeddings(const model_rec_t* route,
                                     const char*        in_body,
                                     char*              url_out,
                                     size_t             url_cap,
                                     const char*        extra_headers[4][2],
                                     int*               n_extra_headers,
                                     char**             out_body,
                                     size_t*            out_body_len);

/** @brief Parse Gemini embedding response and translate to OpenAI standard format. */
int provider_gemini_parse_embeddings(const char* raw_body,
                                     size_t      raw_len,
                                     const char* model,
                                     int*        http_status,
                                     char**      out_body,
                                     size_t*     out_len,
                                     long*       out_ptok);

/** @brief Parse token usage from non-streaming Gemini response JSON. */
int gemini_sniff_usage_json(const char* json_str, long* out_ptok, long* out_ctok, long* out_cached);

/** @brief Lightweight passive line-buffered sniffer for Gemini SSE streams. */
typedef struct gemini_sniffer {
    char   line_buf[8192];    /**< SSE line buffer. */
    size_t line_len;          /**< Line buffer bytes used. */
    long   prompt_tokens;     /**< Accumulated prompt tokens. */
    long   candidates_tokens; /**< Accumulated candidates tokens. */
    long   cached_tokens;     /**< Accumulated cache-hit tokens. */
} gemini_sniffer_t;

/** @brief Reset the sniffer (line buffer and token counters). */
void gemini_sniffer_init(gemini_sniffer_t* s);
/** @brief Feed SSE data chunk by chunk, parsing usageMetadata lines to accumulate tokens.
 *  @return 0 on success (kept for signature compatibility). */
int gemini_sniffer_feed(gemini_sniffer_t* s, const void* chunk, size_t len);
/** @brief Get accumulated prompt/candidates/cached tokens (any out may be NULL). */
void gemini_sniffer_get_tokens(const gemini_sniffer_t* s,
                               long*                   out_ptok,
                               long*                   out_ctok,
                               long*                   out_cached);

#endif /* AIGATE_PROVIDER_GEMINI_H */

/** @file provider_anthropic.h
 *  @brief Anthropic Claude provider adapter (messages API & SSE bridge, spec §5).
 */
#ifndef AIGATE_PROVIDER_ANTHROPIC_H
#define AIGATE_PROVIDER_ANTHROPIC_H

#include "aigate_core.h"
#include "pg_store.h"
#include "provider_adapter.h"

#include <stddef.h>

extern const provider_adapter_t g_provider_anthropic;

/** @brief 1 when provider label is "anthropic". */
int provider_anthropic_supports(const char* provider);

/** @brief Build outbound Anthropic /v1/messages request from OpenAI chat completion.
 * @param route       resolved model route
 * @param in_body     inbound OpenAI JSON body
 * @param url_out     output URL buffer
 * @param url_cap     capacity of url_out
 * @param headers_kv  receives static header pairs [key, value]
 * @param n_headers   receives number of header pairs (2: x-api-key, anthropic-version)
 * @param out_body    receives malloc'd translated Anthropic JSON (caller frees)
 * @param out_body_len receives length of out_body
 * @return 0 ok; -1 on JSON/alloc error.
 */
int provider_anthropic_build(const model_rec_t* route,
                             const char*        in_body,
                             char*              url_out,
                             size_t             url_cap,
                             const char*        headers_kv[4][2],
                             int*               n_headers,
                             char**             out_body,
                             size_t*            out_body_len);

/** @brief Translate Anthropic non-streaming response JSON to OpenAI format.
 * @param anthropic_resp Anthropic JSON response string
 * @param req_model      fallback model name if absent in response
 * @param out_openai     receives malloc'd OpenAI format JSON (caller frees)
 * @param out_openai_len receives length of out_openai
 * @param out_ptok       receives input_tokens
 * @param out_ctok       receives output_tokens
 * @return 0 ok; -1 on error.
 */
int provider_anthropic_resp_to_openai(const char* anthropic_resp,
                                      const char* req_model,
                                      char**      out_openai,
                                      size_t*     out_openai_len,
                                      long*       out_ptok,
                                      long*       out_ctok);

/** @brief State machine for streaming Anthropic SSE to OpenAI SSE chunks. */
typedef struct anthropic_bridge {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    char                 line_buf[4096];
    size_t               line_len;
    char                 current_event[64];
    char                 msg_id[64];
    char                 model[64];
    long                 input_tokens;
    long                 output_tokens;
    bool                 done_emitted;
} anthropic_bridge_t;

/** @brief Initialize bridge with response context. */
void anthropic_bridge_init(anthropic_bridge_t* b, aigate_response_ctx* rc);

/** @brief Feed raw upstream chunk to the bridge; translates and writes OpenAI SSE chunks.
 * @return 0 on success; -1 on client write abort. */
int anthropic_bridge_feed(anthropic_bridge_t* b, const void* chunk, size_t len);

/** @brief Finalize bridge stream (emits [DONE] if not yet emitted). */
int anthropic_bridge_finish(anthropic_bridge_t* b);

#endif /* AIGATE_PROVIDER_ANTHROPIC_H */

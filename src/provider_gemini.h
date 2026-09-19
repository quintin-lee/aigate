/** @file provider_gemini.h
 *  @brief Google Gemini provider adapter (Plan 3, Task 3 & 4).
 */
#ifndef AIGATE_PROVIDER_GEMINI_H
#define AIGATE_PROVIDER_GEMINI_H

#include "aigate_core.h"
#include "pg_store.h"
#include "provider_adapter.h"

#include <stdbool.h>
#include <stddef.h>

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

#endif /* AIGATE_PROVIDER_GEMINI_H */

/** @file provider_openai.h
 *  @brief OpenAI-compatible provider adapter (openai / ollama / azure).
 *
 *  Covers every provider whose wire format matches OpenAI: vanilla OpenAI,
 *  local vLLM/Ollama endpoints, and Azure OpenAI (which adds an
 *  `api-version` query parameter and uses an `api-key` header).
 */
#ifndef AIGATE_PROVIDER_OPENAI_H
#define AIGATE_PROVIDER_OPENAI_H

#include "pg_store.h"

/** @brief Build the exact upstream request for an openai-compatible backend.
 *
 * - URL: endpoint + path; for provider "azure" the api-version from
 *   default_params is appended as a query parameter.
 * - Body: jansson object-merge of default_params under the request body
 *   (request fields win); the caller's request body is the base.
 *
 * @param route      resolved model route
 * @param in_body    client request body JSON (may be NULL → defaults only)
 * @param url_out    out buffer for the final URL
 * @param url_cap    url_out capacity
 * @param out_body   out malloc'd merged JSON body (caller frees)
 * @param out_body_len out merged body length
 * @return 0 ok; -1 on alloc/JSON error.
 * @note The "model" field of the request body is passed through verbatim;
 *       admin controls naming, this adapter does not rewrite it. */
int provider_openai_build(const model_rec_t *route, const char *in_body,
                          char *url_out, size_t url_cap, char **out_body,
                          size_t *out_body_len);

/** @brief 1 when @p provider label is handled by this adapter. */
int provider_openai_supports(const char *provider);

#endif /* AIGATE_PROVIDER_OPENAI_H */

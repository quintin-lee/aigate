/** @file provider_openai.h
 *  @brief OpenAI-compatible provider adapter (openai / ollama / azure).
 *
 *  Covers every provider whose wire format matches OpenAI: vanilla OpenAI,
 *  local vLLM/Ollama endpoints, and Azure OpenAI (which adds an
 *  `api-version` query parameter and uses a Bearer auth header).
 */
#ifndef AIGATE_PROVIDER_OPENAI_H
#define AIGATE_PROVIDER_OPENAI_H

#include "pg_store.h"

/** @brief 1 when @p provider label is handled by this adapter. */
int provider_openai_supports(const char *provider);

/** @brief Build the exact upstream request for an openai-compatible backend.
 * @param route      resolved model route
 * @param up_path    the upstream request path ("/chat/completions",
 *                   "/embeddings", ...) — appended to endpoint
 * @param in_body    client request body JSON (may be NULL → defaults only)
 * @param url_out    out buffer for the final URL
 * @param url_cap    url_out capacity
 * @param out_body   out malloc'd merged JSON body (caller frees)
 * @param out_body_len out merged body length
 * @return 0 ok; -1 on alloc/JSON error.
 * @note URL: endpoint + up_path; for provider "azure" the api-version from
 *       default_params is appended as a query parameter (stripped from the
 *       body). The "model" field of the request body is passed through
 *       verbatim; admin controls naming, this adapter does not rewrite it.
 * @note Merged body: jansson object-merge where request fields win over
 *       default_params fields. */
int provider_openai_build(const model_rec_t *route, const char *up_path,
                         const char *in_body, char *url_out, size_t url_cap,
                         char **out_body, size_t *out_body_len);

#endif /* AIGATE_PROVIDER_OPENAI_H */

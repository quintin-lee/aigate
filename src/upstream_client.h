/** @file upstream_client.h
 *  @brief libcurl upstream transport: one non-streaming call per request.
 *
 *  (SSE streaming is Plan 2.) Each call uses a private easy handle; the
 *  gateway's worker threads each drive their own handle, so no global
 *  multi handle is needed yet.
 */
#ifndef AIGATE_UPSTREAM_CLIENT_H
#define AIGATE_UPSTREAM_CLIENT_H

#include "pg_store.h"

/** @brief Issue one upstream HTTP call.
 * @param route     fully resolved model route (endpoint + upstream_key)
 * @param path      request path (e.g. "/chat/completions")
 * @param body_json merged request body (defaults applied by the provider
 *                  adapter before this call)
 * @param body_len  body length
 * @param timeout_ms 0 → use the configured default
 * @param out_status  receives upstream HTTP status
 * @param out_body    receives malloc'd body (caller frees via free)
 * @param out_body_len receives body length
 * @return 0 on transport success (even on 4xx/5xx responses, status in
 *         out_status); -110 timeout; -502 transport failure. */
int upstream_call(const model_rec_t *route, const char *path,
                  const char *body_json, size_t body_len, long timeout_ms,
                  int *out_status, char **out_body, size_t *out_body_len);

#endif /* AIGATE_UPSTREAM_CLIENT_H */

/** @file upstream_client.h
 *  @brief libcurl upstream transport: one non-streaming call per request.
 *
 *  The URL is produced by the provider adapter (which knows about
 *  provider-specific query params, e.g. Azure api-version); this layer is
 *  provider-agnostic.
 *
 *  (SSE streaming is Plan 2.) Each call uses a private easy handle; the
 *  gateway's worker threads each drive their own handle, so no global
 *  multi handle is needed yet.
 */
#ifndef AIGATE_UPSTREAM_CLIENT_H
#define AIGATE_UPSTREAM_CLIENT_H

#include <stddef.h>

/** @brief Issue one upstream HTTP POST.
 * @param url        fully formed request URL (scheme://host:port/path[?q])
 * @param upstream_key bearer token for the upstream ("" = no auth header)
 * @param body_json  request body (JSON)
 * @param body_len   body length (strlen of @p body_json)
 * @param timeout_ms 0 → default 60000
 * @param out_status receives upstream HTTP status (only valid on rc==0)
 * @param out_body   receives malloc'd body (caller frees via free)
 * @param out_body_len receives body length
 * @return 0 on transport success (even on 4xx/5xx responses, status in
 *         out_status); -110 timeout; -502 transport failure. */
int upstream_call(const char *url, const char *upstream_key,
                  const char *body_json, size_t body_len, long timeout_ms,
                  int *out_status, char **out_body, size_t *out_body_len);

#endif /* AIGATE_UPSTREAM_CLIENT_H */

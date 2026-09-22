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
int upstream_call(const char* url,
                  const char* upstream_key,
                  const char* body_json,
                  size_t      body_len,
                  long        timeout_ms,
                  int*        out_status,
                  char**      out_body,
                  size_t*     out_body_len);

/** @brief Issue one upstream HTTP POST with custom headers (e.g. Anthropic x-api-key). */
int upstream_call_ext(const char* url,
                      const char* upstream_key,
                      const char* extra_headers_kv[][2],
                      int         n_extra_headers,
                      const char* body_json,
                      size_t      body_len,
                      long        timeout_ms,
                      int*        out_status,
                      char**      out_body,
                      size_t*     out_body_len);

/** @brief Streaming chunk callback. Return 0 on success; non-zero aborts transfer. */
typedef int (*upstream_chunk_fn)(void* user_data, const void* chunk, size_t len);

/** @brief Issue one upstream HTTP POST in streaming mode.
 * @param url                fully formed request URL
 * @param upstream_key       bearer token ("" = none)
 * @param extra_headers_kv   optional array of [key, value] headers, or NULL
 * @param n_extra_headers    count of extra headers
 * @param body_json          request body (JSON)
 * @param body_len           body length (0 = strlen(body_json))
 * @param silence_timeout_ms inter-chunk silence timeout in ms (0 = default 30000)
 * @param on_chunk           called on every incoming data chunk
 * @param user_data          passed to on_chunk
 * @param out_status         receives HTTP status code
 * @return 0 on success; -110 on timeout; -502 on transport error.
 * @note When the upstream answers 4xx/5xx before any SSE data, the error
 *       body is accumulated and returned via @p out_err_body (malloc'd,
 *       caller frees via free; NULL when no error or on success paths). */
int upstream_stream_call(const char*       url,
                         const char*       upstream_key,
                         const char*       extra_headers_kv[][2],
                         int               n_extra_headers,
                         const char*       body_json,
                         size_t            body_len,
                         long              silence_timeout_ms,
                         upstream_chunk_fn on_chunk,
                         void*             user_data,
                         int*              out_status,
                         char**            out_err_body,
                         size_t*           out_err_len);

#endif /* AIGATE_UPSTREAM_CLIENT_H */

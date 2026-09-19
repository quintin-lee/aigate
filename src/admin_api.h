/** @file admin_api.h
 *  @brief /admin/v1 management plane (spec §4.2): key + model CRUD,
 *  usage queries. All endpoints authenticate a Bearer admin token
 *  (SHA-256, constant-time compare against the config hash).
 */
#ifndef AIGATE_ADMIN_API_H
#define AIGATE_ADMIN_API_H

#include <stddef.h>

#include "aigate_core.h"
#include "pg_store.h"

/** @brief Admin plane state: pipeline caches (for invalidation after
 *  mutations), the store, and the SHA-256 hex of the admin token. */
typedef struct admin_ctx {
  aigate_core *ac;
  pg_store_t *ps;
  const char *admin_token_hash; /* 64 lowercase hex chars + NUL */
} admin_ctx_t;

/** @brief Dispatch one /admin/v1 request.
 * @param adm      admin state
 * @param uri      full path incl. query ("&#47;admin/v1/usage?key=1")
 * @param method   "GET"/"POST"/"PATCH"/"DELETE"
 * @param bearer   raw admin token (Authorization: Bearer value)
 * @param body     JSON request body (NULL/empty for GET/DELETE)
 * @param out_status  HTTP status of the response
 * @param out_body   malloc'd JSON response body (NUL-terminated; caller frees)
 * @param out_len    body length (bytes, no NUL)
 * @return 0 when a response body was produced; -1 on internal failure
 *  (out_status set to 500, out_body NULL). */
int admin_dispatch(admin_ctx_t *adm, const char *uri, const char *method,
                  const char *bearer, const void *body, size_t body_len,
                  int *out_status, char **out_body, size_t *out_len);

#endif /* AIGATE_ADMIN_API_H */

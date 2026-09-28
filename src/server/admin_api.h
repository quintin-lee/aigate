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

struct redis_pool; /* forward-declare for admin_lockout_set_pool */

struct health_prober;
struct event_bus;
struct response_cache;

/** @brief Admin plane state: pipeline caches (for invalidation after
 *  mutations), the store, and the SHA-256 hex of the admin token. */
typedef struct admin_ctx {
    aigate_core*           ac;                   /* 管线状态（热加载/缓存失效入口） */
    pg_store_t*            ps;                   /* 后备存储（借用） */
    const char*            admin_token_hash;     /* 64 lowercase hex chars + NUL */
    int                    allow_plaintext_keys; /* 1 when direct plaintext provider keys are accepted */
    struct health_prober*  hp;                   /* 健康探针（读快照，可为 NULL） */
    struct event_bus*      eb;                   /* 事件总线（SSE 订阅源，可为 NULL） */
    struct response_cache* rc;                   /* 响应缓存（purge 入口，可为 NULL） */
} admin_ctx_t;

/** @brief Validate an admin bearer token against the configured hash. */
int admin_auth_ok(admin_ctx_t* adm, const char* bearer);

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
int admin_dispatch(admin_ctx_t* adm,
                   const char*  uri,
                   const char*  method,
                   const char*  client_ip, /* may be NULL: lockout disabled */
                   const char*  bearer,
                   const void*  body,
                   size_t       body_len,
                   int*         out_status,
                   char**       out_body,
                   size_t*      out_len);

/** @brief Clear the failed-admin-token lockout table (tests). */
void admin_lockout_reset(void);

/** @brief Set the process-wide lockout policy: @p max_fails failed admin
 *  token attempts before lockout (valid [2..1000]) and @p window_s lockout
 *  length in seconds (valid [5..3600]). Out-of-range values leave the
 *  current policy unchanged. */
void admin_lockout_set_policy(int max_fails, int window_s);

/** @brief Inject a Redis connection pool for distributed admin IP lockout.
 *  Pass NULL to revert to in-process-only mode. */
void admin_lockout_set_pool(struct redis_pool* pool);

/** @brief Pure calculation: transform cost_row_t rows + models pricing into a JSON cost report.
 *  Exported for unit testing. */
char* cost_from_rows_paginated(const cost_row_t*  rows,
                               int                n_rows,
                               const model_rec_t* models,
                               int                n_models,
                               const group_rec_t* groups,
                               int                n_groups,
                               long               group_filter,
                               int                by_model,
                               int                truncated,
                               int                page,
                               int                limit);

char* cost_from_rows(const cost_row_t*  rows,
                     int                n_rows,
                     const model_rec_t* models,
                     int                n_models,
                     const group_rec_t* groups,
                     int                n_groups,
                     long               group_filter,
                     int                by_model,
                     int                truncated);

#endif /* AIGATE_ADMIN_API_H */

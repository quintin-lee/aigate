/** @file transport_civetweb.h
 *  @brief CivetWeb HTTP transport adapter (spec §2.2).
 *
 *  Listens on the configured host:port and routes inbound requests:
 *    - /v1/...       → aigate_handle_request (pipeline core)
 *    - /admin/v1/... → admin_dispatch
 *    - /metrics      → Prometheus text exposition with IP ACL
 */
#ifndef AIGATE_TRANSPORT_CIVETWEB_H
#define AIGATE_TRANSPORT_CIVETWEB_H

#include "aigate_core.h"
#include "pg_store.h"

typedef struct transport_civetweb transport_civetweb_t;

/**
 * @brief Start CivetWeb HTTP server and bind handlers.
 * @param ac               the pipeline core
 * @param ps               the backing store
 * @param admin_token_hash 64 hex chars SHA-256 of the admin token
 * @param listen_addr      e.g. ":8080" or "0.0.0.0:8080"
 * @param metrics_acl      comma-separated IPv4 list / CIDRs (e.g. "127.0.0.1")
 * @return opaque transport handle, or NULL on bind/init failure
 */
transport_civetweb_t* transport_civetweb_start(
    aigate_core* ac,
    pg_store_t* ps,
    const char* admin_token_hash,
    const char* listen_addr,
    const char* metrics_acl);

/**
 * @brief Stop CivetWeb HTTP server and free resources.
 * @param cw transport context
 */
void transport_civetweb_stop(transport_civetweb_t* cw);

#endif /* AIGATE_TRANSPORT_CIVETWEB_H */

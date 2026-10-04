/** @file transport_civetweb.h
 *  @brief CivetWeb HTTP transport adapter (spec §2.2).
 *
 *  Listens on the configured host:port and routes inbound requests:
 *    - /v1/...       → aigate_handle_request (pipeline core)
 *    - /admin/v1/... → admin_dispatch
 *    - /metrics      → Prometheus text exposition with IP ACL
 */

/**
 * @defgroup group_server Server layer
 * @brief Server: HTTP transport, admin API, admin pages.
 */
#ifndef AIGATE_TRANSPORT_CIVETWEB_H
#define AIGATE_TRANSPORT_CIVETWEB_H

#include "aigate_core.h"
#include "pg_store.h"

/** @brief Opaque CivetWeb transport handle. */
typedef struct transport_civetweb transport_civetweb_t;
struct mg_connection;

/**
 * @brief Start CivetWeb HTTP server and bind handlers.
 * @param ac               the pipeline core
 * @param ps               the backing store
 * @param admin_token_hash 64 hex chars SHA-256 of the admin token
 * @param listen_addr      e.g. ":8080" or "0.0.0.0:8080"
 * @param metrics_acl      comma-separated IPv4 list / CIDRs (e.g. "127.0.0.1")
 * @param max_body_bytes   max /v1 request body in bytes (larger → 413)
 * @param worker_threads   number of civetweb worker threads
 * @param request_timeout_ms HTTP request timeout in ms
 * @param trusted_proxies  comma-separated IPv4 list / CIDRs for trusted reverse proxies
 * @return opaque transport handle, or NULL on bind/init failure
 */
transport_civetweb_t* transport_civetweb_start(aigate_core* ac,
                                               pg_store_t*  ps,
                                               const char*  admin_token_hash,
                                               const char*  listen_addr,
                                               const char*  metrics_acl,
                                               long         max_body_bytes,
                                               int          worker_threads,
                                               int          request_timeout_ms,
                                               const char*  trusted_proxies,
                                               const char*  cors_allow_origin);

/**
 * @brief Start CivetWeb HTTP/HTTPS server with optional TLS certificate and key.
 * @param ssl_cert Path to SSL certificate PEM (optional, can be NULL/empty)
 * @param ssl_key  Path to SSL private key PEM (optional, can be NULL/empty)
 */
transport_civetweb_t* transport_civetweb_start_tls(aigate_core* ac,
                                                   pg_store_t*  ps,
                                                   const char*  admin_token_hash,
                                                   const char*  listen_addr,
                                                   const char*  metrics_acl,
                                                   long         max_body_bytes,
                                                   int          worker_threads,
                                                   int          request_timeout_ms,
                                                   const char*  trusted_proxies,
                                                   const char*  cors_allow_origin,
                                                   const char*  ssl_cert,
                                                   const char*  ssl_key);

/**
 * @brief Mark transport as entering/exiting draining state before graceful exit.
 * @param cw transport context
 * @param draining 1 to mark draining (causes /ready to report 503), 0 for normal
 */
void transport_civetweb_set_draining(transport_civetweb_t* cw, int draining);

/**
 * @brief Check whether the transport is in draining state.
 * @param cw transport context
 * @return 1 if draining, 0 otherwise
 */
int transport_civetweb_is_draining(const transport_civetweb_t* cw);

/**
 * @brief Extract safe client IP from peer address, evaluating X-Forwarded-For and X-Real-IP against trusted proxies.
 * @param conn            CivetWeb connection handle (for reading request headers; may be NULL)
 * @param remote_addr     Direct socket remote peer address (e.g. from mg_request_info)
 * @param trusted_proxies Comma-separated CIDR/IPv4 list of trusted reverse proxies
 * @param out_buf         Destination buffer for the resolved IP string
 * @param out_cap         Capacity of out_buf
 * @return Pointer to out_buf, containing resolved client IP
 */
const char* transport_civetweb_extract_client_ip(struct mg_connection* conn,
                                                 const char*           remote_addr,
                                                 const char*           trusted_proxies,
                                                 char*                 out_buf,
                                                 size_t                out_cap);

/**
 * @brief Dynamically update CORS allowed origin on a running transport.
 * @param cw transport context
 * @param origin new CORS allow origin (e.g. "*" or "https://chat.example.com")
 */
void transport_civetweb_update_cors(transport_civetweb_t* cw, const char* origin);

/**
 * @brief Dynamically update trusted proxies list/CIDRs on a running transport.
 * @param cw transport context
 * @param proxies new trusted proxies list (e.g. "127.0.0.1,10.0.0.0/8")
 */
void transport_civetweb_update_trusted_proxies(transport_civetweb_t* cw, const char* proxies);

/**
 * @brief Stop CivetWeb HTTP server and free resources.
 * @param cw transport context
 */
void transport_civetweb_stop(transport_civetweb_t* cw);

#endif /* AIGATE_TRANSPORT_CIVETWEB_H */

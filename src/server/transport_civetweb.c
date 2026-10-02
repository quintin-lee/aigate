/** @file transport_civetweb.c
 *  @brief CivetWeb HTTP transport adapter implementation (see transport_civetweb.h).
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "transport_civetweb.h"
#include "admin_api.h"
#include "admin_ui.h"
#include "aigate_log.h"
#include "metrics.h"
#include "event_bus.h"

#include <civetweb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief CivetWeb transport instance state. */
struct transport_civetweb {
    struct mg_context* ctx;                  /**< CivetWeb context handle */
    aigate_core*       ac;                   /**< pipeline core (borrowed) */
    pg_store_t*        ps;                   /**< backing store (borrowed) */
    char               admin_token_hash[65]; /**< SHA-256 hex of admin token + NUL */
    char               metrics_acl[256];     /**< /metrics IP allowlist text */
    admin_ctx_t        adm;                  /**< admin plane state */
    long               max_body_bytes;       /**< max /v1 request body in bytes */
};

/** @brief Per-request response state for the CivetWeb adapter. */
struct cw_response_state {
    struct mg_connection* conn;             /**< active CivetWeb connection */
    int                   status;           /**< HTTP status staged for flush */
    bool                  headers_sent;     /**< status line already flushed */
    char                  header_buf[4096]; /**< accumulated header block */
    size_t                header_len;       /**< bytes used in header_buf */
    aigate_response_ctx*  rc;               /**< pipeline response context */
};

/** @brief Map an HTTP status code to its reason phrase; unlisted codes return "Response". */
static const char*
http_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    default:
        return "Response";
    }
}

/** @brief response_ctx set_header adapter: append into header_buf while headers are unsent (silently drop on overflow), drop once sent.
 *  @return Always 0. */
static int
cw_set_header(void* impl, const char* name, const char* value)
{
    struct cw_response_state* st = impl;
    if (st->headers_sent) {
        return 0;
    }
    size_t rem = sizeof st->header_buf - st->header_len;
    int    n = snprintf(st->header_buf + st->header_len, rem, "%s: %s\r\n", name, value);
    if (n > 0 && (size_t)n < rem) {
        st->header_len += (size_t)n;
    }
    return 0;
}

/** @brief response_ctx write adapter: flush status line plus accumulated headers on first write, then mg_write chunks.
 *  @param fin Ignored by this transport (CivetWeb needs no explicit end marker).
 *  @return 0 on success; -1 when mg_write fails. */
static int
cw_write(void* impl, const void* buf, size_t len, bool fin)
{
    (void)fin;
    struct cw_response_state* st = impl;
    if (!st->headers_sent) {
        int status = (st->rc != NULL && st->rc->status != 0) ? st->rc->status : st->status;
        mg_printf(
            st->conn, "HTTP/1.1 %d %s\r\n%s\r\n", status, http_reason(status), st->header_buf);
        st->headers_sent = true;
    }
    if (len > 0 && buf != NULL) {
        int n = mg_write(st->conn, buf, len);
        if (n < 0) {
            return -1;
        }
    }
    return 0;
}

/** @brief Extract the caller credential from Authorization / x-api-key / x-goog-api-key / query string (delegates to extract_credential_from_headers).
 *  @return Credential string (borrowed, do not free); NULL when absent. */
static const char*
extract_bearer(struct mg_connection* conn)
{
    const char*                   auth = mg_get_header(conn, "Authorization");
    const char*                   x_api_key = mg_get_header(conn, "x-api-key");
    const char*                   x_goog_key = mg_get_header(conn, "x-goog-api-key");
    const struct mg_request_info* ri = mg_get_request_info(conn);
    const char*                   qs = ri != NULL ? ri->query_string : NULL;
    return extract_credential_from_headers(auth, x_api_key, x_goog_key, qs);
}

/* Content-Length based body reader. Transfer-Encoding: chunked requests
 * (content_length == 0) are treated as empty and answered 400 (P3-12):
 * no mg_read fallback, by design — a JSON body without a declared length
 * is not supported. */
static char*
read_body(struct mg_connection* conn, long long cl, size_t* out_len)
{
    *out_len = 0;
    if (cl <= 0) {
        return NULL;
    }
    char* buf = malloc((size_t)cl + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t total = 0;
    while (total < (size_t)cl) {
        int r = mg_read(conn, buf + total, (size_t)cl - total);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    *out_len = total;
    return buf;
}

/** @brief Write a JSON error response directly (with Content-Length, Connection: close).
 *  @return Always 1 (CivetWeb handled marker). */
static int
send_http_error_json(struct mg_connection* conn, int status, const char* body, size_t len)
{
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json; charset=utf-8\r\n"
              "Content-Length: %zu\r\n"
              "Connection: close\r\n\r\n",
              status,
              http_reason(status),
              len);
    if (len > 0) {
        mg_write(conn, body, len);
    }
    return 1;
}

/** @brief Inference entry: over body limit 413 -> read body -> assemble request/response contexts -> aigate_handle_request.
 *  @return Always 1; 0 when ri is missing to let CivetWeb default handling take over. */
static int
handle_v1(struct mg_connection* conn, void* cbdata)
{
    transport_civetweb_t*         cw = cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
    }

    /* Enforce body size cap */
    if (ri->content_length > cw->max_body_bytes) {
        AIGATE_LOG_WARN("request body too large (%lld bytes, cap %ld) from %s",
                        (long long)ri->content_length,
                        cw->max_body_bytes,
                        ri->remote_addr);
        const char* err413 = "{\"error\":{\"message\":\"request body too "
                             "large\",\"type\":\"payload_too_large\",\"code\":413}}";
        send_http_error_json(conn, 413, err413, (size_t)strlen(err413));
        return 1;
    }
    size_t body_len = 0;
    char*  body = read_body(conn, ri->content_length, &body_len);

    struct cw_response_state resp_state;
    memset(&resp_state, 0, sizeof resp_state);
    resp_state.conn = conn;
    resp_state.status = 200;

    aigate_response_ctx rc;
    memset(&rc, 0, sizeof rc);
    rc.status = 200;
    rc.headers_sent = false;
    rc.impl = &resp_state;
    rc.set_header = cw_set_header;
    rc.write = cw_write;
    resp_state.rc = &rc;

    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = ri->request_method;
    rq.path = ri->local_uri;
    rq.bearer = extract_bearer(conn);
    rq.client_ip = ri->remote_addr;
    rq.body = body;
    rq.body_len = body_len;
    const char* cc = mg_get_header(conn, "Cache-Control");
    if (cc == NULL) {
        cc = mg_get_header(conn, "x-skip-cache");
    }
    rq.cache_control = cc;
    rq.target_provider = mg_get_header(conn, "X-Aigate-Target-Provider");
    rq.traceparent = mg_get_header(conn, "traceparent");
    rq.compress_control = mg_get_header(conn, "X-Aigate-Compress");
    rq.prompt_cache_control = mg_get_header(conn, "X-Aigate-Prompt-Cache");

    aigate_handle_request(cw->ac, &rq, &rc);

    free(body);
    return 1;
}

/** @brief /admin/v1 entry: /admin/v1/events is the authed SSE event subscription stream; the rest dispatch through admin_dispatch and write back JSON.
 *  @return Always 1; 0 when ri is missing. */
static int
handle_admin(struct mg_connection* conn, void* cbdata)
{
    transport_civetweb_t*         cw = cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
    }

    if (strcmp(ri->local_uri, "/admin/v1/events") == 0) {
        if (strcmp(ri->request_method, "GET") != 0 && strcmp(ri->request_method, "HEAD") != 0) {
            mg_send_http_error(conn, 405, "Method Not Allowed");
            return 1;
        }
        const char* bearer = extract_bearer(conn);
        if (!admin_auth_ok(&cw->adm, bearer)) {
            mg_send_http_error(conn, 401, "Unauthorized: invalid admin token");
            return 1;
        }
        if (cw->adm.eb == NULL) {
            mg_send_http_error(conn, 503, "Event bus unavailable");
            return 1;
        }
        int sub_id = event_bus_subscribe(cw->adm.eb);
        if (sub_id <= 0) {
            mg_send_http_error(conn, 503, "Too many event stream subscribers");
            return 1;
        }

        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/event-stream\r\n"
                  "Cache-Control: no-cache, no-transform\r\n"
                  "Connection: keep-alive\r\n"
                  "Transfer-Encoding: chunked\r\n"
                  "Access-Control-Allow-Origin: *\r\n\r\n");

        if (strcmp(ri->request_method, "HEAD") == 0) {
            event_bus_unsubscribe(cw->adm.eb, sub_id);
            return 1;
        }

        char init_ping[128];
        snprintf(
            init_ping, sizeof init_ping, "event: ping\ndata: {\"ts\":%ld}\n\n", (long)time(NULL));
        if (mg_send_chunk(conn, init_ping, (unsigned int)strlen(init_ping)) < 0) {
            event_bus_unsubscribe(cw->adm.eb, sub_id);
            return 1;
        }

        while (1) {
            event_item_t item;
            int          prc = event_bus_pop(cw->adm.eb, sub_id, &item, 15000);
            if (prc < 0) {
                break;
            }
            if (prc == 0) {
                char ping_buf[64];
                snprintf(ping_buf, sizeof ping_buf, ": ping\n\n");
                if (mg_send_chunk(conn, ping_buf, (unsigned int)strlen(ping_buf)) < 0) {
                    break;
                }
                continue;
            }

            char sse_msg[EVENT_MAX_PAYLOAD + 128];
            int  n = snprintf(
                sse_msg, sizeof sse_msg, "event: %s\ndata: %s\n\n", item.event_name, item.payload);
            if (n > 0) {
                if (mg_send_chunk(conn, sse_msg, (unsigned int)n) < 0) {
                    break;
                }
            }
        }

        mg_send_chunk(conn, "", 0);
        event_bus_unsubscribe(cw->adm.eb, sub_id);
        return 1;
    }

    /* Enforce body size cap */
    if (ri->content_length > cw->max_body_bytes) {
        AIGATE_LOG_WARN("request body too large (%lld bytes, cap %ld) from %s",
                        (long long)ri->content_length,
                        cw->max_body_bytes,
                        ri->remote_addr);
        const char* err413 = "{\"error\":{\"message\":\"request body too "
                             "large\",\"type\":\"payload_too_large\",\"code\":413}}";
        send_http_error_json(conn, 413, err413, (size_t)strlen(err413));
        return 1;
    }
    size_t body_len = 0;
    char*  body = read_body(conn, ri->content_length, &body_len);

    char full_uri[1024];
    if (ri->query_string != NULL && ri->query_string[0] != '\0') {
        snprintf(full_uri, sizeof full_uri, "%s?%s", ri->local_uri, ri->query_string);
    } else {
        snprintf(full_uri, sizeof full_uri, "%s", ri->local_uri);
    }

    const char* bearer = extract_bearer(conn);
    int         out_status = 500;
    char*       out_body = NULL;
    size_t      out_len = 0;

    admin_dispatch(&cw->adm,
                   full_uri,
                   ri->request_method,
                   ri->remote_addr,
                   bearer,
                   body,
                   body_len,
                   &out_status,
                   &out_body,
                   &out_len);

    free(body);

    if (out_body != NULL) {
        mg_printf(conn,
                  "HTTP/1.1 %d %s\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zu\r\n\r\n",
                  out_status,
                  http_reason(out_status),
                  out_len);
        mg_write(conn, out_body, out_len);
        free(out_body);
    } else {
        mg_send_http_error(conn, out_status, "%s", http_reason(out_status));
    }
    return 1;
}

/** @brief /metrics entry: 403 when the IP ACL fails, otherwise render Prometheus text (version 0.0.4) and write back.
 *  @return Always 1; 0 when ri is missing. */
static int
handle_metrics(struct mg_connection* conn, void* cbdata)
{
    transport_civetweb_t*         cw = cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
    }

    if (!metrics_acl_allows(ri->remote_addr, cw->metrics_acl)) {
        mg_send_http_error(conn, 403, "Forbidden: IP not allowed for /metrics");
        return 1;
    }

    char* mbuf = malloc(65536);
    if (mbuf == NULL) {
        mg_send_http_error(conn, 500, "Internal error: OOM");
        return 1;
    }
    if (metrics_render(cw->ac->um, mbuf, 65536) != 0) {
        free(mbuf);
        mg_send_http_error(conn, 500, "Internal error: metrics buffer overflow");
        return 1;
    }

    size_t mlen = strlen(mbuf);
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/plain; version=0.0.4\r\n"
              "Content-Length: %zu\r\n\r\n",
              mlen);
    mg_write(conn, mbuf, mlen);
    free(mbuf);
    return 1;
}

/** @brief /admin static page entry: only GET/HEAD go to admin_ui_serve, otherwise 405.
 *  @return Always 1; 0 when ri is missing. */
static int
handle_admin_ui(struct mg_connection* conn, void* cbdata)
{
    (void)cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
    }
    if (strcmp(ri->request_method, "GET") != 0 && strcmp(ri->request_method, "HEAD") != 0) {
        mg_send_http_error(conn, 405, "Method Not Allowed");
        return 1;
    }
    return admin_ui_serve(conn);
}

/** @brief Root path entry: `/` 302-redirects to /admin, the rest return 0 to subsequent handlers.
 *  @return 1 redirect handled; 0 unhandled. */
static int
handle_root(struct mg_connection* conn, void* cbdata)
{
    (void)cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
    }
    if (strcmp(ri->local_uri, "/") == 0) {
        mg_printf(conn,
                  "HTTP/1.1 302 Found\r\n"
                  "Location: /admin\r\n"
                  "Content-Length: 0\r\n\r\n");
        return 1;
    }
    return 0;
}

transport_civetweb_t*
transport_civetweb_start(aigate_core* ac,
                         pg_store_t*  ps,
                         const char*  admin_token_hash,
                         const char*  listen_addr,
                         const char*  metrics_acl,
                         long         max_body_bytes)
{
    transport_civetweb_t* cw = calloc(1, sizeof *cw);
    if (cw == NULL) {
        return NULL;
    }
    cw->ac = ac;
    cw->ps = ps;
    cw->max_body_bytes = max_body_bytes;
    if (admin_token_hash != NULL) {
        snprintf(cw->admin_token_hash, sizeof cw->admin_token_hash, "%s", admin_token_hash);
    }
    if (metrics_acl != NULL) {
        snprintf(cw->metrics_acl, sizeof cw->metrics_acl, "%s", metrics_acl);
    }

    cw->adm.ac = ac;
    cw->adm.ps = ps;
    cw->adm.admin_token_hash = cw->admin_token_hash;
    const char* apk = getenv("AIGATE_ALLOW_PLAINTEXT_KEYS");
    cw->adm.allow_plaintext_keys = (apk != NULL && atoi(apk) == 1);
    if (ac != NULL) {
        cw->adm.hp = ac->hp;
        cw->adm.eb = ac->eb;
        cw->adm.rc = ac->rc;
    }

    /* Admin lockout policy: out-of-range values keep the defaults. */
    const char* lf = getenv("AIGATE_LOCKOUT_MAX_FAILS");
    const char* lw = getenv("AIGATE_LOCKOUT_WINDOW_S");
    admin_lockout_set_policy(lf != NULL ? atoi(lf) : 10, lw != NULL ? atoi(lw) : 300);

    const char* port_spec = listen_addr;
    if (port_spec == NULL || port_spec[0] == '\0') {
        port_spec = "8080";
    } else if (port_spec[0] == ':' && port_spec[1] != '\0') {
        port_spec = port_spec + 1;
    }

    const char* options[] = {
        "listening_ports",
        port_spec,
        "num_threads",
        "16",
        NULL,
    };

    cw->ctx = mg_start(NULL, NULL, options);
    if (cw->ctx == NULL) {
        AIGATE_LOG_ERROR("transport_civetweb: failed to bind on %s", port_spec);
        free(cw);
        return NULL;
    }

    mg_set_request_handler(cw->ctx, "/v1/", handle_v1, cw);
    mg_set_request_handler(cw->ctx, "/v1beta/", handle_v1, cw);
    mg_set_request_handler(cw->ctx, "/admin/v1", handle_admin, cw);
    mg_set_request_handler(cw->ctx, "/admin", handle_admin_ui, cw);
    mg_set_request_handler(cw->ctx, "/metrics", handle_metrics, cw);
    mg_set_request_handler(cw->ctx, "/$", handle_root, cw);

    AIGATE_LOG_INFO("transport_civetweb: listening on %s", port_spec);
    return cw;
}

void
transport_civetweb_stop(transport_civetweb_t* cw)
{
    if (cw == NULL) {
        return;
    }
    if (cw->ctx != NULL) {
        mg_stop(cw->ctx);
        cw->ctx = NULL;
    }
    free(cw);
}

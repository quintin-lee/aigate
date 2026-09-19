/** @file transport_civetweb.c
 *  @brief CivetWeb HTTP transport adapter implementation (see transport_civetweb.h).
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "transport_civetweb.h"
#include "admin_api.h"
#include "aigate_log.h"
#include "metrics.h"

#include <civetweb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct transport_civetweb {
    struct mg_context* ctx;
    aigate_core*       ac;
    pg_store_t*        ps;
    char               admin_token_hash[65];
    char               metrics_acl[256];
    admin_ctx_t        adm;
};

struct cw_response_state {
    struct mg_connection* conn;
    int                   status;
    bool                  headers_sent;
    char                  header_buf[4096];
    size_t                header_len;
};

static const char*
http_reason(int status)
{
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Response";
    }
}

static int
cw_set_header(void* impl, const char* name, const char* value)
{
    struct cw_response_state* st = impl;
    if (st->headers_sent) {
        return 0;
    }
    size_t rem = sizeof st->header_buf - st->header_len;
    int n = snprintf(st->header_buf + st->header_len, rem, "%s: %s\r\n", name, value);
    if (n > 0 && (size_t)n < rem) {
        st->header_len += (size_t)n;
    }
    return 0;
}

static int
cw_write(void* impl, const void* buf, size_t len, bool fin)
{
    (void)fin;
    struct cw_response_state* st = impl;
    if (!st->headers_sent) {
        mg_printf(st->conn,
                  "HTTP/1.1 %d %s\r\n%s\r\n",
                  st->status,
                  http_reason(st->status),
                  st->header_buf);
        st->headers_sent = true;
    }
    if (len > 0 && buf != NULL) {
        mg_write(st->conn, buf, len);
    }
    return 0;
}

static const char*
extract_bearer(struct mg_connection* conn)
{
    const char* auth = mg_get_header(conn, "Authorization");
    if (auth == NULL) {
        return "";
    }
    if (strncmp(auth, "Bearer ", 7) == 0) {
        return auth + 7;
    }
    return auth;
}

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

static int
handle_v1(struct mg_connection* conn, void* cbdata)
{
    transport_civetweb_t*         cw = cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
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

    aigate_request_ctx rq;
    memset(&rq, 0, sizeof rq);
    rq.method = ri->request_method;
    rq.path = ri->local_uri;
    rq.bearer = extract_bearer(conn);
    rq.client_ip = ri->remote_addr;
    rq.body = body;
    rq.body_len = body_len;

    aigate_handle_request(cw->ac, &rq, &rc);

    free(body);
    return 1;
}

static int
handle_admin(struct mg_connection* conn, void* cbdata)
{
    transport_civetweb_t*         cw = cbdata;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    if (ri == NULL) {
        return 0;
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

transport_civetweb_t*
transport_civetweb_start(aigate_core* ac,
                         pg_store_t*  ps,
                         const char*  admin_token_hash,
                         const char*  listen_addr,
                         const char*  metrics_acl)
{
    transport_civetweb_t* cw = calloc(1, sizeof *cw);
    if (cw == NULL) {
        return NULL;
    }
    cw->ac = ac;
    cw->ps = ps;
    if (admin_token_hash != NULL) {
        snprintf(cw->admin_token_hash, sizeof cw->admin_token_hash, "%s", admin_token_hash);
    }
    if (metrics_acl != NULL) {
        snprintf(cw->metrics_acl, sizeof cw->metrics_acl, "%s", metrics_acl);
    }

    cw->adm.ac = ac;
    cw->adm.ps = ps;
    cw->adm.admin_token_hash = cw->admin_token_hash;

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
    mg_set_request_handler(cw->ctx, "/admin/v1", handle_admin, cw);
    mg_set_request_handler(cw->ctx, "/metrics", handle_metrics, cw);

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

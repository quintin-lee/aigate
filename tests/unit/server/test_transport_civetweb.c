/** @file test_transport_civetweb.c
 *  @brief Unit tests for CivetWeb transport: /healthz, /ready, draining lifecycle, and client IP resolution.
 */
#include "run_tests.h"
#include "transport_civetweb.h"
#include "aigate_core.h"
#include "pg_store.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Helper to capture response body from curl */
struct curl_resp_buf {
    char   buf[1024];
    size_t len;
};

static size_t
write_resp_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t                realsize = size * nmemb;
    struct curl_resp_buf* mem = (struct curl_resp_buf*)userp;
    if (mem->len + realsize < sizeof(mem->buf) - 1) {
        memcpy(mem->buf + mem->len, contents, realsize);
        mem->len += realsize;
        mem->buf[mem->len] = '\0';
    }
    return realsize;
}

static long
do_http_get(const char* url, const char* custom_header, char* out_body, size_t out_cap)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    struct curl_resp_buf resp;
    resp.buf[0] = '\0';
    resp.len = 0;

    struct curl_slist* headers = NULL;
    if (custom_header != NULL) {
        headers = curl_slist_append(headers, custom_header);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_resp_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);

    CURLcode res = curl_easy_perform(curl);
    long     http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (out_body && out_cap > 0) {
            snprintf(out_body, out_cap, "%s", resp.buf);
        }
    } else {
        http_code = -1;
    }

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return http_code;
}

struct curl_header_buf {
    char   buf[2048];
    size_t len;
};

static size_t
write_header_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t                  realsize = size * nmemb;
    struct curl_header_buf* mem = (struct curl_header_buf*)userp;
    if (mem->len + realsize < sizeof(mem->buf) - 1) {
        memcpy(mem->buf + mem->len, contents, realsize);
        mem->len += realsize;
        mem->buf[mem->len] = '\0';
    }
    return realsize;
}

static long
do_http_options(const char* url, const char* origin, char* out_headers, size_t out_cap)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    struct curl_header_buf hbuf;
    hbuf.buf[0] = '\0';
    hbuf.len = 0;

    struct curl_slist* headers = NULL;
    if (origin != NULL) {
        char origin_hdr[256];
        snprintf(origin_hdr, sizeof(origin_hdr), "Origin: %s", origin);
        headers = curl_slist_append(headers, origin_hdr);
        headers = curl_slist_append(headers, "Access-Control-Request-Method: POST");
        headers = curl_slist_append(headers,
                                    "Access-Control-Request-Headers: Authorization, Content-Type");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "OPTIONS");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)&hbuf);
    struct curl_resp_buf opt_resp;
    opt_resp.buf[0] = '\0';
    opt_resp.len = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_resp_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&opt_resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);

    CURLcode res = curl_easy_perform(curl);
    long     http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (out_headers && out_cap > 0) {
            snprintf(out_headers, out_cap, "%s", hbuf.buf);
        }
    } else {
        http_code = -1;
    }

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return http_code;
}

static long
do_http_get_with_headers(const char* url, char* out_headers, size_t out_cap)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    struct curl_header_buf hbuf;
    hbuf.buf[0] = '\0';
    hbuf.len = 0;

    struct curl_resp_buf get_resp;
    get_resp.buf[0] = '\0';
    get_resp.len = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_resp_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&get_resp);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)&hbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);

    CURLcode res = curl_easy_perform(curl);
    long     http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (out_headers && out_cap > 0) {
            snprintf(out_headers, out_cap, "%s", hbuf.buf);
        }
    } else {
        http_code = -1;
    }

    curl_easy_cleanup(curl);
    return http_code;
}

static int
stub_zero(void* ctx, ...)
{
    (void)ctx;
    return 0;
}
static int
stub_one(void* ctx, ...)
{
    (void)ctx;
    return 1;
}

TEST_CASE(test_transport_healthz_and_ready)
{
    /* Initialize mock store & core pipeline */
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.list_guardrails_rules = (int (*)(void*, guardrail_rule_t*, int, int*))stub_zero;
    ops.list_shadow_rules = (int (*)(void*, shadow_rule_t*, int, int*))stub_zero;
    ops.list_compressor_rules = (int (*)(void*, compressor_rule_t*, int, int*))stub_zero;
    ops.list_cache_optimizer_rules = (int (*)(void*, cache_optimizer_rule_t*, int, int*))stub_zero;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))stub_zero;
    ops.get_key_by_hash = (int (*)(void*, const char*, key_rec_t*))stub_one;

    pg_store_t* ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake pg store created");

    aigate_core core;
    int         rc = aigate_core_init(&core, ps, NULL, 60000, 5);
    TEST_ASSERT(rc == 0, "core init success");

    /* Start transport on test port 18095 */
    transport_civetweb_t* cw = transport_civetweb_start(&core,
                                                        ps,
                                                        "dummy_hash",
                                                        "127.0.0.1:18095",
                                                        "127.0.0.1",
                                                        1048576,
                                                        16,
                                                        5000,
                                                        "127.0.0.1",
                                                        "*");
    TEST_ASSERT(cw != NULL, "transport started on port 18095");

    char body[512];

    /* 1. Test /healthz (Liveness) */
    long status = do_http_get("http://127.0.0.1:18095/healthz", NULL, body, sizeof(body));
    TEST_ASSERT(status == 200, "/healthz should return 200 OK");
    TEST_ASSERT(strstr(body, "\"status\":\"ok\"") != NULL, "/healthz body contains status ok");

    /* 2. Test /live alias */
    status = do_http_get("http://127.0.0.1:18095/live", NULL, body, sizeof(body));
    TEST_ASSERT(status == 200, "/live should return 200 OK");

    /* 3. Test /ready (Readiness - Normal) */
    status = do_http_get("http://127.0.0.1:18095/ready", NULL, body, sizeof(body));
    TEST_ASSERT(status == 200, "/ready should return 200 OK in normal state");
    TEST_ASSERT(strstr(body, "\"status\":\"ready\"") != NULL, "/ready body contains status ready");

    /* 4. Transition to Draining phase */
    TEST_ASSERT(transport_civetweb_is_draining(cw) == 0, "not draining initially");
    transport_civetweb_set_draining(cw, 1);
    TEST_ASSERT(transport_civetweb_is_draining(cw) == 1, "draining marked");

    /* /ready should now report 503 Service Unavailable */
    status = do_http_get("http://127.0.0.1:18095/ready", NULL, body, sizeof(body));
    TEST_ASSERT(status == 503, "/ready must return 503 when draining");
    TEST_ASSERT(strstr(body, "\"status\":\"draining\"") != NULL, "/ready body reports draining");

    /* /healthz must STILL report 200 OK during draining (pod is alive, just not accepting new traffic) */
    status = do_http_get("http://127.0.0.1:18095/healthz", NULL, body, sizeof(body));
    TEST_ASSERT(status == 200, "/healthz must stay 200 during draining");

    /* Clean shutdown */
    transport_civetweb_stop(cw);
    aigate_core_shutdown(&core);
    pg_store_close(ps);
}

TEST_CASE(test_transport_client_ip_resolution)
{
    char out[64];

    /* 1. NULL peer IP fallback */
    transport_civetweb_extract_client_ip(NULL, NULL, "127.0.0.1", out, sizeof(out));
    TEST_ASSERT(strcmp(out, "127.0.0.1") == 0, "null peer defaults to 127.0.0.1");

    /* 2. Direct untrusted client IP (not in trusted_proxies) */
    transport_civetweb_extract_client_ip(
        NULL, "203.0.113.50", "127.0.0.1,10.0.0.0/8", out, sizeof(out));
    TEST_ASSERT(strcmp(out, "203.0.113.50") == 0, "untrusted peer preserved directly");

    /* 3. Trusted proxy without headers returns proxy IP */
    transport_civetweb_extract_client_ip(NULL, "127.0.0.1", "127.0.0.1", out, sizeof(out));
    TEST_ASSERT(strcmp(out, "127.0.0.1") == 0, "trusted peer without conn headers returns peer IP");

    /* 4. Live server test with real X-Forwarded-For and X-Real-IP */
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.list_guardrails_rules = (int (*)(void*, guardrail_rule_t*, int, int*))stub_zero;
    ops.list_shadow_rules = (int (*)(void*, shadow_rule_t*, int, int*))stub_zero;
    ops.list_compressor_rules = (int (*)(void*, compressor_rule_t*, int, int*))stub_zero;
    ops.list_cache_optimizer_rules = (int (*)(void*, cache_optimizer_rule_t*, int, int*))stub_zero;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))stub_zero;
    ops.get_key_by_hash = (int (*)(void*, const char*, key_rec_t*))stub_one;

    pg_store_t* ps = pg_store_open("unused", &ops);
    aigate_core core;
    aigate_core_init(&core, ps, NULL, 60000, 5);

    /* Trusted proxy list includes 127.0.0.1 and 10.0.0.0/8 */
    transport_civetweb_t* cw = transport_civetweb_start(&core,
                                                        ps,
                                                        "dummy_hash",
                                                        "127.0.0.1:18096",
                                                        "127.0.0.1",
                                                        1048576,
                                                        16,
                                                        5000,
                                                        "127.0.0.1,10.0.0.0/8",
                                                        "*");
    TEST_ASSERT(cw != NULL, "transport started on port 18096");

    char body[512];

    /* Calling an endpoint with X-Forwarded-For chain: client (198.51.100.77), internal proxy (10.0.1.2) */
    long status = do_http_get("http://127.0.0.1:18096/healthz",
                              "X-Forwarded-For: 198.51.100.77, 10.0.1.2",
                              body,
                              sizeof(body));
    TEST_ASSERT(status == 200, "healthz ok with XFF");

    /* Calling with X-Real-IP */
    status = do_http_get(
        "http://127.0.0.1:18096/healthz", "X-Real-IP: 198.51.100.88", body, sizeof(body));
    TEST_ASSERT(status == 200, "healthz ok with X-Real-IP");

    transport_civetweb_stop(cw);
    aigate_core_shutdown(&core);
    pg_store_close(ps);
}

TEST_CASE(test_transport_cors_and_security_headers)
{
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.list_guardrails_rules = (int (*)(void*, guardrail_rule_t*, int, int*))stub_zero;
    ops.list_shadow_rules = (int (*)(void*, shadow_rule_t*, int, int*))stub_zero;
    ops.list_compressor_rules = (int (*)(void*, compressor_rule_t*, int, int*))stub_zero;
    ops.list_cache_optimizer_rules = (int (*)(void*, cache_optimizer_rule_t*, int, int*))stub_zero;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))stub_zero;
    ops.get_key_by_hash = (int (*)(void*, const char*, key_rec_t*))stub_one;

    pg_store_t* ps = pg_store_open("unused", &ops);
    aigate_core core;
    aigate_core_init(&core, ps, NULL, 60000, 5);

    /* Start transport with custom CORS origin */
    transport_civetweb_t* cw = transport_civetweb_start(&core,
                                                        ps,
                                                        "dummy_hash",
                                                        "127.0.0.1:18097",
                                                        "127.0.0.1",
                                                        1048576,
                                                        16,
                                                        5000,
                                                        "127.0.0.1",
                                                        "https://chat.example.com");
    TEST_ASSERT(cw != NULL, "transport started on port 18097");

    char headers[2048];

    /* 1. Test /v1/chat/completions OPTIONS preflight */
    long status = do_http_options("http://127.0.0.1:18097/v1/chat/completions",
                                  "https://chat.example.com",
                                  headers,
                                  sizeof(headers));
    TEST_ASSERT(status == 204, "OPTIONS /v1/chat/completions returns 204 No Content");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Origin: https://chat.example.com") != NULL,
                "CORS origin header present in v1 OPTIONS");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Methods:") != NULL,
                "CORS methods header present");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Headers:") != NULL,
                "CORS headers header present");
    TEST_ASSERT(strstr(headers, "X-Content-Type-Options: nosniff") != NULL,
                "nosniff header present in OPTIONS");
    TEST_ASSERT(strstr(headers, "X-Frame-Options: DENY") != NULL, "DENY header present in OPTIONS");

    /* 2. Test /admin/v1/keys OPTIONS preflight */
    status = do_http_options("http://127.0.0.1:18097/admin/v1/keys",
                             "https://chat.example.com",
                             headers,
                             sizeof(headers));
    TEST_ASSERT(status == 204, "OPTIONS /admin/v1/keys returns 204 No Content");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Origin: https://chat.example.com") != NULL,
                "CORS origin header present in admin OPTIONS");

    /* 3. Test security headers in normal GET /healthz response */
    status = do_http_get_with_headers("http://127.0.0.1:18097/healthz", headers, sizeof(headers));
    TEST_ASSERT(status == 200, "GET /healthz returns 200");
    TEST_ASSERT(strstr(headers, "X-Content-Type-Options: nosniff") != NULL,
                "nosniff in GET response");
    TEST_ASSERT(strstr(headers, "X-Frame-Options: DENY") != NULL,
                "X-Frame-Options in GET response");
    TEST_ASSERT(strstr(headers, "Referrer-Policy: strict-origin-when-cross-origin") != NULL,
                "Referrer-Policy in GET response");

    transport_civetweb_stop(cw);
    aigate_core_shutdown(&core);
    pg_store_close(ps);
}

TEST_CASE(test_transport_dynamic_config_reload)
{
    pg_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.list_guardrails_rules = (int (*)(void*, guardrail_rule_t*, int, int*))stub_zero;
    ops.list_shadow_rules = (int (*)(void*, shadow_rule_t*, int, int*))stub_zero;
    ops.list_compressor_rules = (int (*)(void*, compressor_rule_t*, int, int*))stub_zero;
    ops.list_cache_optimizer_rules = (int (*)(void*, cache_optimizer_rule_t*, int, int*))stub_zero;
    ops.list_models = (int (*)(void*, model_rec_t*, int, int*))stub_zero;
    ops.get_key_by_hash = (int (*)(void*, const char*, key_rec_t*))stub_one;

    pg_store_t* ps = pg_store_open("unused", &ops);
    aigate_core core;
    aigate_core_init(&core, ps, NULL, 60000, 5);

    /* Start transport with initial origin https://old.example.com on port 18098 */
    transport_civetweb_t* cw = transport_civetweb_start(&core,
                                                        ps,
                                                        "dummy_hash",
                                                        "127.0.0.1:18098",
                                                        "127.0.0.1",
                                                        1048576,
                                                        16,
                                                        5000,
                                                        "127.0.0.1",
                                                        "https://old.example.com");
    TEST_ASSERT(cw != NULL, "transport started on port 18098");

    char headers[2048];

    /* 1. Preflight with initial CORS configuration */
    long status = do_http_options("http://127.0.0.1:18098/v1/chat/completions",
                                  "https://old.example.com",
                                  headers,
                                  sizeof(headers));
    TEST_ASSERT(status == 204, "OPTIONS returns 204");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Origin: https://old.example.com") != NULL,
                "initial CORS origin returned");

    /* 2. Dynamically reload CORS origin to https://new.example.com (simulating SIGHUP) */
    transport_civetweb_update_cors(cw, "https://new.example.com");

    status = do_http_options("http://127.0.0.1:18098/v1/chat/completions",
                             "https://new.example.com",
                             headers,
                             sizeof(headers));
    TEST_ASSERT(status == 204, "OPTIONS returns 204 after update");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Origin: https://new.example.com") != NULL,
                "updated CORS origin returned without restart");

    /* 3. Dynamically update trusted proxies */
    transport_civetweb_update_trusted_proxies(cw, "10.0.0.0/8,172.16.0.0/12");

    /* 4. Reset CORS origin with NULL to default "*" */
    transport_civetweb_update_cors(cw, NULL);
    status = do_http_options("http://127.0.0.1:18098/v1/chat/completions",
                             "https://any.example.com",
                             headers,
                             sizeof(headers));
    TEST_ASSERT(status == 204, "OPTIONS returns 204 after reset");
    TEST_ASSERT(strstr(headers, "Access-Control-Allow-Origin: *") != NULL,
                "default wildcard CORS origin returned");

    transport_civetweb_stop(cw);
    aigate_core_shutdown(&core);
    pg_store_close(ps);
}

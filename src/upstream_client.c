#include "upstream_client.h"
#include "aigate_log.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void
curl_init_once(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static pthread_key_t  g_curl_tkey;
static pthread_once_t g_curl_tkey_once = PTHREAD_ONCE_INIT;
static void
curl_tkey_init(void)
{
    pthread_key_create(&g_curl_tkey, NULL);
}

/* Per-thread CURL handle: reused across calls to amortize init.
 * Thread-exit leaks one handle per civetweb worker; accepted. */
static CURL*
thread_curl(void)
{
    pthread_once(&g_curl_tkey_once, curl_tkey_init);
    CURL* c = (CURL*)pthread_getspecific(g_curl_tkey);
    if (c == NULL) {
        c = curl_easy_init();
        if (c != NULL) {
            pthread_setspecific(g_curl_tkey, c);
        }
    }
    return c;
}

struct resp_buf {
    char*  data;
    size_t len;
    size_t cap;
};

/* libcurl write callback: data first, userdata last. */
static size_t
append_body(char* buf, size_t size, size_t nmemb, void* ud)
{
    struct resp_buf* rb = ud;
    size_t           total = size * nmemb;
    if (rb->len + total + 1 > rb->cap) {
        size_t ncap = rb->cap ? rb->cap : 1024;
        while (rb->len + total + 1 > ncap) {
            ncap *= 2;
        }
        char* nd = realloc(rb->data, ncap);
        if (nd == NULL) {
            return 0; /* abort transfer */
        }
        rb->data = nd;
        rb->cap = ncap;
    }
    memcpy(rb->data + rb->len, buf, total);
    rb->len += total;
    rb->data[rb->len] = '\0';
    return total;
}

int
upstream_call_ext(const char* url,
                  const char* upstream_key,
                  const char* extra_headers_kv[][2],
                  int         n_extra_headers,
                  const char* body_json,
                  size_t      body_len,
                  long        timeout_ms,
                  int*        out_status,
                  char**      out_body,
                  size_t*     out_body_len)
{
    struct resp_buf    rb = {0};
    struct curl_slist* hdrs = NULL;
    int                rc = -502;
    long               http_code = 0;

    pthread_once(&g_curl_once, curl_init_once);
    CURL* c = thread_curl();
    if (c == NULL) {
        goto done;
    }
    curl_easy_reset(c);

    int has_custom_auth = 0;
    if (extra_headers_kv != NULL && n_extra_headers > 0) {
        for (int i = 0; i < n_extra_headers; i++) {
            if (extra_headers_kv[i][0] != NULL && extra_headers_kv[i][1] != NULL) {
                if (strcasecmp(extra_headers_kv[i][0], "x-api-key") == 0 ||
                    strcasecmp(extra_headers_kv[i][0], "x-goog-api-key") == 0 ||
                    strcasecmp(extra_headers_kv[i][0], "Authorization") == 0) {
                    has_custom_auth = 1;
                }
                char hdr[1024];
                snprintf(hdr, sizeof hdr, "%s: %s", extra_headers_kv[i][0], extra_headers_kv[i][1]);
                hdrs = curl_slist_append(hdrs, hdr);
            }
        }
    }

    if (!has_custom_auth && upstream_key != NULL && upstream_key[0] != '\0') {
        char auth[1080];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", upstream_key);
        hdrs = curl_slist_append(hdrs, auth);
    }
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_json);
    /* body_len == 0 → use strlen of the NUL-terminated body_json */
    curl_easy_setopt(
        c, CURLOPT_POSTFIELDSIZE, body_len > 0 ? (long)body_len : (long)strlen(body_json));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 60000L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode cret = curl_easy_perform(c);
    if (cret == CURLE_OK) {
        if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
            *out_status = (int)http_code;
            *out_body = rb.data ? rb.data : (char*)"";
            *out_body_len = rb.len;
            rc = 0;
        }
    } else if (cret == CURLE_OPERATION_TIMEDOUT) {
        rc = -110;
    } else {
        AIGATE_LOG_WARN("upstream transport error: %s", curl_easy_strerror(cret));
        rc = -502;
    }

done:
    curl_slist_free_all(hdrs);
    if (rc != 0 && out_body != NULL) {
        *out_body = NULL;
        *out_body_len = 0;
        free(rb.data);
    }
    return rc;
}

int
upstream_call(const char* url,
              const char* upstream_key,
              const char* body_json,
              size_t      body_len,
              long        timeout_ms,
              int*        out_status,
              char**      out_body,
              size_t*     out_body_len)
{
    return upstream_call_ext(url,
                             upstream_key,
                             NULL,
                             0,
                             body_json,
                             body_len,
                             timeout_ms,
                             out_status,
                             out_body,
                             out_body_len);
}

static uint64_t
mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct stream_ctx {
    upstream_chunk_fn on_chunk;
    void*             user_data;
    uint64_t          last_chunk_mono_ns;
    uint64_t          silence_timeout_ns;
    int               aborted;
    CURL*             curl;
    int               status;
};

static size_t
stream_write_cb(char* buf, size_t size, size_t nmemb, void* ud)
{
    struct stream_ctx* sc = ud;
    size_t             total = size * nmemb;
    sc->last_chunk_mono_ns = mono_ns();

    if (sc->status == 0 && sc->curl != NULL) {
        long code = 0;
        if (curl_easy_getinfo(sc->curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) {
            sc->status = (int)code;
        }
    }

    if (sc->status >= 400) {
        return total;
    }

    if (sc->on_chunk != NULL && total > 0) {
        if (sc->on_chunk(sc->user_data, buf, total) != 0) {
            sc->aborted = 1;
            return 0; /* abort transfer */
        }
    }
    return total;
}

static int
stream_xferinfo_cb(
    void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    struct stream_ctx* sc = clientp;
    if (sc->silence_timeout_ns > 0) {
        uint64_t now = mono_ns();
        if (now - sc->last_chunk_mono_ns > sc->silence_timeout_ns) {
            sc->aborted = 2; /* silence timeout */
            return 1;        /* abort transfer */
        }
    }
    return 0;
}

int
upstream_stream_call(const char*       url,
                     const char*       upstream_key,
                     const char*       extra_headers_kv[][2],
                     int               n_extra_headers,
                     const char*       body_json,
                     size_t            body_len,
                     long              silence_timeout_ms,
                     upstream_chunk_fn on_chunk,
                     void*             user_data,
                     int*              out_status)
{
    struct stream_ctx  sc = {0};
    struct curl_slist* hdrs = NULL;
    int                rc = -502;
    long               http_code = 0;

    sc.on_chunk = on_chunk;
    sc.user_data = user_data;
    sc.last_chunk_mono_ns = mono_ns();
    sc.silence_timeout_ns = (silence_timeout_ms > 0 ? silence_timeout_ms : 30000L) * 1000000ull;

    pthread_once(&g_curl_once, curl_init_once);
    CURL* c = thread_curl();
    if (c == NULL) {
        goto done;
    }
    curl_easy_reset(c);
    sc.curl = c;

    int has_custom_auth = 0;
    if (extra_headers_kv != NULL && n_extra_headers > 0) {
        for (int i = 0; i < n_extra_headers; i++) {
            if (extra_headers_kv[i][0] != NULL && extra_headers_kv[i][1] != NULL) {
                if (strcasecmp(extra_headers_kv[i][0], "x-api-key") == 0 ||
                    strcasecmp(extra_headers_kv[i][0], "x-goog-api-key") == 0 ||
                    strcasecmp(extra_headers_kv[i][0], "Authorization") == 0) {
                    has_custom_auth = 1;
                }
                char hdr[1024];
                snprintf(hdr, sizeof hdr, "%s: %s", extra_headers_kv[i][0], extra_headers_kv[i][1]);
                hdrs = curl_slist_append(hdrs, hdr);
            }
        }
    }

    if (!has_custom_auth && upstream_key != NULL && upstream_key[0] != '\0') {
        char auth[1080];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", upstream_key);
        hdrs = curl_slist_append(hdrs, auth);
    }
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream, application/json");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_json);
    curl_easy_setopt(
        c, CURLOPT_POSTFIELDSIZE, body_len > 0 ? (long)body_len : (long)strlen(body_json));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sc);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, stream_xferinfo_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sc);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode cret = curl_easy_perform(c);
    if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
        *out_status = (int)http_code;
    }

    if (cret == CURLE_OK) {
        rc = 0;
    } else if (cret == CURLE_ABORTED_BY_CALLBACK && sc.aborted == 2) {
        rc = -110; /* silence timeout */
    } else if (cret == CURLE_OPERATION_TIMEDOUT) {
        rc = -110;
    } else {
        AIGATE_LOG_WARN("upstream stream transport error: %s", curl_easy_strerror(cret));
        rc = -502;
    }

done:
    curl_slist_free_all(hdrs);
    return rc;
}

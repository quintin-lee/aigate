/** @file upstream_client.c
 *  @brief libcurl 上游 HTTP 客户端：线程复用 easy 句柄、共享 DNS/SSL 会话、非流式/流式/探针三种调用。 */
#include "upstream_client.h"
#include "aigate_log.h"

#include <curl/curl.h>
#include <curl/curlver.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/** curl 共享句柄（DNS/SSL 会话跨 easy 句柄复用，g_curl_once 初始化）。 */
static CURLSH*         g_curl_sh = NULL;
/** curl 共享 DNS 缓存锁。 */
static pthread_mutex_t g_curl_sh_dns_mtx = PTHREAD_MUTEX_INITIALIZER;
/** curl 共享 SSL 会话锁。 */
static pthread_mutex_t g_curl_sh_ssl_mtx = PTHREAD_MUTEX_INITIALIZER;

/** @brief curl_share 加锁回调：DNS/SSL 会话槽分别上对应互斥锁。 */
static void
curl_sh_lock(CURL* handle, curl_lock_data data, curl_lock_access access, void* userptr)
{
    (void)handle;
    (void)access;
    (void)userptr;
    if (data == CURL_LOCK_DATA_DNS) {
        pthread_mutex_lock(&g_curl_sh_dns_mtx);
    } else if (data == CURL_LOCK_DATA_SSL_SESSION) {
        pthread_mutex_lock(&g_curl_sh_ssl_mtx);
    }
}

/** @brief curl_share 解锁回调：与 curl_sh_lock 配对。 */
static void
curl_sh_unlock(CURL* handle, curl_lock_data data, void* userptr)
{
    (void)handle;
    (void)userptr;
    if (data == CURL_LOCK_DATA_DNS) {
        pthread_mutex_unlock(&g_curl_sh_dns_mtx);
    } else if (data == CURL_LOCK_DATA_SSL_SESSION) {
        pthread_mutex_unlock(&g_curl_sh_ssl_mtx);
    }
}

/** 进程级 curl 全局初始化 once 守卫。 */
static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
/** @brief 进程级 curl 全局初始化（pthread_once）：global_init + 共享 DNS/SSL 会话句柄。 */
static void
curl_init_once(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl_sh = curl_share_init();
    if (g_curl_sh != NULL) {
        curl_share_setopt(g_curl_sh, CURLSHOPT_LOCKFUNC, curl_sh_lock);
        curl_share_setopt(g_curl_sh, CURLSHOPT_UNLOCKFUNC, curl_sh_unlock);
        curl_share_setopt(g_curl_sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_curl_sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
}

/** 线程本地 easy 句柄 key（析构回收该线程复用句柄）。 */
static pthread_key_t  g_curl_tkey;
/** 线程 key 初始化 once 守卫。 */
static pthread_once_t g_curl_tkey_once = PTHREAD_ONCE_INIT;

/** @brief 线程退出时回收该线程的复用 easy 句柄（pthread_key 析构）。 */
static void
curl_thread_cleanup(void* val)
{
    if (val != NULL) {
        curl_easy_cleanup((CURL*)val);
    }
}

/** @brief 创建线程局部 easy 句柄槽（pthread_once）。 */
static void
curl_tkey_init(void)
{
    pthread_key_create(&g_curl_tkey, curl_thread_cleanup);
}

/* Per-thread CURL handle: reused across calls to amortize init.
 * Automatically cleaned up on worker thread exit. */
/** @brief 取本线程复用的 easy 句柄（首次 lazy 创建，线程退出自动回收）。
 *  @return 句柄；curl_easy_init 失败返回 NULL。 */
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

/** @brief 每次调用前统一 easy 选项：挂共享句柄、HTTP/2+TLS、TCP keepalive。 */
static void
curl_apply_common_opts(CURL* c)
{
    if (g_curl_sh != NULL) {
        curl_easy_setopt(c, CURLOPT_SHARE, g_curl_sh);
    }
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 30L);
}

/** @brief Non-streaming response accumulator (whole body in memory). */
struct resp_buf {
    char*  data; /**< 累积的响应体 */
    size_t len; /**< 已用字节 */
    size_t cap; /**< 缓冲容量 */
};

/** @brief 非流式响应累积上限（32MB）：防恶意/ misconfigured 上游撑爆 worker 内存，超限按传输错误（-502）上报。
 *  @note 流式错误体累积同样受此上限约束。 */
#define UPSTREAM_RESP_MAX (32 * 1024 * 1024)

/* libcurl write callback: data first, userdata last. */
/** @brief 非流式 write 回调：累积响应体（32MB 上限，超限/分配失败返回 0 中断传输）。
 *  @return 消费字节数；返回 0 中断传输。 */
static size_t
append_body(char* buf, size_t size, size_t nmemb, void* ud)
{
    struct resp_buf* rb = ud;
    size_t           total = size * nmemb;
    if (rb->len + total >= UPSTREAM_RESP_MAX) {
        AIGATE_LOG_WARN("upstream response cap reached, aborting transfer");
        return 0; /* abort: body exceeds the accumulation cap */
    }
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
    curl_apply_common_opts(c);

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
    /* A 30x may point at any protocol (file://, ftp://, ...); confine
     * redirects to http/https so a controlled upstream cannot leak via a
     * redirect to another scheme. */
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
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

/** @brief 单调时钟纳秒（探针延迟计时）。 */
static uint64_t
mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* libcurl write callback: accept all data, discard. */
/** @brief 探针 write 回调：只计大小不存体。@return 恒 size*nmemb。 */
static size_t
discard_body(char* buf, size_t size, size_t nmemb, void* ud)
{
    (void)buf;
    (void)ud;
    return size * nmemb;
}

int
upstream_probe(const char* url,
               const char* hdr_name,
               const char* hdr_value,
               const char* extra_hdr_name,
               const char* extra_hdr_value,
               long        timeout_ms,
               int*        out_status,
               long*       out_latency_ns)
{
    struct curl_slist* hdrs = NULL;
    int                rc = -502;
    long               http_code = 0;
    uint64_t           t0;

    if (out_status != NULL) {
        *out_status = 0;
    }
    if (out_latency_ns != NULL) {
        *out_latency_ns = 0;
    }
    if (url == NULL || url[0] == '\0') {
        return -502;
    }
    t0 = mono_ns();

    pthread_once(&g_curl_once, curl_init_once);
    CURL* c = thread_curl();
    if (c == NULL) {
        return -502;
    }
    curl_easy_reset(c);
    curl_apply_common_opts(c);

    if (hdr_name != NULL && hdr_value != NULL && hdr_name[0] != '\0') {
        char hdr[1080];
        snprintf(hdr, sizeof hdr, "%s: %s", hdr_name, hdr_value);
        hdrs = curl_slist_append(hdrs, hdr);
    }
    if (extra_hdr_name != NULL && extra_hdr_value != NULL && extra_hdr_name[0] != '\0') {
        char hdr[256];
        snprintf(hdr, sizeof hdr, "%s: %s", extra_hdr_name, extra_hdr_value);
        hdrs = curl_slist_append(hdrs, hdr);
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 60000L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    /* confine redirects to http/https like upstream_call_ext */
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode cret = curl_easy_perform(c);
    if (cret == CURLE_OK) {
        if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
            if (out_status != NULL) {
                *out_status = (int)http_code;
            }
            rc = 0;
        }
    } else if (cret == CURLE_OPERATION_TIMEDOUT) {
        rc = -110;
    } else {
        AIGATE_LOG_WARN("upstream probe transport error: %s", curl_easy_strerror(cret));
        rc = -502;
    }

    if (out_latency_ns != NULL) {
        *out_latency_ns = (long)(mono_ns() - t0);
    }
    curl_slist_free_all(hdrs);
    return rc;
}

/** @brief 流式传输上下文（write 回调状态）：分片转发 + 静默超时 + 错误体捕获。 */
struct stream_ctx {
    upstream_chunk_fn on_chunk; /**< 逐分片回调（借用） */
    void*             user_data; /**< 回调透传数据（借用） */
    uint64_t          last_chunk_mono_ns; /**< 末分片单调时间（静默超时基准） */
    uint64_t          silence_timeout_ns; /**< 分片间静默超时（纳秒） */
    int               aborted; /**< 非零=回调要求中断 */
    CURL*             curl; /**< 本次 easy 句柄（借用） */
    int               status; /**< 首包 HTTP 状态码 */
    /* Error-body capture: when the upstream answers 4xx/5xx before the first
     * SSE chunk, the body is accumulated here so the caller can surface the
     * upstream's own error instead of a generic 502. Capped like the
     * non-streaming buffer. */
    char*  err_body; /**< 4xx/5xx 预 SSE 错误体累积缓冲 */
    size_t err_len; /**< 错误体已用字节 */
    size_t err_cap; /**< 错误体缓冲容量 */
};

/** @brief 流式 write 回调：首包记状态码；4xx/5xx 累积错误体，其余分片交 on_chunk（回调非零即中断）。
 *  @return 消费字节数；返回 0 中断传输。 */
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
        /* Accumulate the upstream error body (capped) so the caller can
         * surface it instead of a generic 502. */
        if (total > 0 && sc->err_len + total < UPSTREAM_RESP_MAX) {
            size_t need = sc->err_len + total + 1;
            if (need > sc->err_cap) {
                sc->err_cap = need * 2;
                sc->err_body = realloc(sc->err_body, sc->err_cap);
                if (sc->err_body == NULL) {
                    return 0; /* out of memory: abort */
                }
            }
            memcpy(sc->err_body + sc->err_len, buf, total);
            sc->err_len += total;
            sc->err_body[sc->err_len] = '\0';
        }
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

/** @brief 流式进度回调：分片静默超 silence_timeout 即中断。@return 0 继续；1 中断。 */
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
                     int*              out_status,
                     char**            out_err_body,
                     size_t*           out_err_len)
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
    curl_apply_common_opts(c);
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
    /* Same protocol confinement as the non-streaming path: redirects may
     * only follow http/https, never file:// or other schemes. */
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
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
    if (out_err_body != NULL && out_err_len != NULL) {
        *out_err_body = sc.err_body;
        *out_err_len = sc.err_len;
    } else {
        free(sc.err_body);
    }
    return rc;
}

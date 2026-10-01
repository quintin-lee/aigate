/** @file upstream_hedged.c
 *  @ingroup group_upstream
 *  @brief Hedged concurrent requests execution engine.
 */
#include "upstream_hedged.h"
#include "aigate_log.h"
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Maximum upstream response body buffer capacity (32 MiB). */
#define RESP_MAX_LEN (32 * 1024 * 1024)

/**
 * @brief Thread synchronization and race outcome control block for hedged requests.
 */
typedef struct hedged_race_ctrl {
    pthread_mutex_t mutex; /**< Mutex protecting race completion state and conditions. */
    pthread_cond_t  cond;  /**< Condition variable signaled when any request completes or wins. */
    int ref_count;         /**< Reference counter for safe resource deallocation across threads. */
    int winning_idx;       /**< Index of first winning target (-1 if none yet). */
    volatile int
             cancel_flags[2]; /**< Cooperative cancellation flags set when competitor finishes. */
    int      done[2];         /**< Completion flags for primary (0) and secondary (1). */
    int      http_status[2];  /**< HTTP status codes returned by upstreams. */
    int      rc[2]; /**< Transport return codes (0 on HTTP success, non-zero on network error). */
    char*    resp_body[2]; /**< Dynamically allocated response buffers for each worker. */
    size_t   resp_len[2];  /**< Length of each response body. */
    uint64_t lat_ns[2];    /**< Measured latency in nanoseconds for each worker. */
} hedged_race_ctrl_t;

/**
 * @brief Thread worker context for executing one branch of a hedged request.
 */
typedef struct worker_ctx {
    hedged_race_ctrl_t* ctrl;      /**< Pointer to shared race control block. */
    int                 my_idx;    /**< Target index for this worker (0: primary, 1: secondary). */
    char                url[1024]; /**< Upstream URL. */
    char                key[1024]; /**< API bearer key. */
    char*               payload;   /**< Copied request payload. */
    size_t              payload_len;                  /**< Payload length. */
    char* extra_headers[HEDGED_MAX_EXTRA_HEADERS][2]; /**< Extra header key-value copies. */
    int   n_extra_headers;                            /**< Count of extra headers. */
    long  timeout_ms; /**< Timeout limit for curl in milliseconds. */
} worker_ctx_t;

static uint64_t
get_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
ctrl_release(hedged_race_ctrl_t* ctrl)
{
    if (ctrl == NULL) {
        return;
    }
    pthread_mutex_lock(&ctrl->mutex);
    int remaining = --ctrl->ref_count;
    pthread_mutex_unlock(&ctrl->mutex);
    if (remaining <= 0) {
        free(ctrl->resp_body[0]);
        free(ctrl->resp_body[1]);
        pthread_mutex_destroy(&ctrl->mutex);
        pthread_cond_destroy(&ctrl->cond);
        free(ctrl);
    }
}

static void
worker_ctx_free(worker_ctx_t* ctx)
{
    if (ctx == NULL) {
        return;
    }
    free(ctx->payload);
    for (int i = 0; i < ctx->n_extra_headers; i++) {
        free(ctx->extra_headers[i][0]);
        free(ctx->extra_headers[i][1]);
    }
    free(ctx);
}

static worker_ctx_t*
worker_ctx_create(hedged_race_ctrl_t*           ctrl,
                  int                           my_idx,
                  const hedged_endpoint_spec_t* spec,
                  const hedged_call_params_t*   params)
{
    worker_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->ctrl = ctrl;
    ctx->my_idx = my_idx;
    snprintf(ctx->url, sizeof(ctx->url), "%s", spec->url);
    snprintf(ctx->key, sizeof(ctx->key), "%s", spec->key);
    ctx->timeout_ms = params->timeout_ms > 0 ? (long)params->timeout_ms : 60000L;

    const char* p = (spec->payload != NULL) ? spec->payload : params->payload;
    size_t      plen = (spec->payload != NULL) ? spec->payload_len : params->payload_len;
    if (p != NULL && plen > 0) {
        ctx->payload = malloc(plen + 1);
        if (ctx->payload != NULL) {
            memcpy(ctx->payload, p, plen);
            ctx->payload[plen] = '\0';
            ctx->payload_len = plen;
        }
    }

    ctx->n_extra_headers = spec->n_extra_headers;
    for (int i = 0; i < spec->n_extra_headers && i < HEDGED_MAX_EXTRA_HEADERS; i++) {
        if (spec->extra_headers[i][0] != NULL) {
            ctx->extra_headers[i][0] = strdup(spec->extra_headers[i][0]);
        }
        if (spec->extra_headers[i][1] != NULL) {
            ctx->extra_headers[i][1] = strdup(spec->extra_headers[i][1]);
        }
    }
    return ctx;
}

static size_t
hedged_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    worker_ctx_t*       ctx = (worker_ctx_t*)userdata;
    hedged_race_ctrl_t* ctrl = ctx->ctrl;
    int                 idx = ctx->my_idx;

    if (ctrl->cancel_flags[idx]) {
        return 0; /* abort */
    }

    size_t total = size * nmemb;
    pthread_mutex_lock(&ctrl->mutex);
    if (ctrl->cancel_flags[idx] || ctrl->resp_len[idx] + total >= RESP_MAX_LEN) {
        pthread_mutex_unlock(&ctrl->mutex);
        return 0;
    }

    char* nd = realloc(ctrl->resp_body[idx], ctrl->resp_len[idx] + total + 1);
    if (nd == NULL) {
        pthread_mutex_unlock(&ctrl->mutex);
        return 0;
    }
    ctrl->resp_body[idx] = nd;
    memcpy(ctrl->resp_body[idx] + ctrl->resp_len[idx], ptr, total);
    ctrl->resp_len[idx] += total;
    ctrl->resp_body[idx][ctrl->resp_len[idx]] = '\0';
    pthread_mutex_unlock(&ctrl->mutex);
    return total;
}

static int
hedged_xferinfo_cb(
    void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    worker_ctx_t* ctx = (worker_ctx_t*)clientp;
    if (ctx->ctrl->cancel_flags[ctx->my_idx]) {
        return 1; /* non-zero aborts transfer */
    }
    return 0;
}

static void*
hedged_worker_run(void* arg)
{
    worker_ctx_t*       ctx = (worker_ctx_t*)arg;
    hedged_race_ctrl_t* ctrl = ctx->ctrl;
    int                 idx = ctx->my_idx;
    uint64_t            t0 = get_mono_ns();

    CURL* c = curl_easy_init();
    if (c == NULL) {
        pthread_mutex_lock(&ctrl->mutex);
        ctrl->rc[idx] = -502;
        ctrl->done[idx] = 1;
        pthread_cond_broadcast(&ctrl->cond);
        pthread_mutex_unlock(&ctrl->mutex);
        worker_ctx_free(ctx);
        ctrl_release(ctrl);
        return NULL;
    }

    struct curl_slist* hdrs = NULL;
    int                has_auth = 0;
    for (int i = 0; i < ctx->n_extra_headers; i++) {
        if (ctx->extra_headers[i][0] != NULL && ctx->extra_headers[i][1] != NULL) {
            if (strcasecmp(ctx->extra_headers[i][0], "Authorization") == 0 ||
                strcasecmp(ctx->extra_headers[i][0], "x-api-key") == 0 ||
                strcasecmp(ctx->extra_headers[i][0], "x-goog-api-key") == 0) {
                has_auth = 1;
            }
            char buf[1024];
            snprintf(
                buf, sizeof(buf), "%s: %s", ctx->extra_headers[i][0], ctx->extra_headers[i][1]);
            hdrs = curl_slist_append(hdrs, buf);
        }
    }

    if (!has_auth && ctx->key[0] != '\0') {
        char auth[1080];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->key);
        hdrs = curl_slist_append(hdrs, auth);
    }
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    curl_easy_setopt(c, CURLOPT_URL, ctx->url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, ctx->payload);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)ctx->payload_len);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, ctx->timeout_ms > 0 ? ctx->timeout_ms : 60000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, hedged_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, hedged_xferinfo_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, ctx);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode code = curl_easy_perform(c);
    long     status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    uint64_t lat = get_mono_ns() - t0;

    pthread_mutex_lock(&ctrl->mutex);
    ctrl->http_status[idx] = (int)status;
    ctrl->lat_ns[idx] = lat;
    if (code == CURLE_OK) {
        ctrl->rc[idx] = 0;
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        ctrl->rc[idx] = -110;
    } else {
        ctrl->rc[idx] = -502;
    }
    ctrl->done[idx] = 1;

    /* If this worker returned 200 and no winner has been decided yet, claim win */
    if (ctrl->rc[idx] == 0 && ctrl->http_status[idx] == 200 && ctrl->winning_idx == -1) {
        ctrl->winning_idx = idx;
        ctrl->cancel_flags[1 - idx] = 1;
    }
    pthread_cond_broadcast(&ctrl->cond);
    pthread_mutex_unlock(&ctrl->mutex);

    worker_ctx_free(ctx);
    ctrl_release(ctrl);
    return NULL;
}

int
upstream_call_hedged(const hedged_call_params_t* params, hedged_call_result_t* out_result)
{
    if (params == NULL || out_result == NULL) {
        return -1;
    }
    memset(out_result, 0, sizeof(*out_result));

    /* Check if hedging is eligible */
    bool should_hedge = params->has_secondary && params->secondary.url[0] != '\0';
    if (should_hedge && params->lt != NULL) {
        /* Check hedge budget */
        int budget =
            (params->budget_pct > 0 && params->budget_pct <= 100) ? params->budget_pct : 15;
        if (!latency_tracker_hedge_admitted(params->lt, params->model, budget)) {
            should_hedge = false;
        }
    }

    /* Determine delay window */
    int delay_ms = params->delay_ms;
    if (delay_ms <= 0 && params->lt != NULL) {
        const char* ep =
            params->primary.endpoint[0] ? params->primary.endpoint : params->primary.url;
        delay_ms = (int)latency_tracker_get_p95_ms(params->lt, params->model, ep);
        if (delay_ms < 50) {
            delay_ms = 50;
        }
        if (delay_ms > 5000) {
            delay_ms = 5000;
        }
    }
    if (delay_ms <= 0) {
        delay_ms = 1000;
    }

    hedged_race_ctrl_t* ctrl = calloc(1, sizeof(*ctrl));
    if (ctrl == NULL) {
        return -502;
    }
    pthread_mutex_init(&ctrl->mutex, NULL);
    pthread_cond_init(&ctrl->cond, NULL);
    ctrl->ref_count = 1; /* for this function */
    ctrl->winning_idx = -1;

    worker_ctx_t* primary_ctx = worker_ctx_create(ctrl, 0, &params->primary, params);
    if (primary_ctx == NULL) {
        ctrl_release(ctrl);
        return -502;
    }

    pthread_t      tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ctrl->ref_count++;
    if (pthread_create(&tid, &attr, hedged_worker_run, primary_ctx) != 0) {
        ctrl->ref_count--;
        worker_ctx_free(primary_ctx);
        pthread_attr_destroy(&attr);
        ctrl_release(ctrl);
        return -502;
    }

    bool secondary_started = false;

    pthread_mutex_lock(&ctrl->mutex);

    /* Wait up to delay_ms for Primary to finish or win */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += delay_ms / 1000;
    ts.tv_nsec += (long)(delay_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    while (!ctrl->done[0] && ctrl->winning_idx == -1) {
        int rc = pthread_cond_timedwait(&ctrl->cond, &ctrl->mutex, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
    }

    /* If primary hasn't finished with 200 within delay_ms (or failed), spawn secondary if enabled */
    if (ctrl->winning_idx == -1 && should_hedge) {
        worker_ctx_t* sec_ctx = worker_ctx_create(ctrl, 1, &params->secondary, params);
        if (sec_ctx != NULL) {
            if (params->lt != NULL) {
                latency_tracker_record_hedge(params->lt, params->model);
            }
            out_result->was_hedged = true;
            secondary_started = true;
            ctrl->ref_count++;
            if (pthread_create(&tid, &attr, hedged_worker_run, sec_ctx) != 0) {
                ctrl->ref_count--;
                worker_ctx_free(sec_ctx);
                secondary_started = false;
            }
        }
    }

    /* Wait for a winner or for both workers to finish */
    while (ctrl->winning_idx == -1) {
        if (ctrl->done[0] && (!secondary_started || ctrl->done[1])) {
            /* Both are done and neither was 200 */
            break;
        }
        pthread_cond_wait(&ctrl->cond, &ctrl->mutex);
    }

    /* If a winner was chosen, cancel the loser */
    if (ctrl->winning_idx == 0 && secondary_started) {
        ctrl->cancel_flags[1] = 1;
    } else if (ctrl->winning_idx == 1) {
        ctrl->cancel_flags[0] = 1;
    }

    /* Determine final result */
    int final_rc = -502;
    int win = ctrl->winning_idx;

    if (win == 1) {
        /* Secondary won */
        out_result->winning_target_idx = 1;
        out_result->status = ctrl->http_status[1];
        out_result->body = ctrl->resp_body[1];
        out_result->body_len = ctrl->resp_len[1];
        out_result->latency_ns = ctrl->lat_ns[1];
        final_rc = ctrl->rc[1];
        ctrl->resp_body[1] = NULL; /* transfer ownership */
        ctrl->resp_len[1] = 0;
    } else if (win == 0 || ctrl->done[0]) {
        /* Primary won or both failed, return primary's result */
        out_result->winning_target_idx = 0;
        out_result->status = ctrl->http_status[0];
        out_result->body = ctrl->resp_body[0];
        out_result->body_len = ctrl->resp_len[0];
        out_result->latency_ns = ctrl->lat_ns[0];
        final_rc = ctrl->rc[0];
        ctrl->resp_body[0] = NULL; /* transfer ownership */
        ctrl->resp_len[0] = 0;
    } else if (secondary_started && ctrl->done[1]) {
        out_result->winning_target_idx = 1;
        out_result->status = ctrl->http_status[1];
        out_result->body = ctrl->resp_body[1];
        out_result->body_len = ctrl->resp_len[1];
        out_result->latency_ns = ctrl->lat_ns[1];
        final_rc = ctrl->rc[1];
        ctrl->resp_body[1] = NULL; /* transfer ownership */
        ctrl->resp_len[1] = 0;
    }

    pthread_mutex_unlock(&ctrl->mutex);
    pthread_attr_destroy(&attr);

    /* Release caller's reference to ctrl.
     * Loser thread will release its own reference and free ctrl when done. */
    ctrl_release(ctrl);

    return final_rc;
}

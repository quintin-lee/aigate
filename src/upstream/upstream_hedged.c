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

#define RESP_MAX_LEN (32 * 1024 * 1024)

typedef struct worker_ctx {
    const hedged_endpoint_spec_t* spec;
    const char*                   payload;
    size_t                        payload_len;
    long                          timeout_ms;
    volatile int                  cancel_flag;
    int                           done;
    int                           rc;
    int                           http_status;
    char*                         resp_body;
    size_t                        resp_len;
    uint64_t                      lat_ns;
    pthread_t                     tid;
    pthread_mutex_t*              shared_mutex;
    pthread_cond_t*               shared_cond;
    int*                          winning_idx;
    int                           my_idx;
} worker_ctx_t;

static uint64_t
get_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static size_t
hedged_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    worker_ctx_t* ctx = (worker_ctx_t*)userdata;
    if (ctx->cancel_flag) {
        return 0; /* abort */
    }

    size_t total = size * nmemb;
    if (ctx->resp_len + total >= RESP_MAX_LEN) {
        return 0;
    }

    char* nd = realloc(ctx->resp_body, ctx->resp_len + total + 1);
    if (nd == NULL) {
        return 0;
    }
    ctx->resp_body = nd;
    memcpy(ctx->resp_body + ctx->resp_len, ptr, total);
    ctx->resp_len += total;
    ctx->resp_body[ctx->resp_len] = '\0';
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
    if (ctx->cancel_flag) {
        return 1; /* non-zero aborts transfer */
    }
    return 0;
}

static void*
hedged_worker_run(void* arg)
{
    worker_ctx_t* ctx = (worker_ctx_t*)arg;
    uint64_t      t0 = get_mono_ns();

    CURL* c = curl_easy_init();
    if (c == NULL) {
        ctx->rc = -502;
        goto finish;
    }

    struct curl_slist* hdrs = NULL;
    int                has_auth = 0;
    for (int i = 0; i < ctx->spec->n_extra_headers; i++) {
        if (ctx->spec->extra_headers[i][0] != NULL && ctx->spec->extra_headers[i][1] != NULL) {
            if (strcasecmp(ctx->spec->extra_headers[i][0], "Authorization") == 0 ||
                strcasecmp(ctx->spec->extra_headers[i][0], "x-api-key") == 0 ||
                strcasecmp(ctx->spec->extra_headers[i][0], "x-goog-api-key") == 0) {
                has_auth = 1;
            }
            char buf[1024];
            snprintf(buf,
                     sizeof(buf),
                     "%s: %s",
                     ctx->spec->extra_headers[i][0],
                     ctx->spec->extra_headers[i][1]);
            hdrs = curl_slist_append(hdrs, buf);
        }
    }

    if (!has_auth && ctx->spec->key[0] != '\0') {
        char auth[1080];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->spec->key);
        hdrs = curl_slist_append(hdrs, auth);
    }
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    curl_easy_setopt(c, CURLOPT_URL, ctx->spec->url);
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
    ctx->http_status = (int)status;

    if (code == CURLE_OK) {
        ctx->rc = 0;
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        ctx->rc = -110;
    } else {
        ctx->rc = -502;
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

finish:
    ctx->lat_ns = get_mono_ns() - t0;
    pthread_mutex_lock(ctx->shared_mutex);
    ctx->done = 1;
    /* If this worker returned 200 and no winner has been decided yet, claim win */
    if (ctx->rc == 0 && ctx->http_status == 200 && *ctx->winning_idx == -1) {
        *ctx->winning_idx = ctx->my_idx;
    }
    pthread_cond_broadcast(ctx->shared_cond);
    pthread_mutex_unlock(ctx->shared_mutex);
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
        if (!latency_tracker_hedge_admitted(params->lt, params->model, 15)) {
            should_hedge = false;
        }
    }

    /* Determine delay window */
    int delay_ms = params->delay_ms;
    if (delay_ms <= 0 && params->lt != NULL) {
        delay_ms = (int)latency_tracker_get_p95_ms(params->lt, params->model, params->primary.url);
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

    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);

    int winning_idx = -1;

    worker_ctx_t primary_ctx;
    memset(&primary_ctx, 0, sizeof(primary_ctx));
    primary_ctx.spec = &params->primary;
    primary_ctx.payload = params->payload;
    primary_ctx.payload_len = params->payload_len;
    primary_ctx.timeout_ms = params->timeout_ms;
    primary_ctx.shared_mutex = &mutex;
    primary_ctx.shared_cond = &cond;
    primary_ctx.winning_idx = &winning_idx;
    primary_ctx.my_idx = 0;

    worker_ctx_t secondary_ctx;
    memset(&secondary_ctx, 0, sizeof(secondary_ctx));
    secondary_ctx.spec = &params->secondary;
    secondary_ctx.payload = params->payload;
    secondary_ctx.payload_len = params->payload_len;
    secondary_ctx.timeout_ms = params->timeout_ms;
    secondary_ctx.shared_mutex = &mutex;
    secondary_ctx.shared_cond = &cond;
    secondary_ctx.winning_idx = &winning_idx;
    secondary_ctx.my_idx = 1;

    /* Start Primary Worker */
    if (pthread_create(&primary_ctx.tid, NULL, hedged_worker_run, &primary_ctx) != 0) {
        pthread_mutex_destroy(&mutex);
        pthread_cond_destroy(&cond);
        return -502;
    }

    bool secondary_started = false;

    pthread_mutex_lock(&mutex);

    /* Wait up to delay_ms for Primary to finish or win */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += delay_ms / 1000;
    ts.tv_nsec += (long)(delay_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    while (!primary_ctx.done && winning_idx == -1) {
        int rc = pthread_cond_timedwait(&cond, &mutex, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
    }

    /* If primary hasn't finished with 200 within delay_ms (or failed), spawn secondary if enabled */
    if (winning_idx == -1 && should_hedge) {
        if (params->lt != NULL) {
            latency_tracker_record_hedge(params->lt, params->model);
        }
        out_result->was_hedged = true;
        secondary_started = true;
        pthread_create(&secondary_ctx.tid, NULL, hedged_worker_run, &secondary_ctx);
    }

    /* Wait for a winner or for both workers to finish */
    while (winning_idx == -1) {
        if (primary_ctx.done && (!secondary_started || secondary_ctx.done)) {
            /* Both are done and neither was 200 */
            break;
        }
        pthread_cond_wait(&cond, &mutex);
    }

    /* If a winner was chosen, cancel the loser */
    if (winning_idx == 0 && secondary_started) {
        secondary_ctx.cancel_flag = 1;
    } else if (winning_idx == 1) {
        primary_ctx.cancel_flag = 1;
    }

    pthread_mutex_unlock(&mutex);

    /* Join running workers */
    pthread_join(primary_ctx.tid, NULL);
    if (secondary_started) {
        pthread_join(secondary_ctx.tid, NULL);
    }

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);

    /* Determine final result */
    int final_rc = -502;
    if (winning_idx == 1) {
        /* Secondary won */
        out_result->winning_target_idx = 1;
        out_result->status = secondary_ctx.http_status;
        out_result->body = secondary_ctx.resp_body;
        out_result->body_len = secondary_ctx.resp_len;
        out_result->latency_ns = secondary_ctx.lat_ns;
        final_rc = secondary_ctx.rc;
        free(primary_ctx.resp_body);
    } else if (winning_idx == 0 || primary_ctx.done) {
        /* Primary won or both failed, return primary's result */
        out_result->winning_target_idx = 0;
        out_result->status = primary_ctx.http_status;
        out_result->body = primary_ctx.resp_body;
        out_result->body_len = primary_ctx.resp_len;
        out_result->latency_ns = primary_ctx.lat_ns;
        final_rc = primary_ctx.rc;
        if (secondary_started) {
            free(secondary_ctx.resp_body);
        }
    } else if (secondary_started && secondary_ctx.done) {
        out_result->winning_target_idx = 1;
        out_result->status = secondary_ctx.http_status;
        out_result->body = secondary_ctx.resp_body;
        out_result->body_len = secondary_ctx.resp_len;
        out_result->latency_ns = secondary_ctx.lat_ns;
        final_rc = secondary_ctx.rc;
        free(primary_ctx.resp_body);
    }

    return final_rc;
}

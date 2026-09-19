/** @file usage_meter.c
 *  @brief Usage metering: atomics + HDR latency + 5s PG batch flush. */
#include "usage_meter.h"
#include "aigate_log.h"

#include <stdatomic.h>
#include <hdr/hdr_histogram.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define UM_ACC_CAP 4096
#define UM_MAX_PROVS 8

typedef struct {
    int    in_use;
    long   key_id;
    char   model[128];
    time_t day;
    long   requests, prompt, completion, errors;
    long   cached_prompt;
} um_acc_t;

typedef struct {
    char                  name[32];
    struct hdr_histogram* h;
    int                   in_use;
} um_prov_t;

struct usage_meter {
    pg_store_t*     ps;
    pthread_mutex_t mtx;
    um_acc_t        accs[UM_ACC_CAP];
    um_prov_t       provs[UM_MAX_PROVS];
    atomic_long     reqs, errs, toks, cached_toks;
    pthread_t       worker;
    int             have_worker;
    int             flush_interval_s;
    int             stop;
};

static time_t
utc_midnight_now(void)
{
    time_t now = time(NULL);
    return (time_t)(now - ((uint64_t)now % 86400));
}

static uint64_t
acc_hash(long key_id, const char* model)
{
    uint64_t       h = 1469598103934665603ULL;
    const uint8_t* k = (const uint8_t*)&key_id;
    for (size_t i = 0; i < sizeof key_id; i++) {
        h ^= k[i];
        h *= 1099511628211ULL;
    }
    for (const char* p = model; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static int
is_known_provider(const char* p)
{
    return p && (strcmp(p, "openai") == 0 ||
                 strcmp(p, "ollama") == 0 ||
                 strcmp(p, "azure") == 0 ||
                 strcmp(p, "anthropic") == 0 ||
                 strcmp(p, "gemini") == 0 ||
                 strcmp(p, "deepseek") == 0 ||
                 strcmp(p, "siliconflow") == 0);
}

static um_prov_t*
prov_slot(usage_meter_t* um, const char* p)
{
    if (!is_known_provider(p)) {
        return NULL;
    }
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (um->provs[i].in_use && strcmp(um->provs[i].name, p) == 0) {
            return &um->provs[i];
        }
    }
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (!um->provs[i].in_use) {
            um->provs[i].in_use = 1;
            snprintf(um->provs[i].name, sizeof um->provs[i].name, "%s", p);
            return &um->provs[i];
        }
    }
    return NULL; /* provider slots exhausted; drop the latency sample */
}

static void*
worker_main(void* arg)
{
    usage_meter_t* um = arg;
    while (!um->stop) {
        /* sleep in 1s increments so stop is honored promptly */
        for (int i = 0; i < um->flush_interval_s && !um->stop; i++) {
            sleep(1);
        }
        if (um->stop) {
            break;
        }
        usage_row_t rows[256];
        int         n = 0;
        if (um_drain(um, rows, (int)(sizeof rows / sizeof rows[0]), &n) != 0) {
            AIGATE_LOG_WARN("usage flush drain error");
        }
    }
    return 0;
}

usage_meter_t*
usage_meter_new(pg_store_t* ps, int flush_interval_s)
{
    usage_meter_t* um = calloc(1, sizeof *um);
    if (um == NULL) {
        return NULL;
    }
    um->ps = ps;
    um->flush_interval_s = flush_interval_s;
    pthread_mutex_init(&um->mtx, NULL);
    atomic_init(&um->reqs, 0);
    atomic_init(&um->errs, 0);
    atomic_init(&um->toks, 0);

    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (hdr_init(1, 3600LL * 1000000000LL, 4, &um->provs[i].h) != 0) {
            um->provs[i].h = NULL;
        }
    }

    if (flush_interval_s > 0) {
        um->stop = 0;
        if (pthread_create(&um->worker, NULL, worker_main, um) == 0) {
            um->have_worker = 1;
        } else {
            AIGATE_LOG_WARN("usage_meter worker thread create failed");
        }
    }
    return um;
}

void
usage_meter_free(usage_meter_t* um)
{
    if (um == NULL) {
        return;
    }
    if (um->have_worker) {
        um->stop = 1;
        pthread_join(um->worker, 0);
    }
    /* final drain */
    usage_row_t rows[UM_ACC_CAP];
    int         n = 0;
    um_drain(um, rows, UM_ACC_CAP, &n);
    pthread_mutex_destroy(&um->mtx);
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        hdr_close(um->provs[i].h);
    }
    free(um);
}

void
um_record(usage_meter_t* um,
          long           key_id,
          const char*    model,
          int            http_status,
          long           prompt_tokens,
          long           completion_tokens,
          long           cached_prompt_tokens,
          uint64_t       latency_ns,
          const char*    provider)
{
    atomic_fetch_add(&um->reqs, 1);
    long toks = prompt_tokens + completion_tokens;
    if (http_status >= 500) {
        atomic_fetch_add(&um->errs, 1);
    }
    atomic_fetch_add(&um->toks, toks);
    atomic_fetch_add(&um->cached_toks, cached_prompt_tokens);

    pthread_mutex_lock(&um->mtx);
    um_prov_t* pv = prov_slot(um, provider);
    if (pv != NULL && pv->h != NULL && latency_ns > 0) {
        hdr_record_value(pv->h, (int64_t)latency_ns);
    }

    /* rollover: bump the day when the UTC calendar day changed */
    time_t   day = utc_midnight_now();
    uint64_t slot = acc_hash(key_id, model) & (UM_ACC_CAP - 1);
    for (int i = 0; i < UM_ACC_CAP; i++) {
        int       idx = (int)((slot + (uint64_t)i) & (UM_ACC_CAP - 1));
        um_acc_t* a = &um->accs[idx];
        if (a->in_use && a->key_id == key_id && strcmp(a->model, model) == 0) {
            if (a->day != day) {
                a->day = day;
            }
            a->requests++;
            a->prompt += prompt_tokens;
            a->completion += completion_tokens;
            a->cached_prompt += cached_prompt_tokens;
            if (http_status >= 500) {
                a->errors++;
            }
            pthread_mutex_unlock(&um->mtx);
            return;
        }
        if (!a->in_use) {
            a->in_use = 1;
            a->key_id = key_id;
            snprintf(a->model, sizeof a->model, "%s", model);
            a->day = day;
            a->requests = 1;
            a->prompt = prompt_tokens;
            a->completion = completion_tokens;
            a->cached_prompt = cached_prompt_tokens;
            a->errors = http_status >= 500 ? 1 : 0;
            pthread_mutex_unlock(&um->mtx);
            return;
        }
    }
    pthread_mutex_unlock(&um->mtx); /* table full: drop the row */
}

int
um_drain(usage_meter_t* um, usage_row_t* out, int cap, int* n_out)
{
    *n_out = 0;
    int flushed = 0;
    pthread_mutex_lock(&um->mtx);
    for (int i = 0; i < UM_ACC_CAP; i++) {
        um_acc_t* a = &um->accs[i];
        if (a->in_use) {
            if (flushed >= cap) {
                pthread_mutex_unlock(&um->mtx);
                return -1;
            }
            out[flushed].key_id = a->key_id;
            snprintf(out[flushed].model_name, sizeof out[flushed].model_name, "%s", a->model);
            out[flushed].day = a->day;
            out[flushed].requests = a->requests;
            out[flushed].prompt_tokens = a->prompt;
            out[flushed].completion_tokens = a->completion;
            out[flushed].cached_prompt_tokens = a->cached_prompt;
            out[flushed].errors = a->errors;
            a->in_use = 0;
            a->requests = a->prompt = a->completion = a->cached_prompt = a->errors = 0;
            flushed++;
        }
    }
    int n = flushed;
    pthread_mutex_unlock(&um->mtx);

    if (n > 0 && um->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(um->ps);
        if (ops != NULL && ops->flush_usage != NULL && ops->flush_usage(ops->ctx, out, n) != 0) {
            return -1;
        }
    }
    *n_out = n;
    return 0;
}

long
um_total_requests(usage_meter_t* um)
{
    return atomic_load(&um->reqs);
}

long
um_total_errors(usage_meter_t* um)
{
    return atomic_load(&um->errs);
}

long
um_total_tokens(usage_meter_t* um)
{
    return atomic_load(&um->toks);
}

long
um_total_cached_tokens(usage_meter_t* um)
{
    return atomic_load(&um->cached_toks);
}

int
um_provider_names(usage_meter_t* um, char (*names)[32], int cap)
{
    pthread_mutex_lock(&um->mtx);
    int n = 0;
    for (int i = 0; i < UM_MAX_PROVS && n < cap; i++) {
        if (um->provs[i].in_use) {
            snprintf(names[n++], 32, "%s", um->provs[i].name);
        }
    }
    pthread_mutex_unlock(&um->mtx);
    return n;
}

long
um_provider_sampled(usage_meter_t* um, const char* provider)
{
    pthread_mutex_lock(&um->mtx);
    long v = 0;
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (um->provs[i].in_use && strcmp(um->provs[i].name, provider) == 0 &&
            um->provs[i].h != NULL) {
            v = um->provs[i].h->total_count;
        }
    }
    pthread_mutex_unlock(&um->mtx);
    return v;
}

long
um_provider_percentile_ns(usage_meter_t* um, const char* provider, double percentile)
{
    pthread_mutex_lock(&um->mtx);
    long v = 0;
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (um->provs[i].in_use && strcmp(um->provs[i].name, provider) == 0 &&
            um->provs[i].h != NULL && um->provs[i].h->total_count > 0) {
            v = hdr_value_at_percentile(um->provs[i].h, percentile);
        }
    }
    pthread_mutex_unlock(&um->mtx);
    return v;
}

double
um_provider_mean_ns(usage_meter_t* um, const char* provider)
{
    pthread_mutex_lock(&um->mtx);
    double v = 0;
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (um->provs[i].in_use && strcmp(um->provs[i].name, provider) == 0 &&
            um->provs[i].h != NULL && um->provs[i].h->total_count > 0) {
            v = hdr_mean(um->provs[i].h);
        }
    }
    pthread_mutex_unlock(&um->mtx);
    return v;
}

long
um_provider_count_below_ns(usage_meter_t* um, const char* provider, long le_ns)
{
    pthread_mutex_lock(&um->mtx);
    long v = 0;
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        if (um->provs[i].in_use && strcmp(um->provs[i].name, provider) == 0 &&
            um->provs[i].h != NULL && um->provs[i].h->total_count > 0) {
            struct hdr_iter it;
            hdr_iter_recorded_init(&it, um->provs[i].h);
            while (hdr_iter_next(&it)) {
                if (it.value > le_ns) {
                    break;
                }
                v = it.cumulative_count;
            }
        }
    }
    pthread_mutex_unlock(&um->mtx);
    return v;
}

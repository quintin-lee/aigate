/** @file usage_meter.c
 *  @brief Usage metering: atomics + HDR latency + 5s PG batch flush. */
#include "usage_meter.h"
#include "aigate_log.h"
#include "provider_adapter.h"

#include <stdatomic.h>
#include <hdr/hdr_histogram.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define UM_ACC_CAP 4096
#define UM_MAX_PROVS 16
#define UM_REQ_CAP 4096
#define UM_REQ_BATCH 512

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
    pg_store_t*          ps;
    ratelimit_t*         rl;
    time_t               last_rollover_day;
    pthread_mutex_t      mtx;
    um_acc_t             accs[UM_ACC_CAP];
    um_prov_t            provs[UM_MAX_PROVS];
    atomic_long          reqs, errs, toks, cached_toks;
    pthread_t            worker;
    int                  have_worker;
    int                  flush_interval_s;
    int                  stop;
    usage_request_row_t* req_ring;
    int                  req_head, req_tail;
    atomic_int           req_dropped;
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

/* Gate latency sampling on the same source of truth as routing: a provider
 * label is metered iff the adapter registry resolves it. Unknown labels are
 * silently skipped, and the UM_MAX_PROVS slots backstop any label overflow. */

static um_prov_t*
prov_slot(usage_meter_t* um, const char* p)
{
    if (p == NULL || provider_find(p) == NULL) {
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

/* Drain the audit ring (copy + advance) and flush one batch; re-queue the
 * batch on flush failure so rows are retried next tick. Returns rows flushed, or -1 on error. */
static int
um_flush_request_batch(usage_meter_t* um, usage_request_row_t* rreqs, int rcap)
{
    if (um->ps == NULL || rreqs == NULL || um->req_ring == NULL) {
        return 0;
    }
    int rn = 0;
    if (um_drain_requests(um, rreqs, rcap, &rn) != 0 || rn == 0) {
        return 0;
    }
    const pg_ops_t* ops = pg_store_ops(um->ps);
    if (ops != NULL && ops->flush_usage_requests != NULL &&
        ops->flush_usage_requests(ops->ctx, rreqs, rn) == 0) {
        um_release_requests(um, rn);
        return rn;
    } else {
        um_requeue_requests(um, rn);
        AIGATE_LOG_WARN("usage request flush failed; %d rows requeued", rn);
        return -1;
    }
}
static void*

worker_main(void* arg)
{
    usage_meter_t*       um = arg;
    usage_row_t*         rows = malloc((size_t)UM_ACC_CAP * sizeof *rows);
    usage_request_row_t* rreqs = malloc(UM_REQ_BATCH * sizeof *rreqs);
    if (rows == NULL) {
        AIGATE_LOG_WARN("usage worker row buffer alloc failed; flushing disabled");
        /* stay alive so usage_meter_free's join cannot deadlock; the
         * stop flag is checked every second. */
        while (!um->stop) {
            sleep(1);
        }
        if (rreqs != NULL) {
            free(rreqs);
        }
        return 0;
    }
    while (!um->stop) {
        /* sleep in 1s increments so stop is honored promptly */
        for (int i = 0; i < um->flush_interval_s && !um->stop; i++) {
            sleep(1);
        }
        if (um->stop) {
            break;
        }
        /* rollover daily quotas when the UTC day changed */
        time_t day = utc_midnight_now();
        if (um->rl != NULL && day != um->last_rollover_day) {
            rl_reset_day(um->rl, day);
            um->last_rollover_day = day;
        }
        int n = 0;
        if (um_drain(um, rows, UM_ACC_CAP, &n) != 0) {
            if (n > 0) {
                /* flush failed: re-queue drained rows so nothing is lost */
                um_unflush(um, rows, n);
            }
            AIGATE_LOG_WARN("usage flush failed, %d rows re-queued", n);
        }
        while (!um->stop) {
            int flushed = um_flush_request_batch(um, rreqs, UM_REQ_BATCH);
            if (flushed < UM_REQ_BATCH) {
                break;
            }
        }
    }
    free(rows);
    free(rreqs);
    return 0;
}

usage_meter_t*
usage_meter_new(pg_store_t* ps, ratelimit_t* rl, int flush_interval_s)
{
    usage_meter_t* um = calloc(1, sizeof *um);
    if (um == NULL) {
        return NULL;
    }
    um->ps = ps;
    um->rl = rl;
    um->last_rollover_day = utc_midnight_now();
    um->flush_interval_s = flush_interval_s;
    pthread_mutex_init(&um->mtx, NULL);
    atomic_init(&um->reqs, 0);
    atomic_init(&um->errs, 0);
    atomic_init(&um->toks, 0);
    atomic_init(&um->cached_toks, 0);
    atomic_init(&um->req_dropped, 0);
    um->req_ring = malloc(UM_REQ_CAP * sizeof *um->req_ring);
    if (um->req_ring == NULL) {
        AIGATE_LOG_WARN("usage ring alloc failed; per-request audit disabled");
    }

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
    /* final drain in bounded chunks; a failed flush breaks (tail batch lost) */
    usage_row_t rows[256];
    int         n = 0;
    do {
        int rc = um_drain(um, rows, (int)(sizeof rows / sizeof rows[0]), &n);
        if (rc != 0) {
            if (n > 0) {
                AIGATE_LOG_WARN("usage_meter shutdown: final drain lost %d rows", n);
            }
            break;
        }
    } while (n > 0);
    /* final audit drain in bounded chunks; a failed flush loses the tail batch */
    usage_request_row_t* rreqs = malloc(256 * sizeof *rreqs);
    if (rreqs != NULL && um->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(um->ps);
        int             rn = 0;
        do {
            um_drain_requests(um, rreqs, 256, &rn);
            if (rn > 0) {
                if (ops != NULL && ops->flush_usage_requests != NULL &&
                    ops->flush_usage_requests(ops->ctx, rreqs, rn) != 0) {
                    AIGATE_LOG_WARN("usage_meter shutdown: lost %d audit rows", rn);
                    break;
                }
                um_release_requests(um, rn);
            }
        } while (rn > 0);
        free(rreqs);
    }
    pthread_mutex_destroy(&um->mtx);
    for (int i = 0; i < UM_MAX_PROVS; i++) {
        hdr_close(um->provs[i].h);
    }
    free(um->req_ring);
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
            i = UM_ACC_CAP;
            break;
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
            i = UM_ACC_CAP;
            break;
        }
    }

    /* per-request audit ring (P0-1); written under the same lock, after the
     * accumulator pass so table-full still records the detail row. */
    if (um->req_ring != NULL) {
        int                  slot_idx = um->req_tail % UM_REQ_CAP;
        usage_request_row_t* rr = &um->req_ring[slot_idx];
        rr->key_id = key_id;
        snprintf(rr->model_name, sizeof rr->model_name, "%s", model);
        rr->provider[0] = '\0';
        if (provider != NULL) {
            snprintf(rr->provider, sizeof rr->provider, "%s", provider);
        }
        rr->http_status = http_status;
        rr->prompt_tokens = prompt_tokens;
        rr->completion_tokens = completion_tokens;
        rr->cached_prompt_tokens = cached_prompt_tokens;
        rr->latency_ns = latency_ns;
        rr->ts = time(NULL);
        um->req_tail++;
        if (um->req_tail - um->req_head > UM_REQ_CAP) {
            /* ring full: backpressure on a brand-new slot would evict an
             * unflushed row, so drop this one and count it */
            um->req_tail--;
            atomic_fetch_add(&um->req_dropped, 1);
            AIGATE_LOG_WARN("usage ring full; dropping audit row (total dropped: %d)",
                            atomic_load(&um->req_dropped));
        }
    }
    pthread_mutex_unlock(&um->mtx);
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
    *n_out = n;

    if (n > 0 && um->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(um->ps);
        if (ops != NULL && ops->flush_usage != NULL && ops->flush_usage(ops->ctx, out, n) != 0) {
            return -1;
        }
    }
    return 0;
}

int
um_unflush(usage_meter_t* um, const usage_row_t* rows, int n)
{
    /* Table-full drop is effectively unreachable (P3-7): the 4096-slot
     * accumulator and the 4096-row worker batch are sized to the same cap,
     * and drained rows re-queue into a table that just cleared. The warn
     * below is the safety net, not an expected path. */
    if (n <= 0) {
        return 0;
    }
    int failed = 0;
    pthread_mutex_lock(&um->mtx);
    for (int i = 0; i < n; i++) {
        const usage_row_t* r = &rows[i];
        /* find-or-create the slot; an existing slot's day wins over the row's */
        int      found = 0;
        uint64_t slot = acc_hash(r->key_id, r->model_name) & (UM_ACC_CAP - 1);
        for (int s = 0; s < UM_ACC_CAP; s++) {
            int       idx = (int)((slot + (uint64_t)s) & (UM_ACC_CAP - 1));
            um_acc_t* a = &um->accs[idx];
            if (a->in_use && a->key_id == r->key_id && strcmp(a->model, r->model_name) == 0) {
                a->requests += r->requests;
                a->prompt += r->prompt_tokens;
                a->completion += r->completion_tokens;
                a->cached_prompt += r->cached_prompt_tokens;
                a->errors += r->errors;
                found = 1;
                break;
            }
            if (!a->in_use) {
                a->in_use = 1;
                a->key_id = r->key_id;
                snprintf(a->model, sizeof a->model, "%s", r->model_name);
                a->day = r->day;
                a->requests = r->requests;
                a->prompt = r->prompt_tokens;
                a->completion = r->completion_tokens;
                a->cached_prompt = r->cached_prompt_tokens;
                a->errors = r->errors;
                found = 1;
                break;
            }
        }
        if (!found) {
            failed = 1;
            break; /* table full: cannot re-queue */
        }
    }
    pthread_mutex_unlock(&um->mtx);
    if (failed) {
        AIGATE_LOG_WARN("um_unflush: accumulator table full, %d rows dropped", n);
    }
    return failed ? -1 : 0;
}

int
um_drain_requests(usage_meter_t* um, usage_request_row_t* out, int cap, int* n_out)
{
    *n_out = 0;
    if (um == NULL || um->req_ring == NULL) {
        return 0;
    }
    pthread_mutex_lock(&um->mtx);
    int n = um->req_tail - um->req_head;
    if (n > cap) {
        n = cap;
    }
    for (int i = 0; i < n; i++) {
        out[i] = um->req_ring[(um->req_head + i) % UM_REQ_CAP];
    }
    um->req_head += n;
    pthread_mutex_unlock(&um->mtx);
    *n_out = n;
    return 0;
}

int
um_release_requests(usage_meter_t* um, int n)
{
    (void)um;
    (void)n;
    /* head already advanced by um_drain_requests; no-op kept for caller
     * symmetry with the failure path (um_requeue_requests). */
    return 0;
}

int
um_requeue_requests(usage_meter_t* um, int n)
{
    if (um == NULL || um->req_ring == NULL || n <= 0) {
        return 0;
    }
    pthread_mutex_lock(&um->mtx);
    um->req_head -= n;
    if (um->req_head < um->req_tail - UM_REQ_CAP) {
        int lost = um->req_tail - UM_REQ_CAP - um->req_head;
        um->req_head = um->req_tail - UM_REQ_CAP;
        atomic_fetch_add(&um->req_dropped, lost);
        AIGATE_LOG_WARN("usage ring re-queue overflow: %d rows dropped "
                        "(total dropped: %d)",
                        lost,
                        atomic_load(&um->req_dropped));
    }
    pthread_mutex_unlock(&um->mtx);
    return 0;
}

int
um_requests_dropped(const usage_meter_t* um)
{
    return um == NULL ? 0 : atomic_load((atomic_int*)&um->req_dropped);
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

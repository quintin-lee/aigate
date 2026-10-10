/** @file metrics.c
 *  @brief Prometheus text rendering + ACL check (see metrics.h). */
#include "metrics.h"
#include "aigate_log.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model_router.h"

#define METRICS_MAX_REJECTIONS 128

typedef struct {
    char        model[128];
    atomic_long count;
    int         in_use;
} rejection_metric_entry_t;

static rejection_metric_entry_t g_rejections[METRICS_MAX_REJECTIONS];
static pthread_mutex_t          g_rejection_mtx = PTHREAD_MUTEX_INITIALIZER;

void
metrics_inc_concurrency_rejected(const char* model)
{
    const char* m = (model != NULL && model[0] != '\0') ? model : "unknown";
    pthread_mutex_lock(&g_rejection_mtx);
    for (int i = 0; i < METRICS_MAX_REJECTIONS; i++) {
        if (g_rejections[i].in_use && strcmp(g_rejections[i].model, m) == 0) {
            atomic_fetch_add(&g_rejections[i].count, 1);
            pthread_mutex_unlock(&g_rejection_mtx);
            return;
        }
    }
    for (int i = 0; i < METRICS_MAX_REJECTIONS; i++) {
        if (!g_rejections[i].in_use) {
            g_rejections[i].in_use = 1;
            snprintf(g_rejections[i].model, sizeof g_rejections[i].model, "%s", m);
            atomic_init(&g_rejections[i].count, 1);
            pthread_mutex_unlock(&g_rejection_mtx);
            return;
        }
    }
    pthread_mutex_unlock(&g_rejection_mtx);
}

long
metrics_get_concurrency_rejected(const char* model)
{
    pthread_mutex_lock(&g_rejection_mtx);
    if (model == NULL || model[0] == '\0') {
        long total = 0;
        for (int i = 0; i < METRICS_MAX_REJECTIONS; i++) {
            if (g_rejections[i].in_use) {
                total += atomic_load(&g_rejections[i].count);
            }
        }
        pthread_mutex_unlock(&g_rejection_mtx);
        return total;
    }
    for (int i = 0; i < METRICS_MAX_REJECTIONS; i++) {
        if (g_rejections[i].in_use && strcmp(g_rejections[i].model, model) == 0) {
            long val = atomic_load(&g_rejections[i].count);
            pthread_mutex_unlock(&g_rejection_mtx);
            return val;
        }
    }
    pthread_mutex_unlock(&g_rejection_mtx);
    return 0;
}

void
metrics_reset_concurrency_rejected(void)
{
    pthread_mutex_lock(&g_rejection_mtx);
    memset(g_rejections, 0, sizeof g_rejections);
    pthread_mutex_unlock(&g_rejection_mtx);
}

static int
appendf(char** w, size_t* rem, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*w, *rem, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *rem) {
        return -1;
    }
    *w += n;
    *rem -= (size_t)n;
    return 0;
}

static atomic_uint_fast64_t g_audit_events_info = 0;
static atomic_uint_fast64_t g_audit_events_warn = 0;
static atomic_uint_fast64_t g_audit_events_violation = 0;
static atomic_uint_fast64_t g_audit_events_error = 0;
static atomic_uint_fast64_t g_audit_dropped = 0;
static atomic_uint_fast64_t g_audit_webhook_success = 0;
static atomic_uint_fast64_t g_audit_webhook_failures = 0;

void
metrics_inc_audit_event(audit_severity_t sev)
{
    switch (sev) {
    case AUDIT_SEV_INFO:
        atomic_fetch_add(&g_audit_events_info, 1);
        break;
    case AUDIT_SEV_WARN:
        atomic_fetch_add(&g_audit_events_warn, 1);
        break;
    case AUDIT_SEV_VIOLATION:
        atomic_fetch_add(&g_audit_events_violation, 1);
        break;
    case AUDIT_SEV_ERROR:
        atomic_fetch_add(&g_audit_events_error, 1);
        break;
    }
}

void
metrics_inc_audit_dropped(uint64_t count)
{
    atomic_fetch_add(&g_audit_dropped, count);
}

void
metrics_inc_audit_webhook_success(void)
{
    atomic_fetch_add(&g_audit_webhook_success, 1);
}

void
metrics_inc_audit_webhook_failure(void)
{
    atomic_fetch_add(&g_audit_webhook_failures, 1);
}

void
metrics_reset_audit(void)
{
    atomic_store(&g_audit_events_info, 0);
    atomic_store(&g_audit_events_warn, 0);
    atomic_store(&g_audit_events_violation, 0);
    atomic_store(&g_audit_events_error, 0);
    atomic_store(&g_audit_dropped, 0);
    atomic_store(&g_audit_webhook_success, 0);
    atomic_store(&g_audit_webhook_failures, 0);
}

/** @brief Failover metrics table capacity (upper bound on model from_prov→to_prov entries). */
#define METRICS_MAX_FAILOVERS 128

/** @brief Failover migration counter entry: model + source/target provider + atomic count + in-use flag. */
typedef struct {
    char        model[128];    /**< Model name. */
    char        from_prov[32]; /**< Source provider. */
    char        to_prov[32];   /**< Target provider. */
    atomic_long count;         /**< Migration count (atomic). */
    int         in_use;        /**< Slot occupancy flag. */
} failover_metric_entry_t;

/** Failover metrics table (model from_prov→to_prov counts, guarded by g_failover_mtx). */
static failover_metric_entry_t g_failovers[METRICS_MAX_FAILOVERS];
/** Failover metrics table mutex. */
static pthread_mutex_t g_failover_mtx = PTHREAD_MUTEX_INITIALIZER;
/** Whether the table-full drop warning was already logged (warn once, avoids log flooding). */
static _Atomic int g_failover_warned = 0;
static atomic_long g_hedged_requests_total = 0;
static atomic_long g_hedged_won_total = 0;

void
metrics_inc_hedged_requests(void)
{
    atomic_fetch_add_explicit(&g_hedged_requests_total, 1, memory_order_relaxed);
}

void
metrics_inc_hedged_won(void)
{
    atomic_fetch_add_explicit(&g_hedged_won_total, 1, memory_order_relaxed);
}

long
metrics_total_hedged_requests(void)
{
    return atomic_load_explicit(&g_hedged_requests_total, memory_order_relaxed);
}

long
metrics_total_hedged_won(void)
{
    return atomic_load_explicit(&g_hedged_won_total, memory_order_relaxed);
}

#define TTFT_MAX_PROVIDERS 16
#define TTFT_NUM_BUCKETS 9

static const long TTFT_BUCKETS_NS[TTFT_NUM_BUCKETS] = {
    50000000L,    /* 0.05s / 50ms */
    100000000L,   /* 0.1s / 100ms */
    250000000L,   /* 0.25s / 250ms */
    500000000L,   /* 0.5s / 500ms */
    1000000000L,  /* 1.0s */
    2500000000L,  /* 2.5s */
    5000000000L,  /* 5.0s */
    10000000000L, /* 10.0s */
    30000000000L, /* 30.0s */
};

static const char* TTFT_BUCKET_LE[TTFT_NUM_BUCKETS] = {
    "0.05", "0.1", "0.25", "0.5", "1", "2.5", "5", "10", "30"};

typedef struct provider_ttft {
    int         in_use;
    char        provider[32];
    atomic_long buckets[TTFT_NUM_BUCKETS];
    atomic_long count;
    atomic_long sum_ns;
} provider_ttft_t;

static provider_ttft_t g_ttft_table[TTFT_MAX_PROVIDERS];
static pthread_mutex_t g_ttft_mtx = PTHREAD_MUTEX_INITIALIZER;

void
metrics_record_upstream_ttft(const char* provider, uint64_t ttft_ns)
{
    const char* p = (provider != NULL && provider[0] != '\0') ? provider : "unknown";

    provider_ttft_t* entry = NULL;
    pthread_mutex_lock(&g_ttft_mtx);
    for (int i = 0; i < TTFT_MAX_PROVIDERS; i++) {
        if (g_ttft_table[i].in_use && strcmp(g_ttft_table[i].provider, p) == 0) {
            entry = &g_ttft_table[i];
            break;
        }
    }
    if (entry == NULL) {
        for (int i = 0; i < TTFT_MAX_PROVIDERS; i++) {
            if (!g_ttft_table[i].in_use) {
                g_ttft_table[i].in_use = 1;
                snprintf(g_ttft_table[i].provider, sizeof(g_ttft_table[i].provider), "%s", p);
                for (int b = 0; b < TTFT_NUM_BUCKETS; b++) {
                    atomic_init(&g_ttft_table[i].buckets[b], 0);
                }
                atomic_init(&g_ttft_table[i].count, 0);
                atomic_init(&g_ttft_table[i].sum_ns, 0);
                entry = &g_ttft_table[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_ttft_mtx);

    if (entry != NULL) {
        for (int b = 0; b < TTFT_NUM_BUCKETS; b++) {
            if (ttft_ns <= (uint64_t)TTFT_BUCKETS_NS[b]) {
                atomic_fetch_add(&entry->buckets[b], 1);
            }
        }
        atomic_fetch_add(&entry->count, 1);
        atomic_fetch_add(&entry->sum_ns, (long)ttft_ns);
    }
}

void
metrics_reset_ttft(void)
{
    pthread_mutex_lock(&g_ttft_mtx);
    memset(g_ttft_table, 0, sizeof g_ttft_table);
    pthread_mutex_unlock(&g_ttft_mtx);
}

long
metrics_get_ttft_count(const char* provider)
{
    const char* p = (provider != NULL && provider[0] != '\0') ? provider : "unknown";
    pthread_mutex_lock(&g_ttft_mtx);
    for (int i = 0; i < TTFT_MAX_PROVIDERS; i++) {
        if (g_ttft_table[i].in_use && strcmp(g_ttft_table[i].provider, p) == 0) {
            long c = atomic_load(&g_ttft_table[i].count);
            pthread_mutex_unlock(&g_ttft_mtx);
            return c;
        }
    }
    pthread_mutex_unlock(&g_ttft_mtx);
    return 0;
}

/** @brief Latency histogram bucket count (matches BUCKET_LE length). */
#define NUM_BUCKETS 6
/** Latency histogram bucket upper bounds (nanoseconds, as strings, matching exposition output). */
static const char* BUCKET_LE[NUM_BUCKETS] = {
    "5000000", "50000000", "500000000", "2000000000", "5000000000", "10000000000"};

int
metrics_render(usage_meter_t* um, char* out, size_t cap)
{
    char*  w = out;
    size_t rem = cap;
    int    n;

    long total_reqs = um != NULL ? um_total_requests(um) : 0;
    long total_errs = um != NULL ? um_total_errors(um) : 0;
    long total_toks = um != NULL ? um_total_tokens(um) : 0;
    long total_cached = um != NULL ? um_total_cached_tokens(um) : 0;

    n = snprintf(w,
                 rem,
                 "# HELP aigate_requests_total Total gateway requests.\n"
                 "# TYPE aigate_requests_total counter\n"
                 "aigate_requests_total %ld\n"
                 "# HELP aigate_errors_total Total 5xx+ client errors.\n"
                 "# TYPE aigate_errors_total counter\n"
                 "aigate_errors_total %ld\n"
                 "# HELP aigate_tokens_total Total prompt+completion tokens.\n"
                 "# TYPE aigate_tokens_total counter\n"
                 "aigate_tokens_total %ld\n"
                 "# HELP aigate_tokens_cached_total Total cached prompt tokens.\n"
                 "# TYPE aigate_tokens_cached_total counter\n"
                 "aigate_tokens_cached_total %ld\n"
                 "# HELP aigate_hedged_requests_total Total hedged backup requests issued.\n"
                 "# TYPE aigate_hedged_requests_total counter\n"
                 "aigate_hedged_requests_total %ld\n"
                 "# HELP aigate_hedged_won_total Total hedged requests that won the race.\n"
                 "# TYPE aigate_hedged_won_total counter\n"
                 "aigate_hedged_won_total %ld\n",
                 total_reqs,
                 total_errs,
                 total_toks,
                 total_cached,
                 metrics_total_hedged_requests(),
                 metrics_total_hedged_won());
    if (n < 0 || (size_t)n >= rem) {
        return -1;
    }
    w += n;
    rem -= (size_t)n;

    if (um != NULL) {
        char provs[16][32];
        long counts[16];
        int  nprov = um_provider_names(um, (char(*)[32])provs, 16);
        int  nlive = 0;
        for (int i = 0; i < nprov; i++) {
            counts[i] = um_provider_sampled(um, provs[i]);
            if (counts[i] > 0) {
                nlive++;
            }
        }

        /* Prometheus text format: every sample of a family must be contiguous
         * and HELP/TYPE may appear only once per family, so emit each family
         * in its own pass across providers. */
        if (nlive > 0) {
            if (appendf(&w,
                        &rem,
                        "# HELP aigate_upstream_requests_total Upstream calls by provider.\n"
                        "# TYPE aigate_upstream_requests_total counter\n") != 0) {
                return -1;
            }
            for (int i = 0; i < nprov; i++) {
                if (counts[i] > 0 &&
                    appendf(&w,
                            &rem,
                            "aigate_upstream_requests_total{provider=\"%s\"} %ld\n",
                            provs[i],
                            counts[i]) != 0) {
                    return -1;
                }
            }

            if (appendf(&w,
                        &rem,
                        "# HELP aigate_upstream_latency_ns_ns Upstream latency histogram (ns).\n"
                        "# TYPE aigate_upstream_latency_ns_ns histogram\n") != 0) {
                return -1;
            }
            for (int i = 0; i < nprov; i++) {
                if (counts[i] <= 0) {
                    continue;
                }
                const char* p = provs[i];
                long        sum_ns = (long)(um_provider_mean_ns(um, p) * (double)counts[i]);
                for (int b = 0; b < NUM_BUCKETS; b++) {
                    if (appendf(
                            &w,
                            &rem,
                            "aigate_upstream_latency_ns_ns_bucket{provider=\"%s\",le=\"%s\"} %ld\n",
                            p,
                            BUCKET_LE[b],
                            um_provider_count_below_ns(um, p, atol(BUCKET_LE[b]))) != 0) {
                        return -1;
                    }
                }
                if (appendf(
                        &w,
                        &rem,
                        "aigate_upstream_latency_ns_ns_bucket{provider=\"%s\",le=\"+Inf\"} %ld\n"
                        "aigate_upstream_latency_ns_ns_sum{provider=\"%s\"} %ld\n"
                        "aigate_upstream_latency_ns_ns_count{provider=\"%s\"} %ld\n",
                        p,
                        counts[i],
                        p,
                        sum_ns,
                        p,
                        counts[i]) != 0) {
                    return -1;
                }
            }
        }
    }

    /* Failover counters */
    pthread_mutex_lock(&g_failover_mtx);
    int have_failovers = 0;
    for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
        if (g_failovers[i].in_use && atomic_load(&g_failovers[i].count) > 0) {
            have_failovers = 1;
            break;
        }
    }
    if (have_failovers) {
        n = snprintf(w,
                     rem,
                     "# HELP aigate_failover_total Total failovers between upstream targets.\n"
                     "# TYPE aigate_failover_total counter\n");
        if (n < 0 || (size_t)n >= rem) {
            pthread_mutex_unlock(&g_failover_mtx);
            return -1;
        }
        w += n;
        rem -= (size_t)n;

        for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
            if (!g_failovers[i].in_use) {
                continue;
            }
            long c = atomic_load(&g_failovers[i].count);
            if (c <= 0) {
                continue;
            }
            n = snprintf(
                w,
                rem,
                "aigate_failover_total{model=\"%s\",from_provider=\"%s\",to_provider=\"%s\"} %ld\n",
                g_failovers[i].model,
                g_failovers[i].from_prov,
                g_failovers[i].to_prov,
                c);
            if (n < 0 || (size_t)n >= rem) {
                pthread_mutex_unlock(&g_failover_mtx);
                return -1;
            }
            w += n;
            rem -= (size_t)n;
        }
    }
    pthread_mutex_unlock(&g_failover_mtx);

    /* TTFT histogram */
    pthread_mutex_lock(&g_ttft_mtx);
    int have_ttft = 0;
    for (int i = 0; i < TTFT_MAX_PROVIDERS; i++) {
        if (g_ttft_table[i].in_use && atomic_load(&g_ttft_table[i].count) > 0) {
            have_ttft = 1;
            break;
        }
    }
    if (have_ttft) {
        n = snprintf(
            w,
            rem,
            "# HELP aigate_upstream_ttft_seconds Time to first token histogram by provider "
            "(seconds).\n"
            "# TYPE aigate_upstream_ttft_seconds histogram\n");
        if (n < 0 || (size_t)n >= rem) {
            pthread_mutex_unlock(&g_ttft_mtx);
            return -1;
        }
        w += n;
        rem -= (size_t)n;

        for (int i = 0; i < TTFT_MAX_PROVIDERS; i++) {
            if (!g_ttft_table[i].in_use) {
                continue;
            }
            long count = atomic_load(&g_ttft_table[i].count);
            if (count == 0) {
                continue;
            }
            long        sum_ns = atomic_load(&g_ttft_table[i].sum_ns);
            const char* p = g_ttft_table[i].provider;

            for (int b = 0; b < TTFT_NUM_BUCKETS; b++) {
                long bcount = atomic_load(&g_ttft_table[i].buckets[b]);
                n = snprintf(w,
                             rem,
                             "aigate_upstream_ttft_seconds_bucket{provider=\"%s\",le=\"%s\"} %ld\n",
                             p,
                             TTFT_BUCKET_LE[b],
                             bcount);
                if (n < 0 || (size_t)n >= rem) {
                    pthread_mutex_unlock(&g_ttft_mtx);
                    return -1;
                }
                w += n;
                rem -= (size_t)n;
            }
            n = snprintf(w,
                         rem,
                         "aigate_upstream_ttft_seconds_bucket{provider=\"%s\",le=\"+Inf\"} %ld\n"
                         "aigate_upstream_ttft_seconds_sum{provider=\"%s\"} %.6f\n"
                         "aigate_upstream_ttft_seconds_count{provider=\"%s\"} %ld\n",
                         p,
                         count,
                         p,
                         (double)sum_ns / 1000000000.0,
                         p,
                         count);
            if (n < 0 || (size_t)n >= rem) {
                pthread_mutex_unlock(&g_ttft_mtx);
                return -1;
            }
            w += n;
            rem -= (size_t)n;
        }
    }
    pthread_mutex_unlock(&g_ttft_mtx);

    /* Concurrency rejection counters */
    pthread_mutex_lock(&g_rejection_mtx);
    int have_rejections = 0;
    for (int i = 0; i < METRICS_MAX_REJECTIONS; i++) {
        if (g_rejections[i].in_use && atomic_load(&g_rejections[i].count) > 0) {
            have_rejections = 1;
            break;
        }
    }
    if (have_rejections) {
        if (appendf(&w,
                    &rem,
                    "# HELP aigate_concurrency_rejected_total Total requests rejected due to "
                    "concurrency saturation.\n"
                    "# TYPE aigate_concurrency_rejected_total counter\n") != 0) {
            pthread_mutex_unlock(&g_rejection_mtx);
            return -1;
        }
        for (int i = 0; i < METRICS_MAX_REJECTIONS; i++) {
            if (!g_rejections[i].in_use) {
                continue;
            }
            long c = atomic_load(&g_rejections[i].count);
            if (c <= 0) {
                continue;
            }
            if (appendf(&w,
                        &rem,
                        "aigate_concurrency_rejected_total{model=\"%s\"} %ld\n",
                        g_rejections[i].model,
                        c) != 0) {
                pthread_mutex_unlock(&g_rejection_mtx);
                return -1;
            }
        }
    }
    pthread_mutex_unlock(&g_rejection_mtx);

    /* Upstream in-flight concurrency gauge */
    char eps[128][512];
    int  inflights[128];
    int  n_inflight = model_router_snapshot_in_flight(eps, inflights, 128);
    if (n_inflight > 0) {
        if (appendf(
                &w,
                &rem,
                "# HELP aigate_upstream_inflight Current in-flight requests per upstream target.\n"
                "# TYPE aigate_upstream_inflight gauge\n") != 0) {
            return -1;
        }
        for (int i = 0; i < n_inflight; i++) {
            if (appendf(&w,
                        &rem,
                        "aigate_upstream_inflight{endpoint=\"%s\"} %d\n",
                        eps[i],
                        inflights[i]) != 0) {
                return -1;
            }
        }
    }

    /* Audit logger metrics */
    uint64_t a_info = atomic_load(&g_audit_events_info);
    uint64_t a_warn = atomic_load(&g_audit_events_warn);
    uint64_t a_viol = atomic_load(&g_audit_events_violation);
    uint64_t a_err = atomic_load(&g_audit_events_error);
    uint64_t a_drop = atomic_load(&g_audit_dropped);
    uint64_t a_ws = atomic_load(&g_audit_webhook_success);
    uint64_t a_wf = atomic_load(&g_audit_webhook_failures);

    if (a_info > 0 || a_warn > 0 || a_viol > 0 || a_err > 0 || a_drop > 0 || a_ws > 0 || a_wf > 0) {
        if (appendf(&w,
                    &rem,
                    "# HELP aigate_audit_events_total Total audit events recorded by severity.\n"
                    "# TYPE aigate_audit_events_total counter\n") != 0) {
            return -1;
        }
        if (a_info > 0) {
            if (appendf(&w,
                        &rem,
                        "aigate_audit_events_total{severity=\"info\"} %llu\n",
                        (unsigned long long)a_info) != 0) {
                return -1;
            }
        }
        if (a_warn > 0) {
            if (appendf(&w,
                        &rem,
                        "aigate_audit_events_total{severity=\"warn\"} %llu\n",
                        (unsigned long long)a_warn) != 0) {
                return -1;
            }
        }
        if (a_viol > 0) {
            if (appendf(&w,
                        &rem,
                        "aigate_audit_events_total{severity=\"violation\"} %llu\n",
                        (unsigned long long)a_viol) != 0) {
                return -1;
            }
        }
        if (a_err > 0) {
            if (appendf(&w,
                        &rem,
                        "aigate_audit_events_total{severity=\"error\"} %llu\n",
                        (unsigned long long)a_err) != 0) {
                return -1;
            }
        }

        if (appendf(&w,
                    &rem,
                    "# HELP aigate_audit_dropped_total Total audit events dropped due to ring "
                    "buffer overflow.\n"
                    "# TYPE aigate_audit_dropped_total counter\n"
                    "aigate_audit_dropped_total %llu\n"
                    "# HELP aigate_audit_webhook_success_total Total audit webhook alerts "
                    "delivered successfully.\n"
                    "# TYPE aigate_audit_webhook_success_total counter\n"
                    "aigate_audit_webhook_success_total %llu\n"
                    "# HELP aigate_audit_webhook_failures_total Total audit webhook alert delivery "
                    "failures.\n"
                    "# TYPE aigate_audit_webhook_failures_total counter\n"
                    "aigate_audit_webhook_failures_total %llu\n",
                    (unsigned long long)a_drop,
                    (unsigned long long)a_ws,
                    (unsigned long long)a_wf) != 0) {
            return -1;
        }
    }

    *w = '\0';
    return 0;
}

char*
metrics_render_alloc(usage_meter_t* um, size_t* out_len)
{
    size_t       cap = 65536;
    const size_t max_cap = 16 * 1024 * 1024; /* 16 MB cap */
    while (cap <= max_cap) {
        char* buf = malloc(cap);
        if (buf == NULL) {
            return NULL;
        }
        if (metrics_render(um, buf, cap) == 0) {
            if (out_len != NULL) {
                *out_len = strlen(buf);
            }
            return buf;
        }
        free(buf);
        cap *= 2;
    }
    return NULL;
}

void
metrics_inc_failover(const char* model, const char* from_prov, const char* to_prov)
{
    const char* m = (model != NULL && model[0] != '\0') ? model : "unknown";
    const char* f = (from_prov != NULL && from_prov[0] != '\0') ? from_prov : "unknown";
    const char* t = (to_prov != NULL && to_prov[0] != '\0') ? to_prov : "unknown";

    pthread_mutex_lock(&g_failover_mtx);
    for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
        if (g_failovers[i].in_use && strcmp(g_failovers[i].model, m) == 0 &&
            strcmp(g_failovers[i].from_prov, f) == 0 && strcmp(g_failovers[i].to_prov, t) == 0) {
            atomic_fetch_add(&g_failovers[i].count, 1);
            pthread_mutex_unlock(&g_failover_mtx);
            return;
        }
    }
    for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
        if (!g_failovers[i].in_use) {
            g_failovers[i].in_use = 1;
            snprintf(g_failovers[i].model, sizeof g_failovers[i].model, "%s", m);
            snprintf(g_failovers[i].from_prov, sizeof g_failovers[i].from_prov, "%s", f);
            snprintf(g_failovers[i].to_prov, sizeof g_failovers[i].to_prov, "%s", t);
            atomic_init(&g_failovers[i].count, 1);
            pthread_mutex_unlock(&g_failover_mtx);
            return;
        }
    }
    /* Full table: new model/from/to triples are dropped. Warn once so the
     * silent under-counting is visible without flooding the log. */
    pthread_mutex_unlock(&g_failover_mtx);
    if (atomic_exchange(&g_failover_warned, 1) == 0) {
        AIGATE_LOG_WARN("metrics: failover table full (%d slots); new triples no longer counted",
                        METRICS_MAX_FAILOVERS);
    }
    return;
}

long
metrics_get_failover(const char* model, const char* from_prov, const char* to_prov)
{
    const char* m = (model != NULL && model[0] != '\0') ? model : "unknown";
    const char* f = (from_prov != NULL && from_prov[0] != '\0') ? from_prov : "unknown";
    const char* t = (to_prov != NULL && to_prov[0] != '\0') ? to_prov : "unknown";

    pthread_mutex_lock(&g_failover_mtx);
    long val = 0;
    for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
        if (g_failovers[i].in_use && strcmp(g_failovers[i].model, m) == 0 &&
            strcmp(g_failovers[i].from_prov, f) == 0 && strcmp(g_failovers[i].to_prov, t) == 0) {
            val = atomic_load(&g_failovers[i].count);
            break;
        }
    }
    pthread_mutex_unlock(&g_failover_mtx);
    return val;
}

long
metrics_total_failovers(void)
{
    pthread_mutex_lock(&g_failover_mtx);
    long total = 0;
    for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
        if (g_failovers[i].in_use) {
            total += atomic_load(&g_failovers[i].count);
        }
    }
    pthread_mutex_unlock(&g_failover_mtx);
    return total;
}

void
metrics_reset_failovers(void)
{
    pthread_mutex_lock(&g_failover_mtx);
    memset(g_failovers, 0, sizeof g_failovers);
    pthread_mutex_unlock(&g_failover_mtx);
}

int
metrics_acl_allows(const char* ip, const char* acl)
{
    if (acl == NULL || acl[0] == '\0') {
        return 1;
    }
    if (ip == NULL || ip[0] == '\0') {
        return 0;
    }
    const char* clean_ip = ip;
    if (strcmp(clean_ip, "::1") == 0 || strcmp(clean_ip, "0:0:0:0:0:0:0:1") == 0) {
        clean_ip = "127.0.0.1";
    } else if (strncmp(clean_ip, "::ffff:", 7) == 0) {
        clean_ip += 7;
    }
    struct in_addr addr;
    if (inet_pton(AF_INET, clean_ip, &addr) != 1) {
        return 0;
    }
    const char* p = acl;
    while (*p) {
        /* skip commas/spaces */
        while (*p == ',' || *p == ' ') {
            p++;
        }
        const char* entry = p;
        while (*p && *p != ',' && *p != ' ') {
            p++;
        }
        char entry_buf[64];
        int  elen = (int)(p - entry);
        if (elen > 0 && elen < (int)sizeof entry_buf) {
            memcpy(entry_buf, entry, (size_t)elen);
            entry_buf[elen] = '\0';
            int   cidr = 32;
            char* slash = strchr(entry_buf, '/');
            if (slash != NULL) {
                *slash = '\0';
                cidr = atoi(slash + 1);
                if (cidr < 0 || cidr > 32) {
                    cidr = 32;
                }
            }
            struct in_addr net;
            if (inet_pton(AF_INET, entry_buf, &net) != 1) {
                continue;
            }
            uint32_t a = ntohl(addr.s_addr);
            uint32_t nmask = cidr == 0 ? 0u : (~0u << (32 - cidr));
            if ((a & nmask) == (ntohl(net.s_addr) & nmask)) {
                return 1;
            }
        }
    }
    return 0;
}

/** @file metrics.c
 *  @brief Prometheus text rendering + ACL check (see metrics.h). */
#include "metrics.h"
#include "aigate_log.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define METRICS_MAX_FAILOVERS 128

typedef struct {
    char        model[128];
    char        from_prov[32];
    char        to_prov[32];
    atomic_long count;
    int         in_use;
} failover_metric_entry_t;

static failover_metric_entry_t g_failovers[METRICS_MAX_FAILOVERS];
static pthread_mutex_t         g_failover_mtx = PTHREAD_MUTEX_INITIALIZER;

#define NUM_BUCKETS 6
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
                 "aigate_tokens_cached_total %ld\n",
                 total_reqs,
                 total_errs,
                 total_toks,
                 total_cached);
    if (n < 0 || (size_t)n >= rem) {
        return -1;
    }
    w += n;
    rem -= (size_t)n;

    if (um != NULL) {
        char provs[8][32];
        int  nprov = 0;
        /* re-derive provider names through the meter accessor; cap 8 */
        nprov = um_provider_names(um, (char (*)[32])provs, 8);

    for (int i = 0; i < nprov; i++) {
        const char* p = provs[i];
        long        count = um_provider_sampled(um, p);
        if (count == 0) {
            continue; /* no samples yet for this provider */
        }

        n = snprintf(w,
                     rem,
                     "# HELP aigate_upstream_requests_total Upstream calls by provider.\n"
                     "# TYPE aigate_upstream_requests_total counter\n"
                     "aigate_upstream_requests_total{provider=\"%s\"} %ld\n",
                     p,
                     count);
        if (n < 0 || (size_t)n >= rem) {
            return -1;
        }
        w += n;
        rem -= (size_t)n;

        double mean = um_provider_mean_ns(um, p);
        long   sum_ns = (long)(mean * count);

        n = snprintf(w,
                     rem,
                     "# HELP aigate_upstream_latency_ns_ns Upstream latency histogram (ns).\n"
                     "# TYPE aigate_upstream_latency_ns_ns histogram\n");
        if (n < 0 || (size_t)n >= rem) {
            return -1;
        }
        w += n;
        rem -= (size_t)n;

        for (int b = 0; b < NUM_BUCKETS; b++) {
            long le_ns = atol(BUCKET_LE[b]);
            n = snprintf(w,
                         rem,
                         "aigate_upstream_latency_ns_ns_bucket{provider=\"%s\",le=\"%s\"} %ld\n",
                         p,
                         BUCKET_LE[b],
                         um_provider_count_below_ns(um, p, le_ns));
            if (n < 0 || (size_t)n >= rem) {
                return -1;
            }
            w += n;
            rem -= (size_t)n;
        }
        n = snprintf(w,
                     rem,
                     "aigate_upstream_latency_ns_ns_bucket{provider=\"%s\",le=\"+Inf\"} %ld\n"
                     "aigate_upstream_latency_ns_ns_sum{provider=\"%s\"} %ld\n"
                     "aigate_upstream_latency_ns_ns_count{provider=\"%s\"} %ld\n",
                     p,
                     (long)sum_ns,
                     p,
                     (long)sum_ns,
                     p,
                     count);
        if (n < 0 || (size_t)n >= rem) {
            return -1;
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
            n = snprintf(w,
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

    *w = '\0';
    return 0;
}

void
metrics_inc_failover(const char* model, const char* from_prov, const char* to_prov)
{
    const char* m = (model != NULL && model[0] != '\0') ? model : "unknown";
    const char* f = (from_prov != NULL && from_prov[0] != '\0') ? from_prov : "unknown";
    const char* t = (to_prov != NULL && to_prov[0] != '\0') ? to_prov : "unknown";

    pthread_mutex_lock(&g_failover_mtx);
    for (int i = 0; i < METRICS_MAX_FAILOVERS; i++) {
        if (g_failovers[i].in_use &&
            strcmp(g_failovers[i].model, m) == 0 &&
            strcmp(g_failovers[i].from_prov, f) == 0 &&
            strcmp(g_failovers[i].to_prov, t) == 0) {
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
    pthread_mutex_unlock(&g_failover_mtx);
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
        if (g_failovers[i].in_use &&
            strcmp(g_failovers[i].model, m) == 0 &&
            strcmp(g_failovers[i].from_prov, f) == 0 &&
            strcmp(g_failovers[i].to_prov, t) == 0) {
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
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
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

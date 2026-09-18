/** @file metrics.c
 *  @brief Prometheus text rendering + ACL check (see metrics.h). */
#include "metrics.h"
#include "aigate_log.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BUCKETS 6
static const char* BUCKET_LE[NUM_BUCKETS] = {
    "5000000", "50000000", "500000000", "2000000000", "5000000000", "10000000000"};

int
metrics_render(usage_meter_t* um, char* out, size_t cap)
{
    char*  w = out;
    size_t rem = cap;
    int    n;

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
                 "aigate_tokens_total %ld\n",
                 um_total_requests(um),
                 um_total_errors(um),
                 um_total_tokens(um));
    if (n < 0 || (size_t)n >= rem) {
        return -1;
    }
    w += n;
    rem -= (size_t)n;

    char provs[4][32];
    int  nprov = 0;
    /* re-derive provider names through the meter accessor; cap 4 */
    nprov = um_provider_names(um, (char (*)[32])provs, 4);

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
    *w = '\0';
    return 0;
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

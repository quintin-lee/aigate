/** @file metrics.h
 *  @brief Prometheus text exposition + source-IP ACL check. */
#ifndef AIGATE_METRICS_H
#define AIGATE_METRICS_H

#include "usage_meter.h"

/** @brief Render the current Prometheus text exposition into @p out
 *  (NUL-terminated; caller supplies capacity ≥ metrics_render capacity,
 *  e.g. 16 KB). Truncation when out is too small → returns -1.
 *
 *  Exposed:
 *    aigate_requests_total (counter)
 *    aigate_errors_total   (counter)
 *    aigate_tokens_total   (counter)
 *    aigate_upstream_requests_total{provider="<p>"} (per-provider counter)
 *    aigate_upstream_latency_ns_bucket{provider="<p>",le="<v>"} 0/inf
 *    aigate_upstream_latency_ns_sum{provider="<p>"}
 *    aigate_upstream_latency_ns_count{provider="<p>"}
 */
int metrics_render(usage_meter_t* um, char* out, size_t cap);

/** @brief 1 when @p ip (dotted-quad string) is contained in the comma-
 *  separated CIDR/IPv4 list @p acl ("127.0.0.1,10.0.0.0/8").
 *  @note ACL is IPv4-only by design (spec §5); empty @p acl → allow all. */
int metrics_acl_allows(const char* ip, const char* acl);

#endif /* AIGATE_METRICS_H */

/** @file metrics.h
 *  @ingroup group_observe
 *  @brief Prometheus text exposition + source-IP ACL check. */
#ifndef AIGATE_METRICS_H
#define AIGATE_METRICS_H

#include "usage_meter.h"

/** @brief Render a usage_meter snapshot as Prometheus text exposition.
 *  @param um   Usage table (NULL means no data; static metric headers are still emitted).
 *  @param out  Output buffer; @param cap its capacity.
 *  @return Bytes written (excluding NUL); -1 when the buffer is too small. */
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

/** @brief Increment failover counter for model from_prov -> to_prov. */
void metrics_inc_failover(const char* model, const char* from_prov, const char* to_prov);

/** @brief Sample count of failovers for (model, from_prov, to_prov). */
long metrics_get_failover(const char* model, const char* from_prov, const char* to_prov);

/** @brief Lifetime total of all failovers. */
long metrics_total_failovers(void);

/** @brief Reset failover metrics (for tests). */
void metrics_reset_failovers(void);

/** @brief Increment hedged request counter. */
void metrics_inc_hedged_requests(void);

/** @brief Increment hedged won counter. */
void metrics_inc_hedged_won(void);

/** @brief Lifetime total of all hedged requests. */
long metrics_total_hedged_requests(void);

/** @brief Lifetime total of all hedged won requests. */
long metrics_total_hedged_won(void);

/** @brief Record an upstream Time To First Token (TTFT) sample in nanoseconds. */
void metrics_record_upstream_ttft(const char* provider, uint64_t ttft_ns);

/** @brief Reset TTFT metrics (for tests). */
void metrics_reset_ttft(void);

/** @brief Sample count of TTFT recordings for @p provider. */
long metrics_get_ttft_count(const char* provider);

/** @brief Render into a heap buffer that grows as needed (64 KB → 16 MB cap).
 *  @param um      Usage table (may be NULL).
 *  @param out_len Optional; receives the rendered length (excluding NUL).
 *  @return malloc'd NUL-terminated text (caller frees), or NULL on OOM / > cap. */
char* metrics_render_alloc(usage_meter_t* um, size_t* out_len);

/** @brief Count a request rejected with 429 because every candidate target
 *  for @p model hit its max_concurrent limit. */
void metrics_inc_concurrency_rejected(const char* model);

/** @brief Rejected-by-concurrency count for @p model (NULL → lifetime total). */
long metrics_get_concurrency_rejected(const char* model);

/** @brief Reset concurrency-rejection counters (for tests). */
void metrics_reset_concurrency_rejected(void);

/** @brief 1 when @p ip (dotted-quad string) is contained in the comma-
 *  separated CIDR/IPv4 list @p acl ("127.0.0.1,10.0.0.0/8").
 *  @note ACL is IPv4-only by design (spec §5); empty @p acl → allow all. */
int metrics_acl_allows(const char* ip, const char* acl);

#include "audit_logger.h"

/** @brief Increment audit event counter by severity. */
void metrics_inc_audit_event(audit_severity_t sev);

/** @brief Increment audit dropped counter. */
void metrics_inc_audit_dropped(uint64_t count);

/** @brief Increment audit webhook success counter. */
void metrics_inc_audit_webhook_success(void);

/** @brief Increment audit webhook failure counter. */
void metrics_inc_audit_webhook_failure(void);

/** @brief Reset audit metrics counters (for tests). */
void metrics_reset_audit(void);

#endif /* AIGATE_METRICS_H */

/** @file usage_meter.h
 *  @brief Atomic counters + HDR per-provider latency histograms; 5s PG flush.
 *
 *  Hot path (um_record): atomics + one mutex-taken row accumulate + one
 *  HDR record. All PostgreSQL writes are batched by a background worker
 *  that drains the daily accumulator every flush_interval_s and calls
 *  pg_ops->flush_usage.
 */
#ifndef AIGATE_USAGE_METER_H
#define AIGATE_USAGE_METER_H

#include <stdint.h>
#include "pg_store.h"

typedef struct usage_meter usage_meter_t;

/** @brief Create a meter. flush_interval_s <= 0 disables the background
 *  worker (test mode: call um_drain manually). @return NULL on failure. */
usage_meter_t* usage_meter_new(pg_store_t* ps, int flush_interval_s);

/** @brief Stop the worker, perform a final drain + flush, and free. */
void usage_meter_free(usage_meter_t* um);

/** @brief Record one completed request. @p http_status is the final
 *  status sent to the client; status >= 500 counts as an error.
 *  @p prompt_tokens/@p completion_tokens are upstream usage (0 when
 *  unavailable). @p latency_ns goes to the provider HDR when @p provider
 *  is a known label ("openai","ollama","azure"); NULL/unknown skips it. */
void um_record(usage_meter_t* um,
               long           key_id,
               const char*    model,
               int            http_status,
               long           prompt_tokens,
               long           completion_tokens,
               uint64_t       latency_ns,
               const char*    provider);

/** @brief Lifetime totals (for /metrics). */
long um_total_requests(usage_meter_t* um);
long um_total_errors(usage_meter_t* um);
long um_total_tokens(usage_meter_t* um);

/** @brief Drain the daily accumulator map into @p out (up to @p cap rows),
 *  then flush via pg_ops->flush_usage. Empty map → *n_out = 0, ok.
 *  @return 0 ok; -1 when out would overflow cap or the flush call failed.
 *  @note Rows are value-copied; the caller owns @p out. */
int um_drain(usage_meter_t* um, usage_row_t* out, int cap, int* n_out);

/* Provider histogram accessors for metrics_render. */
/** @brief Fills @p names (char[32] each) with providers that recorded
 *  samples. @return count written (<= cap). */
int um_provider_names(usage_meter_t* um, char (*names)[32], int cap);
/** @brief Sample count for @p provider (0 when unknown). */
long um_provider_sampled(usage_meter_t* um, const char* provider);
/** @brief Percentile (50/90/99) in ns; 0 when the provider has no samples. */
long um_provider_percentile_ns(usage_meter_t* um, const char* provider, double percentile);
/** @brief Mean latency in ns; 0 when no samples. */
double um_provider_mean_ns(usage_meter_t* um, const char* provider);
/** @brief Number of recorded samples with latency <= @p le_ns. */
long um_provider_count_below_ns(usage_meter_t* um, const char* provider, long le_ns);

#endif /* AIGATE_USAGE_METER_H */

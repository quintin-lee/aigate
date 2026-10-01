/** @file latency_tracker.h
 *  @ingroup group_policy
 *  @brief P95 & EWMA latency tracking and hedge budget enforcement.
 */
#ifndef AIGATE_LATENCY_TRACKER_H
#define AIGATE_LATENCY_TRACKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LATENCY_TRACKER_WINDOW_SZ 64
#define LATENCY_TRACKER_MAX_ENTRIES 512

typedef struct latency_tracker latency_tracker_t;

/** @brief Create a new latency tracker instance. */
latency_tracker_t* latency_tracker_create(void);

/** @brief Free a latency tracker instance. */
void latency_tracker_destroy(latency_tracker_t* lt);

/** @brief Record an upstream latency sample in nanoseconds. */
void latency_tracker_record(latency_tracker_t* lt,
                            const char*        model,
                            const char*        endpoint,
                            uint64_t           latency_ns);

/** @brief Return the P95 latency in milliseconds for the given model/endpoint.
 *  Returns default 1000ms if no samples recorded yet. */
uint32_t latency_tracker_get_p95_ms(latency_tracker_t* lt, const char* model, const char* endpoint);

/** @brief Return the EWMA latency in milliseconds for the given model/endpoint. */
uint32_t
latency_tracker_get_ewma_ms(latency_tracker_t* lt, const char* model, const char* endpoint);

/** @brief Check whether a hedged request is admitted for the model under hedge_budget_pct. */
bool latency_tracker_hedge_admitted(latency_tracker_t* lt, const char* model, int budget_pct);

/** @brief Increment total request count for model (called when a request begins). */
void latency_tracker_record_request(latency_tracker_t* lt, const char* model);

/** @brief Increment hedged request count for model (called when a hedge is dispatched). */
void latency_tracker_record_hedge(latency_tracker_t* lt, const char* model);

/** @brief Reset all tracked entries (for testing). */
void latency_tracker_reset(latency_tracker_t* lt);

#endif /* AIGATE_LATENCY_TRACKER_H */

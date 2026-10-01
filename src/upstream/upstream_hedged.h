/** @file upstream_hedged.h
 *  @ingroup group_upstream
 *  @brief Hedged concurrent requests execution engine.
 */
#ifndef AIGATE_UPSTREAM_HEDGED_H
#define AIGATE_UPSTREAM_HEDGED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "latency_tracker.h"

/** @brief Maximum number of custom HTTP header pairs supported in hedged requests. */
#define HEDGED_MAX_EXTRA_HEADERS 16

/**
 * @brief Specification of a single upstream endpoint candidate for hedged execution.
 */
typedef struct hedged_endpoint_spec {
    char        url[1024];     /**< Upstream target URL. */
    char        key[1024];     /**< API authorization key/token. */
    char        endpoint[512]; /**< Endpoint hostname or identifier for metrics tracking. */
    const char* payload;       /**< Request payload buffer (borrowed). */
    size_t      payload_len;   /**< Payload byte length. */
    const char* extra_headers[HEDGED_MAX_EXTRA_HEADERS][2]; /**< Extra header key-value pairs. */
    int         n_extra_headers;                            /**< Number of extra header pairs. */
} hedged_endpoint_spec_t;

/**
 * @brief Input parameters governing hedged speculative request dispatch.
 */
typedef struct hedged_call_params {
    const char*            model;     /**< Target model identifier for metrics and routing. */
    hedged_endpoint_spec_t primary;   /**< Primary upstream target specification. */
    hedged_endpoint_spec_t secondary; /**< Secondary (speculative backup) target specification. */
    bool        has_secondary; /**< True if secondary backup target is configured and eligible. */
    const char* payload;       /**< Shared request payload buffer. */
    size_t      payload_len;   /**< Shared request payload length in bytes. */
    int         timeout_ms;    /**< Maximum total timeout for request in milliseconds. */
    int         delay_ms;      /**< Waiting window before firing secondary hedge. */
    int         budget_pct;    /**< Hedge budget percentage limit (e.g. 15%). */
    latency_tracker_t* lt; /**< Latency tracker instance for EWMA/P95 and hedge budget checks. */
} hedged_call_params_t;

/**
 * @brief Outcome of a hedged request execution.
 */
typedef struct hedged_call_result {
    int      status;             /**< Final HTTP status code from the winning upstream. */
    char*    body;               /**< Allocated response body (caller takes ownership and frees). */
    size_t   body_len;           /**< Length of allocated response body in bytes. */
    int      winning_target_idx; /**< Winning target index (0: primary, 1: secondary). */
    bool     was_hedged;         /**< True if secondary backup request was actually dispatched. */
    uint64_t latency_ns;         /**< Total elapsed latency in nanoseconds. */
} hedged_call_result_t;

/** @brief Execute an upstream request with hedged speculative backup.
 *  @return 0 on success (with HTTP status in result->status), negative on transport error. */
int upstream_call_hedged(const hedged_call_params_t* params, hedged_call_result_t* out_result);

#endif /* AIGATE_UPSTREAM_HEDGED_H */

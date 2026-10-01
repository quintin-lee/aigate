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

#define HEDGED_MAX_EXTRA_HEADERS 16

typedef struct hedged_endpoint_spec {
    char        url[512];
    char        key[1024];
    const char* extra_headers[HEDGED_MAX_EXTRA_HEADERS][2];
    int         n_extra_headers;
} hedged_endpoint_spec_t;

typedef struct hedged_call_params {
    const char*            model;
    hedged_endpoint_spec_t primary;
    hedged_endpoint_spec_t secondary;
    bool                   has_secondary;
    const char*            payload;
    size_t                 payload_len;
    int                    timeout_ms;
    int                    delay_ms; /**< Waiting window before hedging */
    latency_tracker_t*     lt;
} hedged_call_params_t;

typedef struct hedged_call_result {
    int      status;
    char*    body;
    size_t   body_len;
    int      winning_target_idx; /**< 0: primary, 1: secondary */
    bool     was_hedged;         /**< True if hedge was actually fired */
    uint64_t latency_ns;
} hedged_call_result_t;

/** @brief Execute an upstream request with hedged speculative backup.
 *  @return 0 on success (with HTTP status in result->status), negative on transport error. */
int upstream_call_hedged(const hedged_call_params_t* params, hedged_call_result_t* out_result);

#endif /* AIGATE_UPSTREAM_HEDGED_H */

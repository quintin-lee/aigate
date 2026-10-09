/** @file circuit_breaker.h
 *  @ingroup group_policy
 *  @brief Per-endpoint circuit breaker state machine for multi-upstream routing.
 */
#ifndef AIGATE_CIRCUIT_BREAKER_H
#define AIGATE_CIRCUIT_BREAKER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Breaker four-state: closed (admit)/open (reject)/half-open (probing)/sla_degraded (soft degradation). */
typedef enum {
    /** @brief Closed: admitting normally, failure count accumulating. */
    CB_CLOSED = 0,
    /** @brief Open: tripped, rejecting directly with fallback. */
    CB_OPEN = 1,
    /** @brief Half-open: admitting a trickle of probe traffic after cooldown, closing on success. */
    CB_HALF_OPEN = 2,
    /** @brief SLA degraded: latency/TTFT exceeded threshold, transparent fallback active. */
    CB_SLA_DEGRADED = 3,
} cb_state_t;

/** @brief Breaker instance (opaque, defined in circuit_breaker.c). */
typedef struct circuit_breaker circuit_breaker_t;
/** @brief Time source function type: returns the current second-resolution timestamp for breaker cooldown timing (fake clock injection for tests). */
typedef time_t (*cb_time_fn)(void);
struct redis_pool;
struct event_bus;

/** @brief Default trip threshold: open after this many consecutive failures. */
#define CB_DEFAULT_FAILURE_THRESHOLD 3
/** @brief Default cooldown window in seconds: half-open probing allowed only after this many seconds open. */
#define CB_DEFAULT_COOLOFF_SEC 30

/** @brief Allocate and initialize a circuit breaker instance. */
circuit_breaker_t* cb_create(void);

/** @brief Free circuit breaker instance and all tracked entries. */
void cb_destroy(circuit_breaker_t* cb);

/** @brief Configure threshold and cooloff window (default 3 failures, 30s). */
void cb_set_params(circuit_breaker_t* cb, int failure_threshold, int cooloff_sec);

/** @brief Inject custom time provider for testing (pass NULL to reset to time()). */
void cb_set_time_fn(circuit_breaker_t* cb, cb_time_fn fn);

/** @brief Configure shared Redis connection pool (enables distributed circuit breaking). */
void cb_set_redis_pool(circuit_breaker_t* cb, struct redis_pool* pool);
/** @brief Attach event bus for publishing circuit state transition events. */
void cb_set_event_bus(circuit_breaker_t* cb, struct event_bus* eb);
/** @brief Configure fail-open mode when Redis is unavailable (1 = local fallback, 0 = fail closed). Default 1. */
void cb_set_fail_open(circuit_breaker_t* cb, int fail_open);

/** @brief Get current circuit state for a model endpoint ("closed", "open", "half_open"). */
cb_state_t cb_get_state(circuit_breaker_t* cb, const char* model, const char* endpoint);

/** @brief Check if request is allowed to this target.
 *  CLOSED: true.
 *  OPEN: false (unless cooloff elapsed -> transitions to HALF_OPEN and allows 1 probe).
 *  HALF_OPEN: true for 1 probe request, false for concurrent requests while probe pending.
 */
bool cb_allow_request(circuit_breaker_t* cb, const char* model, const char* endpoint);

/** @brief Record successful request (resets failures, transitions HALF_OPEN -> CLOSED). */
void cb_record_success(circuit_breaker_t* cb, const char* model, const char* endpoint);

/** @brief Record failed request (429, 5xx, or transport error).
 *  Increments consecutive failures; trips to OPEN when threshold reached.
 */
void
cb_record_failure(circuit_breaker_t* cb, const char* model, const char* endpoint, int http_status);

/** @brief Return timestamp until which the endpoint is OPEN, or 0 if not OPEN. */
time_t cb_get_open_until(circuit_breaker_t* cb, const char* model, const char* endpoint);

/** @brief Convert state enum to human-readable string ("closed", "open", "half_open"). */
const char* cb_state_to_str(cb_state_t state);

/** @brief Reset all tracked entries (e.g. for testing). */
void cb_reset(circuit_breaker_t* cb);

/**
 * @brief Configure SLA threshold parameters for a model.
 */
void cb_configure_sla(circuit_breaker_t* cb,
                      const char*        model,
                      uint32_t           ttft_max_ms,
                      uint32_t           p95_max_ms,
                      uint32_t           window_size,
                      float              violation_ratio,
                      const char*        fallback_model);

/**
 * @brief Record an SLA sample (TTFT and total latency) for a model endpoint.
 */
void cb_record_sla_sample(circuit_breaker_t* cb,
                          const char*        model,
                          const char*        endpoint,
                          uint32_t           ttft_ms,
                          uint32_t           latency_ms);

/**
 * @brief Get current circuit/SLA state and optional fallback model.
 */
cb_state_t cb_get_sla_state(circuit_breaker_t* cb,
                            const char*        model,
                            const char*        endpoint,
                            char*              out_fallback_model,
                            size_t             fallback_size);

/**
 * @brief Manually override breaker state for a model endpoint.
 */
bool cb_override_state(circuit_breaker_t* cb,
                       const char*        model,
                       const char*        endpoint,
                       cb_state_t         new_state);

/**
 * @brief Get current average TTFT for a model endpoint over sliding window.
 */
uint32_t cb_get_sla_avg_ttft(circuit_breaker_t* cb, const char* model, const char* endpoint);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_CIRCUIT_BREAKER_H */

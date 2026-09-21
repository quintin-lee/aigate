/** @file circuit_breaker.h
 *  @brief Per-endpoint circuit breaker state machine for multi-upstream routing.
 */
#ifndef AIGATE_CIRCUIT_BREAKER_H
#define AIGATE_CIRCUIT_BREAKER_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CB_CLOSED = 0,
    CB_OPEN = 1,
    CB_HALF_OPEN = 2,
} cb_state_t;

typedef struct circuit_breaker circuit_breaker_t;
typedef time_t (*cb_time_fn)(void);

#define CB_DEFAULT_FAILURE_THRESHOLD 3
#define CB_DEFAULT_COOLOFF_SEC 30

/** @brief Allocate and initialize a circuit breaker instance. */
circuit_breaker_t* cb_create(void);

/** @brief Free circuit breaker instance and all tracked entries. */
void cb_destroy(circuit_breaker_t* cb);

/** @brief Configure threshold and cooloff window (default 3 failures, 30s). */
void cb_set_params(circuit_breaker_t* cb, int failure_threshold, int cooloff_sec);

/** @brief Inject custom time provider for testing (pass NULL to reset to time()). */
void cb_set_time_fn(circuit_breaker_t* cb, cb_time_fn fn);

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

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_CIRCUIT_BREAKER_H */

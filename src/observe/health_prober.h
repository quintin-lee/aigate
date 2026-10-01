/** @file health_prober.h
 *  @ingroup group_observe
 *  @brief Active upstream health probing engine for aigate.
 *
 *  Maintains health matrix for all active providers and runs periodic background
 *  probes, publishing state change events to event_bus.
 */
#ifndef AIGATE_HEALTH_PROBER_H
#define AIGATE_HEALTH_PROBER_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "pg_store.h"
#include "event_bus.h"

/** @brief Max providers tracked by the health table; no new entries beyond the cap. */
#define MAX_TRACKED_PROVIDERS 128

/** @brief Upstream health status enum: unknown / healthy / degraded / down / paused. */
typedef enum {
    HEALTH_STATUS_UNKNOWN = 0, /**< Initial state, never probed. */
    HEALTH_STATUS_HEALTHY,     /**< 🟢 RTT < 2000ms, HTTP 200, consecutive successes */
    HEALTH_STATUS_DEGRADED,    /**< 🟡 RTT >= 2000ms, or minor error */
    HEALTH_STATUS_DOWN,        /**< 🔴 consecutive fails >= 2, 401, 5xx, or timeout */
    HEALTH_STATUS_PAUSED       /**< ⚪ disabled or missing key */
} health_status_t;

/** @brief Convert a probe status enum to a readable string.
 *  @param st  Probe status.
 *  @return Status name string (static storage, caller must not free). */
const char* health_status_str(health_status_t st);

/** @brief Single-provider health snapshot: endpoint + status + RTT + consecutive success/failure counts + error info. */
typedef struct {
    long            id;                    /**< provider record ID. */
    char            provider_name[64];     /**< Provider name. */
    char            endpoint[512];         /**< Probe endpoint. */
    char            provider_type[32];     /**< Type: openai/anthropic/gemini. */
    health_status_t status;                /**< Current status. */
    long            latency_ms;            /**< Latest RTT, milliseconds. */
    int             last_http_status;      /**< Latest HTTP status code. */
    time_t          last_check_ts;         /**< Latest probe time, seconds. */
    int             consecutive_failures;  /**< Consecutive failure count. */
    int             consecutive_successes; /**< Consecutive success count. */
    char            last_error[256];       /**< Latest error message. */
} provider_health_t;

/** @brief Health prober instance: background thread + health table + event outlet. */
typedef struct health_prober {
    pthread_mutex_t   lock;            /**< Guards the health table and run state. */
    pthread_cond_t    cond;            /**< Shutdown/immediate-probe signal. */
    pthread_t         thread;          /**< Background probe thread. */
    int               thread_started;  /**< Thread started. */
    int               running;         /**< 1 running, 0 stopped. */
    int               interval_sec;    /**< Probe interval, seconds. */
    pg_store_t*       ps;              /**< Provider inventory source (borrowed). */
    uint8_t           master_key[32];  /**< Master key copy used for probing. */
    int               have_master_key; /**< master_key valid. */
    event_bus_t*      eb;              /**< State-change event outlet, may be NULL. */
    provider_health_t providers[MAX_TRACKED_PROVIDERS]; /**< Health table. */
    int               n_providers;                      /**< Live entries in the table. */
    time_t            last_full_probe_ts;               /**< Last full-probe time, seconds. */
} health_prober_t;

/** @brief Create a new health prober instance. */
health_prober_t* health_prober_new(pg_store_t*    ps,
                                   const uint8_t* master_key,
                                   event_bus_t*   eb,
                                   int            interval_sec);

/** @brief Free the health prober instance. */
void health_prober_free(health_prober_t* hp);

/** @brief Start background periodic prober thread. */
int health_prober_start(health_prober_t* hp);

/** @brief Stop background prober thread. */
void health_prober_stop(health_prober_t* hp);

/** @brief Perform an immediate synchronous probe on all active providers.
 *  Updates internal health table and publishes events. */
int health_prober_probe_all(health_prober_t* hp);

/** @brief Record result of a probe for a specific provider (e.g. from /test endpoint). */
void health_prober_record_result(health_prober_t* hp,
                                 long             provider_id,
                                 const char*      name,
                                 const char*      endpoint,
                                 const char*      provider_type,
                                 int              http_status,
                                 long             latency_ms,
                                 int              probe_rc);

/** @brief Serialize current health status of all providers to a JSON string.
 *  Caller must free the returned string using free(). */
char* health_prober_to_json(health_prober_t* hp);

#endif /* AIGATE_HEALTH_PROBER_H */

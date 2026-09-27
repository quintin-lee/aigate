/** @file health_prober.h
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

#define MAX_TRACKED_PROVIDERS 128

typedef enum {
    HEALTH_STATUS_UNKNOWN = 0,
    HEALTH_STATUS_HEALTHY,   /* 🟢 RTT < 2000ms, HTTP 200, consecutive successes */
    HEALTH_STATUS_DEGRADED,  /* 🟡 RTT >= 2000ms, or minor error */
    HEALTH_STATUS_DOWN,      /* 🔴 consecutive fails >= 2, 401, 5xx, or timeout */
    HEALTH_STATUS_PAUSED     /* ⚪ disabled or missing key */
} health_status_t;

const char* health_status_str(health_status_t st);

typedef struct {
    long            id;
    char            provider_name[64];
    char            endpoint[512];
    char            provider_type[32];
    health_status_t status;
    long            latency_ms;
    int             last_http_status;
    time_t          last_check_ts;
    int             consecutive_failures;
    int             consecutive_successes;
    char            last_error[256];
} provider_health_t;

typedef struct health_prober {
    pthread_mutex_t   lock;
    pthread_cond_t    cond;
    pthread_t         thread;
    int               thread_started;
    int               running;
    int               interval_sec;
    pg_store_t*       ps;
    uint8_t           master_key[32];
    int               have_master_key;
    event_bus_t*      eb;
    provider_health_t providers[MAX_TRACKED_PROVIDERS];
    int               n_providers;
    time_t            last_full_probe_ts;
} health_prober_t;

/** @brief Create a new health prober instance. */
health_prober_t* health_prober_new(pg_store_t*     ps,
                                   const uint8_t*  master_key,
                                   event_bus_t*    eb,
                                   int             interval_sec);

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

/** @file event_bus.h
 *  @brief In-memory pub-sub event bus for real-time gateway telemetry and SSE streaming.
 *
 *  Thread-safe bounded queue per subscriber. Supports broadcasting events to multiple
 *  admin consoles (e.g. GET /admin/v1/events SSE stream).
 */

/**
 * @defgroup group_observe Observe layer
 * @brief Observe: event bus, health prober, metrics.
 */
#ifndef AIGATE_EVENT_BUS_H
#define AIGATE_EVENT_BUS_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

/** @brief Max single-event JSON payload size, bytes. */
#define EVENT_MAX_PAYLOAD 1024
/** @brief Per-subscriber bounded queue capacity; drops new events and counts when full. */
#define EVENT_QUEUE_CAPACITY 64
/** @brief Max subscribers; subscribe returns -1 when exceeded. */
#define MAX_EVENT_SUBSCRIBERS 8

/** @brief Event type enum: request / circuit-breaker / probe / budget-alert / ping. */
typedef enum {
    /** @brief Sentinel: no event, the empty-poll result subscribers receive. */
    EVENT_NONE = 0,
    /** @brief Request event: gateway completed one inference request (with status code and latency). */
    EVENT_REQUEST,
    /** @brief Circuit-breaker event: an upstream endpoint breaker changed state. */
    EVENT_CIRCUIT_BREAKER,
    /** @brief Probe event: upstream health prober finished one round. */
    EVENT_HEALTH_PROBE,
    /** @brief Budget-alert event: a key or group hit a budget threshold. */
    EVENT_BUDGET_ALERT,
    /** @brief Heartbeat event: SSE keepalive ping, subscribers ignore the payload. */
    EVENT_PING
} event_type_t;

/** @brief Single event item: type + event name + JSON payload + timestamp. */
typedef struct {
    event_type_t type;                       /**< Event type. */
    char         event_name[32];             /**< Event name. */
    char         payload[EVENT_MAX_PAYLOAD]; /**< JSON payload. */
    time_t       ts;                         /**< Timestamp, seconds. */
} event_item_t;

/** @brief Single subscriber: bounded ring buffer + wait condition variable. */
typedef struct event_sub {
    event_item_t   queue[EVENT_QUEUE_CAPACITY]; /**< Ring buffer. */
    int            head;                        /**< Read pointer. */
    int            tail;                        /**< Write pointer. */
    int            count;                       /**< Events currently queued. */
    long           dropped_count;               /**< Cumulative drops when the queue is full. */
    int            active;                      /**< 1 active, 0 unsubscribed. */
    int            id;                          /**< Subscriber ID (>0). */
    pthread_cond_t cond;                        /**< New-event arrival signal. */
} event_sub_t;

/** @brief Event bus: subscriber slot table + global lock. */
typedef struct event_bus {
    pthread_mutex_t lock;                   /**< Guards the subscription table and queues. */
    event_sub_t
        subscribers[MAX_EVENT_SUBSCRIBERS]; /**< Subscriber slots (active marks validity). */
    int n_subscribers;                      /**< Live subscriber count. */
    int next_sub_id;                        /**< Next subscriber ID. */
    int destroyed;                          /**< 1 destroyed, pop returns -1. */
} event_bus_t;

/** @brief Create a new event bus. Returns NULL on failure. */
event_bus_t* event_bus_new(void);

/** @brief Destroy the event bus and wake all waiting subscribers. */
void event_bus_free(event_bus_t* eb);

/** @brief Subscribe to events. Returns subscriber ID (>0) or -1 if full. */
int event_bus_subscribe(event_bus_t* eb);

/** @brief Unsubscribe by subscriber ID. */
void event_bus_unsubscribe(event_bus_t* eb, int sub_id);

/** @brief Wait and pop the next event for this subscriber.
 *  @param eb          Event bus.
 *  @param sub_id      Subscriber ID returned by event_bus_subscribe.
 *  @param out         Output event buffer.
 *  @param timeout_ms  Wait timeout in ms. If 0, returns immediately if empty.
 *  @return 1 if event returned, 0 on timeout, -1 on error/destroyed.
 */
int event_bus_pop(event_bus_t* eb, int sub_id, event_item_t* out, int timeout_ms);

/** @brief Publish a generic event to all active subscribers. */
int event_bus_publish(event_bus_t* eb,
                      event_type_t type,
                      const char*  event_name,
                      const char*  json_payload);

/* Helper publish functions (thread-safe, safe no-op if eb == NULL) */

/** @brief Publish one inference-request completion event (thread-safe, no-op when eb is NULL).
 *  @param eb  Event bus, may be NULL.
 *  @param key_id  API key record ID.
 *  @param model  Model name.
 *  @param provider  Provider name.
 *  @param status  HTTP status code.
 *  @param latency_ns  Inference latency, nanoseconds.
 *  @param prompt_tokens  Prompt token count.
 *  @param completion_tokens  Completion token count.
 *  @param cost  Request cost, USD.
 *  @param guardrail_act  Guardrail action description. */
void event_bus_publish_request(event_bus_t* eb,
                               long         key_id,
                               const char*  model,
                               const char*  provider,
                               int          status,
                               uint64_t     latency_ns,
                               long         prompt_tokens,
                               long         completion_tokens,
                               double       cost,
                               const char*  guardrail_act);

/** @brief Publish one circuit-breaker state-transition event (thread-safe, no-op when eb is NULL).
 *  @param eb  Event bus, may be NULL.
 *  @param provider  Provider name.
 *  @param model  Model name.
 *  @param old_state  Pre-transition state name.
 *  @param new_state  Post-transition state name.
 *  @param reason  Transition reason. */
void event_bus_publish_cb(event_bus_t* eb,
                          const char*  provider,
                          const char*  model,
                          const char*  old_state,
                          const char*  new_state,
                          const char*  reason);

/** @brief Publish one upstream health-probe result event (thread-safe, no-op when eb is NULL).
 *  @param eb  Event bus, may be NULL.
 *  @param provider  Provider name.
 *  @param status  Probe status string.
 *  @param latency_ms  Probe RTT, milliseconds.
 *  @param http_status  Probe HTTP status code.
 *  @param error  Error message, may be NULL. */
void event_bus_publish_health(event_bus_t* eb,
                              const char*  provider,
                              const char*  status,
                              long         latency_ms,
                              int          http_status,
                              const char*  error);

/** @brief Publish one budget-threshold alert event (thread-safe, no-op when eb is NULL).
 *  @param eb  Event bus, may be NULL.
 *  @param type  Alert target type (key/group).
 *  @param id  Alert target record ID.
 *  @param name  Alert target name.
 *  @param percent  Budget usage ratio.
 *  @param current_usd  Current usage, USD.
 *  @param budget_usd  Budget cap, USD. */
void event_bus_publish_budget(event_bus_t* eb,
                              const char*  type,
                              long         id,
                              const char*  name,
                              double       percent,
                              double       current_usd,
                              double       budget_usd);

/** @brief Publish one ping heartbeat event (for keepalive/self-check).
 *  @param eb  Event bus. */
void event_bus_publish_ping(event_bus_t* eb);

#endif /* AIGATE_EVENT_BUS_H */

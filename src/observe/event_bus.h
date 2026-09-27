/** @file event_bus.h
 *  @brief In-memory pub-sub event bus for real-time gateway telemetry and SSE streaming.
 *
 *  Thread-safe bounded queue per subscriber. Supports broadcasting events to multiple
 *  admin consoles (e.g. GET /admin/v1/events SSE stream).
 */
#ifndef AIGATE_EVENT_BUS_H
#define AIGATE_EVENT_BUS_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#define EVENT_MAX_PAYLOAD 1024
#define EVENT_QUEUE_CAPACITY 64
#define MAX_EVENT_SUBSCRIBERS 8

typedef enum {
    EVENT_NONE = 0,
    EVENT_REQUEST,
    EVENT_CIRCUIT_BREAKER,
    EVENT_HEALTH_PROBE,
    EVENT_BUDGET_ALERT,
    EVENT_PING
} event_type_t;

typedef struct {
    event_type_t type;
    char         event_name[32];
    char         payload[EVENT_MAX_PAYLOAD];
    time_t       ts;
} event_item_t;

typedef struct event_sub {
    event_item_t   queue[EVENT_QUEUE_CAPACITY];
    int            head;
    int            tail;
    int            count;
    long           dropped_count;
    int            active;
    int            id;
    pthread_cond_t cond;
} event_sub_t;

typedef struct event_bus {
    pthread_mutex_t lock;
    event_sub_t     subscribers[MAX_EVENT_SUBSCRIBERS];
    int             n_subscribers;
    int             next_sub_id;
    int             destroyed;
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

void event_bus_publish_cb(event_bus_t* eb,
                          const char*  provider,
                          const char*  model,
                          const char*  old_state,
                          const char*  new_state,
                          const char*  reason);

void event_bus_publish_health(event_bus_t* eb,
                              const char*  provider,
                              const char*  status,
                              long         latency_ms,
                              int          http_status,
                              const char*  error);

void event_bus_publish_budget(event_bus_t* eb,
                              const char*  type,
                              long         id,
                              const char*  name,
                              double       percent,
                              double       current_usd,
                              double       budget_usd);

/** @brief 发布一次 ping 心跳事件（保活/自检用）。
 *  @param eb  事件总线。 */
void event_bus_publish_ping(event_bus_t* eb);

#endif /* AIGATE_EVENT_BUS_H */

/** @file event_bus.h
 *  @brief In-memory pub-sub event bus for real-time gateway telemetry and SSE streaming.
 *
 *  Thread-safe bounded queue per subscriber. Supports broadcasting events to multiple
 *  admin consoles (e.g. GET /admin/v1/events SSE stream).
 */

/**
 * @defgroup group_observe 可观测层
 * @brief 可观测：事件总线、健康探针、指标。
 */
#ifndef AIGATE_EVENT_BUS_H
#define AIGATE_EVENT_BUS_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

/** @brief 单个事件 JSON 载荷上限，字节。 */
#define EVENT_MAX_PAYLOAD 1024
/** @brief 每个订阅者有界队列容量，满时丢新事件并计数。 */
#define EVENT_QUEUE_CAPACITY 64
/** @brief 最大订阅者数，超限订阅返回 -1。 */
#define MAX_EVENT_SUBSCRIBERS 8

/** @brief 事件类型枚举：请求 / 熔断 / 探针 / 预算告警 / 心跳。 */
typedef enum {
    /** @brief 哨兵值：无事件，订阅者收到的空轮询结果。 */
    EVENT_NONE = 0,
    /** @brief 请求事件：网关完成一次推理请求（含状态码与耗时）。 */
    EVENT_REQUEST,
    /** @brief 熔断事件：某上游端点熔断器状态变迁。 */
    EVENT_CIRCUIT_BREAKER,
    /** @brief 探针事件：上游健康探针完成一轮探测。 */
    EVENT_HEALTH_PROBE,
    /** @brief 预算告警事件：key 或分组用量触及预算阈值。 */
    EVENT_BUDGET_ALERT,
    /** @brief 心跳事件：SSE 保活 ping，订阅者忽略内容。 */
    EVENT_PING
} event_type_t;

/** @brief 单个事件项：类型 + 事件名 + JSON 载荷 + 发生时间。 */
typedef struct {
    event_type_t type;                        /**< 事件类型 */
    char         event_name[32];              /**< 事件名 */
    char         payload[EVENT_MAX_PAYLOAD];  /**< JSON 载荷 */
    time_t       ts;                          /**< 发生时间（秒） */
} event_item_t;

/** @brief 单个订阅者：有界环形缓冲 + 等待条件变量。 */
typedef struct event_sub {
    event_item_t   queue[EVENT_QUEUE_CAPACITY]; /**< 环形缓冲 */
    int            head;     /**< 读指针 */
    int            tail;     /**< 写指针 */
    int            count;    /**< 队列现存事件数 */
    long           dropped_count; /**< 队列满丢弃累计 */
    int            active;   /**< 1 有效，0 已退订 */
    int            id;       /**< 订阅者 ID（>0） */
    pthread_cond_t cond;     /**< 新事件到达通知 */
} event_sub_t;

/** @brief 事件总线：订阅槽表 + 全局锁。 */
typedef struct event_bus {
    pthread_mutex_t lock;     /**< 保护订阅表与队列 */
    event_sub_t     subscribers[MAX_EVENT_SUBSCRIBERS]; /**< 订阅槽（active 标记有效） */
    int             n_subscribers; /**< 有效订阅数 */
    int             next_sub_id;   /**< 下一个订阅 ID */
    int             destroyed;     /**< 1 已销毁，pop 返回 -1 */
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

/** @brief 发布一次推理请求完成事件（线程安全，eb 为 NULL 时空操作）。
 *  @param eb  事件总线，可为 NULL。
 *  @param key_id  API key 记录 ID。
 *  @param model  模型名。
 *  @param provider  供应商名。
 *  @param status  HTTP 状态码。
 *  @param latency_ns  推理耗时，纳秒。
 *  @param prompt_tokens  提示 token 数。
 *  @param completion_tokens  补全 token 数。
 *  @param cost  本次费用，美元。
 *  @param guardrail_act  护栏动作描述。 */
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

/** @brief 发布一次熔断器状态变迁事件（线程安全，eb 为 NULL 时空操作）。
 *  @param eb  事件总线，可为 NULL。
 *  @param provider  供应商名。
 *  @param model  模型名。
 *  @param old_state  变迁前状态名。
 *  @param new_state  变迁后状态名。
 *  @param reason  变迁原因。 */
void event_bus_publish_cb(event_bus_t* eb,
                          const char*  provider,
                          const char*  model,
                          const char*  old_state,
                          const char*  new_state,
                          const char*  reason);

/** @brief 发布一次上游健康探测结果事件（线程安全，eb 为 NULL 时空操作）。
 *  @param eb  事件总线，可为 NULL。
 *  @param provider  供应商名。
 *  @param status  探测状态字符串。
 *  @param latency_ms  探测 RTT，毫秒。
 *  @param http_status  探测 HTTP 状态码。
 *  @param error  错误信息，可为 NULL。 */
void event_bus_publish_health(event_bus_t* eb,
                              const char*  provider,
                              const char*  status,
                              long         latency_ms,
                              int          http_status,
                              const char*  error);

/** @brief 发布一次预算阈值告警事件（线程安全，eb 为 NULL 时空操作）。
 *  @param eb  事件总线，可为 NULL。
 *  @param type  告警对象类型（key/group）。
 *  @param id  告警对象记录 ID。
 *  @param name  告警对象名。
 *  @param percent  已用预算占比。
 *  @param current_usd  当前已用，美元。
 *  @param budget_usd  预算上限，美元。 */
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

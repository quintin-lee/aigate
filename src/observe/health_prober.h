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

/** @brief 健康表最大跟踪供应商数，超限不再新增。 */
#define MAX_TRACKED_PROVIDERS 128

typedef enum {
    HEALTH_STATUS_UNKNOWN = 0,
    HEALTH_STATUS_HEALTHY,   /* 🟢 RTT < 2000ms, HTTP 200, consecutive successes */
    HEALTH_STATUS_DEGRADED,  /* 🟡 RTT >= 2000ms, or minor error */
    HEALTH_STATUS_DOWN,      /* 🔴 consecutive fails >= 2, 401, 5xx, or timeout */
    HEALTH_STATUS_PAUSED     /* ⚪ disabled or missing key */
} health_status_t;

/** @brief 探针状态枚举转可读字符串。
 *  @param st  探针状态。
 *  @return 状态名字符串（静态存储，调用方勿释放）。 */
const char* health_status_str(health_status_t st);

typedef struct {
    long            id;                /* provider 记录 ID */
    char            provider_name[64]; /* 供应商名 */
    char            endpoint[512];     /* 探测端点 */
    char            provider_type[32]; /* 类型：openai/anthropic/gemini */
    health_status_t status;            /* 当前状态 */
    long            latency_ms;        /* 最近 RTT，毫秒 */
    int             last_http_status;  /* 最近 HTTP 状态码 */
    time_t          last_check_ts;     /* 最近探测时间（秒） */
    int             consecutive_failures;  /* 连续失败次数 */
    int             consecutive_successes; /* 连续成功次数 */
    char            last_error[256];   /* 最近错误信息 */
} provider_health_t;

typedef struct health_prober {
    pthread_mutex_t   lock;           /* 保护健康表与运行态 */
    pthread_cond_t    cond;           /* 停机/立即探测通知 */
    pthread_t         thread;         /* 后台探测线程 */
    int               thread_started; /* 线程已启动 */
    int               running;        /* 1 运行中，0 已停 */
    int               interval_sec;   /* 探测周期，秒 */
    pg_store_t*       ps;             /* 供应商清单来源（借用） */
    uint8_t           master_key[32]; /* 探测用主密钥（拷贝） */
    int               have_master_key; /* master_key 有效 */
    event_bus_t*      eb;             /* 状态变更事件出口，可为 NULL */
    provider_health_t providers[MAX_TRACKED_PROVIDERS]; /* 健康表 */
    int               n_providers;    /* 表中有效条目数 */
    time_t            last_full_probe_ts; /* 上次全量探测时间（秒） */
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

/** @file circuit_breaker.c
 *  @brief Implementation of per-endpoint circuit breaker state machine.
 */
#include "circuit_breaker.h"
#include "aigate_log.h"
#include "redis_client.h"
#include "redis_pool.h"
#include "redis_scripts.h"
#include "event_bus.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Bucket array shard count (hashed by model+endpoint). */
#define CB_BUCKETS 64

/** @brief Per-endpoint breaker state: model/endpoint/tri-state/failure count/open deadline/half-open probe/link. */
typedef struct cb_entry {
    char             model[128];             /**< Model name. */
    char             endpoint[512];          /**< Endpoint URL. */
    cb_state_t       state;                  /**< Current tri-state. */
    int              consecutive_failures;   /**< Consecutive failure count. */
    time_t           open_until;             /**< Open-state deadline timestamp. */
    int              half_open_probe_active; /**< Half-open probe in-flight flag. */
    struct cb_entry* next;                   /**< Intra-bucket link. */
} cb_entry_t;

/** @brief Breaker instance: lock/threshold/cooldown/time source/Redis pool/event bus/shard buckets. */
struct circuit_breaker {
    pthread_mutex_t mtx;                 /**< Instance mutex. */
    int             failure_threshold;   /**< Trip threshold (consecutive failures). */
    int             cooloff_sec;         /**< Cooldown window in seconds. */
    cb_time_fn      time_fn;             /**< Time source (fake clock injectable). */
    redis_pool_t*   pool;                /**< Shared Redis pool (distributed breaking, nullable). */
    char            sha_cb[48];          /**< Redis Lua script SHA cache. */
    event_bus_t*    eb;                  /**< Event bus (state transition events, nullable). */
    cb_entry_t*     buckets[CB_BUCKETS]; /**< Endpoint state shard buckets. */
};

/** @brief Current time: injected test clock first, else time(). */
static time_t
get_now(const circuit_breaker_t* cb)
{
    if (cb->time_fn != NULL) {
        return cb->time_fn();
    }
    return time(NULL);
}

/** @brief djb2 hash of model:endpoint modulo bucket count (entry shard location). */
static unsigned int
hash_key(const char* model, const char* endpoint)
{
    unsigned int         h = 5381;
    const unsigned char* p;
    if (model != NULL) {
        for (p = (const unsigned char*)model; *p != '\0'; p++) {
            h = ((h << 5) + h) + *p;
        }
    }
    h = ((h << 5) + h) + ':';
    if (endpoint != NULL) {
        for (p = (const unsigned char*)endpoint; *p != '\0'; p++) {
            h = ((h << 5) + h) + *p;
        }
    }
    return h % CB_BUCKETS;
}

/** @brief Exact lookup of a model+endpoint entry in the hash bucket chain (caller must hold the lock).
 *  @return Entry pointer; NULL when absent. */
static cb_entry_t*
find_entry_locked(circuit_breaker_t* cb, const char* model, const char* endpoint)
{
    unsigned int idx = hash_key(model, endpoint);
    cb_entry_t*  e = cb->buckets[idx];
    while (e != NULL) {
        if (strcmp(e->model, model) == 0 && strcmp(e->endpoint, endpoint) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

/** @brief Look up or create an entry: calloc a new CLOSED entry at the chain head when absent (caller must hold the lock).
 *  @return Entry pointer; NULL on OOM. */
static cb_entry_t*
get_or_create_entry_locked(circuit_breaker_t* cb, const char* model, const char* endpoint)
{
    cb_entry_t* e = find_entry_locked(cb, model, endpoint);
    if (e != NULL) {
        return e;
    }
    unsigned int idx = hash_key(model, endpoint);
    e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    snprintf(e->model, sizeof(e->model), "%s", model ? model : "");
    snprintf(e->endpoint, sizeof(e->endpoint), "%s", endpoint ? endpoint : "");
    e->state = CB_CLOSED;
    e->consecutive_failures = 0;
    e->open_until = 0;
    e->half_open_probe_active = 0;
    e->next = cb->buckets[idx];
    cb->buckets[idx] = e;
    return e;
}

/** @brief Time-driven state advance: OPEN with expired cooldown becomes HALF_OPEN (clear probe flag, emit event). Caller must hold the lock. */
static void
update_state_on_time_locked(circuit_breaker_t* cb, cb_entry_t* e, time_t now)
{
    if (e->state == CB_OPEN && now >= e->open_until) {
        e->state = CB_HALF_OPEN;
        e->half_open_probe_active = 0;
        AIGATE_LOG_INFO(
            "circuit breaker for %s:%s transitioned to HALF_OPEN", e->model, e->endpoint);
        if (cb != NULL && cb->eb != NULL) {
            event_bus_publish_cb(
                cb->eb, e->endpoint, e->model, "OPEN", "HALF_OPEN", "cooloff expired");
        }
    }
}

void
cb_set_event_bus(circuit_breaker_t* cb, struct event_bus* eb)
{
    if (cb == NULL) {
        return;
    }
    pthread_mutex_lock(&cb->mtx);
    cb->eb = eb;
    pthread_mutex_unlock(&cb->mtx);
}

circuit_breaker_t*
cb_create(void)
{
    circuit_breaker_t* cb = calloc(1, sizeof(*cb));
    if (cb == NULL) {
        return NULL;
    }
    pthread_mutex_init(&cb->mtx, NULL);
    cb->failure_threshold = CB_DEFAULT_FAILURE_THRESHOLD;
    cb->cooloff_sec = CB_DEFAULT_COOLOFF_SEC;
    cb->time_fn = NULL;
    cb->eb = NULL;
    return cb;
}

void
cb_reset(circuit_breaker_t* cb)
{
    if (cb == NULL) {
        return;
    }
    pthread_mutex_lock(&cb->mtx);
    for (int i = 0; i < CB_BUCKETS; i++) {
        cb_entry_t* e = cb->buckets[i];
        while (e != NULL) {
            cb_entry_t* next = e->next;
            free(e);
            e = next;
        }
        cb->buckets[i] = NULL;
    }
    pthread_mutex_unlock(&cb->mtx);
}

void
cb_destroy(circuit_breaker_t* cb)
{
    if (cb == NULL) {
        return;
    }
    cb_reset(cb);
    pthread_mutex_destroy(&cb->mtx);
    free(cb);
}

void
cb_set_params(circuit_breaker_t* cb, int failure_threshold, int cooloff_sec)
{
    if (cb == NULL) {
        return;
    }
    pthread_mutex_lock(&cb->mtx);
    if (failure_threshold > 0) {
        cb->failure_threshold = failure_threshold;
    }
    if (cooloff_sec >= 0) {
        cb->cooloff_sec = cooloff_sec;
    }
    pthread_mutex_unlock(&cb->mtx);
}

void
cb_set_time_fn(circuit_breaker_t* cb, cb_time_fn fn)
{
    if (cb == NULL) {
        return;
    }
    pthread_mutex_lock(&cb->mtx);
    cb->time_fn = fn;
    pthread_mutex_unlock(&cb->mtx);
}

void
cb_set_redis_pool(circuit_breaker_t* cb, redis_pool_t* pool)
{
    if (cb == NULL) {
        return;
    }
    pthread_mutex_lock(&cb->mtx);
    cb->pool = pool;
    if (pool != NULL) {
        redisContext* c = redis_pool_acquire(pool);
        if (c != NULL) {
            redis_script_load(c, SCRIPT_CIRCUIT_BREAKER_SYNC, cb->sha_cb);
            redis_pool_release(pool, c);
        }
    }
    pthread_mutex_unlock(&cb->mtx);
}

cb_state_t
cb_get_state(circuit_breaker_t* cb, const char* model, const char* endpoint)
{
    if (cb == NULL || model == NULL || endpoint == NULL) {
        return CB_CLOSED;
    }
    pthread_mutex_lock(&cb->mtx);
    cb_entry_t* e = find_entry_locked(cb, model, endpoint);
    if (e == NULL) {
        pthread_mutex_unlock(&cb->mtx);
        return CB_CLOSED;
    }
    time_t now = get_now(cb);
    update_state_on_time_locked(cb, e, now);
    cb_state_t st = e->state;
    pthread_mutex_unlock(&cb->mtx);
    return st;
}

time_t
cb_get_open_until(circuit_breaker_t* cb, const char* model, const char* endpoint)
{
    if (cb == NULL || model == NULL || endpoint == NULL) {
        return 0;
    }
    pthread_mutex_lock(&cb->mtx);
    cb_entry_t* e = find_entry_locked(cb, model, endpoint);
    if (e == NULL) {
        pthread_mutex_unlock(&cb->mtx);
        return 0;
    }
    time_t now = get_now(cb);
    update_state_on_time_locked(cb, e, now);
    time_t until = (e->state == CB_OPEN) ? e->open_until : 0;
    pthread_mutex_unlock(&cb->mtx);
    return until;
}

/* --- Redis helper ---------------------------------------------------- */

/* Builds the Redis key: aigate:cb:{djb2(model:endpoint) hex8} */
/** @brief Build the distributed breaker Redis key: `aigate:cb:{djb2(model:endpoint) hex8}`. */
static void
cb_redis_key(const char* model, const char* endpoint, char* out, size_t cap)
{
    unsigned int h = hash_key(model, endpoint);
    snprintf(out, cap, "aigate:cb:%08x", h);
}

/**
 * Call SCRIPT_CIRCUIT_BREAKER_SYNC on Redis.
 * @param action  "allow", "success", or "fail"
 * @param cb      circuit breaker (must have pool set)
 * @param key     Redis key string
 * @return redisReply* (array[2]) or NULL on error; caller frees
 */
static redisReply*
redis_cb_call(circuit_breaker_t* cb, const char* action, const char* redis_key)
{
    redisContext* c = redis_pool_acquire(cb->pool);
    if (c == NULL) {
        return NULL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;

    char now_buf[32], fails_buf[16], cool_buf[32];
    snprintf(now_buf, sizeof(now_buf), "%llu", (unsigned long long)now_ms);
    snprintf(fails_buf, sizeof(fails_buf), "%d", cb->failure_threshold);
    /* cooldown_ms = cooloff_sec * 1000 */
    snprintf(cool_buf, sizeof(cool_buf), "%lld", (long long)cb->cooloff_sec * 1000LL);

    const char* keys[1] = {redis_key};
    const char* argv[4] = {action, now_buf, fails_buf, cool_buf};

    redisReply* reply =
        redis_eval_sha(c, cb->sha_cb, SCRIPT_CIRCUIT_BREAKER_SYNC, 1, keys, argv, 4);
    redis_pool_release(cb->pool, c);

    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2 ||
        reply->element[0]->type != REDIS_REPLY_INTEGER ||
        reply->element[1]->type != REDIS_REPLY_INTEGER) {
        if (reply) {
            freeReplyObject(reply);
        }
        return NULL;
    }
    return reply;
}

bool
cb_allow_request(circuit_breaker_t* cb, const char* model, const char* endpoint)
{
    if (cb == NULL || model == NULL || endpoint == NULL) {
        return true;
    }

    if (cb->pool != NULL) {
        char redis_key[96];
        cb_redis_key(model, endpoint, redis_key, sizeof(redis_key));
        redisReply* reply = redis_cb_call(cb, "allow", redis_key);
        if (reply == NULL) {
            /* Fail-Closed: Redis unavailable → deny to prevent broken routing */
            AIGATE_LOG_WARN(
                "circuit breaker redis error for %s:%s (allow), fail-closed deny", model, endpoint);
            return false;
        }
        bool allowed = (reply->element[0]->integer == 1);
        freeReplyObject(reply);
        return allowed;
    }

    pthread_mutex_lock(&cb->mtx);
    cb_entry_t* e = get_or_create_entry_locked(cb, model, endpoint);
    if (e == NULL) {
        pthread_mutex_unlock(&cb->mtx);
        return true;
    }
    time_t now = get_now(cb);
    update_state_on_time_locked(cb, e, now);

    bool allowed = false;
    if (e->state == CB_CLOSED) {
        allowed = true;
    } else if (e->state == CB_HALF_OPEN) {
        if (!e->half_open_probe_active) {
            e->half_open_probe_active = 1;
            allowed = true;
        } else {
            allowed = false;
        }
    } else {
        /* CB_OPEN */
        allowed = false;
    }
    pthread_mutex_unlock(&cb->mtx);
    return allowed;
}

void
cb_record_success(circuit_breaker_t* cb, const char* model, const char* endpoint)
{
    if (cb == NULL || model == NULL || endpoint == NULL) {
        return;
    }

    if (cb->pool != NULL) {
        char redis_key[96];
        cb_redis_key(model, endpoint, redis_key, sizeof(redis_key));
        redisReply* reply = redis_cb_call(cb, "success", redis_key);
        if (reply == NULL) {
            AIGATE_LOG_WARN("circuit breaker redis error for %s:%s (success), local state only",
                            model,
                            endpoint);
        } else {
            freeReplyObject(reply);
        }
        /* Also reset local state in case pool is removed later */
        pthread_mutex_lock(&cb->mtx);
        cb_entry_t* e = find_entry_locked(cb, model, endpoint);
        if (e != NULL) {
            e->state = CB_CLOSED;
            e->consecutive_failures = 0;
            e->open_until = 0;
            e->half_open_probe_active = 0;
        }
        pthread_mutex_unlock(&cb->mtx);
        return;
    }

    pthread_mutex_lock(&cb->mtx);
    cb_entry_t* e = find_entry_locked(cb, model, endpoint);
    if (e != NULL) {
        if (e->state == CB_HALF_OPEN) {
            AIGATE_LOG_INFO("circuit breaker for %s:%s probe succeeded, transitioned to CLOSED",
                            e->model,
                            e->endpoint);
            if (cb->eb != NULL) {
                event_bus_publish_cb(
                    cb->eb, e->endpoint, e->model, "HALF_OPEN", "CLOSED", "probe succeeded");
            }
        }
        e->state = CB_CLOSED;
        e->consecutive_failures = 0;
        e->open_until = 0;
        e->half_open_probe_active = 0;
    }
    pthread_mutex_unlock(&cb->mtx);
}

void
cb_record_failure(circuit_breaker_t* cb, const char* model, const char* endpoint, int http_status)
{
    if (cb == NULL || model == NULL || endpoint == NULL) {
        return;
    }
    /* Filter out non-failover client errors (e.g. 400 Bad Request, 401 Unauthorized, 404 Not Found) */
    if (http_status > 0 && http_status != 429 && (http_status < 500 || http_status > 599)) {
        return;
    }

    if (cb->pool != NULL) {
        char redis_key[96];
        cb_redis_key(model, endpoint, redis_key, sizeof(redis_key));
        redisReply* reply = redis_cb_call(cb, "fail", redis_key);
        if (reply == NULL) {
            AIGATE_LOG_WARN(
                "circuit breaker redis error for %s:%s (fail), local state only", model, endpoint);
        } else {
            long cb_state = reply->element[1]->integer;
            freeReplyObject(reply);
            if (cb_state == 2) {
                AIGATE_LOG_WARN(
                    "circuit breaker for %s:%s tripped to OPEN cluster-wide (status %d)",
                    model,
                    endpoint,
                    http_status);
            }
        }
        /* Mirror into local state too */
        pthread_mutex_lock(&cb->mtx);
        cb_entry_t* e = get_or_create_entry_locked(cb, model, endpoint);
        if (e != NULL) {
            time_t now = get_now(cb);
            update_state_on_time_locked(cb, e, now);
            if (e->state == CB_HALF_OPEN) {
                e->state = CB_OPEN;
                e->open_until = now + cb->cooloff_sec;
                e->half_open_probe_active = 0;
            } else if (e->state == CB_CLOSED) {
                e->consecutive_failures++;
                if (e->consecutive_failures >= cb->failure_threshold) {
                    e->state = CB_OPEN;
                    e->open_until = now + cb->cooloff_sec;
                    e->half_open_probe_active = 0;
                }
            }
        }
        pthread_mutex_unlock(&cb->mtx);
        return;
    }

    pthread_mutex_lock(&cb->mtx);
    cb_entry_t* e = get_or_create_entry_locked(cb, model, endpoint);
    if (e == NULL) {
        pthread_mutex_unlock(&cb->mtx);
        return;
    }
    time_t now = get_now(cb);
    update_state_on_time_locked(cb, e, now);

    if (e->state == CB_HALF_OPEN) {
        /* Probe failed: trip immediately back to OPEN for another cool-off window */
        e->state = CB_OPEN;
        e->open_until = now + cb->cooloff_sec;
        e->half_open_probe_active = 0;
        AIGATE_LOG_WARN(
            "circuit breaker for %s:%s probe failed (status %d), tripped back to OPEN until %ld",
            e->model,
            e->endpoint,
            http_status,
            (long)e->open_until);
        if (cb->eb != NULL) {
            event_bus_publish_cb(
                cb->eb, e->endpoint, e->model, "HALF_OPEN", "OPEN", "probe failed");
        }
    } else if (e->state == CB_CLOSED) {
        e->consecutive_failures++;
        if (e->consecutive_failures >= cb->failure_threshold) {
            e->state = CB_OPEN;
            e->open_until = now + cb->cooloff_sec;
            e->half_open_probe_active = 0;
            AIGATE_LOG_WARN(
                "circuit breaker for %s:%s reached %d failures (status %d), tripped to "
                "OPEN until %ld",
                e->model,
                e->endpoint,
                e->consecutive_failures,
                http_status,
                (long)e->open_until);
            if (cb->eb != NULL) {
                event_bus_publish_cb(
                    cb->eb, e->endpoint, e->model, "CLOSED", "OPEN", "failures reached threshold");
            }
        }
    } else {
        /* Already OPEN: keep the original cool-off window. Refreshing
         * open_until on every failure would extend it indefinitely under
         * continuous traffic and the endpoint would never get the quiet
         * window its half-open probe needs to recover. */
    }
    pthread_mutex_unlock(&cb->mtx);
}

const char*
cb_state_to_str(cb_state_t state)
{
    switch (state) {
    case CB_CLOSED:
        return "closed";
    case CB_OPEN:
        return "open";
    case CB_HALF_OPEN:
        return "half_open";
    default:
        return "unknown";
    }
}

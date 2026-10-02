/** @file shadow.c
 *  @ingroup group_policy
 *  @brief Implementation of traffic shadowing and canary evaluation engine.
 */
#include "policy/shadow.h"
#include "aigate_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Task queue internal structure. */
struct shadow_queue {
    shadow_task_t*  items;         /**< Array of tasks */
    size_t          capacity;      /**< Maximum items */
    size_t          head;          /**< Head index (pop position) */
    size_t          tail;          /**< Tail index (push position) */
    size_t          count;         /**< Current queued count */
    uint64_t        dropped_count; /**< Dropped count due to full buffer */
    pthread_mutex_t lock;          /**< Queue mutex */
    pthread_cond_t  cond;          /**< Condition variable for popping */
};

/** @brief Circular evaluation cache internal structure. */
struct shadow_eval_cache {
    shadow_eval_item_t* items;    /**< Ring buffer of evaluation items */
    size_t              capacity; /**< Capacity */
    size_t              head;     /**< Oldest item index */
    size_t              count;    /**< Current stored items */
    shadow_stats_t      stats;    /**< Cumulative statistics */
    pthread_mutex_t     lock;     /**< Cache mutex */
};

/** @brief Maximum concurrent in-flight pairing slots. */
#define SHADOW_PAIRING_SLOTS_CAPACITY 256

/** @brief In-flight pairing slot for dual-track correlation. */
typedef struct {
    char           eval_id[33];         /**< Unique evaluation ID */
    char           trace_id[33];        /**< Associated trace ID */
    char           source_model[64];    /**< Source primary model */
    char           target_model[64];    /**< Candidate shadow/canary model */
    traffic_mode_t mode;                /**< Traffic mode */
    char           prompt_preview[256]; /**< User prompt snippet */
    bool           active;              /**< Whether slot is occupied */
    bool           primary_done;        /**< Whether primary side has completed */
    bool           shadow_done;         /**< Whether shadow side has completed */

    double primary_latency_ms;          /**< Primary response latency */
    double shadow_latency_ms;           /**< Shadow response latency */
    double primary_ttft_ms;             /**< Primary time to first token */
    double shadow_ttft_ms;              /**< Shadow time to first token */

    int    primary_http_status;         /**< Primary HTTP status */
    int    shadow_http_status;          /**< Shadow HTTP status */
    long   primary_tokens;              /**< Primary token count */
    long   shadow_tokens;               /**< Shadow token count */
    double primary_cost_usd;            /**< Primary estimated cost */
    double shadow_cost_usd;             /**< Shadow estimated cost */

    char     primary_resp_snippet[512]; /**< Primary response snippet */
    char     shadow_resp_snippet[512];  /**< Shadow response snippet */
    uint64_t timestamp_us;              /**< Monotonic timestamp in microseconds */
} pairing_slot_t;

/** @brief Shadow engine internal structure. */
struct shadow_engine {
    struct pg_store*     ps;                                      /**< Database handle */
    shadow_queue_t*      queue;                                   /**< Background task queue */
    shadow_eval_cache_t* cache;                                   /**< Evaluation snapshots cache */
    pairing_slot_t  pairing_slots[SHADOW_PAIRING_SLOTS_CAPACITY]; /**< In-flight pairing slots */
    pthread_mutex_t pair_lock;                                    /**< Pairing slots mutex */
    pthread_t       worker_tid;                                   /**< Background worker thread */
    bool            running;                                      /**< Worker loop flag */
};

/* --- Rule Matching & Sampling --- */

bool
shadow_rule_matches(const shadow_rule_t* rule, const char* model, const char* header_str)
{
    if (rule == NULL || !rule->enabled || model == NULL) {
        return false;
    }
    if (strcmp(rule->source_model, model) != 0) {
        return false;
    }
    if (rule->header_match[0] != '\0') {
        if (header_str == NULL || strstr(header_str, rule->header_match) == NULL) {
            return false;
        }
    }
    return true;
}

bool
shadow_rule_should_sample(const shadow_rule_t* rule)
{
    if (rule == NULL || !rule->enabled) {
        return false;
    }
    if (rule->sample_rate >= 1.0) {
        return true;
    }
    if (rule->sample_rate <= 0.0) {
        return false;
    }
    double roll = (double)rand() / (double)RAND_MAX;
    return roll < rule->sample_rate;
}

/* --- Task Queue Implementation --- */

shadow_queue_t*
shadow_queue_create(size_t capacity)
{
    if (capacity == 0) {
        capacity = SHADOW_QUEUE_DEFAULT_CAPACITY;
    }
    shadow_queue_t* q = calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->items = calloc(capacity, sizeof(shadow_task_t));
    if (q->items == NULL) {
        free(q);
        return NULL;
    }
    q->capacity = capacity;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    return q;
}

void
shadow_task_free(shadow_task_t* task)
{
    if (task == NULL) {
        return;
    }
    if (task->body_copy != NULL) {
        free(task->body_copy);
        task->body_copy = NULL;
    }
}

void
shadow_queue_destroy(shadow_queue_t* q)
{
    if (q == NULL) {
        return;
    }
    pthread_mutex_lock(&q->lock);
    for (size_t i = 0; i < q->count; i++) {
        size_t idx = (q->head + i) % q->capacity;
        shadow_task_free(&q->items[idx]);
    }
    free(q->items);
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
    free(q);
}

bool
shadow_queue_push(shadow_queue_t* q, const shadow_task_t* task)
{
    if (q == NULL || task == NULL) {
        return false;
    }
    pthread_mutex_lock(&q->lock);
    if (q->count >= q->capacity) {
        q->dropped_count++;
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    q->items[q->tail] = *task;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return true;
}

bool
shadow_queue_pop(shadow_queue_t* q, shadow_task_t* out_task, uint32_t timeout_ms)
{
    if (q == NULL || out_task == NULL) {
        return false;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        int rc = pthread_cond_timedwait(&q->cond, &q->lock, &ts);
        if (rc == ETIMEDOUT || rc != 0) {
            if (q->count == 0) {
                pthread_mutex_unlock(&q->lock);
                return false;
            }
            break;
        }
    }

    *out_task = q->items[q->head];
    memset(&q->items[q->head], 0, sizeof(shadow_task_t));
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return true;
}

size_t
shadow_queue_count(shadow_queue_t* q)
{
    if (q == NULL) {
        return 0;
    }
    pthread_mutex_lock(&q->lock);
    size_t c = q->count;
    pthread_mutex_unlock(&q->lock);
    return c;
}

uint64_t
shadow_queue_dropped(shadow_queue_t* q)
{
    if (q == NULL) {
        return 0;
    }
    pthread_mutex_lock(&q->lock);
    uint64_t d = q->dropped_count;
    pthread_mutex_unlock(&q->lock);
    return d;
}

/* --- Circular Evaluation Cache Implementation --- */

shadow_eval_cache_t*
shadow_eval_cache_create(size_t capacity)
{
    if (capacity == 0) {
        capacity = SHADOW_EVAL_DEFAULT_CAPACITY;
    }
    shadow_eval_cache_t* c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->items = calloc(capacity, sizeof(shadow_eval_item_t));
    if (c->items == NULL) {
        free(c);
        return NULL;
    }
    c->capacity = capacity;
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

void
shadow_eval_cache_destroy(shadow_eval_cache_t* c)
{
    if (c == NULL) {
        return;
    }
    pthread_mutex_lock(&c->lock);
    free(c->items);
    pthread_mutex_unlock(&c->lock);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

void
shadow_eval_cache_record(shadow_eval_cache_t* c, const shadow_eval_item_t* item)
{
    if (c == NULL || item == NULL) {
        return;
    }
    pthread_mutex_lock(&c->lock);
    size_t write_idx = 0;
    if (c->count < c->capacity) {
        write_idx = (c->head + c->count) % c->capacity;
        c->count++;
    } else {
        write_idx = c->head;
        c->head = (c->head + 1) % c->capacity;
    }

    c->items[write_idx] = *item;

    /* Update cumulative stats */
    c->stats.total_evaluated++;
    if (item->shadow_http_status >= 200 && item->shadow_http_status < 400) {
        c->stats.successful_shadow++;
    } else {
        c->stats.failed_shadow++;
    }

    c->stats.primary_cost_usd += item->primary_cost_usd;
    c->stats.shadow_cost_usd += item->shadow_cost_usd;
    if (item->primary_cost_usd > item->shadow_cost_usd) {
        c->stats.cost_saved_usd += (item->primary_cost_usd - item->shadow_cost_usd);
    }

    /* Running averages */
    uint64_t n = c->stats.total_evaluated;
    if (n == 1) {
        c->stats.avg_primary_lat_ms = item->primary_latency_ms;
        c->stats.avg_shadow_lat_ms = item->shadow_latency_ms;
    } else {
        c->stats.avg_primary_lat_ms +=
            (item->primary_latency_ms - c->stats.avg_primary_lat_ms) / (double)n;
        c->stats.avg_shadow_lat_ms +=
            (item->shadow_latency_ms - c->stats.avg_shadow_lat_ms) / (double)n;
    }

    pthread_mutex_unlock(&c->lock);
}

int
shadow_eval_cache_get_recent(shadow_eval_cache_t* c, shadow_eval_item_t* out_items, int max_items)
{
    if (c == NULL || out_items == NULL || max_items <= 0) {
        return 0;
    }
    pthread_mutex_lock(&c->lock);
    int to_copy = (int)c->count;
    if (to_copy > max_items) {
        to_copy = max_items;
    }

    for (int i = 0; i < to_copy; i++) {
        size_t idx = (c->head + c->count - 1 - (size_t)i) % c->capacity;
        out_items[i] = c->items[idx];
    }
    pthread_mutex_unlock(&c->lock);
    return to_copy;
}

void
shadow_eval_cache_get_stats(shadow_eval_cache_t* c, shadow_stats_t* out_stats)
{
    if (c == NULL || out_stats == NULL) {
        return;
    }
    pthread_mutex_lock(&c->lock);
    *out_stats = c->stats;
    pthread_mutex_unlock(&c->lock);
}

size_t
shadow_eval_cache_count(shadow_eval_cache_t* c)
{
    if (c == NULL) {
        return 0;
    }
    pthread_mutex_lock(&c->lock);
    size_t count = c->count;
    pthread_mutex_unlock(&c->lock);
    return count;
}

/* --- Engine & Dual-Track Pairing Implementation --- */

static void*
shadow_worker_routine(void* arg)
{
    shadow_engine_t* eng = (shadow_engine_t*)arg;
    while (eng->running) {
        shadow_task_t task;
        if (!shadow_queue_pop(eng->queue, &task, 200)) {
            continue;
        }

        /* In Task 2 background worker executes upstream call */
        shadow_task_free(&task);
    }
    return NULL;
}

shadow_engine_t*
shadow_engine_create(struct pg_store* ps, size_t queue_cap, size_t eval_cap)
{
    shadow_engine_t* eng = calloc(1, sizeof(*eng));
    if (eng == NULL) {
        return NULL;
    }
    eng->ps = ps;
    eng->queue = shadow_queue_create(queue_cap);
    eng->cache = shadow_eval_cache_create(eval_cap);
    if (eng->queue == NULL || eng->cache == NULL) {
        if (eng->queue != NULL) {
            shadow_queue_destroy(eng->queue);
        }
        if (eng->cache != NULL) {
            shadow_eval_cache_destroy(eng->cache);
        }
        free(eng);
        return NULL;
    }
    pthread_mutex_init(&eng->pair_lock, NULL);
    return eng;
}

void
shadow_engine_destroy(shadow_engine_t* eng)
{
    if (eng == NULL) {
        return;
    }
    shadow_engine_stop(eng);
    shadow_queue_destroy(eng->queue);
    shadow_eval_cache_destroy(eng->cache);
    pthread_mutex_destroy(&eng->pair_lock);
    free(eng);
}

int
shadow_engine_start(shadow_engine_t* eng)
{
    if (eng == NULL || eng->running) {
        return 0;
    }
    eng->running = true;
    if (pthread_create(&eng->worker_tid, NULL, shadow_worker_routine, eng) != 0) {
        eng->running = false;
        return -1;
    }
    return 0;
}

void
shadow_engine_stop(shadow_engine_t* eng)
{
    if (eng == NULL || !eng->running) {
        return;
    }
    eng->running = false;
    pthread_join(eng->worker_tid, NULL);
}

bool
shadow_engine_start_pairing(shadow_engine_t* eng,
                            const char*      eval_id,
                            const char*      trace_id,
                            const char*      source_model,
                            const char*      target_model,
                            traffic_mode_t   mode,
                            const char*      prompt_preview)
{
    if (eng == NULL || eval_id == NULL) {
        return false;
    }
    pthread_mutex_lock(&eng->pair_lock);
    pairing_slot_t* slot = NULL;
    for (size_t i = 0; i < SHADOW_PAIRING_SLOTS_CAPACITY; i++) {
        if (!eng->pairing_slots[i].active) {
            slot = &eng->pairing_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&eng->pair_lock);
        return false;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    snprintf(slot->eval_id, sizeof(slot->eval_id), "%s", eval_id);
    if (trace_id != NULL) {
        snprintf(slot->trace_id, sizeof(slot->trace_id), "%s", trace_id);
    }
    if (source_model != NULL) {
        snprintf(slot->source_model, sizeof(slot->source_model), "%s", source_model);
    }
    if (target_model != NULL) {
        snprintf(slot->target_model, sizeof(slot->target_model), "%s", target_model);
    }
    slot->mode = mode;
    if (prompt_preview != NULL) {
        snprintf(slot->prompt_preview, sizeof(slot->prompt_preview), "%s", prompt_preview);
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    slot->timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;

    pthread_mutex_unlock(&eng->pair_lock);
    return true;
}

static void
commit_pairing_slot_locked(shadow_engine_t* eng, pairing_slot_t* slot)
{
    shadow_eval_item_t item;
    memset(&item, 0, sizeof(item));
    snprintf(item.eval_id, sizeof(item.eval_id), "%s", slot->eval_id);
    snprintf(item.trace_id, sizeof(item.trace_id), "%s", slot->trace_id);
    snprintf(item.source_model, sizeof(item.source_model), "%s", slot->source_model);
    snprintf(item.target_model, sizeof(item.target_model), "%s", slot->target_model);
    item.mode = slot->mode;

    item.primary_latency_ms = slot->primary_latency_ms;
    item.shadow_latency_ms = slot->shadow_latency_ms;
    item.primary_ttft_ms = slot->primary_ttft_ms;
    item.shadow_ttft_ms = slot->shadow_ttft_ms;
    item.primary_http_status = slot->primary_http_status;
    item.shadow_http_status = slot->shadow_http_status;
    item.primary_tokens = slot->primary_tokens;
    item.shadow_tokens = slot->shadow_tokens;
    item.primary_cost_usd = slot->primary_cost_usd;
    item.shadow_cost_usd = slot->shadow_cost_usd;

    snprintf(item.prompt_preview, sizeof(item.prompt_preview), "%s", slot->prompt_preview);
    snprintf(item.primary_resp_snippet,
             sizeof(item.primary_resp_snippet),
             "%s",
             slot->primary_resp_snippet);
    snprintf(item.shadow_resp_snippet,
             sizeof(item.shadow_resp_snippet),
             "%s",
             slot->shadow_resp_snippet);
    item.timestamp_us = slot->timestamp_us;

    slot->active = false;
    shadow_eval_cache_record(eng->cache, &item);
}

bool
shadow_engine_record_primary(shadow_engine_t* eng,
                             const char*      eval_id,
                             double           latency_ms,
                             double           ttft_ms,
                             int              http_status,
                             long             tokens,
                             double           cost_usd,
                             const char*      resp_snippet)
{
    if (eng == NULL || eval_id == NULL) {
        return false;
    }
    pthread_mutex_lock(&eng->pair_lock);
    pairing_slot_t* slot = NULL;
    for (size_t i = 0; i < SHADOW_PAIRING_SLOTS_CAPACITY; i++) {
        if (eng->pairing_slots[i].active && strcmp(eng->pairing_slots[i].eval_id, eval_id) == 0) {
            slot = &eng->pairing_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&eng->pair_lock);
        return false;
    }

    slot->primary_latency_ms = latency_ms;
    slot->primary_ttft_ms = ttft_ms;
    slot->primary_http_status = http_status;
    slot->primary_tokens = tokens;
    slot->primary_cost_usd = cost_usd;
    if (resp_snippet != NULL) {
        snprintf(
            slot->primary_resp_snippet, sizeof(slot->primary_resp_snippet), "%s", resp_snippet);
    }
    slot->primary_done = true;

    if (slot->shadow_done) {
        commit_pairing_slot_locked(eng, slot);
    }
    pthread_mutex_unlock(&eng->pair_lock);
    return true;
}

bool
shadow_engine_record_shadow(shadow_engine_t* eng,
                            const char*      eval_id,
                            double           latency_ms,
                            double           ttft_ms,
                            int              http_status,
                            long             tokens,
                            double           cost_usd,
                            const char*      resp_snippet)
{
    if (eng == NULL || eval_id == NULL) {
        return false;
    }
    pthread_mutex_lock(&eng->pair_lock);
    pairing_slot_t* slot = NULL;
    for (size_t i = 0; i < SHADOW_PAIRING_SLOTS_CAPACITY; i++) {
        if (eng->pairing_slots[i].active && strcmp(eng->pairing_slots[i].eval_id, eval_id) == 0) {
            slot = &eng->pairing_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&eng->pair_lock);
        return false;
    }

    slot->shadow_latency_ms = latency_ms;
    slot->shadow_ttft_ms = ttft_ms;
    slot->shadow_http_status = http_status;
    slot->shadow_tokens = tokens;
    slot->shadow_cost_usd = cost_usd;
    if (resp_snippet != NULL) {
        snprintf(slot->shadow_resp_snippet, sizeof(slot->shadow_resp_snippet), "%s", resp_snippet);
    }
    slot->shadow_done = true;

    if (slot->primary_done) {
        commit_pairing_slot_locked(eng, slot);
    }
    pthread_mutex_unlock(&eng->pair_lock);
    return true;
}

size_t
shadow_engine_eval_count(shadow_engine_t* eng)
{
    if (eng == NULL) {
        return 0;
    }
    return shadow_eval_cache_count(eng->cache);
}

int
shadow_engine_get_recent_evals(shadow_engine_t* eng, shadow_eval_item_t* out_items, int max_items)
{
    if (eng == NULL) {
        return 0;
    }
    return shadow_eval_cache_get_recent(eng->cache, out_items, max_items);
}

void
shadow_engine_get_stats(shadow_engine_t* eng, shadow_stats_t* out_stats)
{
    if (eng == NULL || out_stats == NULL) {
        return;
    }
    shadow_eval_cache_get_stats(eng->cache, out_stats);
    out_stats->dropped_shadow_requests = shadow_queue_dropped(eng->queue);
}

bool
shadow_engine_submit_task(shadow_engine_t* eng, const shadow_task_t* task)
{
    if (eng == NULL || task == NULL) {
        return false;
    }
    return shadow_queue_push(eng->queue, task);
}

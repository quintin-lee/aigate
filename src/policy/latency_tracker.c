/** @file latency_tracker.c
 *  @ingroup group_policy
 *  @brief P95 & EWMA latency tracking and hedge budget enforcement.
 */
#include "latency_tracker.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Latency tracking statistics entry for a single model and endpoint combination.
 */
typedef struct latency_entry {
    char     model[64];              /**< Model identifier. */
    char     endpoint[256];          /**< Upstream target endpoint URL/host. */
    uint32_t samples_ms
        [LATENCY_TRACKER_WINDOW_SZ]; /**< Ring buffer of recent latency samples in milliseconds. */
    uint32_t         head;           /**< Ring buffer insert cursor index. */
    uint32_t         count;          /**< Valid sample count in ring buffer (up to window size). */
    uint32_t         cached_p95_ms;  /**< Memoized P95 latency in milliseconds. */
    double           ewma_ms; /**< Exponentially weighted moving average latency in milliseconds. */
    uint64_t         total_requests;  /**< Total requests initiated for this entry. */
    uint64_t         hedged_requests; /**< Number of hedged backup requests dispatched. */
    pthread_rwlock_t rwlock;          /**< Read-write lock protecting metrics and ring buffer. */
} latency_entry_t;

/**
 * @brief Global latency tracker managing a fixed table of model/endpoint entries.
 */
struct latency_tracker {
    latency_entry_t entries
        [LATENCY_TRACKER_MAX_ENTRIES]; /**< Table of tracked model/endpoint latency entries. */
    size_t           count;            /**< Number of allocated entries in table. */
    pthread_rwlock_t table_lock; /**< Read-write lock protecting entry insertion and lookups. */
};

static uint32_t
calc_p95(const uint32_t* samples, uint32_t n)
{
    if (n == 0) {
        return 1000;
    }
    uint32_t copy[LATENCY_TRACKER_WINDOW_SZ];
    memcpy(copy, samples, n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n - 1; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            if (copy[j] < copy[i]) {
                uint32_t tmp = copy[i];
                copy[i] = copy[j];
                copy[j] = tmp;
            }
        }
    }
    size_t idx = (size_t)(n * 0.95);
    if (idx >= n) {
        idx = n - 1;
    }
    return copy[idx];
}

latency_tracker_t*
latency_tracker_create(void)
{
    latency_tracker_t* lt = calloc(1, sizeof(*lt));
    if (lt == NULL) {
        return NULL;
    }
    pthread_rwlock_init(&lt->table_lock, NULL);
    for (size_t i = 0; i < LATENCY_TRACKER_MAX_ENTRIES; i++) {
        pthread_rwlock_init(&lt->entries[i].rwlock, NULL);
    }
    return lt;
}

void
latency_tracker_destroy(latency_tracker_t* lt)
{
    if (lt == NULL) {
        return;
    }
    pthread_rwlock_destroy(&lt->table_lock);
    for (size_t i = 0; i < LATENCY_TRACKER_MAX_ENTRIES; i++) {
        pthread_rwlock_destroy(&lt->entries[i].rwlock);
    }
    free(lt);
}

static latency_entry_t*
get_or_create_entry(latency_tracker_t* lt, const char* model, const char* endpoint)
{
    if (lt == NULL || model == NULL || endpoint == NULL) {
        return NULL;
    }

    pthread_rwlock_rdlock(&lt->table_lock);
    for (size_t i = 0; i < lt->count; i++) {
        if (strcmp(lt->entries[i].model, model) == 0 &&
            strcmp(lt->entries[i].endpoint, endpoint) == 0) {
            pthread_rwlock_unlock(&lt->table_lock);
            return &lt->entries[i];
        }
    }
    pthread_rwlock_unlock(&lt->table_lock);

    /* Upgrade to write lock to insert */
    pthread_rwlock_wrlock(&lt->table_lock);
    /* Re-check in case another thread inserted it */
    for (size_t i = 0; i < lt->count; i++) {
        if (strcmp(lt->entries[i].model, model) == 0 &&
            strcmp(lt->entries[i].endpoint, endpoint) == 0) {
            pthread_rwlock_unlock(&lt->table_lock);
            return &lt->entries[i];
        }
    }

    if (lt->count >= LATENCY_TRACKER_MAX_ENTRIES) {
        pthread_rwlock_unlock(&lt->table_lock);
        return NULL;
    }

    latency_entry_t* entry = &lt->entries[lt->count++];
    snprintf(entry->model, sizeof(entry->model), "%s", model);
    snprintf(entry->endpoint, sizeof(entry->endpoint), "%s", endpoint);
    entry->head = 0;
    entry->count = 0;
    entry->cached_p95_ms = 1000;
    entry->ewma_ms = 1000.0;
    entry->total_requests = 0;
    entry->hedged_requests = 0;

    pthread_rwlock_unlock(&lt->table_lock);
    return entry;
}

void
latency_tracker_record(latency_tracker_t* lt,
                       const char*        model,
                       const char*        endpoint,
                       uint64_t           latency_ns)
{
    latency_entry_t* entry = get_or_create_entry(lt, model, endpoint);
    if (entry == NULL) {
        return;
    }

    uint32_t ms = (uint32_t)(latency_ns / 1000000ULL);
    if (ms == 0) {
        ms = 1;
    }

    pthread_rwlock_wrlock(&entry->rwlock);
    if (entry->count == 0) {
        entry->ewma_ms = (double)ms;
    } else {
        entry->ewma_ms = 0.2 * (double)ms + 0.8 * entry->ewma_ms;
    }

    entry->samples_ms[entry->head % LATENCY_TRACKER_WINDOW_SZ] = ms;
    entry->head++;
    if (entry->count < LATENCY_TRACKER_WINDOW_SZ) {
        entry->count++;
    }

    entry->cached_p95_ms = calc_p95(entry->samples_ms, entry->count);
    pthread_rwlock_unlock(&entry->rwlock);
}

uint32_t
latency_tracker_get_p95_ms(latency_tracker_t* lt, const char* model, const char* endpoint)
{
    if (lt == NULL || model == NULL || endpoint == NULL) {
        return 1000;
    }

    pthread_rwlock_rdlock(&lt->table_lock);
    for (size_t i = 0; i < lt->count; i++) {
        if (strcmp(lt->entries[i].model, model) == 0 &&
            strcmp(lt->entries[i].endpoint, endpoint) == 0) {
            pthread_rwlock_rdlock(&lt->entries[i].rwlock);
            uint32_t res = lt->entries[i].cached_p95_ms;
            pthread_rwlock_unlock(&lt->entries[i].rwlock);
            pthread_rwlock_unlock(&lt->table_lock);
            return res;
        }
    }
    pthread_rwlock_unlock(&lt->table_lock);
    return 1000;
}

uint32_t
latency_tracker_get_ewma_ms(latency_tracker_t* lt, const char* model, const char* endpoint)
{
    if (lt == NULL || model == NULL || endpoint == NULL) {
        return 1000;
    }

    pthread_rwlock_rdlock(&lt->table_lock);
    for (size_t i = 0; i < lt->count; i++) {
        if (strcmp(lt->entries[i].model, model) == 0 &&
            strcmp(lt->entries[i].endpoint, endpoint) == 0) {
            pthread_rwlock_rdlock(&lt->entries[i].rwlock);
            uint32_t res = (uint32_t)lt->entries[i].ewma_ms;
            pthread_rwlock_unlock(&lt->entries[i].rwlock);
            pthread_rwlock_unlock(&lt->table_lock);
            return res;
        }
    }
    pthread_rwlock_unlock(&lt->table_lock);
    return 1000;
}

void
latency_tracker_record_request(latency_tracker_t* lt, const char* model)
{
    latency_entry_t* entry = get_or_create_entry(lt, model, "*");
    if (entry == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&entry->rwlock);
    entry->total_requests++;
    pthread_rwlock_unlock(&entry->rwlock);
}

void
latency_tracker_record_hedge(latency_tracker_t* lt, const char* model)
{
    latency_entry_t* entry = get_or_create_entry(lt, model, "*");
    if (entry == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&entry->rwlock);
    entry->hedged_requests++;
    pthread_rwlock_unlock(&entry->rwlock);
}

bool
latency_tracker_hedge_admitted(latency_tracker_t* lt, const char* model, int budget_pct)
{
    if (budget_pct <= 0) {
        return false;
    }
    latency_entry_t* entry = get_or_create_entry(lt, model, "*");
    if (entry == NULL) {
        return false;
    }
    pthread_rwlock_rdlock(&entry->rwlock);
    uint64_t total = entry->total_requests;
    uint64_t hedged = entry->hedged_requests;
    pthread_rwlock_unlock(&entry->rwlock);

    /* (hedged * 100) / (total + 1) <= budget_pct */
    return ((hedged * 100) / (total + 1)) <= (uint64_t)budget_pct;
}

void
latency_tracker_reset(latency_tracker_t* lt)
{
    if (lt == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&lt->table_lock);
    for (size_t i = 0; i < lt->count; i++) {
        pthread_rwlock_wrlock(&lt->entries[i].rwlock);
        lt->entries[i].head = 0;
        lt->entries[i].count = 0;
        lt->entries[i].cached_p95_ms = 1000;
        lt->entries[i].ewma_ms = 1000.0;
        lt->entries[i].total_requests = 0;
        lt->entries[i].hedged_requests = 0;
        pthread_rwlock_unlock(&lt->entries[i].rwlock);
    }
    lt->count = 0;
    pthread_rwlock_unlock(&lt->table_lock);
}

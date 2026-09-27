/** @file response_cache.h
 *  @brief High-performance sharded in-memory LRU response cache with canonical fingerprinting.
 */
#ifndef AIGATE_RESPONSE_CACHE_H
#define AIGATE_RESPONSE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define CACHE_SHARDS_COUNT 16
#define CACHE_BUCKETS_PER_SHARD 1024

typedef struct cache_entry {
    char                cache_key[65];     /* 64-hex SHA-256 string + '\0' */
    char                model[64];         /* Model name */
    char*               response_body;     /* Cached full JSON response */
    size_t              response_len;      /* Length of response_body */
    int                 status_code;       /* HTTP status (typically 200) */
    long                prompt_tokens;     /* Tokens saved */
    long                completion_tokens; /* Completion tokens saved */
    double              cost_usd;          /* Cost saved */
    time_t              created_at;        /* Created timestamp */
    time_t              expires_at;        /* Expiration timestamp */
    _Atomic int         ref_count;         /* Concurrent reader reference count */

    struct cache_entry* hnext;             /* Hash collision list */
    struct cache_entry* prev;              /* LRU prev */
    struct cache_entry* next;              /* LRU next */
} cache_entry_t;

typedef struct {
    pthread_mutex_t     lock;
    cache_entry_t*      buckets[CACHE_BUCKETS_PER_SHARD];
    cache_entry_t*      lru_head;          /* MRU */
    cache_entry_t*      lru_tail;          /* LRU (eviction target) */
    size_t              count;
    size_t              bytes_used;
    size_t              max_count;
    size_t              max_bytes;
    uint64_t            hits;
    uint64_t            misses;
} cache_shard_t;

typedef struct response_cache {
    cache_shard_t       shards[CACHE_SHARDS_COUNT];
    int                 enabled;
    long                default_ttl_sec;
    size_t              total_max_bytes;
    size_t              total_max_entries;

    _Atomic uint64_t    total_saved_prompt_tokens;
    _Atomic uint64_t    total_saved_completion_tokens;
    _Atomic double      total_saved_cost_usd;
} response_cache_t;

/**
 * @brief Create a new response cache manager.
 * @param max_bytes Total memory cap in bytes across all shards (e.g. 128MB).
 * @param max_entries Total maximum entries across all shards (e.g. 20000).
 * @param default_ttl_sec Default TTL in seconds (e.g. 3600).
 */
response_cache_t* response_cache_new(size_t max_bytes, size_t max_entries, long default_ttl_sec);

/**
 * @brief Free response cache manager and all entries.
 */
void response_cache_free(response_cache_t* rc);

/**
 * @brief Compute canonical SHA-256 fingerprint for an incoming JSON request.
 * @param model Model name (e.g. "gpt-4o").
 * @param json_body Raw request JSON payload.
 * @param body_len Length of json_body.
 * @param out_key Output buffer of at least 65 bytes for hex SHA-256.
 * @return 0 on success, -1 on parsing/hashing error.
 */
int response_cache_fingerprint(const char* model, const char* json_body, size_t body_len, char out_key[65]);

/**
 * @brief Lookup entry by cache_key. If hit, entry is moved to MRU and returned with ref_count incremented.
 * Must call response_cache_release_entry when done.
 */
cache_entry_t* response_cache_get(response_cache_t* rc, const char* key);

/**
 * @brief Decrement reference count and free entry if unlinked.
 */
void response_cache_release_entry(cache_entry_t* entry);

/**
 * @brief Insert or update cache entry.
 */
int response_cache_set(response_cache_t* rc,
                       const char*       key,
                       const char*       model,
                       const char*       body,
                       size_t            len,
                       long              prompt_tokens,
                       long              completion_tokens,
                       double            cost_usd,
                       long              ttl_sec);

/**
 * @brief Purge cache entries, optionally filtered by model (pass NULL for all).
 */
int response_cache_purge(response_cache_t* rc,
                         const char*       model_or_null,
                         size_t*           out_purged_entries,
                         size_t*           out_freed_bytes);

/**
 * @brief Get JSON status string containing hits, misses, hit rate, memory used, etc. Caller must free().
 */
char* response_cache_get_stats_json(response_cache_t* rc);

#endif /* AIGATE_RESPONSE_CACHE_H */

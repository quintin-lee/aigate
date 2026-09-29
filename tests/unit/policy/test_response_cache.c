/** @file test_response_cache.c
 *  @brief Unit tests for sharded LRU response cache and canonical fingerprinting.
 */
#include "response_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

/** @brief Fingerprint-normalization case: whitespace/case folding. */
static void
test_fingerprint_normalization(void)
{
    printf("running test_fingerprint_normalization...\n");
    const char* req1 = "{\"model\":\"GPT-4O\",\"messages\":[{\"role\":\"user\",\"content\":\"hello world\"}],\"temperature\":0.7}";
    const char* req2 = "{\"temperature\":0.7000,\"messages\":[{\"role\":\"user\",\"content\":\"hello world\"}],\"model\":\"gpt-4o\"}";
    const char* req3 = "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"different text\"}],\"temperature\":0.7}";

    char k1[65], k2[65], k3[65];
    assert(response_cache_fingerprint("GPT-4o", req1, strlen(req1), k1) == 0);
    assert(response_cache_fingerprint("gpt-4o", req2, strlen(req2), k2) == 0);
    assert(response_cache_fingerprint("gpt-4o", req3, strlen(req3), k3) == 0);

    /* req1 and req2 should yield the exact same fingerprint despite key order & casing */
    assert(strcmp(k1, k2) == 0);
    /* req3 has different message content, so fingerprint must differ */
    assert(strcmp(k1, k3) != 0);
}

/** @brief Cache put/get and TTL-expiry case. */
static void
test_cache_set_get_ttl(void)
{
    printf("running test_cache_set_get_ttl...\n");
    response_cache_t* rc = response_cache_new(1024 * 1024, 100, 2);
    assert(rc != NULL);

    const char* key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char* body = "{\"choices\":[{\"message\":{\"content\":\"cached response\"}}]}";

    /* 1. Miss initially */
    cache_entry_t* e = response_cache_get(rc, key);
    assert(e == NULL);

    /* 2. Set entry */
    assert(response_cache_set(rc, key, "gpt-4o", body, strlen(body), 10, 5, 0.001, 1) == 0);

    /* 3. Hit immediately */
    e = response_cache_get(rc, key);
    assert(e != NULL);
    assert(strcmp(e->response_body, body) == 0);
    assert(e->prompt_tokens == 10);
    assert(e->completion_tokens == 5);
    response_cache_release_entry(e);

    /* 4. Wait for TTL (1s) to expire */
    sleep(2);
    e = response_cache_get(rc, key);
    assert(e == NULL);

    response_cache_free(rc);
}

/** @brief Cache LRU-eviction case. */
static void
test_cache_lru_eviction(void)
{
    printf("running test_cache_lru_eviction...\n");
    /* Create tiny cache: max 16 entries (1 per shard), max 64KB */
    response_cache_t* rc = response_cache_new(64 * 1024, 16, 3600);
    assert(rc != NULL);

    /* Keys with prefix "00", "01", etc. map to shard 0 */
    char k1[65], k2[65], k3[65];
    snprintf(k1, sizeof(k1), "0011111111111111111111111111111111111111111111111111111111111111");
    snprintf(k2, sizeof(k2), "0022222222222222222222222222222222222222222222222222222222222222");
    snprintf(k3, sizeof(k3), "0033333333333333333333333333333333333333333333333333333333333333");

    const char* b1 = "{\"msg\":\"one\"}";
    const char* b2 = "{\"msg\":\"two\"}";
    const char* b3 = "{\"msg\":\"three\"}";

    response_cache_set(rc, k1, "m1", b1, strlen(b1), 1, 1, 0.0001, 3600);
    response_cache_set(rc, k2, "m1", b2, strlen(b2), 1, 1, 0.0001, 3600);
    response_cache_set(rc, k3, "m1", b3, strlen(b3), 1, 1, 0.0001, 3600);

    /* Shard 0 max_count is 1 (16 entries / 16 shards = 1). Older entries must be evicted. */
    cache_entry_t* e3 = response_cache_get(rc, k3);
    assert(e3 != NULL);
    response_cache_release_entry(e3);

    cache_entry_t* e1 = response_cache_get(rc, k1);
    assert(e1 == NULL);

    response_cache_free(rc);
}

/** @brief Cache purge case. */
static void
test_cache_purge(void)
{
    printf("running test_cache_purge...\n");
    response_cache_t* rc = response_cache_new(1024 * 1024, 100, 3600);
    assert(rc != NULL);

    char k1[65], k2[65];
    snprintf(k1, sizeof(k1), "1111111111111111111111111111111111111111111111111111111111111111");
    snprintf(k2, sizeof(k2), "2222222222222222222222222222222222222222222222222222222222222222");

    response_cache_set(rc, k1, "gpt-4o", "{\"model\":\"gpt-4o\"}", 18, 10, 5, 0.01, 3600);
    response_cache_set(rc, k2, "claude-3-5", "{\"model\":\"claude\"}", 18, 10, 5, 0.01, 3600);

    /* Purge only gpt-4o */
    size_t purged = 0, freed = 0;
    assert(response_cache_purge(rc, "gpt-4o", &purged, &freed) == 0);
    assert(purged == 1);
    assert(freed == 18);

    assert(response_cache_get(rc, k1) == NULL);
    cache_entry_t* e2 = response_cache_get(rc, k2);
    assert(e2 != NULL);
    response_cache_release_entry(e2);

    /* Purge all */
    assert(response_cache_purge(rc, NULL, &purged, &freed) == 0);
    assert(purged == 1);
    assert(response_cache_get(rc, k2) == NULL);

    /* Stats JSON verification */
    char* stats = response_cache_get_stats_json(rc);
    assert(stats != NULL);
    assert(strstr(stats, "\"entries_count\":0") != NULL);
    free(stats);

    response_cache_free(rc);
}

typedef struct {
    response_cache_t* rc;
    int               thread_id;
} worker_arg_t;

/** @brief Cache concurrent worker thread entry. */
static void*
concurrency_worker(void* varg)
{
    worker_arg_t* arg = varg;
    char key[65];
    char body[128];
    for (int i = 0; i < 200; i++) {
        snprintf(key, sizeof(key), "%02x%062d", arg->thread_id % 16, i % 10);
        snprintf(body, sizeof(body), "{\"thread\":%d,\"iter\":%d}", arg->thread_id, i);

        response_cache_set(arg->rc, key, "model-c", body, strlen(body), 5, 5, 0.0005, 3600);
        cache_entry_t* e = response_cache_get(arg->rc, key);
        if (e != NULL) {
            response_cache_release_entry(e);
        }
    }
    return NULL;
}

/** @brief Cache concurrent read/write case. */
static void
test_cache_concurrency(void)
{
    printf("running test_cache_concurrency...\n");
    response_cache_t* rc = response_cache_new(10 * 1024 * 1024, 2000, 3600);
    assert(rc != NULL);

    pthread_t threads[8];
    worker_arg_t args[8];
    for (int i = 0; i < 8; i++) {
        args[i].rc = rc;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, concurrency_worker, &args[i]);
    }

    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }

    char* stats = response_cache_get_stats_json(rc);
    assert(stats != NULL);
    assert(strstr(stats, "\"hits_total\":") != NULL);
    free(stats);

    response_cache_free(rc);
}

void
test_response_cache_all(void)
{
    test_fingerprint_normalization();
    test_cache_set_get_ttl();
    test_cache_lru_eviction();
    test_cache_purge();
    test_cache_concurrency();
    printf("test_response_cache: ALL PASSED\n");
}

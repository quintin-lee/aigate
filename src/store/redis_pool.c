/** @file redis_pool.c
 *  @brief Thread-safe redis connection pool. */
#include "redis_pool.h"
#include "aigate_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct redis_pool {
    char            url[512];
    int             capacity;
    int             timeout_ms;
    int             count;      /* total connections created (idle + acquired) */
    int             idle_count; /* number of available connections in stack */
    redisContext**  stack;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
};

redis_pool_t*
redis_pool_create(const char* url, int pool_size, int timeout_ms)
{
    if (url == NULL || *url == '\0' || pool_size <= 0) {
        return NULL;
    }

    if (pool_size > 512) {
        pool_size = 512;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 100;
    }

    redis_pool_t* pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }

    snprintf(pool->url, sizeof(pool->url), "%s", url);
    pool->capacity = pool_size;
    pool->timeout_ms = timeout_ms;
    pool->count = 0;
    pool->idle_count = 0;

    pool->stack = calloc((size_t)pool_size, sizeof(redisContext*));
    if (pool->stack == NULL) {
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    /* Test initial connection to ensure Redis is reachable */
    redisContext* test_c = redis_connect_url(pool->url, pool->timeout_ms);
    if (test_c == NULL) {
        AIGATE_LOG_ERROR("redis_pool_create: initial connection failed to %s", pool->url);
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cond);
        free(pool->stack);
        free(pool);
        return NULL;
    }

    /* Store initial connection into pool */
    pool->stack[0] = test_c;
    pool->idle_count = 1;
    pool->count = 1;

    return pool;
}

void
redis_pool_destroy(redis_pool_t* pool)
{
    if (pool == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->idle_count; i++) {
        if (pool->stack[i] != NULL) {
            redisFree(pool->stack[i]);
            pool->stack[i] = NULL;
        }
    }
    pool->idle_count = 0;
    pool->count = 0;
    pthread_mutex_unlock(&pool->lock);

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool->stack);
    free(pool);
}

int
redis_pool_capacity(const redis_pool_t* pool)
{
    return pool ? pool->capacity : 0;
}

redisContext*
redis_pool_acquire(redis_pool_t* pool)
{
    if (pool == NULL) {
        return NULL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long nsec = ts.tv_nsec + (long)(pool->timeout_ms % 1000) * 1000000L;
    ts.tv_sec += (time_t)(pool->timeout_ms / 1000) + (time_t)(nsec / 1000000000L);
    ts.tv_nsec = nsec % 1000000000L;

    pthread_mutex_lock(&pool->lock);

    while (pool->idle_count == 0 && pool->count >= pool->capacity) {
        int rc = pthread_cond_timedwait(&pool->cond, &pool->lock, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->lock);
            AIGATE_LOG_ERROR("redis_pool_acquire: pool exhausted, timed out after %d ms",
                             pool->timeout_ms);
            return NULL;
        }
    }

    /* Case 1: An idle connection is available in the stack */
    if (pool->idle_count > 0) {
        redisContext* c = pool->stack[--pool->idle_count];
        pthread_mutex_unlock(&pool->lock);

        /* Health check */
        if (c->err) {
            if (redisReconnect(c) != REDIS_OK) {
                redisFree(c);
                pthread_mutex_lock(&pool->lock);
                pool->count--;
                pthread_cond_signal(&pool->cond);
                pthread_mutex_unlock(&pool->lock);
                return redis_connect_url(pool->url, pool->timeout_ms);
            }
        }
        return c;
    }

    /* Case 2: Pool not yet full, allocate a new connection */
    pool->count++;
    pthread_mutex_unlock(&pool->lock);

    redisContext* c = redis_connect_url(pool->url, pool->timeout_ms);
    if (c == NULL) {
        pthread_mutex_lock(&pool->lock);
        pool->count--;
        pthread_cond_signal(&pool->cond);
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    return c;
}

void
redis_pool_release(redis_pool_t* pool, redisContext* c)
{
    if (pool == NULL || c == NULL) {
        if (c != NULL) {
            redisFree(c);
        }
        return;
    }

    /* If context has an error, attempt to reconnect */
    if (c->err) {
        if (redisReconnect(c) != REDIS_OK) {
            redisFree(c);
            pthread_mutex_lock(&pool->lock);
            pool->count--;
            pthread_cond_signal(&pool->cond);
            pthread_mutex_unlock(&pool->lock);
            return;
        }
    }

    pthread_mutex_lock(&pool->lock);
    if (pool->idle_count < pool->capacity) {
        pool->stack[pool->idle_count++] = c;
        pthread_cond_signal(&pool->cond);
    } else {
        /* Pool somehow overflowed, free excess */
        redisFree(c);
        pool->count--;
    }
    pthread_mutex_unlock(&pool->lock);
}

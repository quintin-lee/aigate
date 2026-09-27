/** @file redis_pool.h
 *  @brief Thread-safe redis connection pool. */
#ifndef AIGATE_REDIS_POOL_H
#define AIGATE_REDIS_POOL_H

#include "redis_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_pool redis_pool_t;

/**
 * @brief Create a thread-safe connection pool for Redis.
 * @param url Connection string
 * @param pool_size Maximum number of pooled connections
 * @param timeout_ms Per-operation and connection timeout
 * @return Allocated pool, or NULL if url is invalid/empty or initial test fails
 */
redis_pool_t* redis_pool_create(const char* url, int pool_size, int timeout_ms);

/**
 * @brief Destroy the pool and close all open connections.
 */
void redis_pool_destroy(redis_pool_t* pool);

/**
 * @brief Acquire an active connection from the pool.
 * @return redisContext* or NULL if timeout or connection failure
 */
redisContext* redis_pool_acquire(redis_pool_t* pool);

/**
 * @brief Return a connection to the pool.
 * @param pool The pool
 * @param c Active connection
 */
void redis_pool_release(redis_pool_t* pool, redisContext* c);

/**
 * @brief Get pool capacity.
 */
int redis_pool_capacity(const redis_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_REDIS_POOL_H */

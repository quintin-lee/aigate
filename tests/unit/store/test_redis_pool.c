/** @file test_redis_pool.c
 *  @brief Unit tests for redis_client and redis_pool. */
#include "run_tests.h"
#include "redis_client.h"
#include "redis_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_redis_pool_invalid_args)
{
    TEST_ASSERT(redis_pool_create(NULL, 10, 100) == NULL, "NULL url rejected");
    TEST_ASSERT(redis_pool_create("", 10, 100) == NULL, "empty url rejected");
    TEST_ASSERT(redis_pool_create("redis://127.0.0.1:6379", 0, 100) == NULL, "zero size rejected");
    TEST_ASSERT(redis_pool_create("redis://127.0.0.1:1", 5, 20) == NULL,
                "unreachable port rejected");
}

TEST_CASE(test_redis_client_eval_and_pool_live)
{
    /* Probe 127.0.0.1:6379 */
    redisContext* probe = redis_connect_url("redis://127.0.0.1:6379", 50);
    if (probe == NULL) {
        /* No local redis running; skip live connection tests */
        return;
    }

    redisReply* pong = (redisReply*)redisCommand(probe, "PING");
    TEST_ASSERT(pong != NULL && strcmp(pong->str, "PONG") == 0, "PING returns PONG");
    if (pong) {
        freeReplyObject(pong);
    }

    /* Test script loading */
    const char* script = "return ARGV[1] * 2";
    char        sha[48] = {0};
    int         rc = redis_script_load(probe, script, sha);
    TEST_ASSERT(rc == 0, "redis_script_load succeeded");
    TEST_ASSERT(strlen(sha) == 40, "sha1 is 40 chars");

    /* Test EVALSHA */
    const char* argv[] = {"21"};
    redisReply* res = redis_eval_sha(probe, sha, script, 0, NULL, argv, 1);
    TEST_ASSERT(res != NULL && res->type == REDIS_REPLY_INTEGER && res->integer == 42,
                "EVALSHA executes correctly (21*2=42)");
    if (res) {
        freeReplyObject(res);
    }

    redisFree(probe);

    /* Test connection pool */
    redis_pool_t* pool = redis_pool_create("redis://127.0.0.1:6379/15", 4, 100);
    TEST_ASSERT(pool != NULL, "redis_pool_create succeeded");
    TEST_ASSERT(redis_pool_capacity(pool) == 4, "pool capacity is 4");

    redisContext* c1 = redis_pool_acquire(pool);
    redisContext* c2 = redis_pool_acquire(pool);
    TEST_ASSERT(c1 != NULL && c2 != NULL && c1 != c2, "acquired distinct connections");

    redis_pool_release(pool, c1);
    redisContext* c3 = redis_pool_acquire(pool);
    TEST_ASSERT(c3 == c1, "reused released connection");

    redis_pool_release(pool, c2);
    redis_pool_release(pool, c3);

    redis_pool_destroy(pool);
}

/** @file ratelimit.c
 *  @brief Token-bucket QPS + daily quotas (see ratelimit.h).
 *
 *  @invariant buckets are created on first touch; the table is open-addressed
 *  with linear probing, sized to a power of two.
 */
#include "ratelimit.h"
#include "redis_client.h"
#include "redis_pool.h"
#include "redis_scripts.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct bucket {
    long     key_id;
    int      in_use;
    double   tokens;         /* available request tokens */
    double   capacity;
    uint64_t last_refill_ns; /* CLOCK_MONOTONIC */
    long     daily_used;
    time_t   day;            /* UTC midnight of the accounting window */
};

struct ratelimit {
    pthread_mutex_t mtx;
    struct bucket*  b;
    size_t          cap;
    size_t          count;
    redis_pool_t*   pool;
    char            sha_qps[48];
    char            sha_quota[48];
};

static uint64_t
mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static time_t
utc_midnight(time_t t)
{
    struct tm tmv;
    gmtime_r(&t, &tmv);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    return timegm(&tmv);
}

static size_t
next_pow2(size_t n)
{
    size_t p = 16;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

static size_t
hash_id(long key_id)
{
    unsigned long x = (unsigned long)key_id;
    x ^= x >> 16;
    x *= 0x45d9f3b;
    x ^= x >> 16;
    return x;
}

ratelimit_t*
ratelimit_new(void)
{
    ratelimit_t* rl = calloc(1, sizeof *rl);
    if (rl == NULL) {
        return NULL;
    }
    pthread_mutex_init(&rl->mtx, NULL);
    rl->cap = next_pow2(16);
    rl->b = calloc(rl->cap, sizeof *rl->b);
    if (rl->b == NULL) {
        pthread_mutex_destroy(&rl->mtx);
        free(rl);
        return NULL;
    }
    return rl;
}

void
ratelimit_free(ratelimit_t* rl)
{
    if (rl == NULL) {
        return;
    }
    free(rl->b);
    pthread_mutex_destroy(&rl->mtx);
    free(rl);
}

void
ratelimit_set_redis_pool(ratelimit_t* rl, redis_pool_t* pool)
{
    if (rl == NULL) {
        return;
    }
    pthread_mutex_lock(&rl->mtx);
    rl->pool = pool;
    if (pool != NULL) {
        redisContext* c = redis_pool_acquire(pool);
        if (c != NULL) {
            redis_script_load(c, SCRIPT_QPS_TOKEN_BUCKET, rl->sha_qps);
            redis_script_load(c, SCRIPT_DAILY_QUOTA_CONSUME, rl->sha_quota);
            redis_pool_release(pool, c);
        }
    }
    pthread_mutex_unlock(&rl->mtx);
}

/* @invariant caller holds rl->mtx. */
static struct bucket*
find_or_make(ratelimit_t* rl, long key_id)
{
    size_t idx = hash_id(key_id) & (rl->cap - 1);
    for (;;) {
        struct bucket* bt = &rl->b[idx];
        if (!bt->in_use) {
            if (rl->count >= rl->cap * 3 / 4) {
                /* rehash at 1.5x */
                size_t         ncap = rl->cap * 2;
                struct bucket* nb = calloc(ncap, sizeof *nb);
                if (nb == NULL) {
                    return NULL;
                }
                for (size_t i = 0; i < rl->cap; i++) {
                    if (!rl->b[i].in_use) {
                        continue;
                    }
                    size_t j = hash_id(rl->b[i].key_id) & (ncap - 1);
                    while (nb[j].in_use) {
                        j = (j + 1) & (ncap - 1);
                    }
                    nb[j] = rl->b[i];
                }
                free(rl->b);
                rl->b = nb;
                rl->cap = ncap;
                idx = hash_id(key_id) & (ncap - 1);
                continue;
            }
            memset(bt, 0, sizeof *bt);
            bt->in_use = 1;
            bt->key_id = key_id;
            rl->count++;
            return bt;
        }
        if (bt->in_use && bt->key_id == key_id) {
            return bt;
        }
        idx = (idx + 1) & (rl->cap - 1);
    }
}

int
rl_allow_request(ratelimit_t* rl, long key_id, int qps, long* retry_ms)
{
    if (rl == NULL) {
        if (retry_ms != NULL) {
            *retry_ms = -1;
        }
        return -1;
    }

    if (rl->pool != NULL) {
        if (qps <= 0) {
            return 0;
        }
        redisContext* c = redis_pool_acquire(rl->pool);
        if (c == NULL) {
            if (retry_ms != NULL) {
                *retry_ms = -1;
            }
            return -1;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;

        char key_buf[64];
        snprintf(key_buf, sizeof(key_buf), "aigate:rl:qps:%ld", key_id);
        const char* keys[1] = { key_buf };

        char now_buf[32], qps_buf[32], cap_buf[32], ttl_buf[16];
        snprintf(now_buf, sizeof(now_buf), "%llu", (unsigned long long)now_ms);
        snprintf(qps_buf, sizeof(qps_buf), "%d", qps);
        snprintf(cap_buf, sizeof(cap_buf), "%d", qps);
        snprintf(ttl_buf, sizeof(ttl_buf), "3");

        const char* argv[4] = { now_buf, qps_buf, cap_buf, ttl_buf };
        redisReply* reply =
            redis_eval_sha(c, rl->sha_qps, SCRIPT_QPS_TOKEN_BUCKET, 1, keys, argv, 4);
        if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2 ||
            reply->element[0]->type != REDIS_REPLY_INTEGER ||
            reply->element[1]->type != REDIS_REPLY_INTEGER) {
            if (reply != NULL) {
                freeReplyObject(reply);
            }
            redis_pool_release(rl->pool, c);
            if (retry_ms != NULL) {
                *retry_ms = -1;
            }
            return -1;
        }

        long status = reply->element[0]->integer;
        long wait_ms = reply->element[1]->integer;
        freeReplyObject(reply);
        redis_pool_release(rl->pool, c);

        if (status == 1) {
            return 0;
        }
        if (retry_ms != NULL) {
            *retry_ms = wait_ms;
        }
        return -1;
    }

    struct bucket* bt;
    pthread_mutex_lock(&rl->mtx);
    bt = find_or_make(rl, key_id);
    if (bt == NULL) {
        pthread_mutex_unlock(&rl->mtx);
        return -1;
    }
    if (qps <= 0) {
        bt->capacity = 0;
        bt->tokens = 0;
        pthread_mutex_unlock(&rl->mtx);
        return 0;
    }
    uint64_t now = mono_ns();
    if (bt->capacity == 0 || bt->last_refill_ns == 0) {
        bt->capacity = (double)qps;
        bt->tokens = (double)qps;
        bt->last_refill_ns = now;
    } else {
        double elapsed_s = (double)(now - bt->last_refill_ns) / 1e9;
        bt->tokens += elapsed_s * (double)qps;
        if (bt->tokens > bt->capacity) {
            bt->tokens = bt->capacity;
        }
        bt->last_refill_ns = now;
    }
    int rc = 0;
    if (bt->tokens >= 1.0) {
        bt->tokens -= 1.0;
    } else {
        double need = (1.0 - bt->tokens) / (double)qps;
        long   ms = (long)(need * 1000.0) + 1;
        if (ms < 1) {
            ms = 1;
        }
        if (retry_ms != NULL) {
            *retry_ms = ms;
        }
        rc = -1;
    }
    pthread_mutex_unlock(&rl->mtx);
    return rc;
}

int
rl_reserve_tokens(ratelimit_t* rl, long key_id, long daily_quota, long tokens)
{
    if (rl == NULL) {
        return -1;
    }

    if (rl->pool != NULL) {
        if (daily_quota <= 0 && tokens <= 0) {
            return 0;
        }
        redisContext* c = redis_pool_acquire(rl->pool);
        if (c == NULL) {
            return -1;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        time_t day_epoch = ts.tv_sec - (ts.tv_sec % 86400);

        char key_buf[64];
        snprintf(key_buf, sizeof(key_buf), "aigate:quota:%ld:%ld", key_id, (long)day_epoch);
        const char* keys[1] = { key_buf };

        char tokens_buf[32], quota_buf[32], ttl_buf[16];
        snprintf(tokens_buf, sizeof(tokens_buf), "%ld", tokens);
        snprintf(quota_buf, sizeof(quota_buf), "%ld", daily_quota);
        snprintf(ttl_buf, sizeof(ttl_buf), "172800");

        const char* argv[3] = { tokens_buf, quota_buf, ttl_buf };
        redisReply* reply =
            redis_eval_sha(c, rl->sha_quota, SCRIPT_DAILY_QUOTA_CONSUME, 1, keys, argv, 3);
        if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2 ||
            reply->element[0]->type != REDIS_REPLY_INTEGER) {
            if (reply != NULL) {
                freeReplyObject(reply);
            }
            redis_pool_release(rl->pool, c);
            return -1;
        }

        long status = reply->element[0]->integer;
        freeReplyObject(reply);
        redis_pool_release(rl->pool, c);
        return (status == 0) ? 0 : -1;
    }

    struct bucket* bt;
    int            rc = 0;
    pthread_mutex_lock(&rl->mtx);
    bt = find_or_make(rl, key_id);
    if (bt == NULL) {
        rc = -1;
    } else {
        /* Record-first: the tokens are always accounted; -1 merely
         * reports that the daily quota is (now) exceeded. */
        bt->daily_used += tokens;
        rc = (daily_quota > 0 && bt->daily_used > daily_quota) ? -1 : 0;
    }
    pthread_mutex_unlock(&rl->mtx);
    return rc;
}

long
rl_remaining_daily(ratelimit_t* rl, long key_id, long daily_quota)
{
    if (rl == NULL) {
        return LONG_MIN;
    }

    if (rl->pool != NULL) {
        if (daily_quota <= 0) {
            return LONG_MAX;
        }
        redisContext* c = redis_pool_acquire(rl->pool);
        if (c == NULL) {
            return LONG_MIN;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        time_t day_epoch = ts.tv_sec - (ts.tv_sec % 86400);

        char key_buf[64];
        snprintf(key_buf, sizeof(key_buf), "aigate:quota:%ld:%ld", key_id, (long)day_epoch);

        redisReply* reply = (redisReply*)redisCommand(c, "GET %s", key_buf);
        if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
            if (reply != NULL) {
                freeReplyObject(reply);
            }
            redis_pool_release(rl->pool, c);
            return LONG_MIN;
        }

        long rem = daily_quota;
        if (reply->type == REDIS_REPLY_STRING) {
            long used = strtol(reply->str, NULL, 10);
            rem = daily_quota - used;
        }
        freeReplyObject(reply);
        redis_pool_release(rl->pool, c);
        return rem;
    }

    struct bucket* bt;
    long           rem;
    pthread_mutex_lock(&rl->mtx);
    bt = find_or_make(rl, key_id);
    if (bt == NULL || daily_quota <= 0) {
        rem = LONG_MAX;
    } else {
        rem = daily_quota - bt->daily_used;
    }
    pthread_mutex_unlock(&rl->mtx);
    return rem;
}

void
rl_reset_day(ratelimit_t* rl, time_t now)
{
    time_t mid = utc_midnight(now);
    pthread_mutex_lock(&rl->mtx);
    for (size_t i = 0; i < rl->cap; i++) {
        struct bucket* bt = &rl->b[i];
        if (bt->in_use && bt->day != mid) {
            bt->day = mid;
            bt->daily_used = 0;
        }
    }
    pthread_mutex_unlock(&rl->mtx);
}

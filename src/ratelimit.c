/** @file ratelimit.c
 *  @brief Token-bucket QPS + daily quotas (see ratelimit.h).
 *
 *  @invariant buckets are created on first touch; the table is open-addressed
 *  with linear probing, sized to a power of two.
 */
#include "ratelimit.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct bucket {
  long key_id;
  int  in_use;
  double tokens;          /* available request tokens */
  double capacity;
  uint64_t last_refill_ns; /* CLOCK_MONOTONIC */
  long daily_used;
  time_t day;             /* UTC midnight of the accounting window */
};

struct ratelimit {
  pthread_mutex_t mtx;
  struct bucket *b;
  size_t cap;
  size_t count;
};

static uint64_t mono_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static time_t utc_midnight(time_t t)
{
  struct tm tmv;
  gmtime_r(&t, &tmv);
  tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
  return timegm(&tmv);
}

static size_t next_pow2(size_t n)
{
  size_t p = 16;
  while (p < n)
    p <<= 1;
  return p;
}

static size_t hash_id(long key_id)
{
  unsigned long x = (unsigned long)key_id;
  x ^= x >> 16;
  x *= 0x45d9f3b;
  x ^= x >> 16;
  return x;
}

ratelimit_t *ratelimit_new(void)
{
  ratelimit_t *rl = calloc(1, sizeof *rl);
  if (rl == NULL)
    return NULL;
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

void ratelimit_free(ratelimit_t *rl)
{
  if (rl == NULL)
    return;
  free(rl->b);
  pthread_mutex_destroy(&rl->mtx);
  free(rl);
}

/* @invariant caller holds rl->mtx. */
static struct bucket *find_or_make(ratelimit_t *rl, long key_id)
{
  size_t idx = hash_id(key_id) & (rl->cap - 1);
  for (;;) {
    struct bucket *bt = &rl->b[idx];
    if (!bt->in_use) {
      if (rl->count >= rl->cap * 3 / 4) {
        /* rehash at 1.5x */
        size_t ncap = rl->cap * 2;
        struct bucket *nb = calloc(ncap, sizeof *nb);
        if (nb == NULL)
          return NULL;
        for (size_t i = 0; i < rl->cap; i++) {
          if (!rl->b[i].in_use)
            continue;
          size_t j = hash_id(rl->b[i].key_id) & (ncap - 1);
          while (nb[j].in_use)
            j = (j + 1) & (ncap - 1);
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
    if (bt->in_use && bt->key_id == key_id)
      return bt;
    idx = (idx + 1) & (rl->cap - 1);
  }
}

int rl_allow_request(ratelimit_t *rl, long key_id, int qps, long *retry_ms)
{
  struct bucket *bt;
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
    if (bt->tokens > bt->capacity)
      bt->tokens = bt->capacity;
    bt->last_refill_ns = now;
  }
  int rc = 0;
  if (bt->tokens >= 1.0) {
    bt->tokens -= 1.0;
  } else {
    double need = (1.0 - bt->tokens) / (double)qps;
    long ms = (long)(need * 1000.0) + 1;
    if (ms < 1)
      ms = 1;
    if (retry_ms != NULL)
      *retry_ms = ms;
    rc = -1;
  }
  pthread_mutex_unlock(&rl->mtx);
  return rc;
}

int rl_reserve_tokens(ratelimit_t *rl, long key_id, long daily_quota, long tokens)
{
  struct bucket *bt;
  int rc = 0;
  pthread_mutex_lock(&rl->mtx);
  bt = find_or_make(rl, key_id);
  if (bt == NULL)
    rc = -1;
  else if (daily_quota > 0 && bt->daily_used + tokens > daily_quota)
    rc = -1;
  else
    bt->daily_used += tokens;
  pthread_mutex_unlock(&rl->mtx);
  return rc;
}

long rl_remaining_daily(ratelimit_t *rl, long key_id, long daily_quota)
{
  struct bucket *bt;
  long rem;
  pthread_mutex_lock(&rl->mtx);
  bt = find_or_make(rl, key_id);
  if (bt == NULL || daily_quota <= 0)
    rem = LONG_MAX;
  else
    rem = daily_quota - bt->daily_used;
  pthread_mutex_unlock(&rl->mtx);
  return rem;
}

void rl_reset_day(ratelimit_t *rl, time_t now)
{
  time_t mid = utc_midnight(now);
  pthread_mutex_lock(&rl->mtx);
  for (size_t i = 0; i < rl->cap; i++) {
    struct bucket *bt = &rl->b[i];
    if (bt->in_use && bt->day != mid) {
      bt->day = mid;
      bt->daily_used = 0;
    }
  }
  pthread_mutex_unlock(&rl->mtx);
}

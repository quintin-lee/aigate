/** @file ratelimit.h
 *  @brief Per-key QPS token buckets + daily token quotas.
 *
 *  The bucket table is an open-addressed map keyed by key_id, guarded by a
 *  single mutex. Token refill is computed lazily from the monotonic clock so
 *  no background thread is needed on the hot path.
 */
#ifndef AIGATE_RATELIMIT_H
#define AIGATE_RATELIMIT_H

#include <limits.h>
#include <time.h>

/** @brief Rate limiter state; created once per process. */
typedef struct ratelimit ratelimit_t;

/** @brief New limiter with a default table; grows on demand. */
ratelimit_t* ratelimit_new(void);

/** @brief Free the limiter and all buckets. */
void ratelimit_free(ratelimit_t* rl);

/** @brief Admit one request under @p key_id's QPS budget.
 * @param qps    the key's configured QPS (0 = unlimited, always admitted)
 * @param retry_ms receives ms until a token is expected when denied
 * @return 0 admitted; -1 denied (rate), with *retry_ms set. */
int rl_allow_request(ratelimit_t* rl, long key_id, int qps, long* retry_ms);

/** @brief Consume @p tokens of the key's daily quota after upstream usage.
 * Tokens are recorded first (accounting always happens); the return
 * reports whether the running total now exceeds @p daily_quota.
 * @param daily_quota 0 = unlimited (never returns -1)
 * @return 0 within quota (or unlimited); -1 over quota or allocation failure. */
int rl_reserve_tokens(ratelimit_t* rl, long key_id, long daily_quota, long tokens);

/** @brief Remaining daily tokens; LONG_MAX when unlimited or unused yet. */
long rl_remaining_daily(ratelimit_t* rl, long key_id, long daily_quota);

/** @brief Reset daily counters for @p now (flush worker calls on rollover). */
void rl_reset_day(ratelimit_t* rl, time_t now);

#endif /* AIGATE_RATELIMIT_H */

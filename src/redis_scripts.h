/** @file redis_scripts.h
 *  @brief Lua scripts for atomic Redis clustering operations. */
#ifndef AIGATE_REDIS_SCRIPTS_H
#define AIGATE_REDIS_SCRIPTS_H

/* QPS token bucket:
 * KEYS[1]: aigate:rl:qps:{key_id}
 * ARGV[1]: now_ms
 * ARGV[2]: qps
 * ARGV[3]: capacity
 * ARGV[4]: ttl_s
 * Returns: {status, retry_ms} where status=1 (admitted), 0 (denied)
 */
static const char SCRIPT_QPS_TOKEN_BUCKET[] =
    "local key = KEYS[1]\n"
    "local now = tonumber(ARGV[1])\n"
    "local qps = tonumber(ARGV[2])\n"
    "local capacity = tonumber(ARGV[3])\n"
    "local ttl = tonumber(ARGV[4])\n"
    "local data = redis.call('HMGET', key, 'tokens', 'last_ms')\n"
    "local tokens = tonumber(data[1])\n"
    "local last_ms = tonumber(data[2])\n"
    "if not tokens or not last_ms then\n"
    "    tokens = capacity\n"
    "    last_ms = now\n"
    "else\n"
    "    local delta = math.max(0, now - last_ms)\n"
    "    tokens = math.min(capacity, tokens + delta * (qps / 1000.0))\n"
    "    last_ms = now\n"
    "end\n"
    "if tokens >= 1.0 then\n"
    "    tokens = tokens - 1.0\n"
    "    redis.call('HMSET', key, 'tokens', tokens, 'last_ms', last_ms)\n"
    "    redis.call('EXPIRE', key, ttl)\n"
    "    return {1, 0}\n"
    "else\n"
    "    local wait_ms = math.ceil((1.0 - tokens) * 1000.0 / qps)\n"
    "    redis.call('HMSET', key, 'tokens', tokens, 'last_ms', last_ms)\n"
    "    redis.call('EXPIRE', key, ttl)\n"
    "    return {0, wait_ms}\n"
    "end\n";

/* Daily Token Quota Consume:
 * KEYS[1]: aigate:quota:{key_id}:{day_epoch}
 * ARGV[1]: tokens_consumed
 * ARGV[2]: daily_quota
 * ARGV[3]: ttl_s
 * Returns: {status, total} where status=0 (ok), -1 (over quota)
 */
static const char SCRIPT_DAILY_QUOTA_CONSUME[] =
    "local key = KEYS[1]\n"
    "local consumed = tonumber(ARGV[1])\n"
    "local quota = tonumber(ARGV[2])\n"
    "local ttl = tonumber(ARGV[3])\n"
    "local total = redis.call('INCRBY', key, consumed)\n"
    "if total == consumed then\n"
    "    redis.call('EXPIRE', key, ttl)\n"
    "end\n"
    "if quota > 0 and total > quota then\n"
    "    return {-1, total}\n"
    "end\n"
    "return {0, total}\n";

/* Admin IP lockout:
 * KEYS[1]: aigate:lockout:{client_ip}
 * ARGV[1]: max_fails
 * ARGV[2]: window_s
 * Returns: {is_locked, current_fails}
 */
static const char SCRIPT_ADMIN_LOCKOUT[] =
    "local key = KEYS[1]\n"
    "local max_fails = tonumber(ARGV[1])\n"
    "local window_s = tonumber(ARGV[2])\n"
    "local fails = redis.call('INCR', key)\n"
    "if fails == 1 then\n"
    "    redis.call('EXPIRE', key, window_s)\n"
    "end\n"
    "if fails >= max_fails then\n"
    "    return {1, fails}\n"
    "end\n"
    "return {0, fails}\n";

/* Circuit Breaker Sync:
 * KEYS[1]: aigate:cb:{endpoint_hash}
 * ARGV[1]: action ('allow', 'success', 'fail')
 * ARGV[2]: now_ms
 * ARGV[3]: max_fails
 * ARGV[4]: cooldown_ms
 * Returns: {allowed, current_state} (state: 0=closed, 1=half_open, 2=open)
 */
static const char SCRIPT_CIRCUIT_BREAKER_SYNC[] =
    "local key = KEYS[1]\n"
    "local action = ARGV[1]\n"
    "local now_ms = tonumber(ARGV[2])\n"
    "local max_fails = tonumber(ARGV[3])\n"
    "local cooldown_ms = tonumber(ARGV[4])\n"
    "local d = redis.call('HMGET', key, 'state', 'fails', 'open_until', 'probe_active')\n"
    "local state = tonumber(d[1]) or 0\n"
    "local fails = tonumber(d[2]) or 0\n"
    "local open_until = tonumber(d[3]) or 0\n"
    "local probe = tonumber(d[4]) or 0\n"
    "if action == 'allow' then\n"
    "    if state == 2 then\n"
    "        if now_ms >= open_until then\n"
    "            state = 1\n"
    "            redis.call('HMSET', key, 'state', 1, 'fails', fails, 'probe_active', 1)\n"
    "            redis.call('EXPIRE', key, 86400)\n"
    "            return {1, 1}\n"
    "        else\n"
    "            return {0, 2}\n"
    "        end\n"
    "    elseif state == 1 then\n"
    "        if probe == 0 then\n"
    "            redis.call('HSET', key, 'probe_active', 1)\n"
    "            redis.call('EXPIRE', key, 86400)\n"
    "            return {1, 1}\n"
    "        else\n"
    "            return {0, 1}\n"
    "        end\n"
    "    else\n"
    "        return {1, 0}\n"
    "    end\n"
    "elseif action == 'success' then\n"
    "    if state == 1 or fails > 0 or state == 2 then\n"
    "        redis.call('HMSET', key, 'state', 0, 'fails', 0, 'open_until', 0, 'probe_active', 0)\n"
    "        redis.call('EXPIRE', key, 86400)\n"
    "    end\n"
    "    return {1, 0}\n"
    "elseif action == 'fail' then\n"
    "    if state == 1 then\n"
    "        state = 2\n"
    "        open_until = now_ms + cooldown_ms\n"
    "        redis.call('HMSET', key, 'state', 2, 'open_until', open_until, 'probe_active', 0)\n"
    "        redis.call('EXPIRE', key, 86400)\n"
    "        return {0, 2}\n"
    "    else\n"
    "        fails = fails + 1\n"
    "        if fails >= max_fails then\n"
    "            state = 2\n"
    "            open_until = now_ms + cooldown_ms\n"
    "            redis.call('HMSET', key, 'state', 2, 'fails', fails, 'open_until', open_until, 'probe_active', 0)\n"
    "            redis.call('EXPIRE', key, 86400)\n"
    "            return {0, 2}\n"
    "        else\n"
    "            redis.call('HMSET', key, 'fails', fails)\n"
    "            redis.call('EXPIRE', key, 86400)\n"
    "            return {1, 0}\n"
    "        end\n"
    "    end\n"
    "end\n"
    "return {1, 0}\n";

#endif /* AIGATE_REDIS_SCRIPTS_H */

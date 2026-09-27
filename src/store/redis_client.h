/** @file redis_client.h
 *  @brief hiredis connection helpers and Lua script evaluation. */
#ifndef AIGATE_REDIS_CLIENT_H
#define AIGATE_REDIS_CLIENT_H

#include <hiredis/hiredis.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse Redis URL and connect with timeout.
 * Format: redis://[:password@]host[:port][/db] or host:port
 * @param url Redis connection string
 * @param timeout_ms Connection and read/write timeout in milliseconds
 * @return redisContext* or NULL on failure
 */
redisContext* redis_connect_url(const char* url, int timeout_ms);

/**
 * @brief Load a Lua script into Redis and populate its 40-char hex SHA1.
 * @param c Active redis context
 * @param script Lua script source
 * @param sha_out Buffer of at least 41 bytes for hex sha1
 * @return 0 on success, -1 on failure
 */
int redis_script_load(redisContext* c, const char* script, char* sha_out);

/**
 * @brief Execute a Lua script via EVALSHA with automatic NOSCRIPT fallback.
 * @param c Active redis context
 * @param sha Hex SHA1 of script
 * @param script Full script source for fallback
 * @param numkeys Number of keys
 * @param keys Array of key strings
 * @param argv Array of arg strings
 * @param argc Number of arg strings
 * @return redisReply* (caller must free with freeReplyObject), or NULL on network failure
 */
redisReply* redis_eval_sha(redisContext* c,
                           const char*   sha,
                           const char*   script,
                           int           numkeys,
                           const char**  keys,
                           const char**  argv,
                           int           argc);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_REDIS_CLIENT_H */

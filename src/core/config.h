/** @file config.h
 *  @ingroup group_core
 *  @brief Startup configuration loaded from environment variables (spec section 5). */
#ifndef AIGATE_CONFIG_H
#define AIGATE_CONFIG_H

/** @brief Process configuration; all fields are fixed-length (stack-allocated). */
typedef struct aigate_config {
    char listen[64];           /**< "host:port" 或 ":port"，默认 ":8080"。 */
    char pg_dsn[1024];         /**< 必填。 */
    char admin_token_hash[65]; /**< AIGATE_ADMIN_TOKEN 的 SHA-256 hex，本处计算。 */
    char master_key[65];       /**< 64 位小写 hex（32 字节），可选（空表未设）。 */
    int  upstream_timeout_ms;  /**< 默认 60000，范围 (0, 600000]。 */
    char metrics_acl[256];     /**< 逗号分隔 IPv4 CIDR，默认 "127.0.0.1"。 */
    int  usage_flush_s;        /**< AIGATE_USAGE_FLUSH_S，默认 5，范围 [1,3600]。 */
    long max_body_bytes;       /**< AIGATE_MAX_BODY_BYTES，默认 10MiB，范围 (0,1GiB]。 */
    char redis_url[512];       /**< AIGATE_REDIS_URL，默认 ""（禁用）。 */
    int  redis_timeout_ms;     /**< AIGATE_REDIS_TIMEOUT_MS，默认 100，范围 [1,60000]。 */
    int  redis_pool_size;      /**< AIGATE_REDIS_POOL_SIZE，默认 32，范围 [1,512]。 */
} aigate_config;

/** @brief Fill @p out from environment variables.
 * @return 0 on success.
 * @return -1 with AIGATE_LOG_ERROR diagnostics when AIGATE_PG_DSN or
 *         AIGATE_ADMIN_TOKEN is missing, when AIGATE_MASTER_KEY is present but
 *         not 64 hex chars, or when AIGATE_UPSTREAM_TIMEOUT_MS is out of range.
 * @note Defaults: AIGATE_LISTEN ":8080", AIGATE_UPSTREAM_TIMEOUT_MS 60000,
 *       AIGATE_METRICS_ACL "127.0.0.1". */
int aigate_config_load(aigate_config* out);

#endif /* AIGATE_CONFIG_H */

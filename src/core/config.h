/** @file config.h
 *  @ingroup group_core
 *  @brief Startup configuration loaded from environment variables (spec section 5). */
#ifndef AIGATE_CONFIG_H
#define AIGATE_CONFIG_H

/** @brief Process configuration; all fields are fixed-length (stack-allocated). */
typedef struct aigate_config {
    char listen[64];           /**< "host:port" or ":port", default ":8080" */
    char pg_dsn[1024];         /**< Required */
    char admin_token_hash[65]; /**< SHA-256 hex of AIGATE_ADMIN_TOKEN, computed here */
    char master_key[65];       /**< 64 lowercase hex chars (32 bytes), optional (empty if unset) */
    int  upstream_timeout_ms;  /**< Default 60000, range (0, 600000] */
    char metrics_acl[256];     /**< Comma-separated IPv4 CIDR, default "127.0.0.1" */
    int  usage_flush_s;        /**< AIGATE_USAGE_FLUSH_S, default 5, range [1,3600] */
    long max_body_bytes;       /**< AIGATE_MAX_BODY_BYTES, default 10MiB, range (0,1GiB] */
    char redis_url[512];       /**< AIGATE_REDIS_URL, default "" (disabled) */
    int  redis_timeout_ms;     /**< AIGATE_REDIS_TIMEOUT_MS, default 100, range [1,60000] */
    int  redis_pool_size;      /**< AIGATE_REDIS_POOL_SIZE, default 32, range [1,512] */
    int  worker_threads;       /**< AIGATE_WORKER_THREADS, default 64, range [4, 4096] */
    int request_timeout_ms; /**< AIGATE_REQUEST_TIMEOUT_MS, default 300000, range [1000, 3600000] */
    char   trusted_proxies[256];   /**< AIGATE_TRUSTED_PROXIES, default "127.0.0.1" */
    int    drain_timeout_s;        /**< AIGATE_DRAIN_TIMEOUT_S, default 15, range [0, 120] */
    int    redis_fail_open;        /**< AIGATE_REDIS_FAIL_OPEN, default 1 (0 or 1) */
    char   cors_allow_origin[128]; /**< AIGATE_CORS_ALLOW_ORIGIN, default "*" */
    char   log_format[16];         /**< AIGATE_LOG_FORMAT, "text" (default) or "json" */
    char   log_level[16];       /**< AIGATE_LOG_LEVEL, "debug", "info" (default), "warn", "error" */
    char   ssl_cert[512];       /**< AIGATE_SSL_CERT, path to SSL certificate PEM */
    char   ssl_key[512];        /**< AIGATE_SSL_KEY, path to SSL private key PEM */
    char   audit_log_file[512]; /**< AIGATE_AUDIT_LOG_FILE, default "" */
    int    audit_max_size_mb;   /**< AIGATE_AUDIT_MAX_SIZE_MB, default 100 */
    int    audit_max_backups;   /**< AIGATE_AUDIT_MAX_BACKUPS, default 5 */
    char   audit_webhook_url[512];   /**< AIGATE_AUDIT_WEBHOOK_URL, default "" */
    char   audit_webhook_format[32]; /**< AIGATE_AUDIT_WEBHOOK_FORMAT, default "standard" */
    int    audit_max_prompt_len;     /**< AIGATE_AUDIT_MAX_PROMPT_LEN, default 4096 */
    double audit_sample_rate;        /**< AIGATE_AUDIT_SAMPLE_RATE, default 1.0 */
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

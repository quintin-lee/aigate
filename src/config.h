/** @file config.h
 *  @brief Startup configuration loaded from environment variables (spec section 5). */
#ifndef AIGATE_CONFIG_H
#define AIGATE_CONFIG_H

/** @brief Process configuration; all fields are fixed-length (stack-allocated). */
typedef struct aigate_config {
    char listen[64];           /* "host:port" or ":port", default ":8080" */
    char pg_dsn[1024];         /* required */
    char admin_token_hash[65]; /* SHA-256 hex of AIGATE_ADMIN_TOKEN, computed here */
    char master_key[65];       /* 64 lowercase hex = 32 bytes, optional (empty = unset) */
    int  upstream_timeout_ms;  /* default 60000, range (0, 600000] */
    char metrics_acl[256];     /* comma-separated IPv4 CIDRs, default "127.0.0.1" */
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

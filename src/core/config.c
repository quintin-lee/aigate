/** @file config.c
 *  @brief Environment-variable configuration loader (see config.h). */
#include "config.h"
#include "aigate_log.h"
#include "sha256.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/** @brief Read env var (default if unset/empty) and truncate into @p out.
 *  @return Always 0. */
static int
env_str(const char* name, const char* defval, char* out, size_t cap)
{
    const char* v = getenv(name);
    if (v == NULL || *v == '\0') {
        v = defval;
    }
    snprintf(out, cap, "%s", v);
    return 0;
}

int
aigate_config_load(aigate_config* out)
{
    const char *raw, *token, *mkey;
    char        raw_key[1024];

    memset(out, 0, sizeof *out);

    env_str("AIGATE_LISTEN", ":8080", out->listen, sizeof out->listen);

    raw = getenv("AIGATE_PG_DSN");
    if (raw == NULL || *raw == '\0') {
        AIGATE_LOG_ERROR("AIGATE_PG_DSN is required");
        return -1;
    }
    if (strlen(raw) >= sizeof out->pg_dsn) {
        AIGATE_LOG_ERROR("AIGATE_PG_DSN too long (max %zu)", sizeof out->pg_dsn - 1);
        return -1;
    }
    snprintf(out->pg_dsn, sizeof out->pg_dsn, "%s", raw);

    token = getenv("AIGATE_ADMIN_TOKEN");
    if (token == NULL || *token == '\0') {
        AIGATE_LOG_ERROR("AIGATE_ADMIN_TOKEN is required");
        return -1;
    }
    if (sha256_hex(token, strlen(token), out->admin_token_hash) != 0) {
        AIGATE_LOG_ERROR("sha256 failed");
        return -1;
    }

    mkey = getenv("AIGATE_MASTER_KEY");
    if (mkey != NULL && *mkey != '\0') {
        if (strlen(mkey) != 64) {
            AIGATE_LOG_ERROR("AIGATE_MASTER_KEY must be 64 hex chars (32 bytes)");
            return -1;
        }
        for (int i = 0; i < 64; i++) {
            if (!isxdigit((unsigned char)mkey[i])) {
                AIGATE_LOG_ERROR("AIGATE_MASTER_KEY must be 64 hex chars");
                return -1;
            }
        }
        snprintf(out->master_key, sizeof out->master_key, "%s", mkey);
    }

    env_str("AIGATE_UPSTREAM_TIMEOUT_MS", "60000", raw_key, sizeof raw_key);
    out->upstream_timeout_ms = atoi(raw_key);
    if (out->upstream_timeout_ms <= 0 || out->upstream_timeout_ms > 600000) {
        AIGATE_LOG_ERROR("AIGATE_UPSTREAM_TIMEOUT_MS must be in (0, 600000], got %d",
                         out->upstream_timeout_ms);
        return -1;
    }

    env_str("AIGATE_METRICS_ACL", "127.0.0.1", out->metrics_acl, sizeof out->metrics_acl);

    env_str("AIGATE_USAGE_FLUSH_S", "5", raw_key, sizeof raw_key);
    out->usage_flush_s = atoi(raw_key);
    if (out->usage_flush_s < 1 || out->usage_flush_s > 3600) {
        AIGATE_LOG_ERROR("AIGATE_USAGE_FLUSH_S must be in [1,3600], got %d", out->usage_flush_s);
        return -1;
    }

    env_str("AIGATE_MAX_BODY_BYTES", "10485760", raw_key, sizeof raw_key);
    out->max_body_bytes = atol(raw_key);
    if (out->max_body_bytes <= 0 || out->max_body_bytes > 1073741824L) {
        AIGATE_LOG_ERROR("AIGATE_MAX_BODY_BYTES must be in (0,1GiB], got %ld", out->max_body_bytes);
        return -1;
    }

    env_str("AIGATE_REDIS_URL", "", out->redis_url, sizeof out->redis_url);

    env_str("AIGATE_REDIS_TIMEOUT_MS", "100", raw_key, sizeof raw_key);
    out->redis_timeout_ms = atoi(raw_key);
    if (out->redis_timeout_ms <= 0 || out->redis_timeout_ms > 60000) {
        out->redis_timeout_ms = 100;
    }

    env_str("AIGATE_REDIS_POOL_SIZE", "32", raw_key, sizeof raw_key);
    out->redis_pool_size = atoi(raw_key);
    if (out->redis_pool_size <= 0 || out->redis_pool_size > 512) {
        out->redis_pool_size = 32;
    }

    env_str("AIGATE_WORKER_THREADS", "64", raw_key, sizeof raw_key);
    out->worker_threads = atoi(raw_key);
    if (out->worker_threads < 4 || out->worker_threads > 4096) {
        AIGATE_LOG_ERROR("AIGATE_WORKER_THREADS must be in [4, 4096], got %d", out->worker_threads);
        return -1;
    }

    env_str("AIGATE_REQUEST_TIMEOUT_MS", "300000", raw_key, sizeof raw_key);
    out->request_timeout_ms = atoi(raw_key);
    if (out->request_timeout_ms < 1000 || out->request_timeout_ms > 3600000) {
        AIGATE_LOG_ERROR("AIGATE_REQUEST_TIMEOUT_MS must be in [1000, 3600000], got %d",
                         out->request_timeout_ms);
        return -1;
    }

    env_str(
        "AIGATE_TRUSTED_PROXIES", "127.0.0.1", out->trusted_proxies, sizeof out->trusted_proxies);

    env_str("AIGATE_DRAIN_TIMEOUT_S", "15", raw_key, sizeof raw_key);
    out->drain_timeout_s = atoi(raw_key);
    if (out->drain_timeout_s < 0 || out->drain_timeout_s > 120) {
        AIGATE_LOG_ERROR("AIGATE_DRAIN_TIMEOUT_S must be in [0, 120], got %d",
                         out->drain_timeout_s);
        return -1;
    }

    env_str("AIGATE_REDIS_FAIL_OPEN", "1", raw_key, sizeof raw_key);
    out->redis_fail_open = (atoi(raw_key) != 0) ? 1 : 0;

    env_str("AIGATE_CORS_ALLOW_ORIGIN", "*", out->cors_allow_origin, sizeof out->cors_allow_origin);
    if (out->cors_allow_origin[0] == '\0') {
        snprintf(out->cors_allow_origin, sizeof out->cors_allow_origin, "*");
    }

    env_str("AIGATE_LOG_FORMAT", "text", out->log_format, sizeof out->log_format);
    if (strcasecmp(out->log_format, "json") != 0 && strcasecmp(out->log_format, "text") != 0) {
        AIGATE_LOG_WARN("invalid AIGATE_LOG_FORMAT '%s', defaulting to 'text'", out->log_format);
        snprintf(out->log_format, sizeof out->log_format, "text");
    }

    env_str("AIGATE_LOG_LEVEL", "info", out->log_level, sizeof out->log_level);
    if (strcasecmp(out->log_level, "debug") != 0 && strcasecmp(out->log_level, "info") != 0 &&
        strcasecmp(out->log_level, "warn") != 0 && strcasecmp(out->log_level, "error") != 0) {
        AIGATE_LOG_WARN("invalid AIGATE_LOG_LEVEL '%s', defaulting to 'info'", out->log_level);
        snprintf(out->log_level, sizeof out->log_level, "info");
    }

    env_str("AIGATE_SSL_CERT", "", out->ssl_cert, sizeof out->ssl_cert);
    env_str("AIGATE_SSL_KEY", "", out->ssl_key, sizeof out->ssl_key);

    return 0;
}

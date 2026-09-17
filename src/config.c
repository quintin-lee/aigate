/** @file config.c
 *  @brief Environment-variable configuration loader (see config.h). */
#include "config.h"
#include "aigate_log.h"
#include "sha256.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int env_str(const char *name, const char *defval, char *out, size_t cap)
{
  const char *v = getenv(name);
  if (v == NULL || *v == '\0')
    v = defval;
  snprintf(out, cap, "%s", v);
  return 0;
}

int aigate_config_load(aigate_config *out)
{
  const char *raw, *token, *mkey;
  char raw_key[1024];

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

  return 0;
}

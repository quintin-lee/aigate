/** @file schema_sql.h
 *  @brief Embedded copy of schema/schema.sql (generated; do not edit by hand). */
#ifndef AIGATE_SCHEMA_SQL_H
#define AIGATE_SCHEMA_SQL_H

static const char SCHEMA_SQL[] =
    R"SQL(-- aigate schema (version 1); applied by pg_store_migrate() in one transaction.
-- Idempotent: safe to re-run; schema_migrations tracks applied versions.
CREATE TABLE IF NOT EXISTS api_keys (
  key_id          BIGSERIAL PRIMARY KEY,
  key_hash        TEXT NOT NULL,
  name            TEXT NOT NULL,
  allowed_models  TEXT[] NOT NULL DEFAULT '{}',
  rate_qps        INT NOT NULL DEFAULT 0,
  daily_token_quota INT NOT NULL DEFAULT 0,
  expires_at      TIMESTAMPTZ,
  revoked_at      TIMESTAMPTZ,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX IF NOT EXISTS ux_api_keys_hash ON api_keys (key_hash);

CREATE TABLE IF NOT EXISTS models (
  model_name       TEXT PRIMARY KEY,
  provider         TEXT NOT NULL,
  endpoint         TEXT NOT NULL,
  upstream_key_ref TEXT,
  default_params   JSONB NOT NULL DEFAULT '{}',
  enabled          BOOLEAN NOT NULL DEFAULT true
);

CREATE TABLE IF NOT EXISTS upstream_secrets (
  ref      TEXT PRIMARY KEY,
  cipher   BYTEA NOT NULL,
  provider TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS usage_daily (
  key_id          BIGINT NOT NULL REFERENCES api_keys(key_id) ON DELETE CASCADE,
  model_name      TEXT NOT NULL REFERENCES models(model_name) ON DELETE CASCADE,
  day             DATE NOT NULL,
  requests        BIGINT NOT NULL DEFAULT 0,
  prompt_tokens   BIGINT NOT NULL DEFAULT 0,
  completion_tokens BIGINT NOT NULL DEFAULT 0,
  errors          BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (key_id, model_name, day)
);

CREATE TABLE IF NOT EXISTS admin_tokens (
  token_hash TEXT PRIMARY KEY,
  name       TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS schema_migrations (
  version    INT PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO schema_migrations(version) VALUES (1) ON CONFLICT (version) DO NOTHING;
)SQL";

#endif /* AIGATE_SCHEMA_SQL_H */

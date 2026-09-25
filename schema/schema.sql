-- aigate schema (version 1); applied by pg_store_migrate() in one transaction.
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

-- Migration v2: prompt cache token metering
ALTER TABLE usage_daily ADD COLUMN IF NOT EXISTS cached_prompt_tokens BIGINT NOT NULL DEFAULT 0;
INSERT INTO schema_migrations(version) VALUES (2) ON CONFLICT (version) DO NOTHING;

-- Migration v3: multi-upstream targets & load balancing policy
ALTER TABLE models ADD COLUMN IF NOT EXISTS targets JSONB NOT NULL DEFAULT '[]';
ALTER TABLE models ADD COLUMN IF NOT EXISTS lb_policy TEXT NOT NULL DEFAULT 'priority';
INSERT INTO schema_migrations(version) VALUES (3) ON CONFLICT (version) DO NOTHING;

-- Migration v4: upstream providers management
CREATE TABLE IF NOT EXISTS providers (
  id            BIGSERIAL PRIMARY KEY,
  name          TEXT NOT NULL UNIQUE,
  provider_type TEXT NOT NULL,
  endpoint      TEXT NOT NULL,
  api_key       TEXT NOT NULL DEFAULT '',
  models        TEXT[] NOT NULL DEFAULT '{}',
  enabled       BOOLEAN NOT NULL DEFAULT true,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO schema_migrations(version) VALUES (4) ON CONFLICT (version) DO NOTHING;

-- Migration v5: BIGINT quota + model name length guard (matches C 128 cap)
ALTER TABLE api_keys ALTER COLUMN daily_token_quota TYPE BIGINT;
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'ck_models_name_len') THEN
        ALTER TABLE models ADD CONSTRAINT ck_models_name_len CHECK (char_length(model_name) <= 127);
    END IF;
END
$$;
INSERT INTO schema_migrations(version) VALUES (5) ON CONFLICT (version) DO NOTHING;

-- Migration v6: per-request usage audit detail (P0-1)
CREATE TABLE IF NOT EXISTS usage_requests (
  key_id          BIGINT NOT NULL,
  model_name      TEXT NOT NULL,
  provider        TEXT NOT NULL DEFAULT '',
  http_status     INT NOT NULL,
  prompt_tokens   BIGINT NOT NULL DEFAULT 0,
  completion_tokens BIGINT NOT NULL DEFAULT 0,
  cached_prompt_tokens BIGINT NOT NULL DEFAULT 0,
  latency_ns      BIGINT NOT NULL DEFAULT 0,
  ts              BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_usage_requests_key_ts ON usage_requests (key_id, ts);
CREATE INDEX IF NOT EXISTS ix_usage_requests_ts ON usage_requests (ts);
INSERT INTO schema_migrations(version) VALUES (6) ON CONFLICT (version) DO NOTHING;

-- Migration v7: dept groups + model pricing (internal SaaS cost attribution)
CREATE TABLE IF NOT EXISTS groups (
  id         BIGSERIAL PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS group_id BIGINT
  REFERENCES groups(id) ON DELETE SET NULL;
ALTER TABLE models ADD COLUMN IF NOT EXISTS pricing JSONB NOT NULL DEFAULT '{}';
INSERT INTO schema_migrations(version) VALUES (7) ON CONFLICT (version) DO NOTHING;



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

-- Migration v8: reasoning tokens for OpenAI Responses API / CoT models
ALTER TABLE usage_requests ADD COLUMN IF NOT EXISTS reasoning_tokens BIGINT NOT NULL DEFAULT 0;
INSERT INTO schema_migrations(version) VALUES (8) ON CONFLICT (version) DO NOTHING;

-- Migration v9: guardrails rules and budget limits
CREATE TABLE IF NOT EXISTS guardrails_rules (
  id          BIGSERIAL PRIMARY KEY,
  rule_type   TEXT NOT NULL,
  pattern     TEXT NOT NULL,
  action      TEXT NOT NULL DEFAULT 'block',
  category    TEXT NOT NULL DEFAULT 'general',
  enabled     BOOLEAN NOT NULL DEFAULT true,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS ix_guardrails_enabled ON guardrails_rules(enabled, rule_type);

ALTER TABLE api_keys 
  ADD COLUMN IF NOT EXISTS guardrails_enabled BOOLEAN NOT NULL DEFAULT true,
  ADD COLUMN IF NOT EXISTS monthly_cost_budget NUMERIC(12, 4) NOT NULL DEFAULT 0.0000,
  ADD COLUMN IF NOT EXISTS monthly_token_budget BIGINT NOT NULL DEFAULT 0;

ALTER TABLE groups 
  ADD COLUMN IF NOT EXISTS monthly_budget_usd NUMERIC(12, 4) NOT NULL DEFAULT 0.0000;

ALTER TABLE usage_requests 
  ADD COLUMN IF NOT EXISTS guardrail_action TEXT NOT NULL DEFAULT '';

INSERT INTO schema_migrations(version) VALUES (9) ON CONFLICT (version) DO NOTHING;

-- Migration v10: prompt templates and modes for models and api_keys
ALTER TABLE models
  ADD COLUMN IF NOT EXISTS system_prompt TEXT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS prompt_mode INT NOT NULL DEFAULT 0;

ALTER TABLE api_keys
  ADD COLUMN IF NOT EXISTS system_prompt TEXT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS prompt_mode INT NOT NULL DEFAULT 0;

INSERT INTO schema_migrations(version) VALUES (10) ON CONFLICT (version) DO NOTHING;

-- Migration v11: external webhook moderation plugin support
ALTER TABLE guardrails_rules
  ADD COLUMN IF NOT EXISTS webhook_secret TEXT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS timeout_ms INT NOT NULL DEFAULT 500,
  ADD COLUMN IF NOT EXISTS fail_mode VARCHAR(16) NOT NULL DEFAULT 'open',
  ADD COLUMN IF NOT EXISTS phase VARCHAR(16) NOT NULL DEFAULT 'inbound';

INSERT INTO schema_migrations(version) VALUES (11) ON CONFLICT (version) DO NOTHING;

-- Migration v12: adaptive latency routing & hedged requests support
ALTER TABLE models
  ADD COLUMN IF NOT EXISTS hedged_delay_ms INT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS hedge_budget_pct INT NOT NULL DEFAULT 15,
  ADD COLUMN IF NOT EXISTS hedged_enabled BOOLEAN NOT NULL DEFAULT FALSE;

INSERT INTO schema_migrations(version) VALUES (12) ON CONFLICT (version) DO NOTHING;

-- Migration v13: traffic shadowing and canary A/B testing rules
CREATE TABLE IF NOT EXISTS shadow_rules (
  id              BIGSERIAL PRIMARY KEY,
  source_model    TEXT NOT NULL,
  target_model    TEXT NOT NULL,
  target_provider TEXT NOT NULL DEFAULT '',
  mode            VARCHAR(16) NOT NULL DEFAULT 'shadow',
  sample_rate     DOUBLE PRECISION NOT NULL DEFAULT 1.0,
  header_match    TEXT NOT NULL DEFAULT '',
  enabled         BOOLEAN NOT NULL DEFAULT true,
  timeout_ms      INT NOT NULL DEFAULT 10000,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS ix_shadow_rules_enabled ON shadow_rules(enabled, source_model);
INSERT INTO schema_migrations(version) VALUES (13) ON CONFLICT (version) DO NOTHING;

-- Migration v14: prompt compression and token pruning rules
CREATE TABLE IF NOT EXISTS compressor_rules (
    id                  VARCHAR(36) PRIMARY KEY,
    model_pattern       VARCHAR(64) NOT NULL,
    enabled             BOOLEAN NOT NULL DEFAULT TRUE,
    level               INTEGER NOT NULL DEFAULT 1,
    min_tokens          INTEGER NOT NULL DEFAULT 2048,
    max_history_turns   INTEGER NOT NULL DEFAULT 6,
    target_ratio        DOUBLE PRECISION NOT NULL DEFAULT 0.60,
    preserve_system     BOOLEAN NOT NULL DEFAULT TRUE,
    preserve_code       BOOLEAN NOT NULL DEFAULT TRUE,
    preserve_tools      BOOLEAN NOT NULL DEFAULT TRUE,
    created_at          BIGINT NOT NULL,
    updated_at          BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_compressor_rules_model ON compressor_rules(enabled, model_pattern);
INSERT INTO schema_migrations(version) VALUES (14) ON CONFLICT (version) DO NOTHING;

-- Migration v15: prompt cache prefix alignment and optimization rules
CREATE TABLE IF NOT EXISTS cache_optimizer_rules (
    id                          VARCHAR(36) PRIMARY KEY,
    model_pattern               VARCHAR(64) NOT NULL,
    enabled                     BOOLEAN NOT NULL DEFAULT TRUE,
    sort_tools                  BOOLEAN NOT NULL DEFAULT TRUE,
    sink_dynamic_system         BOOLEAN NOT NULL DEFAULT TRUE,
    inject_anthropic_breakpoints BOOLEAN NOT NULL DEFAULT TRUE,
    min_tokens_threshold        INTEGER NOT NULL DEFAULT 1024,
    created_at                  BIGINT NOT NULL,
    updated_at                  BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_cache_optimizer_rules_model ON cache_optimizer_rules(enabled, model_pattern);
INSERT INTO schema_migrations(version) VALUES (15) ON CONFLICT (version) DO NOTHING;

-- Migration v16: persistent audit violations for compliance and forensics
CREATE TABLE IF NOT EXISTS audit_violations (
    id                  BIGSERIAL PRIMARY KEY,
    trace_id            VARCHAR(64) NOT NULL,
    tenant_id           VARCHAR(64) NOT NULL DEFAULT '',
    client_ip           VARCHAR(48) NOT NULL DEFAULT '',
    model               VARCHAR(64) NOT NULL,
    routed_model        VARCHAR(64) NOT NULL DEFAULT '',
    severity            VARCHAR(16) NOT NULL,
    rule_tag            VARCHAR(64) NOT NULL,
    http_status         INT NOT NULL DEFAULT 400,
    ttft_ms             INT NOT NULL DEFAULT 0,
    total_latency_ms    INT NOT NULL DEFAULT 0,
    fallback_reason     VARCHAR(32) NOT NULL DEFAULT '',
    prompt_snapshot     TEXT,
    completion_snapshot TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_audit_violations_search ON audit_violations (tenant_id, model, rule_tag, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_audit_violations_trace ON audit_violations (trace_id);
INSERT INTO schema_migrations(version) VALUES (16) ON CONFLICT (version) DO NOTHING;

-- Migration v17: zero-width watermark enabling on api_keys
ALTER TABLE api_keys
  ADD COLUMN IF NOT EXISTS watermark_enabled BOOLEAN NOT NULL DEFAULT false;

INSERT INTO schema_migrations(version) VALUES (17) ON CONFLICT (version) DO NOTHING;

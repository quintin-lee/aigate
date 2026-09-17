# AI Gateway (aigate) — Design Spec

Date: 2026-09-17
Status: Approved (brainstorming complete)

## 1. Overview

A single-binary C (C11/C17) AI gateway that proxies LLM requests from clients
speaking OpenAI-compatible protocol to multiple upstream providers, with:

- **Secure model access**: per-client API keys (hashed, revocable, expiring,
  per-key model allowlists, rate limits), admin-token-protected management API.
- **Model monitoring**: Prometheus `/metrics` (QPS, latency HDR histograms,
  errors) plus per-key/per-model daily token usage persisted to PostgreSQL.
- **Dynamic model configuration**: full CRUD of models, keys, quotas, and
  usage queries through `/admin/v1` REST endpoints; PostgreSQL as system of
  record; in-memory LRU caches on the hot path.
- **SSE streaming pass-through** for `stream: true` completions.

Language: C (C17), POSIX. No C++ in the binary.

## 2. Architecture & Transport Seam

### 2.1 Layers

```
 Client ─OpenAI-compatible─▶ civetweb thread pool (transport_civetweb.c)
 Admin  ─Bearer admin──────▶     │  HTTP + SSE handlers
                                  ▼
                          aigate_handle_request()   ← THE SEAM
                          (aigate_core.c)
                              ├── auth_key.c        (LRU + SHA-256 key check)
                              ├── ratelimit.c       (token bucket, atomics)
                              ├── model_router.c    (name → provider/endpoint)
                              ├── upstream_client.c (libcurl, provider adapters)
                              └── usage_meter.c     (atomics, HDR histogram)
                                  │
                          pg_store.c (async batch worker) ──▶ PostgreSQL
                          metrics.c  ──▶ /metrics (Prometheus text)
```

### 2.2 The Seam (A-now / B-later)

Gateway core is **transport-agnostic**. It never touches civetweb types; it
works on a `request_ctx` filled by the transport and writes back through
callbacks. This is the reserved slice for a future libuv (fully async, plan
B) transport: B only writes new `request_ctx` fillers + write adapters; the
core is untouched.

```c
typedef struct aigate_response_ctx aigate_response_ctx;

/** Write one response chunk; @p fin marks the last chunk. */
typedef int (*aigate_write_fn)(aigate_response_ctx *rc,
                               const void *buf, size_t len, bool fin);
/** Set a response header before body starts. */
typedef int (*aigate_set_header_fn)(aigate_response_ctx *rc,
                                    const char *name, const char *value);

/**
 * Process one inbound request end-to-end:
 * auth → route → ratelimit → upstream → write response → meter.
 * Must be re-entrant; never blocks on I/O longer than an upstream call.
 */
int aigate_handle_request(aigate_request_ctx *rq,
                          aigate_response_ctx *rc,
                          aigate_write_fn write,
                          aigate_set_header_fn set_header);
```

`aigate_request_ctx` fields: `method, path, headers (kv list), body
(bytes+len), client_ip`. Upstream I/O is libcurl multi/easy per worker
thread; SSE writes are per-chunk through `write` (non-blocking on the
civetweb side; chunked in libuv later).

### 2.3 Component Table

| Component | Responsibility |
|---|---|
| `transport_civetweb.c` | civetweb routes → ctx fillers, SSE write-back adapter |
| `aigate_core.c` | pipeline orchestration; the seam above |
| `auth_key.c` | Bearer → SHA-256 → LRU check; expiry/revocation; model allowlist; constant-time compare; LRU invalidation on admin changes |
| `ratelimit.c` | per-key token bucket (QPS) + daily token quota; atomic counters; async flush to PG on threshold |
| `model_router.c` | model name → {provider, endpoint, default_params}; 404 on unknown |
| `upstream_client.c` | libcurl transport + provider adapters: `provider_openai.c` (OpenAI-compatible direct, incl. vLLM/Ollama), `provider_anthropic.c` (protocol translation), `provider_azure.c`; timeouts; one retry on non-streaming 502/504 |
| `usage_meter.c` | atomics: requests, prompt/completion tokens, errors, per-key×model latency HDR histogram |
| `admin_api.c` | `/admin/v1` CRUD + usage queries; admin-token (SHA-256) check |
| `pg_store.c` | libpq wrapper, connection management, schema migration runner (embedded `schema.sql`) |
| `metrics.c` | Prometheus text exposition at `/metrics`; periodic PG usage upsert worker (every 5s) |
| `lru.c` | generic LRU with invalidation callbacks |

## 3. Data Model (PostgreSQL)

```sql
CREATE TABLE api_keys (
  key_id        BIGSERIAL PRIMARY KEY,
  key_hash      TEXT NOT NULL,              -- SHA-256 of plaintext key
  name          TEXT NOT NULL,
  allowed_models TEXT[] NOT NULL DEFAULT '{}',  -- {} = all models
  rate_qps      INT NOT NULL DEFAULT 0,      -- 0 = unlimited
  daily_token_quota INT NOT NULL DEFAULT 0,  -- 0 = unlimited
  expires_at    TIMESTAMPTZ,
  revoked_at    TIMESTAMPTZ,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX ux_api_keys_hash ON api_keys (key_hash);

CREATE TABLE models (
  model_name     TEXT PRIMARY KEY,
  provider       TEXT NOT NULL,             -- openai | anthropic | azure | ollama
  endpoint       TEXT NOT NULL,
  upstream_key_ref TEXT,                   -- 'env:OPENAI_API_KEY' or 'pg:<cipher>'
  default_params JSONB NOT NULL DEFAULT '{}',
  enabled        BOOLEAN NOT NULL DEFAULT true
);

CREATE TABLE upstream_secrets (
  ref TEXT PRIMARY KEY,                    -- 'pg:<cipher>'
  cipher BYTEA NOT NULL,                   -- AES-256-GCM, master key from AIGATE_MASTER_KEY
  provider TEXT NOT NULL
);

CREATE TABLE usage_daily (
  key_id BIGINT NOT NULL REFERENCES api_keys(key_id),
  model_name TEXT NOT NULL REFERENCES models(model_name),
  day DATE NOT NULL,
  requests BIGINT NOT NULL DEFAULT 0,
  prompt_tokens BIGINT NOT NULL DEFAULT 0,
  completion_tokens BIGINT NOT NULL DEFAULT 0,
  errors BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (key_id, model_name, day)
);

CREATE TABLE admin_tokens (
  token_hash TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE schema_migrations (
  version INT PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

- Hot path reads only memory (LRU). PG writes are batched: usage upserts
  every 5s from the metrics worker; key/model changes flush-through (admin
  writes are synchronous in their own transaction, then invalidate the LRU
  entry).
- Plaintext client key is returned exactly once at creation.

## 4. API Surface

### 4.1 Data plane (OpenAI-compatible, Bearer client key)

- `POST /v1/chat/completions` — sync + SSE streaming (`stream: true`)
- `POST /v1/completions`
- `POST /v1/embeddings`
- `GET /v1/models` — lists enabled models visible to the calling key

Responses and errors use OpenAI wire format:
`{"error": {"message", "type", "code"}}` with type
`auth_error|rate_limit|model_not_found|upstream_error|internal` and HTTP
401/429/404/502/500. Non-streaming upstream failures: one retry (200ms),
then 502 with `X-Upstream-Provider` response header.

Timeouts: non-streaming 60s total; streaming — no total cap, 30s
inter-chunk silence kills the stream with 502.

### 4.2 Management plane (`/admin/v1`, Bearer admin token)

| Method & path | Description |
|---|---|
| `POST /admin/v1/keys` | create key; response includes plaintext once |
| `GET /admin/v1/keys` | list (hashes only) |
| `PATCH /admin/v1/keys/{id}` | quota, allowlist, expiry |
| `DELETE /admin/v1/keys/{id}` | revoke |
| `POST /admin/v1/models` | register model |
| `GET /admin/v1/models` | list |
| `PATCH /admin/v1/models/{name}` | update params/endpoint |
| `DELETE /admin/v1/models/{name}` | disable/remove |
| `GET /admin/v1/usage?key=&model=&from=&to=` | usage query |

`GET /metrics` — Prometheus text; open (no token) but configurable IP
allowlist (`AIGATE_METRICS_ACL`).

## 5. Security & Ops

- Client/admin keys: SHA-256 at rest, constant-time compare.
- Upstream secrets: AES-256-GCM in `upstream_secrets` (master key =
  `AIGATE_MASTER_KEY` env var), or env-var references (preferred).
- Logs: key_id, model, token counts, latency, status — never full
  prompts/completions.
- TLS optional via civetweb cert config (`AIGATE_TLS_CERT`/`AIGATE_TLS_KEY`).

### Boot config (env vars)

| Var | Default | Notes |
|---|---|---|
| `AIGATE_LISTEN` | `:8080` | listen address |
| `AIGATE_PG_DSN` | (required) | PostgreSQL DSN |
| `AIGATE_ADMIN_TOKEN` | (required) | bootstrap admin token; hashed into `admin_tokens` |
| `AIGATE_MASTER_KEY` | optional | 32-byte hex; required to store upstream secrets in PG |
| `AIGATE_TLS_CERT` / `AIGATE_TLS_KEY` | off | TLS |
| `AIGATE_UPSTREAM_TIMEOUT_MS` | 60000 | non-streaming timeout |
| `AIGATE_METRICS_ACL` | `127.0.0.1` | CIDR list for `/metrics` |

## 6. Testing

- **Unit (C, assert-based):**
  - `ratelimit`: token-bucket concurrency correctness (multi-thread QPS
    boundary tests).
  - `auth_key`: LRU hit/invalidate, expiry/revocation, constant-time compare.
  - `upstream_client` adapters: protocol translation against a local
    civetweb mock upstream returning fixed bodies/SSE streams.
  - `usage_meter`: atomic counters + HDR histogram accuracy.
- **Integration (Python drivers, `tests/integration/`):**
  - real gateway + mock upstream stub + PostgreSQL (Docker or testcontainer):
    create key → auth OK → 429 on limit → revoke → 401 → disable model →
    404 → usage rows land in PG → SSE chunk pass-through verified.
- **Perf baseline:** `wrk`/`hey` non-streaming P99 on a single core; 1000
  concurrent SSE connections held for 5 minutes.

## 7. Build & Layout

- CMake (C17). Vendors as source subdirs: **civetweb, jansson,
  hdr_histogram**. System deps: libcurl, libpq, OpenSSL, pthreads.
  `cmake -DRUN_TESTS=ON` builds test binaries; optional Doxygen target.
- **All `src/` files carry Doxygen comments:**
  - file header: `/** @file ... @brief module purpose */`
  - every exported API: `@brief / @param / @return`
  - internal statics + invariants: brief comments + `@invariant/@note` where
    applicable.

```
aigate/
├── CMakeLists.txt
├── cmake/
├── src/
│   ├── main.c  config.c/h
│   ├── aigate_core.c/h
│   ├── transport_civetweb.c/h
│   ├── auth_key.c/h  ratelimit.c/h  model_router.c/h
│   ├── upstream_client.c/h
│   ├── provider_openai.c  provider_anthropic.c  provider_azure.c
│   ├── usage_meter.c/h  admin_api.c/h  pg_store.c/h
│   ├── metrics.c/h  lru.c/h
├── vendor/   # civetweb, jansson, hdr_histogram
├── schema/schema.sql
├── tests/unit/  tests/integration/
├── docs/superpowers/specs/
└── README.md
```

## 8. Non-Goals

- No OAuth2/OIDC, no multi-node clustering/HA, no request-content audit
  logging, no load-balancing/failover routing across providers (single
  provider per model name), no C++ in the binary.

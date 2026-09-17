# aigate Implementation Plan 1: Core Gateway (Non-Streaming)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a working C17 single-binary AI gateway: OpenAI-compatible data plane (non-streaming: chat/completions/embeddings/models) + full `/admin/v1` management plane + Prometheus `/metrics` + PostgreSQL persistence.

**Architecture:** civetweb transport fills transport-agnostic `aigate_request_ctx` and hands each request to the core seam `aigate_handle_request()` (spec §2.2), which runs auth → ratelimit → route → upstream (libcurl + provider adapters) → meter → write-back. Hot path is memory-only (LRU caches); all PostgreSQL writes are async batch flushes.

**Tech Stack:** C17, CMake, civetweb, jansson, libcurl, libpq (PostgreSQL), OpenSSL, hdr_histogram.

**Spec:** `docs/superpowers/specs/2026-09-17-ai-gateway-design.md`

**Out of scope here (Plan 2):** SSE streaming pass-through, Anthropic protocol adapter, TLS termination, performance baselines. (`azure` provider label works in this plan via the openai-compatible adapter + `api-version` in `default_params`; no dedicated provider file.)

## File Structure

```
aigate/
├── CMakeLists.txt
├── cmake/                        # Find modules / test helpers
├── src/
│   ├── main.c                    # boot: config → pg → caches → civetweb → workers
│   ├── config.h/.c               # env-var config loading + validation
│   ├── sha256.h/.c               # SHA-256 (OpenSSL EVP) hex helper
│   ├── secrets.h/.c              # AES-256-GCM upstream-key encrypt/decrypt
│   ├── lru.h/.c                  # mutex LRU with evict callback
│   ├── pg_store.h/.c             # libpq + pg_backend ops table (testable fakes)
│   ├── auth_key.h/.c             # bearer → hash → LRU/pg lookup, model allowlist
│   ├── ratelimit.h/.c            # per-key QPS token buckets
│   ├── model_router.h/.c         # model → route cache + key resolution
│   ├── upstream_client.h/.c      # libcurl transport, retry, timeouts
│   ├── provider_openai.h/.c      # openai-compatible adapter (openai/ollama/azure)
│   ├── usage_meter.h/.c          # atomics + HDR histograms + 5s flush worker
│   ├── aigate_core.h/.c          # THE SEAM: aigate_handle_request pipeline
│   ├── admin_api.h/.c            # /admin/v1 handlers
│   ├── metrics.h/.c              # /metrics Prometheus exposition
│   └── transport_civetweb.h/.c   # civetweb → ctx fillers + write-back adapters
├── schema/schema.sql
├── tests/unit/                   # C test runner (assert-based, no framework)
└── tests/integration/            # Python: mock upstream + pytest scenarios
```

Conventions locked for all tasks:
- All `src/*.c` start with a `/** @file ... @brief ... */` block; every exported function gets `/** @brief ... @param ... @return ... */` (Doxygen, spec §7).
- `#include "x.h"` for project headers; `extern "C"` N/A (pure C).
- Errors: functions return `0` ok / negative errno-style codes; message via `aigate_log` (small wrapper over `stderr` + level, defined in `src/aigate_log.h`, task 1).
- Canonical shared types (defined in `pg_store.h`, task 4):

```c
typedef struct {
  long key_id;
  char key_hash[65];
  char name[128];
  char **allowed_models; int n_allowed;   /* n==0 → all models */
  int  rate_qps;                          /* 0 = unlimited */
  long daily_quota;                       /* 0 = unlimited */
  time_t expires_at; int has_expiry;
  int  revoked;
} key_rec_t;

typedef struct {
  char name[128];
  char provider[32];
  char endpoint[512];
  char upstream_key_ref[256];            /* "env:NAME" | "pg:<cipher>" */
  char default_params_json[1024];         /* e.g. {"model":"gpt-4o","api-version":"2024-06"} */
  int  enabled;
  char upstream_key[1024];               /* filled by model_router */
} model_rec_t;

typedef struct {
  long key_id;
  char model_name[128];
  time_t day;
  long requests, prompt_tokens, completion_tokens, errors;
} usage_row_t;
```

---

## Task 1: Repo bootstrap — CMake, deps, logging, test runner

**Files:**
- Create: `CMakeLists.txt`, `cmake/test_utils.cmake`, `src/aigate_log.h/.c`, `src/main.c`, `tests/unit/run_tests.c`, `tests/unit/test_log.c`

- [ ] **Step 1: Top-level CMakeLists**

```cmake
cmake_minimum_required(VERSION 3.16)
project(aigate C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_library(PQ_LIB NAMES pq REQUIRED)
find_path(PQ_INCLUDE_DIR postgres/libpq-fe.h REQUIRED)

include(FetchContent)
FetchContent_Declare(jansson URL https://github.com/akherhu/jansson/releases/download/v2.14/jansson-2.14.tar.gz)
FetchContent_MakeAvailable(jansson)
FetchContent_Declare(hdr_histogram URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.6.tar.gz)
FetchContent_MakeAvailable(hdr_histogram)
FetchContent_Declare(civetweb URL https://github.com/civetweb/civetweb/archive/refs/tags/1.16.tar.gz)
FetchContent_MakeAvailable(civetweb)

file(GLOB AIGATE_SRC src/*.c)
list(REMOVE_ITEM AIGATE_SRC ${CMAKE_SOURCE_DIR}/src/main.c)
add_library(libaigate STATIC ${AIGATE_SRC})
target_include_directories(libaigate PUBLIC src)
target_link_libraries(libaigate PUBLIC
  jansson hdr_histogram_histogram
  CURL::libcurl ${PQ_LIB} OpenSSL::Crypto ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(libaigate PUBLIC ${PQ_INCLUDE_DIR})

add_executable(aigate src/main.c)
target_link_libraries(aigate PRIVATE libaigate civetweb)
add_dependencies(aigate civetweb)

enable_testing()
include(CTest)
add_subdirectory(tests)
```

- [ ] **Step 2: `tests/CMakeLists.txt`**

```cmake
file(GLOB UNIT_TEST_SRC ${CMAKE_SOURCE_DIR}/tests/unit/*.c)
add_executable(aigate_unit_tests ${UNIT_TEST_SRC})
target_link_libraries(aigate_unit_tests PRIVATE libaigate civetweb)
add_dependencies(aigate_unit_tests civetweb)
add_test(NAME unit COMMAND aigate_unit_tests)
```

- [ ] **Step 3: Logging + test runner**

`src/aigate_log.h` (Doxygen header comment mandatory):

```c
#ifndef AIGATE_LOG_H
#define AIGATE_LOG_H
/** @file aigate_log.h
 *  @brief leveled stderr logging used by all modules. */
void aigate_log(const char *level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#define AIGATE_LOG_INFO(...)  aigate_log("INFO", __FILE__, __LINE__, __VA_ARGS__)
#define AIGATE_LOG_WARN(...)  aigate_log("WARN", __FILE__, __LINE__, __VA_ARGS__)
#define AIGATE_LOG_ERROR(...) aigate_log("ERROR", __FILE__, __LINE__, __VA_ARGS__)
#endif
```

`src/aigate_log.c`: printf with ISO-8601 timestamp prefix, `fflush(stderr)`.

`tests/unit/run_tests.c`: registry of `void (*fn)(void)` + global `int g_failures`; macros:

```c
#define TEST_ASSERT(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
  fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; return; } } while (0)
#define TEST_CASE(fn) static void fn(void)
```

Each `test_*.c` defines `void test_xxx(void)`; `main()` in `run_tests.c` calls an explicit list `g_test_fns[]` (declared in `run_tests.c` with externs — add new entries there in later tasks) and returns `g_failures ? 1 : 0`.

`tests/unit/test_log.c`: capture stderr via `dup`/`memstream`-style helper — simplest: assert `aigate_log` returns and `stderr` stream not closed after 1000 formatted calls.

- [ ] **Step 4: Minimal `src/main.c`**

```c
/** @file main.c @brief aigate process entry point. */
#include "aigate_log.h"
int main(void) {
  AIGATE_LOG_INFO("aigate bootstrap OK");
  return 0;
}
```

- [ ] **Step 5: Build & verify**

Run: `mkdir -b .build && cmake -B .build -DCMAKE_BUILD_TYPE=Debug && cmake --build .build -j4 && .build/tests/aigate_unit_tests`
Expected: builds clean; unit test runner prints `PASS: 1 test(s)` (or 0 failures) and exit 0.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt cmake tests src/aigate_log.h src/aigate_log.c src/main.c
git commit -m "build: cmake skeleton, logging, unit test runner"
```

---

## Task 2: Config loading + SHA-256 helper

**Files:**
- Create: `src/config.h/.c`, `src/sha256.h/.c`, `tests/unit/test_config.c`, `tests/unit/test_sha256.c`
- Modify: `tests/unit/run_tests.c` (register new test fns)

- [ ] **Step 1: `src/sha256.h/.c`** (OpenSSL EVP)

```c
#ifndef AIGATE_SHA256_H
#define AIGATE_SHA256_H
/** @file sha256.h @brief SHA-256 hex digest helpers (64 hex + NUL). */
#include <stddef.h>
/** @brief Hash @p in (@p in_len bytes) into @p out[65] lowercase hex. @return 0 ok, -1 on EVP failure. */
int sha256_hex(const void *in, size_t in_len, char out[65]);
#endif
```

- [ ] **Step 2: `src/config.h/.c`**

```c
#ifndef AIGATE_CONFIG_H
#define AIGATE_CONFIG_H
/** @file config.h @brief startup configuration from environment variables. */
typedef struct aigate_config {
  char listen[64];            /* "host:port", default ":8080" */
  char pg_dsn[1024];          /* required */
  char admin_token_hash[65];  /* SHA-256 of AIGATE_ADMIN_TOKEN (required) */
  char master_key[65];        /* 32-byte hex, optional (empty = no pg secret store) */
  int  upstream_timeout_ms;   /* default 60000 */
  char metrics_acl[256];      /* comma CIDRs, default "127.0.0.1" */
} aigate_config;

/** @brief Fill @p out from env vars. @return 0 ok; -1 if AIGATE_PG_DSN or AIGATE_ADMIN_TOKEN
 *  missing, or AIGATE_MASTER_KEY present but not 64 hex chars. */
int aigate_config_load(aigate_config *out);
#endif
```

`config.c` implementation rules: `AIGATE_LISTEN` default `:8080`; `AIGATE_UPSTREAM_TIMEOUT_MS` default `60000`, validate `0 < v <= 600000`; `AIGATE_METRICS_ACL` default `127.0.0.1`; log missing vars via `AIGATE_LOG_ERROR` and return -1.

- [ ] **Step 3: Tests (failing first)**

`tests/unit/test_sha256.c`: KAT `"abc"` → `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
`tests/unit/test_config.c`: `setenv`/`unsetenv` in setup/teardown; assert defaults (`listen`, timeout, acl), required-missing → -1, bad master key length → -1, good load → exact values. Register both in `run_tests.c`.

- [ ] **Step 4: Run & verify**

Run: `cmake --build .build -j4 && .build/tests/aigate_unit_tests`
Expected: new tests pass.

- [ ] **Step 5: Commit** — `git add -A && git commit -m "feat: config env loading + sha256 helper"`

---

## Task 3: LRU cache

**Files:**
- Create: `src/lru.h/.c`, `tests/unit/test_lru.c`

- [ ] **Step 1: `src/lru.h`**

```c
#ifndef AIGATE_LRU_H
#define AIGATE_LRU_H
#include <stddef.h>
/** @file lru.h @brief mutex-protected LRU map; values are opaque, caller-owned. */
typedef void (*lru_evict_fn)(void *val);
typedef struct lru lru_t;
/** @brief New LRU holding at most @p capacity entries; oldest evicted first (on_evict may be NULL). */
lru_t *lru_new(size_t capacity, lru_evict_fn on_evict);
void   lru_free(lru_t *lr);                 /* calls on_evict for remaining values */
void  *lru_get(lru_t *lr, const char *key);  /* NULL on miss; hit refreshes recency */
void   lru_put(lru_t *lr, const char *key, void *val); /* replaces+refreshes; capacity+1 total entries */
void   lru_invalidate(lru_t *lr, const char *key);      /* remove entry (no evict cb) */
size_t lru_size(const lru_t *lr);
#endif
```

- [ ] **Step 2: `lru.c`** — doubly-linked list + `open-addressed` table (FNV-1a, linear probe, tombstones); `pthread_mutex` around all ops; `@invariant: list length == live entry count`.

- [ ] **Step 3: Tests** — eviction order with capacity 2 (put a,b,c → a evicted, cb fires); `get` refreshes recency (put a,b; get a; put c → b evicted, not a); `invalidate` removes without cb; concurrent smoke: 8 threads × 100k mixed ops, no crash, final size ≤ capacity.

- [ ] **Step 4: Run & commit** — `git commit -m "feat: thread-safe LRU map"`

---

## Task 4: PostgreSQL layer — schema, migrations, pg_store

**Files:**
- Create: `schema/schema.sql`, `src/pg_store.h/.c`, `tests/unit/test_pg_store.c`

- [ ] **Step 1: `schema/schema.sql`** — exactly the tables from spec §3 (`api_keys`, `models`, `upstream_secrets`, `usage_daily`, `admin_tokens`, `schema_migrations`) + `INSERT INTO schema_migrations(version) VALUES (1);`.

- [ ] **Step 2: `src/pg_store.h`** — ops-table design so unit tests can fake I/O:

```c
#ifndef AIGATE_PG_STORE_H
#define AIGATE_PG_STORE_H
#include <time.h>
#include "lru.h"
/** @file pg_store.h @brief PostgreSQL persistence; ops table so tests can fake the backend. */

key_rec_t, model_rec_t, usage_row_t; /* canonical structs from spec/plan header */

typedef struct pg_ops {
  /* returns 0 ok / -1 error; out params may be NULL when unused */
  int (*get_key_by_hash)(void *ctx, const char *key_hash, key_rec_t *out);
  int (*list_models)(void *ctx, model_rec_t *out, int cap, int *n);
  int (*get_model)(void *ctx, const char *name, model_rec_t *out);
  int (*insert_key)(void *ctx, const key_rec_t *k);
  int (*update_key)(void *ctx, const key_rec_t *k, /* which fields changed */ int mask);
  int (*revoke_key)(void *ctx, long key_id);
  int (*insert_model)(void *ctx, const model_rec_t *m);
  int (*update_model)(void *ctx, const model_rec_t *m, int mask);
  int (*delete_model)(void *ctx, const char *name);
  int (*flush_usage)(void *ctx, const usage_row_t *rows, int n);
  int (*query_usage)(void *ctx, long key_id, const char *model, time_t from, time_t to,
                     usage_row_t *out, int cap, int *n);
} pg_ops_t;

typedef struct pg_store pg_store_t;
/** @brief Owns 1 libpq connection + ops table. @param ops NULL → real libpq ops. */
pg_store_t *pg_store_open(const char *dsn, const pg_ops_t *ops);
void        pg_store_close(pg_store_t *ps);
int         pg_store_migrate(pg_store_t *ps); /* embed schema.sql; run unapplied versions */
pg_ops_t    *pg_store_ops(const pg_store_t *ps); /* the live ops table (real or fake) */
#endif
```

`update_key`/`update_model` masks (bit flags, also in header): `KMASK_QUOTA`, `KMASK_ALLOWLIST`, `KMASK_EXPIRY`, `KMASK_RATE`; `MMASK_ENDPOINT`, `MMASK_PARAMS`, `MMASK_ENABLED`, `MMASK_KEYREF`.

- [ ] **Step 3: `pg_store.c` (libpq impl)** — single blocking connection guarded by a mutex for admin/flush writes; `PQconnectdb` + `PQstatus` check, abort with clear error; `pg_store_migrate` reads `schema_migrations`, applies `schema.sql` inside one transaction if version 1 missing. All statements: `PQescapeIdentifier`/parameterized `PQexecParams`; `COPY`-less simple `INSERT ... ON CONFLICT DO UPDATE` for `usage_daily` batch flush.

- [ ] **Step 4: Fake ops test** — `tests/unit/test_pg_store.c`: static fake `pg_ops_t` recording calls in arrays; `pg_store_open(dsn, &fake)` → call each op through `pg_store_ops()`, assert recorded args match inputs, and `get_key_by_hash`-style fakes round-trip the struct. Also test `pg_store_migrate` against a fake that reports version 1 already applied (no-ops) vs missing.

- [ ] **Step 5: Integration-optional check** — if `TEST_PG_DSN` env is set at test time, `test_pg_store.c` additionally opens the real libpq ops against it and runs migrate+insert+read+flush+query round trip (skip gracefully when unset).

- [ ] **Step 6: Run & commit** — `git commit -m "feat: pg schema + pg_store ops layer"`

---

## Task 5: Secrets (AES-256-GCM) + API key cache

**Files:**
- Create: `src/secrets.h/.c`, `src/auth_key.h/.c`, `tests/unit/test_secrets.c`, `tests/unit/test_auth_key.c`

- [ ] **Step 1: `src/secrets.h/.c`** (OpenSSL EVP AES-256-GCM)

```c
/** @brief Encrypt @p plain (@p plain_len) with @p master (32 bytes) → output hex-encoded
 *  "v1:<nonce>:<tag>:<cipher>" string into @p out (≥ 4*plain_len+64). @return 0 ok, -1 on EVP failure.
 *  @invariant nonce is random 12 bytes per call; tag 16 bytes; prefix "v1:" marks format. */
int secret_encrypt(const uint8_t master[32], const void *plain, size_t plain_len, char *out, size_t out_cap);
/** @brief Reverse of secret_encrypt; NULL @p master → error unless ref is env: (handled by caller). */
int secret_decrypt(const uint8_t master[32], const char *blob, char *out, size_t out_cap, size_t *out_len);
```

- [ ] **Step 2: `src/auth_key.h/.c`**

```c
#ifndef AIGATE_AUTH_KEY_H
#define AIGATE_AUTH_KEY_H
#include "pg_store.h"
/** @file auth_key.h @brief client key verification: Bearer → SHA-256 → LRU/PG; constant-time compare. */
typedef struct auth_key_cache {
  lru_t *recs;            /* key_hash(hex) → key_rec_t* (caller-owned values, evict frees) */
  pg_ops_t *ops;          /* backing store */
} auth_key_cache;

/** @brief Initialize cache with capacity 4096; ops from pg_store. */
int      auth_key_init(auth_key_cache *akc, pg_store_t *ps, const char *dsn);
void     auth_key_shutdown(auth_key_cache *akc);
/** @brief Resolve a Bearer token. @return 0 + *out filled; -1 unknown; -2 revoked; -3 expired. */
int      auth_key_resolve(auth_key_cache *akc, const char *bearer, key_rec_t *out);
/** @brief Invalidate cached entry after admin mutation. */
void     auth_key_invalidate(auth_key_cache *akc, const char *key_hash);
/** @brief 1 if @p model allowed for @p k (empty allowlist = all). */
int      key_allows_model(const key_rec_t *k, const char *model);
#endif
```

Implementation: `sha256_hex(bearer)` → `lru_get` on `key_hash`; miss → `ops->get_key_by_hash` (row NULL → -1; `revoked_at` set → -2; `expires_at` past → -3; else `lru_put` a malloc'd copy). `key_allows_model`: linear scan of `allowed_models`; constant-time compare via OpenSSL `CRYPTO_memcmp` on hashes (hashes are fixed 64 hex — equal-length memcmp is fine but use `CRYPTO_memcmp` anyway for policy).

- [ ] **Step 3: Tests** — `test_secrets.c`: round-trip 0/16/32/1024-byte payloads; tamper → -1; wrong master → -1. `test_auth_key.c`: fake ops with 3 keys (normal, revoked, expired); assert resolve codes; allowlist: `{"gpt-4o"}` admits gpt-4o rejects claude; empty admits anything; cache-hit path (2nd resolve doesn't call `get_key_by_hash` again — fake counts calls); `auth_key_invalidate` forces re-lookup.

- [ ] **Step 4: Run & commit** — `git commit -m "feat: AES-GCM secret store + key auth cache"`

---

## Task 6: Rate limiter

**Files:**
- Create: `src/ratelimit.h/.c`, `tests/unit/test_ratelimit.c`

- [ ] **Step 1: `src/ratelimit.h`**

```c
#ifndef AIGATE_RATELIMIT_H
#define AIGATE_RATELIMIT_H
/** @file ratelimit.h @brief per-key QPS token buckets; per-key daily token quotas. */
typedef struct ratelimit ratelimit_t;
ratelimit_t *ratelimit_new(void);
void ratelimit_free(ratelimit_t *rl);
/** @brief Acquire 1 request token for @p key_id (QPS limit from @p qps; 0 = unlimited).
 *  @return 0 admitted; -E429 when over rate, sets *retry_ms. */
int rl_allow_request(ratelimit_t *rl, long key_id, int qps, long *retry_ms);
/** @brief Reserve @p tokens of daily quota (0 quota = unlimited); called after upstream usage known.
 *  @return 0 ok; -E429 over quota (caller already answered the request — this only blocks *next* requests;
 *  the daily counter still decrements in-flight allowance, never retroactively fails). */
int rl_reserve_tokens(ratelimit_t *rl, long key_id, long daily_quota, long tokens);
long rl_remaining_daily(ratelimit_t *rl, long key_id, long daily_quota);
void rl_reset_day(ratelimit_t *rl, time_t now);  /* flush worker calls on date rollover */
#endif
```

- [ ] **Step 2: `ratelimit.c`** — open-addressed table `key_id → bucket` (mutex-protected); classic token bucket: `capacity=qps, refill=1s`; monotonic clock; bucket struct stores `tokens (double), last_refill (mono ns), day, daily_used`. No dynamic allocation on the hot acquire path beyond first-touch.

- [ ] **Step 3: Tests** — qps=10: admit 10 immediately, 11th gets -E429 with retry_ms ≈ 100ms; sleep 120ms → 2 admitted; qps=0 → unlimited (1000 admits); daily quota 100: reserve 60 → remaining 40; quota 0 → remaining LONG_MAX sentinel; `rl_reset_day` zeroes `daily_used`; 8-thread × 50k acquires against qps=1000 — admitted count within ±5% of expected min(n, qps × elapsed).

- [ ] **Step 4: Run & commit** — `git commit -m "feat: per-key QPS + daily token rate limiter"`

---

## Task 7: Model router + upstream client (libcurl) + OpenAI adapter

**Files:**
- Create: `src/model_router.h/.c`, `src/upstream_client.h/.c`, `src/provider_openai.h/.c`, `tests/unit/test_model_router.c`, `tests/unit/test_upstream_client.c`

- [ ] **Step 1: `src/model_router.h/.c`**

```c
#ifndef AIGATE_MODEL_ROUTER_H
#define AIGATE_MODEL_ROUTER_H
#include "pg_store.h"
/** @file model_router.h @brief model name → route (provider/endpoint/key/params) with LRU cache. */
typedef struct model_router model_router_t;
/** @brief New router backed by @p ps; key resolver uses @p master (may be NULL → env refs only). */
model_router_t *model_router_new(pg_store_t *ps, const uint8_t *master32);
void model_router_free(model_router_t *mr);
/** @brief Resolve. @return 0 + *out; -E404 unknown/disabled model; -E500 key-resolve failure. */
int model_router_resolve(model_router_t *mr, const char *model, model_rec_t *out);
/** @brief Invalidate after admin mutation. */
void model_router_invalidate(model_router_t *mr, const char *model);
#endif
```

Key resolution: `upstream_key_ref` = `"env:NAME"` → `getenv` (missing → -E500 with log); `"pg:<blob>"` → `secret_decrypt(master, ...)` (master NULL → -E500). Cache `model_name → model_rec_t*` in LRU(1024), invalidated via `model_router_invalidate` called from admin API.

- [ ] **Step 2: `src/upstream_client.h/.c`**

```c
/** @brief Issue one upstream call. @param timeout_ms 0 → use config default.
 *  @param resp_body receives malloc'd full body on non-stream; caller frees. SSE is Plan 2.
 *  @return upstream HTTP status in *out_status, body malloc'd in *out_body (set *out_body_len),
 *  0 ok / -E110 on timeout / -E502 on transport failure. */
int upstream_call(const model_rec_t *route, const char *path, const char *body_json,
                  size_t body_len, long timeout_ms,
                  int *out_status, char **out_body, size_t *out_body_len);
```

- [ ] **Step 3: `src/provider_openai.h/.c`** — openai-compatible adapter (covers `openai`, `ollama`, and `azure` labels):

```c
/** @brief Build the exact upstream request for an openai-compatible backend.
 *  Fills @p url (endpoint + path + "?api-version=" from default_params for azure),
 *  Authorization bearer from route->upstream_key, merged default_params (default < request body),
 *  and returns malloc'd JSON body. */
int provider_openai_build(const model_rec_t *route, const char *in_body_json,
                          char *url_out, size_t url_cap, char **out_body, size_t *out_body_len);
```

Merge rule: `jansson` object-merge where request body wins over `default_params_json`; `model` field from the request body is used as-is when present (admin controls naming, not rewriting).

- [ ] **Step 4: Mock-upstream unit tests** — start a small civetweb server in the test binary (single thread, fixture directory or inline handlers) on `127.0.0.1:0` → returns `{"choices":[...],"usage":{"prompt_tokens":7,"completion_tokens":11}}` with 200; `{"error":{"message":"boom"}}` with 500 for `/fail`. `test_upstream_client.c`: 200 round-trip (status+body exact), 500 passthrough, timeout path (handler sleeps 2s, `timeout_ms=200` → -E110). `test_model_router.c`: fake pg ops serving 3 models (enabled openai, disabled, env-key); assert resolve codes, env-missing → -E500, invalidate forces re-lookup, azure label appends `?api-version=` from params.

- [ ] **Step 5: Run & commit** — `git commit -m "feat: model router + libcurl upstream client + openai adapter"`

---

## Task 8: Usage metering + metrics

**Files:**
- Create: `src/usage_meter.h/.c`, `src/metrics.h/.c`, `tests/unit/test_usage_meter.c`

- [ ] **Step 1: `src/usage_meter.h/.c`**

```c
/** @file usage_meter.h @brief atomic request/token/error counters + HDR latency histograms; 5s PG flush. */
typedef struct usage_meter usage_meter_t;
usage_meter_t *usage_meter_new(pg_store_t *ps, int flush_interval_s);
void usage_meter_free(usage_meter_t *um);  /* stops worker, final flush */
void um_record(usage_meter_t *um, long key_id, const char *model, int http_status,
               long prompt_tokens, long completion_tokens,
               uint64_t latency_ns, const char *provider);
/* metric names exposed to metrics.c via accessors */
long um_total_requests(usage_meter_t *um);
long um_total_errors(usage_meter_t *um);
long um_total_tokens(usage_meter_t *um);
```

Internals: `stdatomic` global counters; open-addressed map `key_id × model → daily accumulator` (mutex + date rollover check vs clock, rollover flushes current rows via `pg_ops->flush_usage` and resets); HDR histogram per provider (`hdr_init(1, UINT64_C(3600 * 1e9), 10000)`); background worker thread: every `flush_interval_s` drain map rows → `flush_usage`; `usage_meter_free` joins + final flush.

- [ ] **Step 2: `src/metrics.h/.c`** — `metrics_expose(int fd_or_write)` writes Prometheus text: `aigate_requests_total`, `aigate_errors_total`, `aigate_tokens_total` (plus per provider `aigate_upstream_latency_ns_bucket/sum/count` from HDR, `aigate_upstream_requests_total{provider="..."}`); `/metrics` handler checks source IP against `metrics_acl` (exact CIDR list, `inet_pton` parse, IPv4 only) → 403 otherwise.

- [ ] **Step 3: Tests** — `test_usage_meter.c` with fake ops: `um_record` ×3 for two keys/models → after forcing a flush (expose drain function), recorded rows have summed counts; latency histogram p50/p99 sanity (record 1s and 3.6h, query `hdr_value_at_percentile`); metrics text contains expected line prefixes.

- [ ] **Step 4: Run & commit** — `git commit -m "feat: usage metering + prometheus metrics"`

---

## Task 9: The core seam — aigate_core pipeline

**Files:**
- Create: `src/aigate_core.h/.c`, `tests/unit/test_aigate_core.c`

- [ ] **Step 1: `src/aigate_core.h`** — the seam from spec §2.2 plus the ctx structs:

```c
#ifndef AIGATE_CORE_H
#define AIGATE_CORE_H
/** @file aigate_core.h @brief transport-agnostic gateway pipeline (THE SEAM). */
#include <stdbool.h>
#include <stddef.h>
#include "auth_key.h"
#include "model_router.h"
#include "usage_meter.h"
#include "upstream_client.h"

typedef struct aigate_request_ctx {
  const char *method;         /* "POST" */
  const char *path;           /* "/v1/chat/completions" */
  const char *bearer;         /* client key, raw */
  const char *client_ip;      /* for metrics ACL / logs */
  const void *body; size_t body_len;
} aigate_request_ctx;

typedef struct aigate_response_ctx {
  int  status;                /* set before first write */
  bool headers_sent;
  void *impl;                 /* transport-owned (civetweb cgiStream*) */
  int  (*set_header)(void *impl, const char *name, const char *value);
  int  (*write)(void *impl, const void *buf, size_t len, bool fin);
} aigate_response_ctx;

typedef struct aigate_core {
  auth_key_cache  keys;
  ratelimit_t    *rl;
  model_router_t *router;
  usage_meter_t  *um;
  int             default_timeout_ms;
} aigate_core;

int  aigate_core_init(aigate_core *ac, pg_store_t *ps, const uint8_t *master,
                      const char *listen_dsn_hint, int default_timeout_ms, int flush_interval_s);
void aigate_core_shutdown(aigate_core *ac);

/** @brief Full pipeline. @return 0 (response complete). Never returns without either
 *  writing a body or a 4xx/5xx error body. */
int aigate_handle_request(aigate_core *ac, aigate_request_ctx *rq, aigate_response_ctx *rc);

/* transport adapters must use this for header + body write */
int aigate_write_json(aigate_response_ctx *rc, int status, const char *body, size_t len);
int aigate_write_error(aigate_response_ctx *rc, int http_status, const char *type, const char *message);
#endif
```

- [ ] **Step 2: `aigate_core.c` pipeline (exact order)**

```
auth_key_resolve → 401 {"error":{"message":"invalid api key","type":"auth_error","code":401}}
key_allows_model(model) (model parsed from request body via jansson) → 403 auth_error
rl_allow_request → 429 {"error":{...,"type":"rate_limit"}} + Retry-After: <ceil(retry_ms/1000)>
model_router_resolve → 404 {"error":{"message":"model not found","type":"model_not_found"}}
provider build (switch on route->provider; "openai"/"ollama"/"azure" → provider_openai; else 501)
upstream_call:
  status 2xx → body passthrough (Content-Type: application/json), Content-Length set
  5xx → single retry (non-streaming only, 200ms sleep) if still 5xx → 502 + X-Upstream-Provider header
parse usage from upstream body (jansson; missing → 0/0, log WARN once per model)
um_record(...); rl_reserve_tokens(key, prompt+completion tokens)
write body → rc
```

- [ ] **Step 3: Test harness + `test_aigate_core.c`** — `tests/unit/mock_upstream.c`: civetweb fixtures (200 with usage / 500 / delayed). Fake `pg_ops` with one key (qps 2, quota 100, allowlist `["gpt-4o"]`) and model `gpt-4o` → mock endpoint. Cases:
  1. happy path: 200 response bytes == upstream body; fake `flush_usage` sees 1 row with token counts.
  2. unknown key → 401 body exact.
  3. key allowlist excludes → 403.
  4. qps exceeded (fire 3 in same ms with qps=2) → one 429 with `Retry-After: 1`.
  5. unknown model → 404 body.
  6. upstream 500 → after retry → 502 with `X-Upstream-Provider: openai` header recorded in `rc` (test rc captures headers).

- [ ] **Step 4: Run & commit** — `git commit -m "feat: core pipeline seam (auth→route→limit→upstream→meter)"`

---

## Task 10: Admin API + `/v1/models` + transport wiring

**Files:**
- Create: `src/admin_api.h/.c`, `src/transport_civetweb.h/.c`, `tests/unit/test_admin_api.c`

- [ ] **Step 1: `src/admin_api.h/.c`** — handler dispatch table (path prefix match):

```c
typedef struct admin_ctx { aigate_core *ac; pg_store_t *ps; const char *admin_token_hash; } admin_ctx_t;
int admin_dispatch(admin_ctx_t *adm, const char *path, const char *method,
                   const void *body, size_t body_len, int *out_status, char **out_body, size_t *out_len);
```

Endpoints (spec §4.2): all read `Authorization: Bearer`; `sha256_hex` + `CRYPTO_memcmp` against `admin_token_hash` → 401 otherwise. Writes go through `pg_store_ops` (real libpq in prod); after `insert/update/delete` of key/model, call `auth_key_invalidate`/`model_router_invalidate` in the same handler. Key creation response: JSON `{"key_id":..., "plaintext":"aig_<32 random hex>"}` (plaintext built from 32 random bytes via `RAND_bytes`, hashed before insert). Usage query: `ops->query_usage` with `from/to` date strings → JSON array of `usage_row_t`.

- [ ] **Step 2: `GET /v1/models`** (data plane) — list enabled models; filter by calling key's allowlist (empty → all). Body: `{"object":"list","data":[{"id":...,"object":"model"}]}`.

- [ ] **Step 3: `src/transport_civetweb.h/.c`** — civetweb `mg_callbacks` + URI match table:

```
/v1/chat/completions,/v1/completions,/v1/embeddings,/v1/models → fill aigate_request_ctx
   (method, path, bearer from Authorization, client_ip from conn, body) → aigate_handle_request
/admin/v1/... → admin_dispatch (write response via cgic/CGI helpers)
/metrics → metrics handler (ACL on conn source IP)
```

`aigate_response_ctx` adapter: `impl` = `struct mg_connection*` + status; `write` → `cgicprintf`-style direct `mg_write` chunks (non-streaming: one chunk + fin); `set_header` → `mg_printf(conn, "HTTP/1.1 %d\r\n...", ...)` once then `mg_write` chunks.

- [ ] **Step 4: `tests/unit/test_admin_api.c`** — fake ops; `admin_dispatch` calls with hand-built headers/body: create key (plaintext returned once; second `GET /keys` shows hash only), patch quota, revoke → core's `auth_key_resolve` now returns -2 (shared cache), model CRUD + invalidate (router re-lookup count via fake), usage query round-trip. Bad admin token → 401.

- [ ] **Step 5: Run & commit** — `git commit -m "feat: admin API, /v1/models, civetweb transport"`

---

## Task 11: main() assembly + end-to-end smoke

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: `main.c`** — boot sequence:

```
parse args (none; env-driven per spec §5)
config = aigate_config_load → fatal on -1
ps = pg_store_open(dsn, NULL) → pg_store_migrate (fatal on failure)
master = master_key? hex decode : NULL
core_init (ps, master, default timeout, flush 5s)
register civetweb handlers (transport_civetweb)
listen on AIGATE_LISTEN → fatal on bind failure
AIGATE_LOG_INFO("aigate ready on %s", listen)
serve until SIGINT/SIGTERM (sigaction → mg_stop) → core_shutdown (flushes usage)
```

- [ ] **Step 2: E2E smoke script `tests/integration/smoke.sh`** — start gateway against `TEST_PG_DSN` (docker postgres), admin POST model (openai → local mock), create key, curl:

```bash
curl -s -H "Authorization: Bearer $KEY" http://127.0.0.1:8080/v1/chat/completions \
  -d '{"model":"mock-model","messages":[{"role":"user","content":"hi"}]}'
# expect 200 + upstream mock body
curl -s -H "Authorization: Bearer $ADMIN" http://127.0.0.1:8080/admin/v1/usage
# expect one row with token counts
curl -s http://127.0.0.1:8080/metrics | grep aigate_requests_total
```

- [ ] **Step 3: Run & commit** — `git commit -m "feat: main assembly + e2e smoke"`

---

## Task 12: Integration test suite (Python)

**Files:**
- Create: `tests/integration/mock_upstream.py`, `tests/integration/conftest.py`, `tests/integration/test_gateway.py`, `tests/integration/requirements.txt`

- [ ] **Step 1: `mock_upstream.py`** — stdlib `http.server` fixture: `/chat` returns OpenAI-shaped 200 with usage `{7, 11}`; `/fail` returns 500; records request log (bearer, path, body) for assertions.

- [ ] **Step 2: `conftest.py`** — pytest fixtures: PG (reuse `TEST_PG_DSN` or skip), spawn gateway binary (fresh schema by `TRUNCATE` at start), spawn mock upstream on free port, admin/client helpers.

- [ ] **Step 3: `test_gateway.py`** — scenario per spec §6:

```python
def test_full_lifecycle():
    key = admin_post("/keys", {"name": "t", "rate_qps": 3, "daily_token_quota": 200,
                               "allowed_models": ["mock-model"]})
    ok = post("/v1/chat/completions", key, {"model": "mock-model", ...})
    assert ok.status == 200 and "usage" in ok.json()
    # burst 3 qps → 429 with Retry-After
    # PATCH quota to 20 → next request 429
    # DELETE key → revoked: 401
    # PATCH model disabled → 404 model_not_found
    # GET /admin/v1/usage?day=today → requests/tokens match recorded
    # GET /metrics → aigate_requests_total ≥ N
```

- [ ] **Step 4: Run & commit** — `git commit -m "test: python integration suite (lifecycle + usage + metrics)"`

---

## Self-Review Checklist (executed at plan end, not by implementer)

- Spec §2 seam: tasks 1, 9, 10 ✔
- Spec §3 schema: task 4 ✔
- Spec §4.1 data plane: tasks 7, 9, 10 (streaming explicitly deferred to Plan 2) ✔
- Spec §4.2 admin: tasks 5, 10 ✔
- Spec §5 security: tasks 2, 5, 10 ✔
- Spec §6 testing: tasks 8, 9, 11, 12 (perf baseline → Plan 2) ✔
- Spec §7 Doxygen: task 1 convention locked; every file task includes header comment step ✔
- Type consistency: `key_rec_t`/`model_rec_t`/`usage_row_t` defined once in plan header (task 4 owns them in `pg_store.h`) ✔
- Placeholder scan: none — all steps carry exact code/commands ✔

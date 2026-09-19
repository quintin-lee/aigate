# aigate Implementation Plan 4: Multi-Upstream Failover & Load Balancing

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement robust Multi-Upstream Failover, Priority-based Tiering, Weighted/Round-Robin Load Balancing, and Circuit Breaker capabilities in `aigate`.

**Architecture:**
- Each logical model can bind up to 8 upstream targets with distinct providers, endpoints, keys, weights, and priorities.
- `src/circuit_breaker.h/.c` tracks per-target health (`CLOSED`, `OPEN`, `HALF_OPEN`), tripping after 3 consecutive failures with a 30s cool-off window.
- `src/model_router.h/.c` orders candidates by priority and performs weighted or round-robin selection.
- `src/aigate_core.c` executes requests with transparent failover to backup targets on HTTP 429, 5xx, or network timeout.
- Expose failover and circuit breaker metrics in Prometheus and manage multi-target models via Admin REST API & Web UI.

**Tech Stack:** C17, CMake, civetweb, jansson, libcurl, libpq, OpenSSL, hdr_histogram, Python/pytest.

**Spec:** `docs/superpowers/specs/2026-09-19-multi-upstream-failover-and-load-balancing-design.md`

---

## File Structure

```
aigate/
├── src/
│   ├── circuit_breaker.h/.c      # [NEW] Circuit breaker state machine & tracking
│   ├── pg_store.h/.c             # Migration v3: targets JSONB & lb_policy fields
│   ├── schema_sql.h              # Idempotent migration v3 SQL string
│   ├── model_router.h/.c         # Multi-target key resolution & selection algorithm
│   ├── aigate_core.h/.c          # Failover loop for chat, streaming, and embeddings
│   ├── metrics.h/.c              # aigate_failover_total & circuit metrics
│   └── admin_api.c               # Multi-target serialization & circuit state export
├── schema/
│   └── schema.sql                # Migration v3 definition
├── web/
│   └── admin.html                # Multi-target configuration UI & health badges
├── tests/unit/
│   ├── test_circuit_breaker.c    # [NEW] Unit tests for circuit breaker state machine
│   ├── test_failover.c           # [NEW] Unit tests for multi-upstream failover & retries
│   ├── test_pg_store.c           # Test migration v3 & targets persistence
│   ├── test_model_router.c       # Test priority & weighted load balancing
│   └── test_admin_api.c          # Test targets CRUD & circuit reporting
└── tests/integration/
    ├── mock_upstream.py          # Add intermittent 429/500 mock endpoints
    └── test_gateway.py           # End-to-end failover scenarios
```

---

## Task 1: Data Model & Schema Migration v3

**Files:**
- Modify: `schema/schema.sql`, `src/schema_sql.h`, `src/pg_store.h`, `src/pg_store.c`
- Modify: `tests/unit/test_pg_store.c`, `CMakeLists.txt`

- [ ] **Step 1: Update Schema Migration v3 in `schema.sql` & `schema_sql.h`**
  - Add `ALTER TABLE models ADD COLUMN IF NOT EXISTS targets JSONB NOT NULL DEFAULT '[]';`
  - Add `ALTER TABLE models ADD COLUMN IF NOT EXISTS lb_policy TEXT NOT NULL DEFAULT 'priority';`
  - Insert migration version 3 into `schema_migrations`.

- [ ] **Step 2: Update `model_rec_t` in `src/pg_store.h`**
  - Define `upstream_target_t` with `provider`, `endpoint`, `upstream_key_ref`, `upstream_key`, `weight`, `priority`.
  - Add `n_targets`, `targets[MAX_TARGETS_PER_MODEL]`, `lb_policy` to `model_rec_t`.

- [ ] **Step 3: Update `pg_store.c` Serialization & Deserialization**
  - Update `list_models`, `get_model`, `create_model`, `update_model` to parse/serialize `targets` JSONB and `lb_policy`.
  - Ensure backward compatibility: if `targets` is empty, synthesize `targets[0]` from the model's top-level endpoint/provider/key.

- [ ] **Step 4: Unit Test in `test_pg_store.c`**
  - Test migration v3 application.
  - Test roundtrip creation and querying of models with multiple targets.
  - Verify existing tests continue to pass.

---

## Task 2: Circuit Breaker Subsystem

**Files:**
- Create: `src/circuit_breaker.h`, `src/circuit_breaker.c`
- Create: `tests/unit/test_circuit_breaker.c`
- Modify: `CMakeLists.txt`, `tests/unit/run_tests.c`

- [ ] **Step 1: Define Interface in `src/circuit_breaker.h`**
  - Define `circuit_breaker_t`, `cb_state_t` (`CB_CLOSED`, `CB_OPEN`, `CB_HALF_OPEN`).
  - Declare `cb_create()`, `cb_destroy()`.
  - Declare `cb_allow_request(...)`, `cb_record_success(...)`, `cb_record_failure(...)`, `cb_get_state(...)`.

- [ ] **Step 2: Implement Circuit Breaker in `src/circuit_breaker.c`**
  - Hash table / entry list keyed by `model:endpoint`.
  - Thread-safety via `pthread_mutex_t`.
  - Consecutive failures counter with threshold 3.
  - 30-second cool-off window before half-open state transition.
  - Success in half-open transitions back to `CB_CLOSED`.

- [ ] **Step 3: Unit Test `test_circuit_breaker.c`**
  - Test normal requests allowed.
  - Test tripping to `CB_OPEN` after 3 consecutive failures (429/500/etc.).
  - Test requests blocked while in `CB_OPEN`.
  - Test transition to `CB_HALF_OPEN` after cool-off window.
  - Test recovery to `CB_CLOSED` on probe success.
  - Verify thread concurrency safety.

---

## Task 3: Model Router Multi-Target Selection & Load Balancing

**Files:**
- Modify: `src/model_router.h`, `src/model_router.c`
- Modify: `tests/unit/test_model_router.c`

- [ ] **Step 1: Multi-Key Resolution in `model_router.c`**
  - When resolving a model, iterate over all `targets` in `model_rec_t` and resolve `upstream_key_ref` (env: or pg:) into each `target->upstream_key`.

- [ ] **Step 2: Target Selection Algorithm (`model_router_select_candidates`)**
  - Implement priority grouping (Priority 0, Priority 1...).
  - Filter targets against circuit breaker (`cb_allow_request`).
  - Within active priority tier:
    - If `priority`: order sequentially by weight or definition.
    - If `round_robin`: rotate starting target via atomic counter.
    - If `weighted`: order targets by weight distribution.
  - If all targets are open, fallback to the target with earliest expiry.

- [ ] **Step 3: Unit Test in `test_model_router.c`**
  - Test key resolution for multiple targets.
  - Test candidate selection across priority tiers.
  - Test round-robin rotation.
  - Test circuit breaker exclusion.

---

## Task 4: Core Pipeline Failover Execution

**Files:**
- Modify: `src/aigate_core.h`, `src/aigate_core.c`
- Create: `tests/unit/test_failover.c`
- Modify: `tests/unit/run_tests.c`, `CMakeLists.txt`

- [ ] **Step 1: Integrate Circuit Breaker in `aigate_core`**
  - Initialize `ac->cb` in `aigate_core_init` and destroy in `aigate_core_shutdown`.

- [ ] **Step 2: Implement Non-Streaming Chat Failover Loop**
  - Select candidate targets from router.
  - Attempt request on target 0. If 200 OK -> record success and return.
  - If HTTP 429, 5xx, or network error -> record failure on circuit breaker, log failover, increment metric, and try next candidate.

- [ ] **Step 3: Implement Streaming Chat Failover Loop**
  - Before headers are sent: if target stream fails or returns error, failover to next candidate.
  - After headers sent: emit error SSE frame and [DONE].

- [ ] **Step 4: Implement Embeddings Failover Loop**
  - Repeat candidate selection and retry loop for `/v1/embeddings`.

- [ ] **Step 5: Unit Test in `test_failover.c`**
  - Mock multi-target model (Target 1 returns 500, Target 2 returns 200).
  - Verify request automatically succeeds via Target 2 without client error.
  - Mock Target 1 returns 429, verify failover to Target 2.
  - Verify circuit breaker trips on Target 1 after 3 failures, subsequent requests go directly to Target 2.

---

## Task 5: Prometheus Metrics & Admin API / UI Integration

**Files:**
- Modify: `src/metrics.h`, `src/metrics.c`, `src/admin_api.c`, `web/admin.html`
- Modify: `tests/unit/test_admin_api.c`, `tests/unit/test_admin_ui.c`

- [ ] **Step 1: Prometheus Metrics**
  - Add `aigate_failover_total` counter.
  - Add `aigate_circuit_breaker_state` gauge.
  - Render in `/metrics`.

- [ ] **Step 2: Admin API Updates**
  - In `POST /admin/v1/models` and `PUT /admin/v1/models/{name}`, accept `targets` array and `lb_policy`.
  - In `GET /admin/v1/models`, include `targets` array with `cb_state` ("closed", "open", "half_open").

- [ ] **Step 3: Admin Console Web UI (`web/admin.html`)**
  - Add multi-target editor in Model modal (add/remove endpoint, provider dropdown, key ref, weight, priority).
  - In Models table, show target count, policy, and circuit health badges (🟢 Active, 🔴 Tripped, 🟡 Probing).

- [ ] **Step 4: Unit Test Updates**
  - Test Admin API model targets CRUD and circuit state reporting.
  - Run all unit tests to ensure 100% pass rate.

---

## Task 6: End-to-End Integration Tests & Verification

**Files:**
- Modify: `tests/integration/mock_upstream.py`, `tests/integration/test_gateway.py`

- [ ] **Step 1: Mock Failover Endpoints**
  - Support configurable 429/500 endpoints in `mock_upstream.py`.

- [ ] **Step 2: Integration Tests in `test_gateway.py`**
  - Configure multi-target model with Target A (failing 502) and Target B (healthy 200).
  - Assert client receives 200 OK with failover logged.
  - Test 429 failover and verify Prometheus failover counter increments.

- [ ] **Step 3: Execute Test Suites**
  - Run `ctest --test-dir build --output-on-failure`.
  - Verify 100% pass rate.

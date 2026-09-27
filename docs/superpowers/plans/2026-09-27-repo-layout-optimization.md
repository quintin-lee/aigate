# Repo Layout Optimization (Scheme A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mirror tests/unit into the same 7 layers as src/, unify on a single build/ dir convention, and add a docs map — move-only, no code changes.

**Architecture:** Pure `git mv` relocation (31 test files into 7 new subdirs) plus one GLOB line change in tests/CMakeLists.txt using the established `CONFIGURE_DEPENDS` pattern; one new docs index file. `run_tests.c` uses `extern` registration (no `#include` of test files), so relocation needs zero code edits.

**Tech Stack:** CMake (GLOB CONFIGURE_DEPENDS), git mv, ctest

**Spec:** `docs/superpowers/specs/2026-09-27-repo-layout-optimization-design.md`

---

### Task 1: Baseline

**Files:** none (read-only)

- [ ] **Step 1: Confirm clean tree**

```bash
git status --short
```

Expected: empty output.

- [ ] **Step 2: Full build from canonical dir**

```bash
cmake -B build -S . && cmake --build build -j
```

Expected: exit 0.

- [ ] **Step 3: Record baseline test count**

```bash
ctest --test-dir build
```

Expected: 6/6 pass. Write the number down — Task 8 must match it.

---

### Task 2: Relocate common/core/observe tests (8 files)

**Files:**
- Move: `tests/unit/test_lru.c` → `tests/unit/common/test_lru.c`
- Move: `tests/unit/test_sha256.c` → `tests/unit/common/test_sha256.c`
- Move: `tests/unit/test_aigate_core.c` → `tests/unit/core/test_aigate_core.c`
- Move: `tests/unit/test_config.c` → `tests/unit/core/test_config.c`
- Move: `tests/unit/test_log.c` → `tests/unit/core/test_log.c`
- Move: `tests/unit/test_secrets.c` → `tests/unit/core/test_secrets.c`
- Move: `tests/unit/test_event_bus.c` → `tests/unit/observe/test_event_bus.c`
- Move: `tests/unit/test_health_prober.c` → `tests/unit/observe/test_health_prober.c`

> WARNING: run `git mv` commands SERIALLY in one shell command (chained with `&&`). Parallel git mv races on `index.lock` (observed last session). Same warning applies to Tasks 3–5.

- [ ] **Step 1: Move the 8 files**

```bash
mkdir -p tests/unit/common tests/unit/core tests/unit/observe && git mv tests/unit/test_lru.c tests/unit/common/test_lru.c && git mv tests/unit/test_sha256.c tests/unit/common/test_sha256.c && git mv tests/unit/test_aigate_core.c tests/unit/core/test_aigate_core.c && git mv tests/unit/test_config.c tests/unit/core/test_config.c && git mv tests/unit/test_log.c tests/unit/core/test_log.c && git mv tests/unit/test_secrets.c tests/unit/core/test_secrets.c && git mv tests/unit/test_event_bus.c tests/unit/observe/test_event_bus.c && git mv tests/unit/test_health_prober.c tests/unit/observe/test_health_prober.c
```

Expected: exit 0, no `index.lock` error.

- [ ] **Step 2: Verify placement**

```bash
ls tests/unit/common tests/unit/core tests/unit/observe
```

Expected: `common` shows `test_lru.c test_sha256.c`; `core` shows 4 files; `observe` shows 2 files.

---

### Task 3: Relocate policy tests (6 files)

**Files:**
- Move: `tests/unit/test_auth_key.c` → `tests/unit/policy/test_auth_key.c`
- Move: `tests/unit/test_budget_enforce.c` → `tests/unit/policy/test_budget_enforce.c`
- Move: `tests/unit/test_circuit_breaker.c` → `tests/unit/policy/test_circuit_breaker.c`
- Move: `tests/unit/test_guardrails.c` → `tests/unit/policy/test_guardrails.c`
- Move: `tests/unit/test_ratelimit.c` → `tests/unit/policy/test_ratelimit.c`
- Move: `tests/unit/test_response_cache.c` → `tests/unit/policy/test_response_cache.c`

- [ ] **Step 1: Move the 6 files (serially, chained with &&)**

```bash
mkdir -p tests/unit/policy && git mv tests/unit/test_auth_key.c tests/unit/policy/test_auth_key.c && git mv tests/unit/test_budget_enforce.c tests/unit/policy/test_budget_enforce.c && git mv tests/unit/test_circuit_breaker.c tests/unit/policy/test_circuit_breaker.c && git mv tests/unit/test_guardrails.c tests/unit/policy/test_guardrails.c && git mv tests/unit/test_ratelimit.c tests/unit/policy/test_ratelimit.c && git mv tests/unit/test_response_cache.c tests/unit/policy/test_response_cache.c
```

Expected: exit 0.

- [ ] **Step 2: Verify placement**

```bash
ls tests/unit/policy
```

Expected: 6 files listed above.

---

### Task 4: Relocate server/store tests (5 files)

**Files:**
- Move: `tests/unit/test_admin_api.c` → `tests/unit/server/test_admin_api.c`
- Move: `tests/unit/test_admin_ui.c` → `tests/unit/server/test_admin_ui.c`
- Move: `tests/unit/test_pg_store.c` → `tests/unit/store/test_pg_store.c`
- Move: `tests/unit/test_redis_pool.c` → `tests/unit/store/test_redis_pool.c`
- Move: `tests/unit/test_usage_meter.c` → `tests/unit/store/test_usage_meter.c`

- [ ] **Step 1: Move the 5 files (serially, chained with &&)**

```bash
mkdir -p tests/unit/server tests/unit/store && git mv tests/unit/test_admin_api.c tests/unit/server/test_admin_api.c && git mv tests/unit/test_admin_ui.c tests/unit/server/test_admin_ui.c && git mv tests/unit/test_pg_store.c tests/unit/store/test_pg_store.c && git mv tests/unit/test_redis_pool.c tests/unit/store/test_redis_pool.c && git mv tests/unit/test_usage_meter.c tests/unit/store/test_usage_meter.c
```

Expected: exit 0.

- [ ] **Step 2: Verify placement**

```bash
ls tests/unit/server tests/unit/store
```

Expected: `server` shows 2 files; `store` shows 3 files.

---

### Task 5: Relocate upstream tests (12 files)

**Files:**
- Move: `tests/unit/test_embeddings.c` → `tests/unit/upstream/test_embeddings.c`
- Move: `tests/unit/test_failover.c` → `tests/unit/upstream/test_failover.c`
- Move: `tests/unit/test_gemini_stream.c` → `tests/unit/upstream/test_gemini_stream.c`
- Move: `tests/unit/test_model_router.c` → `tests/unit/upstream/test_model_router.c`
- Move: `tests/unit/test_provider_anthropic.c` → `tests/unit/upstream/test_provider_anthropic.c`
- Move: `tests/unit/test_provider_deepseek.c` → `tests/unit/upstream/test_provider_deepseek.c`
- Move: `tests/unit/test_provider_gemini.c` → `tests/unit/upstream/test_provider_gemini.c`
- Move: `tests/unit/test_provider_openai.c` → `tests/unit/upstream/test_provider_openai.c`
- Move: `tests/unit/test_provider_probe.c` → `tests/unit/upstream/test_provider_probe.c`
- Move: `tests/unit/test_stream_pipeline.c` → `tests/unit/upstream/test_stream_pipeline.c`
- Move: `tests/unit/test_upstream_client.c` → `tests/unit/upstream/test_upstream_client.c`
- Move: `tests/unit/test_upstream_streaming.c` → `tests/unit/upstream/test_upstream_streaming.c`

- [ ] **Step 1: Move the 12 files (serially, chained with &&)**

```bash
mkdir -p tests/unit/upstream && git mv tests/unit/test_embeddings.c tests/unit/upstream/test_embeddings.c && git mv tests/unit/test_failover.c tests/unit/upstream/test_failover.c && git mv tests/unit/test_gemini_stream.c tests/unit/upstream/test_gemini_stream.c && git mv tests/unit/test_model_router.c tests/unit/upstream/test_model_router.c && git mv tests/unit/test_provider_anthropic.c tests/unit/upstream/test_provider_anthropic.c && git mv tests/unit/test_provider_deepseek.c tests/unit/upstream/test_provider_deepseek.c && git mv tests/unit/test_provider_gemini.c tests/unit/upstream/test_provider_gemini.c && git mv tests/unit/test_provider_openai.c tests/unit/upstream/test_provider_openai.c && git mv tests/unit/test_provider_probe.c tests/unit/upstream/test_provider_probe.c && git mv tests/unit/test_stream_pipeline.c tests/unit/upstream/test_stream_pipeline.c && git mv tests/unit/test_upstream_client.c tests/unit/upstream/test_upstream_client.c && git mv tests/unit/test_upstream_streaming.c tests/unit/upstream/test_upstream_streaming.c
```

Expected: exit 0.

- [ ] **Step 2: Verify root holds only run_tests + mock**

```bash
ls tests/unit/*.c tests/unit/*.h
```

Expected: exactly `mock_upstream.c run_tests.c mock_upstream.h run_tests.h` — nothing else.

---

### Task 6: Update tests/CMakeLists.txt GLOB

**Files:**
- Modify: `tests/CMakeLists.txt` (1 line)

Current content:

```cmake
file(GLOB UNIT_TEST_SRC ${CMAKE_SOURCE_DIR}/tests/unit/*.c)
```

- [ ] **Step 1: Replace the GLOB line**

New content:

```cmake
file(GLOB UNIT_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/*.c ${CMAKE_SOURCE_DIR}/tests/unit/*/*.c)
```

This mirrors the top-level pattern (`file(GLOB AIGATE_SRC CONFIGURE_DEPENDS src/*.c src/*/*.c)`). `CONFIGURE_DEPENDS` makes CMake re-glob when files are added.

- [ ] **Step 2: Confirm the diff is exactly one line**

```bash
git diff tests/CMakeLists.txt
```

Expected: one `-` line, one `+` line, nothing else.

---

### Task 7: Create docs/README.md repo map

**Files:**
- Create: `docs/README.md`

- [ ] **Step 1: Write the file with this exact content**

```markdown
# aigate Repo Map

## Top-level directories

- `src/` — gateway source, 7 layers: `core/` (lifecycle/config/log/secrets),
  `common/` (lru/sha256), `upstream/` (model router + providers + client),
  `policy/` (auth/budget/circuit-breaker/guardrails/ratelimit/response-cache),
  `store/` (postgres/redis/usage-meter), `observe/` (event-bus/health/metrics),
  `server/` (admin API + UI + transport). `main.c` stays at `src/` root.
- `tests/unit/` — mirrors `src/` layers 1:1 (`test_<module>.c` lives in the
  same layer as `src/<layer>/<module>.c`); `run_tests.c` is the registry runner,
  `mock_upstream.c` is the shared stub.
- `tests/integration/` — pytest end-to-end suite (`test_gateway.py`,
  `test_redis_clustering.py`) plus `smoke.sh`.
- `docs/architecture/` — architecture notes; `docs/superpowers/` — specs/plans/reports.
- `web/admin.html` — ops dashboard single page, baked into the binary by
  `scripts/embed_html.py` (edit the page → re-run the script → rebuild).
- `scripts/` — build/dev helpers (`embed_html.py`).
- `schema/schema.sql` — postgres schema. `cmake/` — reserved for toolchain modules.
- `third_party/` — vendored tarballs (civetweb, hdr_histogram).

## Build (canonical)

```bash
cmake -B build -S . && cmake --build build -j && ctest --test-dir build
```

`build/` is the only documented build directory (see `.gitignore`).
```

- [ ] **Step 2: Verify the file renders (head check)**

```bash
head -5 docs/README.md
```

Expected: `# aigate Repo Map` as line 1.

---

### Task 8: Verify + commit

**Files:** none new (verification + commit of Tasks 2–7)

- [ ] **Step 1: Confirm all 31 moves are 100% renames**

```bash
git add -A && git status --short | head -40
```

Expected: 31 `R100` rename lines + 1 modified `tests/CMakeLists.txt` + 1 new `docs/README.md`. Any rename below `R100` → STOP, investigate before continuing.

- [ ] **Step 2: Reconfigure, rebuild, test**

```bash
cmake -B build -S . && cmake --build build -j && ctest --test-dir build
```

Expected: build exit 0; ctest result identical to Task 1 baseline (6/6).

- [ ] **Step 3: Commit**

```bash
git commit -m "refactor: 🗂️ mirror tests/unit into src 7-layer layout"
```

Expected: commit created; `git status --short` empty afterwards.

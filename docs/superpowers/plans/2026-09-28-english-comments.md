# English Comments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Translate all ~1998 Chinese comment lines to English with zero behavior change.

**Architecture:** 9 independent batches (7 src layers + tests + web); each batch translates comment prose only, then passes 5 checks before commit. Batches touch disjoint paths, so each is revertible alone.

**Tech Stack:** C11 / Doxygen comments, CMake + ctest, doxygen 1.18.0, `grep -P` CJK detection.

**Global rules (apply to every batch):**
- Translate ONLY comment text: `//`, `/* */`, `/** */`, `/**< */` prose and Doxygen `@brief/@param/@return` descriptions. Keep all markers, alignment, and line positions.
- NEVER touch: string literals (logs, errors, API output), identifiers, preprocessor directives, `tests/**/__pycache__`.
- Unified terms: 熔断=circuit breaker, 护栏=guardrails, 配额=quota, 预算=budget, 探针=probe, 降级=fallback, 限流=rate limiting, 熔断 states closed/open/half-open.
- Example translation pattern (from `src/policy/circuit_breaker.h`):
  ```c
  // BEFORE: /* 熔断阈值：连续失败3次后打开 */
  // AFTER:  /* Trip threshold: open after 3 consecutive failures */
  ```

---

### Task 0: Baseline

**Files:** none (read-only).

- [ ] **Step 1: Confirm clean tree**

Run: `git status --short`
Expected: empty output.

- [ ] **Step 2: Record baseline build + tests**

Run: `cmake --build build -j 2>&1 | grep -icE "warning|error"; ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"`
Expected: `0` then `100% tests passed out of 6`.

- [ ] **Step 3: Record baseline CJK counts**

Run: `grep -rP '[\x{4e00}-\x{9fff}]' src/ tests/ --include='*.c' --include='*.h' --include='*.py' web/ | wc -l`
Expected: `1998` (872 src + 172 tests + 954 web).

---

### Task 1: common + main.c (26 lines)

**Files:**
- Modify: `src/common/lru.c` (21 lines), `src/common/lru.h` (2 lines), `src/main.c` (3 lines)

- [ ] **Step 1: Translate `src/common/lru.c`, `src/common/lru.h`, `src/main.c`**

Read each CJK comment line (`grep -nP '[\x{4e00}-\x{9fff}]'` on the three files), rewrite prose in English in place. Example (`src/common/lru.c` LRU eviction callback note):
```c
// BEFORE: /** @brief LRU 淘汰回调：释放被淘汰节点的 key/value。 */
// AFTER:  /** @brief LRU eviction callback: frees the evicted node's key/value. */
```

- [ ] **Step 2: Verify zero CJK remains in batch paths**

Run: `grep -rP '[\x{4e00}-\x{9fff}]' src/common/ src/main.c | wc -l`
Expected: `0`.

- [ ] **Step 3: Verify comment-only diff**

Run: `git diff -U0 -- src/common/ src/main.c | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/|//|^\+\s*\*' ; echo CHECK-DONE`
Expected: only `CHECK-DONE` (all added lines are comment lines).

- [ ] **Step 4: Build + tests + doxygen**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"; doxygen Doxyfile 2>&1 | grep -ci warning`
Expected: no warnings/errors, `100% tests passed out of 6`, `0`.

- [ ] **Step 5: Commit**

```bash
git add src/common/ src/main.c
git commit -m "docs: translate common and main comments to English"
```

---

### Task 2: core (92 lines)

**Files:**
- Modify: `src/core/aigate_core.c` (49), `src/core/aigate_core.h` (24), `src/core/aigate_log.c` (1), `src/core/aigate_log.h` (3), `src/core/config.c` (2), `src/core/config.h` (11), `src/core/secrets.c` (2)

- [ ] **Step 1: Translate the 7 core files**

Same procedure as Task 1 Step 1. Example (`src/core/config.h`):
```c
// BEFORE: uint16_t port; /* 监听端口 */
// AFTER:  uint16_t port; /**< Listen port. */
```

- [ ] **Step 2: Verify zero CJK in `src/core/`**

Run: `grep -rP '[\x{4e00}-\x{9fff}]' src/core/ | wc -l`
Expected: `0`.

- [ ] **Step 3: Verify comment-only diff** (same command shape as Task 1 Step 3, path `src/core/`)

- [ ] **Step 4: Build + tests + doxygen** (same as Task 1 Step 4)

- [ ] **Step 5: Commit**

```bash
git add src/core/
git commit -m "docs: translate core comments to English"
```

---

### Task 3: observe (121 lines)

**Files:**
- Modify: `src/observe/event_bus.c` (2), `src/observe/event_bus.h` (67), `src/observe/health_prober.c` (4), `src/observe/health_prober.h` (32), `src/observe/metrics.c` (12), `src/observe/metrics.h` (4)

- [ ] **Step 1: Translate the 6 observe files** (procedure as Task 1 Step 1)

- [ ] **Step 2: Zero-CJK check on `src/observe/`** → expect `0`

- [ ] **Step 3: Comment-only diff check** (path `src/observe/`)

- [ ] **Step 4: Build + tests + doxygen**

- [ ] **Step 5: Commit**

```bash
git add src/observe/
git commit -m "docs: translate observe comments to English"
```

---

### Task 4: server (97 lines)

**Files:**
- Modify: `src/server/admin_api.c` (70), `src/server/admin_api.h` (5), `src/server/transport_civetweb.c` (20), `src/server/transport_civetweb.h` (2)

- [ ] **Step 1: Translate the 4 server files** (procedure as Task 1 Step 1)

- [ ] **Step 2: Zero-CJK check on `src/server/`** → expect `0`

- [ ] **Step 3: Comment-only diff check** (path `src/server/`)

- [ ] **Step 4: Build + tests + doxygen**

- [ ] **Step 5: Commit**

```bash
git add src/server/
git commit -m "docs: translate server comments to English"
```

---

### Task 5: policy (147 lines)

**Files:**
- Modify: `src/policy/auth_key.c` (1), `src/policy/budget_enforce.c` (21), `src/policy/budget_enforce.h` (1), `src/policy/circuit_breaker.c` (26), `src/policy/circuit_breaker.h` (8), `src/policy/guardrails.c` (13), `src/policy/guardrails.h` (32), `src/policy/ratelimit.c` (18), `src/policy/response_cache.c` (6), `src/policy/response_cache.h` (21)

- [ ] **Step 1: Translate the 10 policy files** (procedure as Task 1 Step 1)

- [ ] **Step 2: Zero-CJK check on `src/policy/`** → expect `0`

- [ ] **Step 3: Comment-only diff check** (path `src/policy/`)

- [ ] **Step 4: Build + tests + doxygen**

- [ ] **Step 5: Commit**

```bash
git add src/policy/
git commit -m "docs: translate policy comments to English"
```

---

### Task 6: store (224 lines)

**Files:**
- Modify: `src/store/pg_store.c` (51), `src/store/pg_store.h` (121), `src/store/redis_client.c` (2), `src/store/redis_pool.c` (9), `src/store/usage_meter.c` (41)

- [ ] **Step 1: Translate the 5 store files** (procedure as Task 1 Step 1)

- [ ] **Step 2: Zero-CJK check on `src/store/`** → expect `0`

- [ ] **Step 3: Comment-only diff check** (path `src/store/`)

- [ ] **Step 4: Build + tests + doxygen**

- [ ] **Step 5: Commit**

```bash
git add src/store/
git commit -m "docs: translate store comments to English"
```

---

### Task 7: upstream (165 lines)

**Files:**
- Modify: `src/upstream/model_router.c` (5), `src/upstream/model_router.h` (4), `src/upstream/provider_adapter.c` (2), `src/upstream/provider_adapter.h` (7), `src/upstream/provider_anthropic.c` (16), `src/upstream/provider_anthropic.h` (22), `src/upstream/provider_gemini.c` (32), `src/upstream/provider_gemini.h` (10), `src/upstream/provider_openai.c` (28), `src/upstream/provider_openai.h` (1), `src/upstream/upstream_client.c` (38)

- [ ] **Step 1: Translate the 11 upstream files** (procedure as Task 1 Step 1)

- [ ] **Step 2: Zero-CJK check on `src/upstream/`** → expect `0`

- [ ] **Step 3: Comment-only diff check** (path `src/upstream/`)

- [ ] **Step 4: Build + tests + doxygen**

- [ ] **Step 5: Commit**

```bash
git add src/upstream/
git commit -m "docs: translate upstream comments to English"
```

---

### Task 8: tests (172 lines)

**Files (25):** `tests/integration/test_gateway.py` (1), `tests/unit/common/test_lru.c` (2), `tests/unit/core/test_aigate_core.c` (18), `tests/unit/core/test_config.c` (1), `tests/unit/core/test_log.c` (1), `tests/unit/core/test_secrets.c` (1), `tests/unit/mock_upstream.c` (1), `tests/unit/policy/test_auth_key.c` (3), `tests/unit/policy/test_circuit_breaker.c` (2), `tests/unit/policy/test_guardrails.c` (12), `tests/unit/policy/test_ratelimit.c` (1), `tests/unit/policy/test_response_cache.c` (6), `tests/unit/run_tests.c` (5), `tests/unit/server/test_admin_api.c` (36), `tests/unit/server/test_admin_ui.c` (1), `tests/unit/store/test_pg_store.c` (35), `tests/unit/store/test_usage_meter.c` (4), `tests/unit/upstream/test_embeddings.c` (6), `tests/unit/upstream/test_failover.c` (7), `tests/unit/upstream/test_gemini_stream.c` (3), `tests/unit/upstream/test_model_router.c` (5), `tests/unit/upstream/test_provider_anthropic.c` (7), `tests/unit/upstream/test_provider_deepseek.c` (2), `tests/unit/upstream/test_stream_pipeline.c` (11), `tests/unit/upstream/test_upstream_streaming.c` (1)

- [ ] **Step 1: Translate the 25 test files** (procedure as Task 1 Step 1; TEST_CASE names are identifiers — do NOT rename them, translate only comment lines)

- [ ] **Step 2: Zero-CJK check**

Run: `grep -rP '[\x{4e00}-\x{9fff}]' tests/ --include='*.c' --include='*.h' --include='*.py' | wc -l`
Expected: `0`.

- [ ] **Step 3: Comment-only diff check** (path `tests/`)

- [ ] **Step 4: Build + tests + doxygen**

- [ ] **Step 5: Commit**

```bash
git add tests/
git commit -m "docs: translate test comments to English"
```

---

### Task 9: web (954 lines)

**Files:**
- Modify: `web/admin.html` (954 lines)

- [ ] **Step 1: Translate JS `//` comments in `web/admin.html`**

Read each CJK comment line, rewrite in English in place. HTML-embedded `<script>` comments only; do NOT touch user-visible UI strings (button labels, placeholders) — those are string literals, out of scope per spec §1. Example:
```js
// BEFORE: // 刷新令牌列表
// AFTER:  // Refresh the token list
```

- [ ] **Step 2: Zero-CJK check**

Run: `grep -cP '[\x{4e00}-\x{9fff}]' web/admin.html`
Expected: `0`.

- [ ] **Step 3: Comment-only diff check** (path `web/admin.html`; note: `//` lines pass the `//` filter in the check command — verify by eyeballing `git diff --stat` shows 0 deletions beyond comment-line realignment)

- [ ] **Step 4: Build + tests + doxygen** (admin.html is embedded at build time via embed_html.py — rebuild regenerates; ctest must stay 6/6)

- [ ] **Step 5: Commit**

```bash
git add web/admin.html
git commit -m "docs: translate admin UI comments to English"
```

---

### Task 10: Final verification

**Files:** none (read-only).

- [ ] **Step 1: Full-tree zero-CJK check**

Run: `grep -rP '[\x{4e00}-\x{9fff}]' src/ tests/ --include='*.c' --include='*.h' --include='*.py' web/ | wc -l`
Expected: `0`.

- [ ] **Step 2: doxygen zero warnings**

Run: `doxygen Doxyfile 2>&1 | grep -ci warning`
Expected: `0`.

- [ ] **Step 3: Build zero warnings + ctest 6/6**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"`
Expected: no warnings/errors, `100% tests passed out of 6`.

- [ ] **Step 4: Confirm 9 batch commits on top of plan**

Run: `git log --oneline -12`
Expected: 9 `docs: translate ...` commits plus plan/spec commits below.

# Doxygen 分组体系补齐 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 30 个头文件补齐 Doxygen 分组体系（7 个 @defgroup + 23 行 @ingroup）并新建最小 Doxyfile，doxygen 零警告跑通。

**Architecture:** 纯注释改动 + 1 个新配置文件。Task 1 加 7 个分组定义（每层锚定头文件 1 块）；Task 2 给其余 23 头文件各加 1 行 @ingroup；Task 3 新建 Doxyfile 并三验证后单提交。

**Tech Stack:** C (Doxygen 注释)、doxygen 二进制、CMake/CTest（回归验证）。

---

### Task 1: 7 个锚定头文件加 @defgroup

**Files:**
- Modify: `src/common/lru.h` (@file 块后)
- Modify: `src/core/aigate_core.h` (@file 块后)
- Modify: `src/observe/event_bus.h` (@file 块后)
- Modify: `src/policy/guardrails.h` (@file 块后)
- Modify: `src/server/transport_civetweb.h` (@file 块后)
- Modify: `src/store/pg_store.h` (@file 块后)
- Modify: `src/upstream/provider_adapter.h` (@file 块后)

每个文件：在 `@file` 块结束 `*/` 之后、`#ifndef` 之前插入以下块（仅 @brief 行按表替换）：

```c
/**
 * @defgroup group_common 通用层
 * @brief 通用：LRU 缓存、SHA-256。
 */
```

7 组 @brief 全文（照抄）：

- `src/common/lru.h`: `@defgroup group_common 通用层` / `@brief 通用：LRU 缓存、SHA-256。`
- `src/core/aigate_core.h`: `@defgroup group_core 核心层` / `@brief 核心：网关上下文、配置、密钥、日志。`
- `src/observe/event_bus.h`: `@defgroup group_observe 可观测层` / `@brief 可观测：事件总线、健康探针、指标。`
- `src/policy/guardrails.h`: `@defgroup group_policy 策略层` / `@brief 策略：鉴权、预算、熔断、护栏、限流、响应缓存。`
- `src/server/transport_civetweb.h`: `@defgroup group_server 服务层` / `@brief 服务：HTTP 传输、管理 API、后台页面。`
- `src/store/pg_store.h`: `@defgroup group_store 存储层` / `@brief 存储：Postgres、Redis、用量计量。`
- `src/upstream/provider_adapter.h`: `@defgroup group_upstream 上游层` / `@brief 上游：模型路由、供应商适配、客户端。`

- [ ] **Step 1: 7 个文件各插入对应分组块**

以 `src/common/lru.h` 为例（其余 6 个同式，仅块内容按上表替换），原：

```c
 *  owner can release it.
 */
#ifndef AIGATE_LRU_H
```

改后：

```c
 *  owner can release it.
 */

/**
 * @defgroup group_common 通用层
 * @brief 通用：LRU 缓存、SHA-256。
 */
#ifndef AIGATE_LRU_H
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff --stat && git diff | grep '^-' | grep -v '^---'`
Expected: 7 files changed, 第二条命令无输出（0 deletions）

---

### Task 2: 23 个头文件各加一行 @ingroup

**Files:** 以下 23 个头文件各改 1 行（插入位置统一：紧随首行 `/** @file xxx.h` 之后的新行，内容为 ` *  @ingroup <组>`，与块内 ` *  @brief` 对齐风格一致）：

- common: `src/common/sha256.h` → `group_common`
- core: `src/core/aigate_log.h`、`src/core/config.h`、`src/core/secrets.h` → `group_core`
- observe: `src/observe/health_prober.h`、`src/observe/metrics.h` → `group_observe`
- policy: `src/policy/auth_key.h`、`src/policy/budget_enforce.h`、`src/policy/circuit_breaker.h`、`src/policy/ratelimit.h`、`src/policy/response_cache.h` → `group_policy`
- server: `src/server/admin_api.h`、`src/server/admin_ui.h` → `group_server`
- store: `src/store/redis_client.h`、`src/store/redis_pool.h`、`src/store/redis_scripts.h`、`src/store/schema_sql.h`、`src/store/usage_meter.h` → `group_store`
- upstream: `src/upstream/model_router.h`、`src/upstream/provider_anthropic.h`、`src/upstream/provider_gemini.h`、`src/upstream/provider_openai.h`、`src/upstream/upstream_client.h` → `group_upstream`

- [ ] **Step 1: 23 个文件各插入一行 @ingroup**

以 `src/common/sha256.h` 为例（其余 22 个同式，仅组名替换），原首两行：

```c
/** @file sha256.h
 *  @brief ...
```

改后：

```c
/** @file sha256.h
 *  @ingroup group_common
 *  @brief ...
```

即：每个文件只新增一行 ` *  @ingroup <组>`，不碰其他任何行。

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff --stat | tail -1 && git diff | grep '^-' | grep -v '^---'`
Expected: 30 files changed（Task1 的 7 个 + 本 Task 23 个），第二条命令无输出

---

### Task 3: 新建 Doxyfile + 三验证 + 提交

**Files:**
- Create: `Doxyfile`
- Test: 全量构建 + `ctest` + `doxygen Doxyfile`

- [ ] **Step 1: 新建 Doxyfile（全文照抄）**

```ini
PROJECT_NAME           = aigate
INPUT                  = src
RECURSIVE              = YES
GENERATE_HTML          = YES
OUTPUT_DIRECTORY       = build/docs
QUIET                  = YES
```

- [ ] **Step 2: doxygen 零警告验证**

Run: `doxygen Doxyfile 2>&1 | grep -iE "warning|error"; echo "doxygen check done"`
Expected: 只输出 `doxygen check done`（无警告无错误）

- [ ] **Step 3: 全量构建零警告 + ctest 6/6**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"`
Expected: 构建无输出；`100% tests passed out of 6`

- [ ] **Step 4: 纯注释机检**

Run: `git diff -U0 -- 'src/*.h' 'src/*/*.h' | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: 无输出（所有加行都是注释行；Doxyfile 是新文件不在此范围）

- [ ] **Step 5: 提交**

```bash
git add src/common/lru.h src/common/sha256.h src/core/aigate_core.h src/core/aigate_log.h src/core/config.h src/core/secrets.h src/observe/event_bus.h src/observe/health_prober.h src/observe/metrics.h src/policy/guardrails.h src/policy/auth_key.h src/policy/budget_enforce.h src/policy/circuit_breaker.h src/policy/ratelimit.h src/policy/response_cache.h src/server/transport_civetweb.h src/server/admin_api.h src/server/admin_ui.h src/store/pg_store.h src/store/redis_client.h src/store/redis_pool.h src/store/redis_scripts.h src/store/schema_sql.h src/store/usage_meter.h src/upstream/provider_adapter.h src/upstream/model_router.h src/upstream/provider_anthropic.h src/upstream/provider_gemini.h src/upstream/provider_openai.h src/upstream/upstream_client.h Doxyfile
git commit -m "docs: add Doxygen groups and Doxyfile"
```

Expected: `31 files changed`（30 头文件 + Doxyfile），工作树干净。

# src 多目录拆分 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `src/` 根目录 28 个模块按 spec 搬入 7 个子目录，只改顶层 `CMakeLists.txt` 两处，构建与测试全绿，无业务逻辑变更。

**Architecture:** 纯文件搬迁 + 构建收集面适配。`git mv` 保留历史；GLOB 加一层递归 + `CONFIGURE_DEPENDS`；`target_include_directories` 追加子目录使 `#include "xxx.h"` 零改动；`libaigate` 仍为单个静态库。

**Tech Stack:** C17, CMake 3.16+, gcc (`-Wall -Wextra -Werror`), ctest。

**Spec:** `docs/superpowers/specs/2026-09-27-src-multi-dir-split-design.md`（commit `4688327`）。

**Note on TDD:** 本次零行为变更，不新增测试；等价验证手段为迁移前后 `ctest` 基线对比（Task 1 记录基线，Task 11 对比）。

---

### Task 1: 记录迁移前构建与测试基线

**Files:** 无改动（只读）。

- [ ] **Step 1: 确认工作区脏文件清单**

```bash
git status --short
```

Expected: 看到 `M src/admin_api.c`、`M src/admin_api.h`、`M src/aigate_core.h` 等已修改文件，以及 `?? src/response_cache.c`、`?? src/response_cache.h`、`?? tests/unit/test_response_cache.c` 等未跟踪文件。记下完整清单，后续 Task 2 只提交这些文件。

- [ ] **Step 2: 全量构建基线**

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
```

Expected: exit 0（`build/` 目录已存在，复用即可；`-Werror` 下零警告）。

- [ ] **Step 3: 运行测试基线并记录结果**

```bash
ctest --test-dir build 2>&1 | tail -5
```

Expected: 记录通过/失败数（例如 `100% tests passed, 0 tests failed out of N`）。把 `N` 和失败名记下来，Task 11 必须与之一致。若基线已有失败，记下名字，Task 11 允许同样失败、但不允许新增失败。

---

### Task 2: 隔离用户未提交修改（单独 checkpoint 提交）

**Files:**
- Modify (commit, 无内容改动）: `git status --short` 中全部 `M`/`??` 文件。

- [ ] **Step 1: 暂存全部现状改动**

```bash
git add -A && git status --short
```

Expected: 所有 `M`/`??` 条目变为 `M`/`A`（staged），无剩余 unstaged。

- [ ] **Step 2: 单独提交为 checkpoint**

```bash
git commit -m "chore: checkpoint uncommitted work before src split"
```

Expected: `git status --short` 输出为空（干净树）。这一步保证迁移 diff 是纯改名 + CMake 两处，不混入业务改动。

---

### Task 3: 搬迁 `core/`（启动编排与基础配置）

**Files:**
- Move: `src/aigate_core.c`, `src/aigate_core.h`, `src/config.c`, `src/config.h`, `src/aigate_log.c`, `src/aigate_log.h`, `src/secrets.c`, `src/secrets.h` → `src/core/`

- [ ] **Step 1: 建目录并搬迁**

```bash
mkdir -p src/core && git mv src/aigate_core.c src/aigate_core.h src/config.c src/config.h src/aigate_log.c src/aigate_log.h src/secrets.c src/secrets.h src/core/ && ls src/core/
```

Expected: `ls` 输出 8 个文件，无报错。

---

### Task 4: 搬迁 `common/`（无依赖小工具）

**Files:**
- Move: `src/sha256.c`, `src/sha256.h`, `src/lru.c`, `src/lru.h` → `src/common/`

- [ ] **Step 1: 建目录并搬迁**

```bash
mkdir -p src/common && git mv src/sha256.c src/sha256.h src/lru.c src/lru.h src/common/ && ls src/common/
```

Expected: `ls` 输出 4 个文件，无报错。

---

### Task 5: 搬迁 `upstream/`（上游调用与选路）

**Files:**
- Move: `src/upstream_client.c`, `src/upstream_client.h`, `src/provider_adapter.c`, `src/provider_adapter.h`, `src/provider_openai.c`, `src/provider_openai.h`, `src/provider_anthropic.c`, `src/provider_anthropic.h`, `src/provider_gemini.c`, `src/provider_gemini.h`, `src/model_router.c`, `src/model_router.h` → `src/upstream/`

- [ ] **Step 1: 建目录并搬迁**

```bash
mkdir -p src/upstream && git mv src/upstream_client.c src/upstream_client.h src/provider_adapter.c src/provider_adapter.h src/provider_openai.c src/provider_openai.h src/provider_anthropic.c src/provider_anthropic.h src/provider_gemini.c src/provider_gemini.h src/model_router.c src/model_router.h src/upstream/ && ls src/upstream/
```

Expected: `ls` 输出 12 个文件，无报错。

---

### Task 6: 搬迁 `policy/`（网关策略中间件，含未跟踪新文件）

**Files:**
- Move: `src/auth_key.c`, `src/auth_key.h`, `src/ratelimit.c`, `src/ratelimit.h`, `src/budget_enforce.c`, `src/budget_enforce.h`, `src/guardrails.c`, `src/guardrails.h`, `src/circuit_breaker.c`, `src/circuit_breaker.h`（已跟踪）→ `src/policy/`
- Move: `src/response_cache.c`, `src/response_cache.h`（未跟踪，Task 2 已 `git add`，现为 staged 新文件）→ `src/policy/`

- [ ] **Step 1: 建目录并搬迁已跟踪文件**

```bash
mkdir -p src/policy && git mv src/auth_key.c src/auth_key.h src/ratelimit.c src/ratelimit.h src/budget_enforce.c src/budget_enforce.h src/guardrails.c src/guardrails.h src/circuit_breaker.c src/circuit_breaker.h src/policy/ && ls src/policy/
```

Expected: `ls` 输出 10 个文件，无报错。

- [ ] **Step 2: 搬迁 `response_cache`（Task 2 后已是 staged 状态，`git mv` 可用）**

```bash
git mv src/response_cache.c src/response_cache.h src/policy/ && ls src/policy/
```

Expected: `ls` 输出 12 个文件，无报错。若 `git mv` 报 `not under version control`（说明 Task 2 漏加），改用 `mv src/response_cache.c src/response_cache.h src/policy/ && git add -A`，效果等价。

---

### Task 7: 搬迁 `store/`（持久化与计量，含两个纯头文件）

**Files:**
- Move: `src/pg_store.c`, `src/pg_store.h`, `src/redis_client.c`, `src/redis_client.h`, `src/redis_pool.c`, `src/redis_pool.h`, `src/redis_scripts.h`, `src/usage_meter.c`, `src/usage_meter.h`, `src/schema_sql.h` → `src/store/`

- [ ] **Step 1: 建目录并搬迁**

```bash
mkdir -p src/store && git mv src/pg_store.c src/pg_store.h src/redis_client.c src/redis_client.h src/redis_pool.c src/redis_pool.h src/redis_scripts.h src/usage_meter.c src/usage_meter.h src/schema_sql.h src/store/ && ls src/store/
```

Expected: `ls` 输出 10 个文件，无报错。

---

### Task 8: 搬迁 `observe/`（可观测性）

**Files:**
- Move: `src/metrics.c`, `src/metrics.h`, `src/event_bus.c`, `src/event_bus.h`, `src/health_prober.c`, `src/health_prober.h` → `src/observe/`

- [ ] **Step 1: 建目录并搬迁**

```bash
mkdir -p src/observe && git mv src/metrics.c src/metrics.h src/event_bus.c src/event_bus.h src/health_prober.c src/health_prober.h src/observe/ && ls src/observe/
```

Expected: `ls` 输出 6 个文件，无报错。

---

### Task 9: 搬迁 `server/`（对外服务面）

**Files:**
- Move: `src/transport_civetweb.c`, `src/transport_civetweb.h`, `src/admin_api.c`, `src/admin_api.h`, `src/admin_ui.c`, `src/admin_ui.h` → `src/server/`

- [ ] **Step 1: 建目录并搬迁**

```bash
mkdir -p src/server && git mv src/transport_civetweb.c src/transport_civetweb.h src/admin_api.c src/admin_api.h src/admin_ui.c src/admin_ui.h src/server/ && ls src/server/
```

Expected: `ls` 输出 6 个文件，无报错。

- [ ] **Step 2: 确认 `src/` 根目录无残留**

```bash
ls src/*.c src/*.h
```

Expected: 仅输出 `src/main.c`。若出现其他文件，说明某 Task 漏搬，回查对应 Task 补搬。

---

### Task 10: 改顶层 `CMakeLists.txt`（两处）

**Files:**
- Modify: `CMakeLists.txt:69`（GLOB 行）
- Modify: `CMakeLists.txt:80`（`target_include_directories` 行）

- [ ] **Step 1: GLOB 加递归一层与 `CONFIGURE_DEPENDS`**

旧（第 69 行）：

```cmake
file(GLOB AIGATE_SRC src/*.c)
```

新：

```cmake
file(GLOB AIGATE_SRC CONFIGURE_DEPENDS src/*.c src/*/*.c)
```

- [ ] **Step 2: include path 追加 7 个子目录**

旧（第 80 行）：

```cmake
target_include_directories(libaigate PUBLIC src ${CMAKE_BINARY_DIR}/generated)
```

新：

```cmake
target_include_directories(libaigate PUBLIC src src/core src/common src/upstream src/policy src/store src/observe src/server ${CMAKE_BINARY_DIR}/generated)
```

- [ ] **Step 3: 确认 diff 仅两行**

```bash
git diff CMakeLists.txt
```

Expected: diff 恰为上述两行变更，无其他改动。`tests/CMakeLists.txt` 不碰。

---

### Task 11: 全量构建 + 测试 + 验收

**Files:** 无改动（验证）。

- [ ] **Step 1: 重新配置并全量构建**

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
```

Expected: exit 0，`-Werror` 下零新增警告。注意：必须重跑 `cmake -S . -B build`（刷新 GLOB 文件列表），不能只 `cmake --build`。

- [ ] **Step 2: 运行全部测试并对比基线**

```bash
ctest --test-dir build 2>&1 | tail -5
```

Expected: 通过数与 Task 1 基线一致，无新增失败。若基线全绿则此处必须全绿。

- [ ] **Step 3: 验收残留与纯改名检查**

```bash
ls src/*.c src/*.h && echo "---" && git add -A -n 2>/dev/null; git status --short | head -70
```

Expected: `ls` 仅输出 `src/main.c`；`git status` 显示全为 `R`（改名）与 `M CMakeLists.txt`，无业务文件内容变更。可再抽查 `git diff -M --stat` 确认相似度（改名文件应显示 `N similarity index` 高百分比）。

- [ ] **Step 4: 提交迁移**

```bash
git add -A && git commit -m "refactor: 🗂️ split src into 7 layered subdirectories"
```

Expected: `git status --short` 为空，`git log --oneline -1` 显示新提交。

---

## Self-Review

**1. Spec 覆盖度：**
- §2 目录划分 → Task 3–9（一一对应 7 目录，28 模块 + 2 纯头文件全覆盖：8+4+12+12+10+6+6=58 个文件 + `main.c` 留守 = 59，与现状 `29 .c + 30 .h` 一致）。
- §3 构建改动两处 → Task 10（GLOB + include，共两行 diff）。
- §4 迁移步骤 1–5 → Task 3–9（`git mv`）、Task 10（CMake）、Task 11 Step 1–2（构建+ctest）、`-Werror` 由构建天然强制。
- §4 未提交新文件归位 → Task 2（checkpoint）+ Task 6 Step 2（`response_cache` → `policy/`）。
- §5 验收标准 4 条 → Task 11 Step 1–3（exit 0 / ctest 基线对比 / 无残留 / 纯改名）。
- §6 风险 → `CONFIGURE_DEPENDS`（Task 10 Step 1）、`git mv`（Task 3–9）已落实；单库接受项无需任务。

**2. 占位符扫描：** 无 TBD/TODO/"类似 Task N"；每步含完整命令与 Expected。

**3. 类型一致性：** 不适用（无新增函数/类型；纯搬迁，`#include` 零改动）。

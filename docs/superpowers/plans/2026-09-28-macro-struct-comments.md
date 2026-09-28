# 宏与结构体字段注释 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 src/ 全部头文件的无文档公开宏加 `/** @brief */`、无注释结构体字段加行尾 `/* */`，纯注释变更。

**Architecture:** 按 7 个 src 子目录分 7 批，每批独立提交；每批三验证（零警告构建、ctest 6/6、纯注释机检）。只加行、不删改代码。

**Tech Stack:** C11, CMake, ctest, Doxygen 风格注释（宏 `/** */` + 字段行尾 `/* */`，与既有 70 处风格一致）。

---

### Task 0: 基线审计

**Files:**（只读，无修改）

- [ ] **Step 1: 确认工作树干净**

Run: `git status --short`
Expected: 无输出

- [ ] **Step 2: 全量构建记基线**

Run: `cmake --build build -j 2>&1 | tail -2`
Expected: exit 0

- [ ] **Step 3: 测试记基线**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: 6/6 全绿（记下耗时供后续对比）

---

### Task 1: common + core 批

**Files:**
- Modify: `src/core/aigate_core.h`（约 19 个无注释字段）
- Modify: `src/core/aigate_log.h`（3 个无文档宏）
- Skip: `src/core/config.h`（11 字段全有注释）、`src/common/lru.h`（仅守卫宏+函数原型，无缺口）

**格式 worked example（字段，照抄风格）：**

```c
// 修改前
typedef struct auth_key_cache {
    lru_t*   recs;
} auth_key_cache;
// 修改后（字段已有的 /* */ 风格就是这样；新加的照此写中文一句话）
    lru_t*   recs;    /* key_hash(hex) → key_rec_t*（堆值，逐出释放） */
```

**宏 worked example：**

```c
/** @brief key 缓存 LRU 最大条目数。 */
#define AUTH_KEY_CACHE_MAX 1024
```

- [ ] **Step 1: 给 aigate_log.h 的 3 个宏逐个加 `/** @brief … */`（守卫宏 `AIGATE_*_H` 跳过）**

- [ ] **Step 2: 给 aigate_core.h 内 `typedef struct { … };` 块中无行尾注释的成员行加 `/* … */`；函数原型（带括号参数的行）一律跳过；已有 `/* */` 的 3 行不动**

- [ ] **Step 3: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 4: 测试**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿

- [ ] **Step 5: 纯注释机检**

Run: `git diff -U0 -- src/core | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

- [ ] **Step 6: 提交（只许加本批路径）**

```bash
git add src/core/aigate_core.h src/core/aigate_log.h
git commit -m "docs: annotate core macros and struct fields"
```

---

### Task 2: observe 批

**Files:**
- Modify: `src/observe/event_bus.h`（3 宏 + 17 字段）
- Modify: `src/observe/health_prober.h`（1 宏 + 24 字段）

- [ ] **Step 1: 两文件宏加 `/** @brief … */`（守卫宏跳过）**

- [ ] **Step 2: 两文件结构体成员行加行尾 `/* … */`；函数原型跳过；位域/函数指针字段注明单位与取值**

- [ ] **Step 3: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 4: 测试**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿

- [ ] **Step 5: 纯注释机检**

Run: `git diff -U0 -- src/observe | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

- [ ] **Step 6: 提交（只许加本批路径）**

```bash
git add src/observe/event_bus.h src/observe/health_prober.h
git commit -m "docs: annotate observe macros and struct fields"
```

---

### Task 3: server 批

**Files:**
- Modify: `src/server/admin_api.h`（约 4 个无注释字段：7 成员行 − 3 已有）
- Skip: `src/server/transport_civetweb.h`（无缺口）

- [ ] **Step 1: admin_api.h 结构体成员行加行尾 `/* … */`；函数原型跳过；已有 3 行不动**

- [ ] **Step 2: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 3: 测试**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿

- [ ] **Step 4: 纯注释机检**

Run: `git diff -U0 -- src/server | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

- [ ] **Step 5: 提交（只许加本批路径）**

```bash
git add src/server/admin_api.h
git commit -m "docs: annotate server struct fields"
```

---

### Task 4: policy 批

**Files:**
- Modify: `src/policy/circuit_breaker.h`（2 宏）
- Modify: `src/policy/response_cache.h`（2 宏 + 约 16 字段：32 成员行 − 16 已有）
- Modify: `src/policy/guardrails.h`（约 3 字段：6 − 3 已有）
- Skip: `src/policy/auth_key.h`、`ratelimit.h`、`budget_enforce.h`（无缺口）

- [ ] **Step 1: 三文件宏加 `/** @brief … */`（守卫宏跳过）**

- [ ] **Step 2: response_cache.h、guardrails.h 成员行加行尾 `/* … */`；函数原型跳过；已有注释不动**

- [ ] **Step 3: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 4: 测试**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿

- [ ] **Step 5: 纯注释机检**

Run: `git diff -U0 -- src/policy | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

- [ ] **Step 6: 提交（只许加本批路径）**

```bash
git add src/policy/circuit_breaker.h src/policy/response_cache.h src/policy/guardrails.h
git commit -m "docs: annotate policy macros and struct fields"
```

---

### Task 5: store 批

**Files:**
- Modify: `src/store/pg_store.h`（21 宏 + 约 40+ 字段；75 成员式行 − 26 已有 − 函数原型）
- Skip: `src/store/redis_pool.h`、`usage_meter.h`（无缺口）

**位掩码宏 worked example（KMASK_/MMASK_/PMASK_ 三组逐行注）：**

```c
/** @brief key 更新掩码：限速字段。 */
#define KMASK_RATE (1 << 0)
```

- [ ] **Step 1: 21 个宏逐个加 `/** @brief … */`（守卫宏跳过）；掩码组每行说明该位含义**

- [ ] **Step 2: 结构体成员行加行尾 `/* … */`；判别式：只动 `typedef struct { … };` 花括号内的储值声明，凡带 `(…)` 参数表的函数原型一律跳过；已有 26 行不动**

- [ ] **Step 3: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 4: 测试**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿

- [ ] **Step 5: 纯注释机检**

Run: `git diff -U0 -- src/store | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

- [ ] **Step 6: 提交（只许加本批路径）**

```bash
git add src/store/pg_store.h
git commit -m "docs: annotate store macros and struct fields"
```

---

### Task 6: upstream 批

**Files:**
- Modify: `src/upstream/provider_anthropic.h`（17 字段）
- Modify: `src/upstream/provider_adapter.h`（约 4 字段：5 − 1 已有）
- Modify: `src/upstream/provider_gemini.h`（5 字段）
- Modify: `src/upstream/model_router.h`（约 2 字段：5 − 3 已有）
- Skip: 各文件守卫宏（唯一宏行，无缺口）

- [ ] **Step 1: 四文件结构体成员行加行尾 `/* … */`；函数原型跳过；已有注释不动**

- [ ] **Step 2: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 3: 测试**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿

- [ ] **Step 4: 纯注释机检**

Run: `git diff -U0 -- src/upstream | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

- [ ] **Step 5: 提交（只许加本批路径）**

```bash
git add src/upstream/provider_anthropic.h src/upstream/provider_adapter.h src/upstream/provider_gemini.h src/upstream/model_router.h
git commit -m "docs: annotate upstream struct fields"
```

---

### Task 7: 全仓终验

**Files:**（只读，无修改）

- [ ] **Step 1: 确认 7 批提交全部落地**

Run: `git log --oneline -8`
Expected: 含 Task 1–6 的 6 个 `docs: annotate …` 注释提交

- [ ] **Step 2: 最终零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"; echo BUILD_OK`
Expected: 只输出 BUILD_OK

- [ ] **Step 3: 最终测试与基线对比**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: 6/6 全绿，与 Task 0 基线一致

- [ ] **Step 4: 全量纯注释机检（7 批合并）**

Run: `git diff 473757e..HEAD -U0 -- 'src/*.h' | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)' ; echo CHECK_DONE`
Expected: 除 CHECK_DONE 外无输出

# Enum + Typedef 注释补齐 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 `cb_state_t`、`event_type_t` 的 9 个枚举成员与 `cb_time_fn` 加 Doxygen 块注释。

**Architecture:** 纯注释追加（只加行、不改代码），单批次一次提交；`lru_evict_fn` 与 `upstream_chunk_fn` 实测已有 `@brief`，不在范围内。

**Tech Stack:** C11, Doxygen 块注释，CMake + ctest。

---

### Task 1: 熔断器枚举与时间函数注释

**Files:**
- Modify: `src/policy/circuit_breaker.h:14-21`

- [ ] **Step 1: 加 4 块注释**

```c
typedef enum {
    /** @brief 关闭态：正常放行，失败计数累计中。 */
    CB_CLOSED = 0,
    /** @brief 打开态：熔断中，直接拒绝并走降级。 */
    CB_OPEN = 1,
    /** @brief 半开态：冷却后放少量探测流量，成功则关闭。 */
    CB_HALF_OPEN = 2,
} cb_state_t;
```

```c
/** @brief 时间源函数类型：返回当前秒级时间戳，用于熔断冷却计时（便于测试注入假时钟）。 */
typedef time_t (*cb_time_fn)(void);
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff -U0 -- src/policy/circuit_breaker.h | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: 空输出（无非注释加行），`git diff --stat` 显示 0 deletions。

---

### Task 2: 事件总线枚举注释

**Files:**
- Modify: `src/observe/event_bus.h:21-28`

- [ ] **Step 1: 加 6 块注释**

```c
typedef enum {
    /** @brief 哨兵值：无事件，订阅者收到的空轮询结果。 */
    EVENT_NONE = 0,
    /** @brief 请求事件：网关完成一次推理请求（含状态码与耗时）。 */
    EVENT_REQUEST,
    /** @brief 熔断事件：某上游端点熔断器状态变迁。 */
    EVENT_CIRCUIT_BREAKER,
    /** @brief 探针事件：上游健康探针完成一轮探测。 */
    EVENT_HEALTH_PROBE,
    /** @brief 预算告警事件：key 或分组用量触及预算阈值。 */
    EVENT_BUDGET_ALERT,
    /** @brief 心跳事件：SSE 保活 ping，订阅者忽略内容。 */
    EVENT_PING
} event_type_t;
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff -U0 -- src/observe/event_bus.h | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: 空输出，0 deletions。

---

### Task 3: 三验证并提交

- [ ] **Step 1: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"`
Expected: 空输出。

- [ ] **Step 2: ctest 与基线一致**

Run: `ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"`
Expected: `100% tests passed out of 6`。

- [ ] **Step 3: 全范围纯注释机检**

Run: `git diff -U0 -- 'src/*.h' 'src/*/*.h' | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: 空输出。

- [ ] **Step 4: 提交**

```bash
git add src/policy/circuit_breaker.h src/observe/event_bus.h docs/superpowers/specs/2026-09-28-enum-typedef-comments-design.md
git commit -m "docs: annotate enum members and cb_time_fn with Doxygen comments"
```

Expected: 2 头文件约 30 insertions、0 deletions（含 spec 实测修正）。

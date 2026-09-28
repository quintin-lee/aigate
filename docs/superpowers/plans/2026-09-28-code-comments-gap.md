# 代码注释查漏补缺 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐实测的 19 处注释缺口（18 个 static 全局量一行注释 + metrics_render 的 @brief），一次提交。

**Architecture:** 纯注释增补，不碰任何代码行。按模块分 4 个编辑 Task（core+main / observe / server / upstream），最后统一三验证后单次提交。

**Tech Stack:** C11, Doxygen `/** */` 中文注释，CMake + ctest。

**Spec:** `docs/superpowers/specs/2026-09-28-code-comments-gap-design.md`（已批准；本计划据逐行核对修正计数：18 非 20，因 g_lockout_fails/window/pool、g_rr_counter 已有 `/* */` 说明）。

---

### Task 1: core + main 全局量（3 处）

**Files:**
- Modify: `src/core/aigate_log.c:10`
- Modify: `src/core/secrets.c:37`
- Modify: `src/main.c:35`

- [ ] **Step 1: 加 3 行注释**

`src/core/aigate_log.c` 第 10 行上加：
```c
/** stderr 日志互斥锁（aigate_log 全路径持锁，防多线程交错）。 */
static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
```

`src/core/secrets.c` 第 37 行上加：
```c
/** hex 编解码查表（小写）。 */
static const char HEXD[] = "0123456789abcdef";
```

`src/main.c` 第 35 行上加：
```c
/** 停机标志：SIGINT/SIGTERM 处理器置 1，主循环退出走优雅停机。 */
static volatile sig_atomic_t g_stop = 0;
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff --stat && git diff -U0 -- src/core/aigate_log.c src/core/secrets.c src/main.c | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|$)'`
Expected: 无输出（新增行全是注释）。

---

### Task 2: observe 全局量 + metrics_render（5 处）

**Files:**
- Modify: `src/observe/metrics.c:23-28`
- Modify: `src/observe/metrics.h:21`

- [ ] **Step 1: 加 4 行全局量注释**

```c
/** 熔断指标表（model from_prov→to_prov 计数，g_failover_mtx 保护）。 */
static failover_metric_entry_t g_failovers[METRICS_MAX_FAILOVERS];
/** 熔断指标表互斥锁。 */
static pthread_mutex_t         g_failover_mtx = PTHREAD_MUTEX_INITIALIZER;
/** 表满丢弃警告是否已打过（仅 warn 一次，防日志刷屏）。 */
static _Atomic int             g_failover_warned = 0;
```
第 28 行 `BUCKET_LE` 上加：
```c
/** 延迟直方图桶上界（纳秒，字符串形式，与 exposition 输出一致）。 */
```

- [ ] **Step 2: metrics_render 补 @brief**

`src/observe/metrics.h` 第 21 行声明上加（现有指标列表注释保留，在其上方另起一块）：
```c
/** @brief 把 usage_meter 快照渲染为 Prometheus 文本 exposition。
 *  @param um   用量表（NULL 表无数据，仍输出静态指标头）。
 *  @param out  输出缓冲；@param cap 其容量。
 *  @return 写入字节数（不含 NUL）；缓冲不足返回 -1。 */
```

- [ ] **Step 3: 确认 diff 只有加行**

Run: `git diff -U0 -- src/observe/metrics.c src/observe/metrics.h | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|$)'`
Expected: 无输出。

---

### Task 3: server 全局量（3 处）

**Files:**
- Modify: `src/server/admin_api.c:92,101,102`

- [ ] **Step 1: 加 3 行注释**

```c
/** 分布式熔断 Lua 脚本 SHA（随 g_lockout_pool 初始化加载，空串表未加载）。 */
static char          g_lockout_sha[48] = { 0 };
```
```c
/** 本地管理口熔断计数槽（按 IP 分片，g_lockout_mtx 保护）。 */
static lockout_slot_t  g_lockout[LOCKOUT_SLOTS];
/** 本地熔断槽互斥锁。 */
static pthread_mutex_t g_lockout_mtx = PTHREAD_MUTEX_INITIALIZER;
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff -U0 -- src/server/admin_api.c | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|$)'`
Expected: 无输出。

---

### Task 4: upstream 全局量（9 处）

**Files:**
- Modify: `src/upstream/provider_adapter.c:12`
- Modify: `src/upstream/provider_openai.c:491`
- Modify: `src/upstream/upstream_client.c:15,16,17,46,61,62`

- [ ] **Step 1: 加 9 行注释**

```c
/** 供应商适配器注册表（NULL 结尾，按 supports(provider) 顺序匹配）。 */
static const provider_adapter_t* s_adapters[] = {
```
```c
/** Responses API 认证头线程本地缓存（"Bearer <key>"，避 per-request snprintf）。 */
static _Thread_local char s_responses_bearer_auth[2048];
```
```c
/** curl 共享句柄（DNS/SSL 会话跨 easy 句柄复用，g_curl_once 初始化）。 */
static CURLSH*         g_curl_sh = NULL;
/** curl 共享 DNS 缓存锁。 */
static pthread_mutex_t g_curl_sh_dns_mtx = PTHREAD_MUTEX_INITIALIZER;
/** curl 共享 SSL 会话锁。 */
static pthread_mutex_t g_curl_sh_ssl_mtx = PTHREAD_MUTEX_INITIALIZER;
```
```c
/** 进程级 curl 全局初始化 once 守卫。 */
static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
```
```c
/** 线程本地 easy 句柄 key（析构回收该线程复用句柄）。 */
static pthread_key_t  g_curl_tkey;
/** 线程 key 初始化 once 守卫。 */
static pthread_once_t g_curl_tkey_once = PTHREAD_ONCE_INIT;
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff -U0 -- src/upstream/ | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|$)'`
Expected: 无输出。

---

### Task 5: 三验证 + 提交

- [ ] **Step 1: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"`
Expected: 无输出。

- [ ] **Step 2: ctest 与基线一致**

Run: `ctest --test-dir build 2>&1 | tail -2`
Expected: `100% tests passed out of 6`。

- [ ] **Step 3: 全量纯注释机检**

Run: `git diff -U0 -- '*.c' '*.h' | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$)'`
Expected: 无输出。

- [ ] **Step 4: 提交**

```bash
git add src/core/aigate_log.c src/core/secrets.c src/main.c src/observe/metrics.c src/observe/metrics.h src/server/admin_api.c src/upstream/provider_adapter.c src/upstream/provider_openai.c src/upstream/upstream_client.c
git commit -m "docs: annotate globals and metrics_render"
```
Expected: `9 files changed, 19 insertions(+)`，0 deletions。

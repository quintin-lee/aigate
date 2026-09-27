# 代码注释完善 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 `src/` 约 230 个无文档函数补齐 Doxygen 注释（`.h` 约 60 + static 约 170），分 7 批独立提交。

**Architecture:** 纯加行注释，不动任何代码逻辑。公开函数注释落 `.h` 声明处，static 函数注释落 `.c` 定义处；已有文档的约 160 处一个字不碰。每批：补注释 → 纯注释机检 → 构建零警告 → ctest 6/6 → 独立提交。

**Tech Stack:** C17，Doxygen `/** @brief/@param/@return */` 约定，CMake + CTest。

---

## 全局约定（所有 Task 通用）

**注释模板**（@brief 动词开头、不复述函数名；@param 只写签名看不出的信息）：

```c
/** @brief 一句话：做什么。
 *  @param req  输入说明：含义 + 合法范围/特殊值（NULL 是否允许）。
 *  @param[out] out  输出说明：调用后里面是什么。
 *  @return 成功返回什么；失败返回什么、什么情况下失败。 */
```

**三铁律**：禁止复读式 @brief；纯入参用 `@param`，输出参数用 `@param[out]`；写注释前必须读一遍函数体，禁止编造行为。

**TDD 说明**：本计划无行为变更，没有“失败测试”步骤；编译器（`-Werror`）+ ctest + 纯注释机检即质量门。

**脏树 guard**：工作区现有未提交改动（`README.md`、`docs/DEVELOPMENT.md` 新建，`docs/README.md` 修改）与本计划无关。提交时只许 `git add` 本批路径，严禁 `git add -A` / `git commit -a`。

**每批通用验证**（Task 1–7 的验证步骤逐字执行，命令相同）：

```bash
cmake -S . -B build && cmake --build build
```
Expected: exit 0，零警告。

```bash
ctest --test-dir build
```
Expected: `100% tests passed, 0 tests failed out of 6`（与基线一致）。

```bash
git diff -U0 -- '*.c' '*.h' | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)'
```
Expected: 无输出（只允许 `+` 注释行 / `+` 空行）。

---

### Task 0: 基线审计

**Files:** 只读，不修改任何文件。

- [ ] **Step 1: 记录基线状态**

```bash
git status --short && git log --oneline -3
```
Expected: 看到 `M docs/README.md`、`?? README.md`、`?? docs/DEVELOPMENT.md`（他人未提交的文档活，勿碰）；HEAD 在 `7b64ccf`。

- [ ] **Step 2: 基线构建 + 测试**

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```
Expected: 构建 exit 0；`100% tests passed, 0 tests failed out of 6`。记下数字，后续 7 批必须一致。

- [ ] **Step 3: 审计待补数量**

```bash
git grep -c '^static' -- 'src/*/*.c' 'src/*.c' | awk -F: '{s+=$2} END {print "static defs:", s}'
```
Expected: 输出 static 定义总数（约 240，含多声明同行，作参考）。逐文件人工确认以 Task 1–7 的“待补”数为准。

---

### Task 1: common 批（4 文件，待补 8：全 static，`.h` 已有文档不动）

**Files:**
- Modify: `src/common/lru.c`（约 5：fnv1a、next_pow2、rec_unlink 等）
- Modify: `src/common/sha256.c`（约 3）
- Read-only: `src/common/lru.h`、`src/common/sha256.h`（已有文档，对照术语）

- [ ] **Step 1: 补 `src/common/lru.c` 等的 static 注释**

逐个读函数体后加注释。示范（`src/common/lru.c:36`，必须照此质量）：

Before:
```c
static size_t
fnv1a(const char* s, size_t cap)
```

After:
```c
/** @brief FNV-1a 哈希并对桶数取模。
 *  @param s    NUL 结尾的键，不许 NULL。
 *  @param cap  哈希表容量（> 0）。
 *  @return h(s) % cap，用作桶下标。 */
static size_t
fnv1a(const char* s, size_t cap)
```

- [ ] **Step 2: 跑通用验证**（见“每批通用验证”，三条命令逐字执行）
- [ ] **Step 3: 提交**（只加本批路径）

```bash
git add src/common && git commit -m "docs: annotate common with Doxygen comments"
```

---

### Task 2: core 批 + main.c（9 文件，待补 19：`.h` 1 + static 16 + main.c 2）

**Files:**
- Modify: `src/core/aigate_core.c`、`src/core/aigate_log.c`、`src/core/config.c`、`src/core/secrets.c`、`src/main.c`
- Modify: `src/core/aigate_core.h`（仅 1 处：`aigate_write_error` 声明，见下）
- Read-only: 其余 `.h`（对照术语）

- [ ] **Step 1: 补 static 注释**（读体后写；如 `mono_ns` 只写“单调时钟纳秒，供延迟计算/懒惰 refill”，不许编造时钟源细节）

- [ ] **Step 2: 补唯一的 `.h` 缺口**（`src/core/aigate_core.h:85`）：

Before:
```c
aigate_write_error(aigate_response_ctx* rc, int http_status, const char* type, const char* message);
```

After（先读 `src/core/aigate_core.c` 中对应定义体再定稿，示例方向）：
```c
/** @brief 写 JSON 错误响应（HTTP 状态 + {type,message} 体）。
 *  @param rc           响应上下文。
 *  @param http_status  HTTP 状态码。
 *  @param type         错误类型字符串。
 *  @param message      错误描述字符串。 */
aigate_write_error(aigate_response_ctx* rc, int http_status, const char* type, const char* message);
```

- [ ] **Step 3: 补 `src/main.c`**（`sig_handler` + `main`，说明信号处理与启动流程，一句话即可）
- [ ] **Step 4: 跑通用验证**
- [ ] **Step 5: 提交**

```bash
git add src/core src/main.c && git commit -m "docs: annotate core with Doxygen comments"
```

---

### Task 3: observe 批（6 文件，待补 9：`.h` 2 + static 7）

**Files:**
- Modify: `src/observe/event_bus.c`、`src/observe/event_bus.h`、`src/observe/health_prober.c`、`src/observe/health_prober.h`、`src/observe/metrics.c`
- Read-only: `src/observe/metrics.h`（已有文档）

- [ ] **Step 1: 补两个 `.h` 缺口**（先读对应 `.c` 体再定稿；`metrics.c` 的 static 全局量如 `g_failovers` 是“多声明同行”，是变量不是函数——跳过，只注函数）

`src/observe/health_prober.h:26` 方向：
```c
/** @brief 探针状态枚举转可读字符串。
 *  @param st  探针状态。
 *  @return 状态名字符串（静态存储，调用方勿释放）。 */
const char* health_status_str(health_status_t st);
```

`src/observe/event_bus.h:115` 方向：
```c
/** @brief 发布一次 ping 心跳事件（保活/自检用）。
 *  @param eb  事件总线。 */
void event_bus_publish_ping(event_bus_t* eb);
```

- [ ] **Step 2: 补 `metrics.c` 等的 static 函数注释**（跳过纯变量行）
- [ ] **Step 3: 跑通用验证**
- [ ] **Step 4: 提交**

```bash
git add src/observe && git commit -m "docs: annotate observe with Doxygen comments"
```

---

### Task 4: server 批（6 文件，待补 48：全 static，`.h` 已有文档不动）

**Files:**
- Modify: `src/server/transport_civetweb.c`（主力，约 40+：`http_reason`、`cw_set_header`、`cw_write` 等）、`src/server/admin_api.c`、`src/server/admin_ui.c`
- Read-only: 三个 `.h`（对照术语）

- [ ] **Step 1: 补 static 注释**（读体后写）。示范（`src/server/transport_civetweb.c:40`）：

Before:
```c
static const char* http_reason(int status)
```

After:
```c
/** @brief HTTP 状态码转原因短语。
 *  @param status  HTTP 状态码（如 200）。
 *  @return 原因短语静态字符串；未知码返回占位串（读函数体确认具体值）。 */
static const char* http_reason(int status)
```

- [ ] **Step 2: 跑通用验证**
- [ ] **Step 3: 提交**

```bash
git add src/server && git commit -m "docs: annotate server with Doxygen comments"
```

---

### Task 5: policy 批（12 文件，待补 31：`.h` 11 + static 20）

**Files:**
- Modify: `src/policy/guardrails.c`、`src/policy/guardrails.h`（主力：`ac_trie_*` 系列）、`src/policy/auth_key.c`、`src/policy/ratelimit.c`、`src/policy/budget_enforce.c`、`src/policy/circuit_breaker.c`、`src/policy/response_cache.c` 及对应 `.h` 缺口处

- [ ] **Step 1: 补 `.h` 缺口**（如 `guardrails.h:33-35` 的 `ac_trie_create/destroy/insert`；先读 `.c` 体确认 trie 语义再写）方向：

```c
/** @brief 新建 AC 自动机（关键词匹配用）。
 *  @return 新实例，OOM 返回 NULL（读函数体确认）。 */
ac_trie_t*  ac_trie_create(void);
```

- [ ] **Step 2: 补 static 注释**（如 `auth_key.c:13 free_rec_cb`、`ratelimit.c:39 mono_ns`、`ratelimit.c:47 utc_midnight`；`utc_midnight` 必须写清“取当日 UTC 零点”的语义）
- [ ] **Step 3: 跑通用验证**
- [ ] **Step 4: 提交**

```bash
git add src/policy && git commit -m "docs: annotate policy with Doxygen comments"
```

---

### Task 6: store 批（10 文件，待补 54：`.h` 12 + static 42）

**Files:**
- Modify: `src/store/pg_store.c`、`src/store/pg_store.h`（主力）、`src/store/redis_client.c`、`src/store/redis_pool.c`、`src/store/usage_meter.c` 及对应 `.h`
- Read-only: `src/store/redis_scripts.h`、`src/store/schema_sql.h`（无函数，按需对照）

- [ ] **Step 1: 补 `.h` 缺口**（`pg_store.h:239-242` 四个 guardrails 规则包装函数；已有 `pg_ops` 表级文档，对照其返回码约定“0 成功 / -1 错误”）。示范：

Before:
```c
int pg_store_list_guardrails_rules(const pg_store_t* ps, guardrail_rule_t* out, int cap, int* n);
```

After:
```c
/** @brief 列出 guardrails 规则。
 *  @param ps   存储句柄。
 *  @param out  调用方提供的数组，容量 ≥ @p cap。
 *  @param cap  @p out 最大容纳条数。
 *  @param n    接收实际写入条数。
 *  @return 0 成功；-1 存储错误。 */
int pg_store_list_guardrails_rules(const pg_store_t* ps, guardrail_rule_t* out, int cap, int* n);
```

- [ ] **Step 2: 补 static 注释**（如 `join_provider_models`、`copy_field`、`pq_lock`；`pq_lock` 注明“取 libpq 上下文内部锁”）
- [ ] **Step 3: 跑通用验证**
- [ ] **Step 4: 提交**

```bash
git add src/store && git commit -m "docs: annotate store with Doxygen comments"
```

---

### Task 7: upstream 批（12 文件，待补 59：`.h` 7 + static 52，另补 1 个文件头）

**Files:**
- Modify: `src/upstream/provider_anthropic.c`、`src/upstream/provider_gemini.c`、`src/upstream/provider_openai.c`、`src/upstream/provider_adapter.c`、`src/upstream/model_router.c`、`src/upstream/upstream_client.c`（主力）及对应 `.h` 缺口处

- [ ] **Step 1: 补 `src/upstream/upstream_client.c` 缺失的文件头**（全仓唯一；先读 `src/upstream/upstream_client.h` 的 `@file/@brief` 再镜像措辞）方向：

```c
/** @file upstream_client.c
 *  @brief 共享 curl 句柄与上游 HTTP 客户端实现（见 upstream_client.h）。 */
```

- [ ] **Step 2: 补 `.h` 缺口**（如 `provider_anthropic.h:92-94` 的 sniffer 三件套；先读 `.c` 体确认 SSE 解析语义）。示范：

Before:
```c
void anthropic_sniffer_init(anthropic_sniffer_t* s);
```

After:
```c
/** @brief 初始化 Anthropic SSE 嗅探器状态（清零计数，为分块投喂做准备）。
 *  @param s  嗅探器状态（调用方分配）。 */
void anthropic_sniffer_init(anthropic_sniffer_t* s);
```

- [ ] **Step 3: 补 static 注释**（`upstream_client.c` 的 `curl_sh_lock` 等 static 全局量是变量——跳过，只注函数；`provider_adapter.h:120 provider_probe_plan` 按 `.h` 缺口处理）
- [ ] **Step 4: 跑通用验证**
- [ ] **Step 5: 提交**

```bash
git add src/upstream && git commit -m "docs: annotate upstream with Doxygen comments"
```

---

## Self-Review

**1. Spec coverage:** §4 模板+铁律 → 全局约定；§5 七批表 → Task 1–7（文件清单与待补数一致，upstream 文件头 → Task 7 Step 1）；§6 验证 → 通用验证三命令；§7 不动清单 → 脏树 guard + “已有文档不动” + schema/redis_scripts 只读。覆盖完全。

**2. Placeholder scan:** 无 TBD/TODO/“类似 Task N”式转述——每批 worked example 均为实测真实签名；`（读函数体确认）` 标注的是执行动作（worker 必做），非占位。

**3. Type consistency:** `@param[out]` 用法全 plan 一致；`git add` 路径均为 `src/<dir>`（Task 2 另加 `src/main.c`）；提交信息统一 `docs: annotate <dir> with Doxygen comments`。

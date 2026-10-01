# 自适应动态延迟路由与对冲请求（Hedged Requests）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 AIGate 构建 P95 / EWMA 动态延迟感知追踪器、自适应自愈路由策略（`latency_p95` 与 `dynamic_weighted`），以及协作型长尾对冲并发竞速引擎（Hedged Requests，非流式与流式 TTFT 竞速），并在管理 API、Web 控制台和监控指标中完成端到端落地。

**Architecture:**
- **延迟追踪器 (`latency_tracker`):** 环形滑动窗口（64 个采样点）+ EWMA 平滑延迟（$\alpha = 0.2$）+ QuickSelect 快速 P95 实时计算 + Hedge Budget 配额保护（默认 ≤15%）。
- **自适应路由 (`model_router`):** 增加 `latency_p95`（根据实时 P95 排序置顶）与 `dynamic_weighted`（延迟倒数动态分配权重），实现自动化慢节点规避与自愈。
- **协作对冲引擎 (`upstream_hedged`):** 首发请求超 P95 延迟（或配置的 `hedged_delay_ms`）未返回且未超预算时，派生次级 Worker 请求备用节点。先到先得（Winner Takes All），败者立即优雅断开（CURL cancel callback）。流式场景以 TTFT（首字节）为竞速裁决点。
- **数据库与管理端:** Migration v12 在 `models` 表增加 `hedged_delay_ms`, `hedge_budget_pct`, `hedged_enabled`；Web 控制台增加策略切换、对冲开关与预算设置。

**Tech Stack:** C17, libcurl, pthreads, Jansson, PostgreSQL / libpq, Tailwind CSS, Python 3, Pytest.

---

## 文件影响范围映射

- **数据访问与存储层：**
  - `src/store/schema_sql.h`: 增加 Migration v12 SQL。
  - `src/store/pg_store.h`: 扩展 `model_rec_t` 结构体字段。
  - `src/store/pg_store.c`: 更新 `pq_get_model`, `pq_list_models`, `pq_create_model`, `pq_update_model`。
- **延迟感知与策略层：**
  - `src/policy/latency_tracker.h`, `src/policy/latency_tracker.c`: 实现采样环形缓冲区、EWMA、P95 计算与配额校验。
  - `src/upstream/model_router.h`, `src/upstream/model_router.c`: 引入 `latency_tracker`，实现 `latency_p95` 与 `dynamic_weighted`。
- **对冲引擎与核心链路：**
  - `src/upstream/upstream_hedged.h`, `src/upstream/upstream_hedged.c`: 实现非流式与流式（TTFT 裁决）双通道协作对冲引擎。
  - `src/core/pipeline_chat.c`: 将同步与流式执行接入对冲引擎。
  - `src/observe/metrics.h`, `src/observe/metrics.c`: 增加对冲与获胜 Prometheus 计数器。
- **管理端与控制台：**
  - `src/server/admin_api.c`: 支持对冲相关字段解析与校验。
  - `web/admin.html`: 增加负载均衡策略选择、对冲开关与延迟/配额输入项。
- **测试套件：**
  - `tests/unit/policy/test_latency_tracker.c`: 单元测试延迟追踪。
  - `tests/unit/upstream/test_model_router_adaptive.c`: 单元测试自适应路由排序。
  - `tests/unit/upstream/test_upstream_hedged.c`: 单元测试对冲状态机。
  - `tests/integration/test_adaptive_hedging.py`: 端到端 Python 集成测试。

---

## 任务拆分列表

### Task 1: 数据库迁移与数据访问层扩展 (Migration v12)

**Files:**
- Modify: `src/store/schema_sql.h`
- Modify: `src/store/pg_store.h`
- Modify: `src/store/pg_store.c`

- [ ] **Step 1: 在 `schema_sql.h` 中编写 Migration v12**

在 `src/store/schema_sql.h` 末尾增加：
```sql
-- Migration v12: adaptive latency routing & hedged requests support
ALTER TABLE models
  ADD COLUMN IF NOT EXISTS hedged_delay_ms INT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS hedge_budget_pct INT NOT NULL DEFAULT 15,
  ADD COLUMN IF NOT EXISTS hedged_enabled BOOLEAN NOT NULL DEFAULT FALSE;

INSERT INTO schema_migrations(version) VALUES (12) ON CONFLICT (version) DO NOTHING;
```

- [ ] **Step 2: 在 `pg_store.h` 中扩展 `model_rec_t` 结构体**

在 `src/store/pg_store.h` 的 `typedef struct model_rec` 中增加：
```c
    int     hedged_delay_ms;    /**< 0 for auto-P95, >0 for static ms delay */
    int     hedge_budget_pct;   /**< Max % of requests that can trigger hedge (default: 15) */
    bool    hedged_enabled;     /**< True if hedged speculative execution is active */
```

- [ ] **Step 3: 更新 `pg_store.c` 模型查询、创建与更新逻辑**

在 `src/store/pg_store.c` 中：
1. `pq_get_model` 与 `pq_list_models`: SQL 查询增加 `COALESCE(hedged_delay_ms, 0), COALESCE(hedge_budget_pct, 15), COALESCE(hedged_enabled, false)` 并映射到 `out->hedged_delay_ms`, `out->hedge_budget_pct`, `out->hedged_enabled`。
2. `pq_create_model` 与 `pq_update_model`: 增加对应参数绑定与更新。

- [ ] **Step 4: 编译并验证全量单元测试**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 编译 0 警告，所有单元测试通过。

- [ ] **Step 5: Commit Task 1**

```bash
git add src/store/schema_sql.h src/store/pg_store.h src/store/pg_store.c
git commit -m "feat(store): 🗄️ add migration v12 for adaptive routing and hedged requests"
```

---

### Task 2: P95 / EWMA 动态延迟追踪器引擎与单元测试

**Files:**
- Create: `src/policy/latency_tracker.h`
- Create: `src/policy/latency_tracker.c`
- Create: `tests/unit/policy/test_latency_tracker.c`
- Modify: `tests/unit/run_tests.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 编写 `src/policy/latency_tracker.h` 接口**

```c
#ifndef AIGATE_LATENCY_TRACKER_H
#define AIGATE_LATENCY_TRACKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LATENCY_TRACKER_WINDOW_SZ 64
#define LATENCY_TRACKER_MAX_ENTRIES 512

typedef struct latency_tracker latency_tracker_t;

latency_tracker_t* latency_tracker_create(void);
void latency_tracker_destroy(latency_tracker_t* lt);

void latency_tracker_record(latency_tracker_t* lt,
                            const char*        model,
                            const char*        endpoint,
                            uint64_t           latency_ns);

uint32_t latency_tracker_get_p95_ms(latency_tracker_t* lt,
                                    const char*        model,
                                    const char*        endpoint);

uint32_t latency_tracker_get_ewma_ms(latency_tracker_t* lt,
                                     const char*        model,
                                     const char*        endpoint);

bool latency_tracker_hedge_admitted(latency_tracker_t* lt,
                                    const char*        model,
                                    int                budget_pct);

void latency_tracker_record_hedge(latency_tracker_t* lt, const char* model);

void latency_tracker_reset(latency_tracker_t* lt);

#endif /* AIGATE_LATENCY_TRACKER_H */
```

- [ ] **Step 2: 编写失败的单元测试 `tests/unit/policy/test_latency_tracker.c`**

测试涵盖：
1. 记录 100 个样本并验证 P95 分位计算准确度（例如输入递增延迟序列 10ms..100ms，验证 P95 约等于 95ms）。
2. EWMA 平滑指数衰减正确性。
3. Hedge Budget 配额放行与超出配额阻断校验（当请求数=10，对冲数=2 时，超过 15% 预算返回 false）。
4. 多线程并发读写安全。

- [ ] **Step 3: 运行测试验证失败**

Run: `cmake --build .build -j$(nproc)`  
Expected: 编译报未定义符号错误。

- [ ] **Step 4: 实现 `src/policy/latency_tracker.c`**

实现环形队列写入、P95 快速选择排序、EWMA 平滑计算、读写锁保护与配额统计。

- [ ] **Step 5: 运行单测验证全量通过**

Run: `./.build/tests/aigate_unit_tests`  
Expected: `test_latency_tracker` 全项 PASSED。

- [ ] **Step 6: Commit Task 2**

```bash
git add src/policy/latency_tracker.h src/policy/latency_tracker.c tests/unit/policy/test_latency_tracker.c tests/unit/run_tests.c CMakeLists.txt
git commit -m "feat(policy): ⏱️ implement P95 and EWMA latency tracker with hedge budget"
```

---

### Task 3: 自适应路由策略与候选通道动态排序

**Files:**
- Modify: `src/upstream/model_router.h`
- Modify: `src/upstream/model_router.c`
- Create: `tests/unit/upstream/test_model_router_adaptive.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写单元测试 `tests/unit/upstream/test_model_router_adaptive.c`**

1. 测试 `latency_p95`：配置两个目标 A（模拟 P95=500ms）和 B（模拟 P95=50ms），断言选择出的首选候选通道为 B，备选通道为 A。
2. 测试 `dynamic_weighted`：配置 A（EWMA=20ms）与 B（EWMA=200ms），验证多次轮询分配下，低延迟节点命中比例显著高于高延迟节点。
3. 测试当没有延迟数据时（冷启动）安全回退。

- [ ] **Step 2: 扩展 `model_router.h` 与 `model_router.c`**

1. 修改 `model_router_select_candidates` 签名，接收 `latency_tracker_t* lt`。
2. 在同一优先级层级中：
   - 若 `strcmp(model->lb_policy, "latency_p95") == 0`，提取各 healthy 目标的 P95 并按升序排序候选。
   - 若 `strcmp(model->lb_policy, "dynamic_weighted") == 0`，根据 $W_i = \max(1, 1000 / (EWMA_i + 10))$ 计算动态权重轮询。
3. 全局更新既有调用处，传递 `lt`（可为 NULL 保留原始行为）。

- [ ] **Step 3: 运行单测验证全量通过**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: `test_model_router_adaptive` 全项 PASSED，无回归。

- [ ] **Step 4: Commit Task 3**

```bash
git add src/upstream/model_router.h src/upstream/model_router.c tests/unit/upstream/test_model_router_adaptive.c tests/unit/run_tests.c
git commit -m "feat(router): 🧭 implement adaptive latency-aware routing policies"
```

---

### Task 4: 协作型对冲请求执行引擎 (`upstream_hedged`)

**Files:**
- Create: `src/upstream/upstream_hedged.h`
- Create: `src/upstream/upstream_hedged.c`
- Create: `tests/unit/upstream/test_upstream_hedged.c`
- Modify: `tests/unit/run_tests.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 编写 `upstream_hedged.h` 接口与状态机定义**

```c
#ifndef AIGATE_UPSTREAM_HEDGED_H
#define AIGATE_UPSTREAM_HEDGED_H

#include <stdbool.h>
#include <stdint.h>
#include "pg_store.h"
#include "latency_tracker.h"

typedef struct hedged_call_params {
    const char*        model;
    upstream_target_t  primary;
    upstream_target_t  secondary;
    bool               has_secondary;
    const char*        payload;
    size_t             payload_len;
    int                timeout_ms;
    int                delay_ms;          /**< Waiting window before hedging */
    latency_tracker_t* lt;
} hedged_call_params_t;

typedef struct hedged_call_result {
    int    status;
    char*  body;
    size_t body_len;
    int    winning_target_idx;            /**< 0: primary, 1: secondary */
    bool   was_hedged;                    /**< True if hedge was actually fired */
    uint64_t latency_ns;
} hedged_call_result_t;

int upstream_call_hedged(const hedged_call_params_t* params,
                         hedged_call_result_t*       out_result);

#endif /* AIGATE_UPSTREAM_HEDGED_H */
```

- [ ] **Step 2: 编写单元测试 `tests/unit/upstream/test_upstream_hedged.c`**

1. 测试快速 Primary 情况下，不触发对冲，直接返回 0 Redundant Requests。
2. 测试 Primary 超时未返回触发 Secondary，Secondary 快速返回 200，断言 `winning_target_idx == 1` 且 `was_hedged == true`。
3. 测试 Primary 与 Secondary 竞争取消逻辑。

- [ ] **Step 3: 实现 `src/upstream/upstream_hedged.c`**

1. 实现主工作线程与条件变量超时等待。
2. 当 $D$ 毫秒到期且满足配额时，派生次级线程向 `params->secondary` 发起请求。
3. 实现 curl 取消回调函数（`CURLOPT_XFERINFOFUNCTION`），当检测到被取消时返回非 0 终止。
4. 使用原子操作（CAS）保证首个返回 HTTP 200 的线程胜出，并触发胜出/败者资源回收。

- [ ] **Step 4: 编译并运行单元测试**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 单元测试全量通过。

- [ ] **Step 5: Commit Task 4**

```bash
git add src/upstream/upstream_hedged.h src/upstream/upstream_hedged.c tests/unit/upstream/test_upstream_hedged.c tests/unit/run_tests.c CMakeLists.txt
git commit -m "feat(upstream): ⚡ implement hedged concurrent request execution engine"
```

---

### Task 5: 核心流水线集成（Chat 同步与流式 TTFT 竞速集成）与监控指标

**Files:**
- Modify: `src/core/aigate_core.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `src/observe/metrics.h`
- Modify: `src/observe/metrics.c`

- [ ] **Step 1: 在 `aigate_ctx_t` 中持有 `latency_tracker` 并在启动/停止时初始化与释放**

在 `aigate_core.c` 的 `aigate_init` 中调用 `latency_tracker_create()`，在 `aigate_stop` 中释放。

- [ ] **Step 2: 在 `src/observe/metrics.c` 中增加对冲指标**

增加指标：
- `aigate_hedged_requests_total`
- `aigate_hedged_won_total`

- [ ] **Step 3: 在 `src/core/pipeline_chat.c` 中接入对冲调用与延迟记录**

1. 在 `handle_chat_sync` 中：
   - 若 `q->route.hedged_enabled && q->n_candidates >= 2`：
     - 构建 `hedged_call_params_t`，调用 `upstream_call_hedged`。
     - 将耗时记录入 `latency_tracker_record`。
     - 若 `was_hedged`，增加对应 metrics 计数。
2. 在流式流程中：
   - 提取 TTFT 并在首个 chunk 返回时通过 `latency_tracker_record` 记录首字节延迟。

- [ ] **Step 4: 编译并验证全量现有测试**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 编译 0 警告，单测 100% 通过。

- [ ] **Step 5: Commit Task 5**

```bash
git add src/core/aigate_core.h src/core/aigate_core.c src/core/pipeline_chat.c src/observe/metrics.h src/observe/metrics.c
git commit -m "feat(core): 🔗 wire hedged execution engine and latency tracking into core pipeline"
```

---

### Task 6: 管理端 API 与 Web 控制台可视化管理

**Files:**
- Modify: `src/server/admin_api.c`
- Modify: `web/admin.html`

- [ ] **Step 1: 更新 `admin_api.c` 支持模型对冲参数**

1. 在 `/admin/v1/models` 的 GET 响应中序列化 `hedged_delay_ms`, `hedge_budget_pct`, `hedged_enabled`。
2. 在 POST/PUT 模型接口中解析并校验这些字段：
   - `hedged_delay_ms >= 0`
   - `hedge_budget_pct` 范围为 `0..100`
   - `hedged_enabled` 为布尔值。

- [ ] **Step 2: 更新 `web/admin.html` 模型管理表单与徽章渲染**

1. 在模型创建/编辑弹窗中增加：
   - 负载均衡策略下拉框选项：`⚡ latency_p95 (自适应 P95 优先)` 与 `⚖️ dynamic_weighted (动态延迟加权)`。
   - 对冲请求开关（`Hedged Requests Enabled`）。
   - 对冲延迟（`hedged_delay_ms`，0 为自动自适应）。
   - 对冲预算上限（`hedge_budget_pct`，默认 15%）。
2. 在表格行中渲染对冲策略高对比徽章（如 `Hedged (P95)`）。

- [ ] **Step 3: 重新生成头文件并验证构建**

Run: `cmake --build .build -j$(nproc)`  
Expected: 成功构建 `aigate`，嵌入式 HTML 正常更新。

- [ ] **Step 4: Commit Task 6**

```bash
git add src/server/admin_api.c web/admin.html
git commit -m "feat(ui): 🎛️ add adaptive routing and hedged requests configuration to admin console"
```

---

### Task 7: 端到端自动化集成测试套件

**Files:**
- Create: `tests/integration/test_adaptive_hedging.py`

- [ ] **Step 1: 编写端到端集成测试脚本**

在 `tests/integration/test_adaptive_hedging.py` 中：
1. 启动轻量多端点 Mock 服务（端点 A 模拟延迟 1500ms，端点 B 快速返回 50ms）。
2. 配置模型：包含目标 A 与目标 B，开启 `hedged_enabled = true`，`hedged_delay_ms = 100`。
3. **Case 1: 验证对冲降尾延迟**：发出 Chat 请求，断言总耗时显著小于 1500ms（约在 150~250ms 内完成），且响应成功由端点 B 提供。
4. **Case 2: 验证快路径 0 冗余**：当端点 A 快速返回时，验证端点 B 未收到任何多余请求。
5. **Case 3: 验证流式 TTFT 竞速**：发出流式请求，验证首包在备选端点快速到达时顺利流式输出。
6. **Case 4: 验证 Hedge Budget 保护**：突发连续触发对冲时，对冲比例达到上限后自动降级为普通单发。

- [ ] **Step 2: 运行集成测试**

Run: `pytest tests/integration/test_adaptive_hedging.py -v`  
Expected: 全部测试用例 PASSED。

- [ ] **Step 3: 运行全量回归测试**

Run: `./.build/tests/aigate_unit_tests`  
Run: `pytest tests/integration/test_gateway.py -k "guardrails or failover" -v`  
Expected: 100% 测试通过，0 回归。

- [ ] **Step 4: Commit Task 7**

```bash
git add tests/integration/test_adaptive_hedging.py
git commit -m "test(routing): 🧪 add end-to-end integration tests for adaptive routing and hedged requests"
```

---

## 验证与验收标准

1. **编译构建**：`cmake --build .build -j$(nproc)` 0 警告 0 报错，内嵌 Web UI 自动同步生成。
2. **单元测试**：所有现有 202 个单测 + 新增的 `test_latency_tracker`, `test_model_router_adaptive`, `test_upstream_hedged` 全部通过。
3. **集成测试**：`pytest tests/integration/test_adaptive_hedging.py` 覆盖快路径、对冲降尾、流式 TTFT 与 Hedge Budget。
4. **长尾收益**：Mock 慢请求下网关延迟由 >1.5s 显著下降至 <250ms。

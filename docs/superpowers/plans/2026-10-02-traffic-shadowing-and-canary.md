# 流量镜像与金丝雀灰度分流评估引擎 (Traffic Shadowing & Canary A/B Testing Engine) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建纯 C17 高性能流量镜像与金丝雀灰度分流评估引擎，支持毫秒级异步零风险流量复制（< 10μs 开销）、真实生产流量比例灰度切流、多维模型回答 Side-by-Side 左右双栏对比与时延/成本效益评估看板。

**Architecture:**
- **双模式调度 (Dual Mode)**:
  - **SHADOW 模式**: 主请求在网关前置解析后，以零阻塞有界环形队列克隆请求（容量 256 槽位，满载立即丢弃记数），由独立后台 Worker 异步调用目标影子模型，记录耗时、TTFT、Token 与预估成本，客户端 100% 隔离不受影响；
  - **CANARY 模式**: 生产流量按设定的百分比切流至候选模型，响应头附带 `x-aigate-canary: true`，结合熔断器遇连续 3 次 5xx 自动回滚切回主模型；
- **双路对齐与归并引擎 (Pair Merger)**:
  - 主请求与影子请求通过唯一的 `eval_id` 与 W3C `trace_id` 关联，无论主模型还是影子模型先完成，均在轻量级并发配对槽位中聚合为 `shadow_eval_item_t`；
  - 内存极速有界循环队列（容量 200 槽位）保留最近详细双路对比快照（Prompt 摘要、主模型 vs 影子模型响应文本），用于 Web 控制台实时渲染 Side-by-Side 列表；
- **持久化与管理 API**:
  - PostgreSQL 存储 `shadow_rules` 与聚合指标，提供完整的 `/admin/v1/shadow/*` RESTful 管理端点；
  - Web 控制台提供 4 大效益 KPI 卡片、规则配置面板、双路对齐数据流表格与左右双栏文本对比模态框。

**Tech Stack:** C17 (GCC/Clang), POSIX Threads & Clocks, Libcurl / Upstream Client, Jansson JSON, PostgreSQL / Fake DB, CivetWeb, HTML5 / Tailwind CSS / Vanilla JS.

---

## File Structure

- **New Files:**
  - `src/policy/shadow.h`: 流量镜像与金丝雀规则结构、评测条目、有界队列、环形结果缓存及引擎生命周期接口声明。
  - `src/policy/shadow.c`: 异步任务队列、双路对齐合并器、后台影子 Worker 线程池、HTTP 客户端执行与指标计算、内存环形快照池实现。
  - `tests/unit/policy/test_shadow.c`: 单元测试套件，覆盖规则匹配、采样率判定、队列无阻塞推拉丢弃、双路对齐合并、环形缓存溢出回滚及自愈熔断逻辑。
- **Modified Files:**
  - `CMakeLists.txt`: 将 `src/policy/shadow.c` 加入 `libaigate` 源码列表，将 `tests/unit/policy/test_shadow.c` 加入测试构建列表。
  - `src/store/pg_store.h` & `src/store/pg_store.c`: 新增 `shadow_rules` 数据结构、表结构定义与 CRUD 存储接口。
  - `src/upstream/model_router.h` & `src/upstream/model_router.c`: 增加金丝雀规则判定接口 `model_router_apply_canary`，集成 3 次 5xx 连续异常自动熔断回滚逻辑。
  - `src/core/aigate_core_internal.h`: 在 `aigate_ctx` 中加入 `shadow_engine_t* shadow_eng`，在 `chat_req_t` 中加入影子评测与金丝雀追踪上下文。
  - `src/core/aigate_core.h` & `src/core/aigate_core.c`: 在网关核心初始化与销毁流程中接入 `shadow_engine`，在请求入口与出口挂接分流与配对钩子。
  - `src/core/pipeline_chat.c`: 在请求入站时执行零拷贝克隆入队，在同步/流式响应结束时投递主请求性能与消耗快照至配对引擎。
  - `src/server/admin_api.h` & `src/server/admin_api.c`: 新增 `/admin/v1/shadow/rules`、`/admin/v1/shadow/evaluations`、`/admin/v1/shadow/stats` REST 端点。
  - `tests/unit/server/test_admin_api.c`: 覆盖规则 CRUD、快照查询与全局 KPI 统计端点。
  - `tests/unit/run_tests.c`: 注册 `test_shadow` 单元测试。
  - `web/admin.html`: 新增「🧪 流量镜像与金丝雀」侧边栏、4 大效益 KPI 卡片、规则配置抽屉、Side-by-Side 详细对比与文本 Diff 模态框。

---

### Task 1: Shadow Core Data Structures, Task Queue & Evaluation Cache

**Files:**
- Create: `src/policy/shadow.h`
- Create: `src/policy/shadow.c`
- Modify: `CMakeLists.txt`
- Create: `tests/unit/policy/test_shadow.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write unit tests for rule matching, queue operations, and evaluation ring buffer**

Create `tests/unit/policy/test_shadow.c`:
```c
#include "test_framework.h"
#include "policy/shadow.h"
#include <string.h>

TEST_CASE(test_shadow_rule_matching_and_sampling)
{
    shadow_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.id = 1;
    strncpy(rule.source_model, "gpt-4o", sizeof(rule.source_model) - 1);
    strncpy(rule.target_model, "deepseek-chat", sizeof(rule.target_model) - 1);
    rule.mode = TRAFFIC_MODE_SHADOW;
    rule.sample_rate = 1.0;
    rule.enabled = true;
    strncpy(rule.header_match, "x-env: test", sizeof(rule.header_match) - 1);

    /* Matching source model and header */
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "x-env: test") == true, "rule matches model and header");
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-3.5-turbo", "x-env: test") == false, "mismatched model fails");
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "x-env: prod") == false, "mismatched header fails");

    /* Disabled rule */
    rule.enabled = false;
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "x-env: test") == false, "disabled rule fails");
}

TEST_CASE(test_shadow_queue_push_pop_overflow)
{
    shadow_queue_t* q = shadow_queue_create(4);
    TEST_ASSERT(q != NULL, "queue created");

    shadow_task_t t1;
    memset(&t1, 0, sizeof(t1));
    strncpy(t1.eval_id, "eval-001", sizeof(t1.eval_id) - 1);

    TEST_ASSERT(shadow_queue_push(q, &t1) == true, "push 1 ok");
    TEST_ASSERT(shadow_queue_count(q) == 1, "count is 1");

    /* Fill to capacity */
    for (int i = 2; i <= 4; i++) {
        shadow_task_t t;
        memset(&t, 0, sizeof(t));
        snprintf(t.eval_id, sizeof(t.eval_id), "eval-00%d", i);
        TEST_ASSERT(shadow_queue_push(q, &t) == true, "push ok");
    }
    TEST_ASSERT(shadow_queue_count(q) == 4, "queue full at 4");

    /* Push when full: non-blocking drop */
    shadow_task_t extra;
    memset(&extra, 0, sizeof(extra));
    strncpy(extra.eval_id, "eval-overflow", sizeof(extra.eval_id) - 1);
    TEST_ASSERT(shadow_queue_push(q, &extra) == false, "push on full queue returns false");
    TEST_ASSERT(shadow_queue_dropped(q) == 1, "dropped counter incremented");

    /* Pop FIFO */
    shadow_task_t popped;
    TEST_ASSERT(shadow_queue_pop(q, &popped, 100) == true, "pop succeeds");
    TEST_ASSERT(strcmp(popped.eval_id, "eval-001") == 0, "FIFO ordering verified");
    TEST_ASSERT(shadow_queue_count(q) == 3, "count decremented to 3");

    shadow_queue_destroy(q);
}

TEST_CASE(test_shadow_eval_cache_circular_and_stats)
{
    shadow_eval_cache_t* cache = shadow_eval_cache_create(5);
    TEST_ASSERT(cache != NULL, "cache created");

    for (int i = 1; i <= 7; i++) {
        shadow_eval_item_t item;
        memset(&item, 0, sizeof(item));
        snprintf(item.eval_id, sizeof(item.eval_id), "eval-%d", i);
        item.primary_latency_ms = 100.0;
        item.shadow_latency_ms = 40.0;
        item.primary_cost_usd = 0.01;
        item.shadow_cost_usd = 0.002;
        item.shadow_http_status = 200;
        shadow_eval_cache_record(cache, &item);
    }

    TEST_ASSERT(shadow_eval_cache_count(cache) == 5, "capped at capacity 5");

    shadow_eval_item_t list[5];
    int n = shadow_eval_cache_get_recent(cache, list, 5);
    TEST_ASSERT(n == 5, "retrieved 5 items");
    /* Most recent first */
    TEST_ASSERT(strcmp(list[0].eval_id, "eval-7") == 0, "most recent item is eval-7");
    TEST_ASSERT(strcmp(list[4].eval_id, "eval-3") == 0, "oldest remaining is eval-3");

    shadow_stats_t stats;
    shadow_eval_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.total_evaluated == 7, "total evaluated is 7");
    TEST_ASSERT(stats.successful_shadow == 7, "all 7 successful");
    TEST_ASSERT(stats.cost_saved_usd > 0.05, "cost saved recorded correctly");

    shadow_eval_cache_destroy(cache);
}
```

Register `test_shadow_rule_matching_and_sampling`, `test_shadow_queue_push_pop_overflow`, and `test_shadow_eval_cache_circular_and_stats` in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because `policy/shadow.h` does not exist yet.

- [ ] **Step 3: Implement data structures, queue, and evaluation circular cache**

Create `src/policy/shadow.h`:
- Define `traffic_mode_t`: `TRAFFIC_MODE_SHADOW = 0`, `TRAFFIC_MODE_CANARY = 1`.
- Define `shadow_rule_t`:
  - `id`, `source_model[64]`, `target_model[64]`, `target_provider[32]`, `mode`, `sample_rate`, `header_match[64]`, `enabled`, `timeout_ms`.
- Define `shadow_eval_item_t`:
  - `eval_id[33]`, `trace_id[33]`, `source_model[64]`, `target_model[64]`, `mode`.
  - `primary_latency_ms`, `shadow_latency_ms`, `primary_ttft_ms`, `shadow_ttft_ms`.
  - `primary_http_status`, `shadow_http_status`, `primary_tokens`, `shadow_tokens`.
  - `primary_cost_usd`, `shadow_cost_usd`.
  - `prompt_preview[256]`, `primary_resp_snippet[512]`, `shadow_resp_snippet[512]`.
  - `timestamp_us`.
- Define `shadow_task_t`:
  - Request cloning metadata and payload buffer.
- Define `shadow_stats_t`:
  - `total_evaluated`, `successful_shadow`, `failed_shadow`, `cost_saved_usd`, `primary_cost_usd`, `shadow_cost_usd`, `avg_primary_lat_ms`, `avg_shadow_lat_ms`.
- Declare queue and cache APIs with full Doxygen comments:
  - `bool shadow_rule_matches(const shadow_rule_t* rule, const char* model, const char* header_str);`
  - `bool shadow_rule_should_sample(const shadow_rule_t* rule);`
  - `shadow_queue_t* shadow_queue_create(size_t capacity);`
  - `void shadow_queue_destroy(shadow_queue_t* q);`
  - `bool shadow_queue_push(shadow_queue_t* q, const shadow_task_t* task);`
  - `bool shadow_queue_pop(shadow_queue_t* q, shadow_task_t* out_task, uint32_t timeout_ms);`
  - `size_t shadow_queue_count(shadow_queue_t* q);`
  - `uint64_t shadow_queue_dropped(shadow_queue_t* q);`
  - `shadow_eval_cache_t* shadow_eval_cache_create(size_t capacity);`
  - `void shadow_eval_cache_destroy(shadow_eval_cache_t* c);`
  - `void shadow_eval_cache_record(shadow_eval_cache_t* c, const shadow_eval_item_t* item);`
  - `int shadow_eval_cache_get_recent(shadow_eval_cache_t* c, shadow_eval_item_t* out_items, int max_items);`
  - `void shadow_eval_cache_get_stats(shadow_eval_cache_t* c, shadow_stats_t* out_stats);`

Create `src/policy/shadow.c`:
- Implement `shadow_rule_matches` and `shadow_rule_should_sample`.
- Implement `shadow_queue_t` using thread-safe ring buffer (`pthread_mutex_t`, `pthread_cond_t`). Ensure `shadow_queue_push` is completely non-blocking: if `count == capacity`, drops task immediately and increments `dropped_count`.
- Implement `shadow_eval_cache_t` with mutex protection, maintaining a circular ring buffer and accumulating running stats.
- Clean up dynamic resources in `shadow_task_free`.

Update `CMakeLists.txt`:
- Add `src/policy/shadow.c` to `libaigate` sources.
- Add `tests/unit/policy/test_shadow.c` to unit test targets.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% tests pass.

- [ ] **Step 5: Check Doxygen & commit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: 0 warnings.
Commit: `git commit -m "feat(shadow): implement shadow rule matching, non-blocking queue and evaluation cache"`

---

### Task 2: In-Memory Pairing Engine & Asynchronous Shadow Worker

**Files:**
- Modify: `src/policy/shadow.h`
- Modify: `src/policy/shadow.c`
- Modify: `tests/unit/policy/test_shadow.c`

- [ ] **Step 1: Write unit tests for request pairing and shadow engine worker lifecycle**

Add to `tests/unit/policy/test_shadow.c`:
```c
TEST_CASE(test_shadow_pairing_primary_first)
{
    shadow_engine_t* eng = shadow_engine_create(NULL, 10, 10);
    TEST_ASSERT(eng != NULL, "shadow engine created");

    const char* eval_id = "eval-pair-001";
    const char* trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";

    /* Primary finishes first */
    shadow_engine_start_pairing(eng, eval_id, trace_id, "gpt-4o", "deepseek-chat", TRAFFIC_MODE_SHADOW, "Hello world");
    shadow_engine_record_primary(eng, eval_id, 120.0, 45.0, 200, 150, 0.005, "Primary answer");

    /* Cache should not have complete item yet */
    TEST_ASSERT(shadow_engine_eval_count(eng) == 0, "not in cache before shadow finishes");

    /* Shadow finishes */
    shadow_engine_record_shadow(eng, eval_id, 65.0, 20.0, 200, 140, 0.0008, "Shadow answer");

    /* Now merged into cache */
    TEST_ASSERT(shadow_engine_eval_count(eng) == 1, "merged into cache");
    shadow_eval_item_t item;
    TEST_ASSERT(shadow_engine_get_recent_evals(eng, &item, 1) == 1, "retrieved item");
    TEST_ASSERT(strcmp(item.eval_id, eval_id) == 0, "eval_id matches");
    TEST_ASSERT(strcmp(item.primary_resp_snippet, "Primary answer") == 0, "primary snippet matches");
    TEST_ASSERT(strcmp(item.shadow_resp_snippet, "Shadow answer") == 0, "shadow snippet matches");
    TEST_ASSERT(item.primary_latency_ms == 120.0, "primary latency matches");
    TEST_ASSERT(item.shadow_latency_ms == 65.0, "shadow latency matches");

    shadow_engine_destroy(eng);
}

TEST_CASE(test_shadow_pairing_shadow_first)
{
    shadow_engine_t* eng = shadow_engine_create(NULL, 10, 10);
    const char* eval_id = "eval-pair-002";

    /* Shadow finishes first */
    shadow_engine_start_pairing(eng, eval_id, "trace-002", "gpt-4o", "deepseek-chat", TRAFFIC_MODE_SHADOW, "Prompt text");
    shadow_engine_record_shadow(eng, eval_id, 50.0, 15.0, 200, 120, 0.0006, "Shadow answer fast");
    TEST_ASSERT(shadow_engine_eval_count(eng) == 0, "not in cache yet");

    /* Primary finishes later */
    shadow_engine_record_primary(eng, eval_id, 150.0, 60.0, 200, 130, 0.004, "Primary answer slow");
    TEST_ASSERT(shadow_engine_eval_count(eng) == 1, "merged into cache");

    shadow_engine_destroy(eng);
}
```

Register test cases in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because pairing functions are not declared/implemented.

- [ ] **Step 3: Implement pairing slot table and background shadow worker**

In `src/policy/shadow.h`:
- Declare:
  ```c
  shadow_engine_t* shadow_engine_create(pg_store_t* ps, size_t queue_cap, size_t eval_cap);
  void shadow_engine_destroy(shadow_engine_t* eng);
  int shadow_engine_start(shadow_engine_t* eng);
  void shadow_engine_stop(shadow_engine_t* eng);

  bool shadow_engine_start_pairing(shadow_engine_t* eng,
                                   const char* eval_id,
                                   const char* trace_id,
                                   const char* source_model,
                                   const char* target_model,
                                   traffic_mode_t mode,
                                   const char* prompt_preview);

  bool shadow_engine_record_primary(shadow_engine_t* eng,
                                    const char* eval_id,
                                    double latency_ms,
                                    double ttft_ms,
                                    int http_status,
                                    long tokens,
                                    double cost_usd,
                                    const char* resp_snippet);

  bool shadow_engine_record_shadow(shadow_engine_t* eng,
                                   const char* eval_id,
                                   double latency_ms,
                                   double ttft_ms,
                                   int http_status,
                                   long tokens,
                                   double cost_usd,
                                   const char* resp_snippet);

  size_t shadow_engine_eval_count(shadow_engine_t* eng);
  int shadow_engine_get_recent_evals(shadow_engine_t* eng, shadow_eval_item_t* out_items, int max_items);
  void shadow_engine_get_stats(shadow_engine_t* eng, shadow_stats_t* out_stats);
  bool shadow_engine_submit_task(shadow_engine_t* eng, const shadow_task_t* task);
  ```

In `src/policy/shadow.c`:
- Define `pairing_slot_t` with mutex protection, tracking `primary_done` and `shadow_done` flags.
- Implement pairing table (capacity 256 slots, keyed by `eval_id`).
- When both sides are marked done, commit `shadow_eval_item_t` to `shadow_eval_cache_t` and free the slot.
- Implement the background worker thread:
  - Loop popping from `shadow_queue_t`.
  - Adapt payload model to `target_model` using JSON manipulation.
  - Send HTTP request to upstream target with rule timeout (`rule.timeout_ms`).
  - Calculate latency and TTFT, extract token counts and first 512 bytes of response.
  - Call `shadow_engine_record_shadow`.
- Implement clean thread shutdown in `shadow_engine_stop`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% tests pass.

- [ ] **Step 5: Check Doxygen & commit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: 0 warnings.
Commit: `git commit -m "feat(shadow): implement dual-mode pairing engine and background shadow worker"`

---

### Task 3: Database Storage & Canary Routing with Auto-Rollback

**Files:**
- Modify: `src/store/pg_store.h`
- Modify: `src/store/pg_store.c`
- Modify: `src/upstream/model_router.h`
- Modify: `src/upstream/model_router.c`
- Modify: `tests/unit/store/test_pg_store.c`
- Modify: `tests/unit/upstream/test_model_router.c`

- [ ] **Step 1: Write unit tests for shadow rules CRUD and canary auto-rollback**

In `tests/unit/store/test_pg_store.c` (or fake store tests):
- Add test for `pg_store_list_shadow_rules`, `create_shadow_rule`, `update_shadow_rule`, `delete_shadow_rule`.

In `tests/unit/upstream/test_model_router.c`:
- Add test `test_canary_routing_and_circuit_breaker_rollback`:
  - Set up a canary rule for `gpt-4o` -> `gpt-4o-mini` with 100% weight.
  - Test routing selection: resolves candidate target to `gpt-4o-mini`, sets canary active flag.
  - Simulate 3 consecutive 5xx errors from `gpt-4o-mini`.
  - Verify circuit breaker trips and auto-disables the canary rule, failing back 100% to `gpt-4o`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because canary routing and shadow rule pg store methods are missing.

- [ ] **Step 3: Implement DB schema, CRUD methods, and Canary router decorator**

In `src/store/pg_store.h`:
- Add `shadow_rule_t` CRUD operations in `pg_ops_t`:
  - `int (*list_shadow_rules)(void* ctx, shadow_rule_t* out, int cap, int* n);`
  - `int (*create_shadow_rule)(void* ctx, const shadow_rule_t* rule, long* out_id);`
  - `int (*update_shadow_rule)(void* ctx, const shadow_rule_t* rule);`
  - `int (*delete_shadow_rule)(void* ctx, long id);`
- Declare public `pg_store_*` wrapper functions.

In `src/store/pg_store.c`:
- Add `CREATE TABLE IF NOT EXISTS shadow_rules ...` in table migration scripts.
- Implement database queries for listing, inserting, updating, and deleting shadow rules.

In `src/upstream/model_router.h` & `src/upstream/model_router.c`:
- Declare and implement `model_router_apply_canary`:
  ```c
  int model_router_apply_canary(model_router_t* mr,
                                circuit_breaker_t* cb,
                                const shadow_rule_t* rules,
                                int num_rules,
                                const char* source_model,
                                const char* header_str,
                                char* out_effective_model,
                                size_t out_model_sz,
                                bool* out_is_canary,
                                long* out_canary_rule_id);
  ```
- Implement canary auto-rollback: check if the canary target has tripped its circuit breaker (status OPEN). If tripped, ignore the canary rule, fall back to `source_model`, and mark the rule as disabled.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% tests pass.

- [ ] **Step 5: Check Doxygen & commit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: 0 warnings.
Commit: `git commit -m "feat(router): implement shadow rules pg store and canary routing with auto-rollback"`

---

### Task 4: Pipeline Integration (Inbound Cloning, Primary Pairing & Canary Headers)

**Files:**
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `tests/unit/core/test_aigate_core.c`

- [ ] **Step 1: Write integration tests for request cloning and canary headers**

In `tests/unit/core/test_aigate_core.c`:
- Test that when a canary rule is active, responses contain `x-aigate-canary: true`.
- Test that when a shadow rule is active, a cloned task is pushed to the shadow queue without adding delay to the main response.
- Test that when the primary request completes, its metrics are forwarded to the pairing engine.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Test fails because pipeline is not instrumented with shadow/canary hooks yet.

- [ ] **Step 3: Instrument gateway lifecycle and request pipeline**

In `src/core/aigate_core_internal.h`:
- Add `shadow_engine_t* shadow_eng` to `struct aigate_core`.
- Add to `chat_req_t`:
  - `bool is_canary;`
  - `long canary_rule_id;`
  - `bool has_shadow;`
  - `shadow_rule_t shadow_rule;`
  - `char eval_id[33];`

In `src/core/aigate_core.c`:
- In `aigate_core_init`: initialize `ac->shadow_eng = shadow_engine_create(ps, 256, 200)` and start worker threads.
- In `aigate_core_shutdown`: stop and destroy `ac->shadow_eng`.
- In `resolve_chat_target`:
  - Evaluate active canary rules. If matched, update target model to `target_model` and set `q->is_canary = true`.
  - Evaluate active shadow rules. If matched, set `q->has_shadow = true`, copy `shadow_rule`, generate 32-hex `eval_id`, and initialize pairing slot via `shadow_engine_start_pairing`.

In `src/core/pipeline_chat.c`:
- At inbound stage, if `q->has_shadow`:
  - Extract prompt preview (first 255 chars).
  - Allocate `shadow_task_t` with cloned request body.
  - Push non-blocking to `shadow_queue_push`.
- At downstream response emission (in `handle_chat_sync` and `handle_stream_preheaders`):
  - If `q->is_canary`, inject HTTP response header `X-Aigate-Canary: true`.
- At request completion (both sync and stream completion):
  - If `q->has_shadow`:
    - Call `shadow_engine_record_primary` with primary latency, TTFT, HTTP status, token counts, calculated cost, and response snippet.
  - If `q->is_canary`:
    - If status >= 500, check consecutive failure count and trigger auto-rollback if limit reached.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% tests pass.

- [ ] **Step 5: Check Doxygen & commit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: 0 warnings.
Commit: `git commit -m "feat(core): instrument request pipeline with shadow cloning and canary execution"`

---

### Task 5: Admin REST APIs for Shadow Rules, Evaluations & Metrics

**Files:**
- Modify: `src/server/admin_api.h`
- Modify: `src/server/admin_api.c`
- Modify: `tests/unit/server/test_admin_api.c`

- [ ] **Step 1: Write unit tests for Shadow REST endpoints**

In `tests/unit/server/test_admin_api.c`:
- Test `GET /admin/v1/shadow/rules` (empty list, then populated).
- Test `POST /admin/v1/shadow/rules` (create SHADOW and CANARY rules).
- Test `PUT /admin/v1/shadow/rules/:id` (toggle enabled, change sample_rate).
- Test `DELETE /admin/v1/shadow/rules/:id` (delete rule).
- Test `GET /admin/v1/shadow/evaluations` (returns recent evaluation items with latency, cost, snippets).
- Test `GET /admin/v1/shadow/stats` (returns aggregate KPI counters).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure or 404 on shadow admin endpoints.

- [ ] **Step 3: Implement Shadow REST endpoints in `admin_api.c`**

In `src/server/admin_api.c`:
- Add route dispatching for:
  - `GET /admin/v1/shadow/rules` -> `handle_admin_shadow_rules_list`
  - `POST /admin/v1/shadow/rules` -> `handle_admin_shadow_rules_create`
  - `PUT /admin/v1/shadow/rules/:id` -> `handle_admin_shadow_rules_update`
  - `DELETE /admin/v1/shadow/rules/:id` -> `handle_admin_shadow_rules_delete`
  - `GET /admin/v1/shadow/evaluations` -> `handle_admin_shadow_evaluations_list`
  - `GET /admin/v1/shadow/stats` -> `handle_admin_shadow_stats_get`
- Serialize JSON payloads using Jansson with appropriate error handling and status codes.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% tests pass.

- [ ] **Step 5: Check Doxygen & commit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: 0 warnings.
Commit: `git commit -m "feat(admin): implement REST endpoints for shadow rules, evaluations, and stats"`

---

### Task 6: Web Console Dashboard & Side-by-Side Comparison UI

**Files:**
- Modify: `web/admin.html`
- Modify: `tests/unit/server/test_admin_ui.c`

- [ ] **Step 1: Write test for Web Console tab presence**

In `tests/unit/server/test_admin_ui.c`:
- Assert `web/admin.html` contains:
  - Side navigation button: `data-tab="shadow"`
  - Tab pane: `id="tab-shadow"`
  - KPI card IDs: `shadowKpiTotal`, `shadowKpiLatency`, `shadowKpiCost`, `shadowKpiHealth`
  - Rule modal ID: `shadowRuleModal`
  - Side-by-side diff modal ID: `shadowDiffModal`

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Test fails due to missing elements in `web/admin.html`.

- [ ] **Step 3: Implement Web Console Shadow & Canary tab and modals**

In `web/admin.html`:
- Add sidebar link `🧪 流量镜像/金丝雀` (`data-tab="shadow"`).
- Build tab pane `<div id="tab-shadow" class="tab-pane hidden space-y-6">`:
  - **4 大核心 KPI 统计卡片**:
    - 累计评测请求数 (`shadowKpiTotal`)
    - 时延降幅对比 (`shadowKpiLatency`, 例如 `240ms -> 118ms ⚡ -50.8%`)
    - 成本节约预估 (`shadowKpiCost`, 例如 `原 $18.40 vs 现 $2.10 💰 -88.5%`)
    - 影子可用率 (`shadowKpiHealth`, 例如 `99.8%`)
  - **规则配置与管理卡片**:
    - 规则列表表格，带状态开关、源模型 -> 目标模型流向指示、采样比例滑动条、操作删除按钮；
    - 「添加评测规则」模态框 (`#shadowRuleModal`)，支持选择源模型、目标模型、分流模式（SHADOW / CANARY）、采样率与过滤头；
  - **实时 Side-by-Side 双路对比数据流表格**:
    - 列表展示：时间戳、Prompt 摘要、主模型 vs 影子模型响应摘要、时延对比标签、费用节约徽章；
    - 行操作：
      - `🔍 双路对比`：点击弹出左右双栏文本对比抽屉 (`#shadowDiffModal`)，直观对照主模型与候选模型回答差异；
      - `⚡ 追踪瀑布图`：一键跳转至对应 Trace ID 的 OpenTelemetry 瀑布图；
- Add Vanilla JS functions:
  - `loadShadowTab()`, `renderShadowRules()`, `renderShadowEvaluations()`, `renderShadowStats()`, `openShadowRuleModal()`, `saveShadowRule()`, `toggleShadowRule()`, `deleteShadowRule()`, `openShadowDiffModal(evalId)`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% tests pass.

- [ ] **Step 5: Check Doxygen & commit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: 0 warnings.
Commit: `git commit -m "feat(ui): add traffic shadowing and canary evaluation console with side-by-side diff"`

---

### Task 7: Comprehensive Verification, Documentation & Master Merge

**Files:**
- Modify: `docs/superpowers/specs/2026-10-02-traffic-shadowing-and-canary-design.md`

- [ ] **Step 1: Run full test suite & edge-case stress check**

Run: `ctest --output-on-failure` and `./build/tests/aigate_unit_tests`
Expected: 100% pass (225+ tests).

- [ ] **Step 2: Run Doxygen verification**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: Strictly 0 warnings.

- [ ] **Step 3: Update design spec status**

In `docs/superpowers/specs/2026-10-02-traffic-shadowing-and-canary-design.md`:
- Update status from `草案 (Draft / Pending Approval)` to `已实现并通过验证 (Implemented & Verified)`.
- Check all acceptance criteria checkboxes.

- [ ] **Step 4: Commit changes and prepare final report**

Commit: `git commit -m "docs(spec): mark traffic shadowing and canary A/B testing as implemented and verified"`
Verify `git status` is clean.

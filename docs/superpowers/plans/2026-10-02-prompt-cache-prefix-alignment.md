# 提示词前缀对齐与 KV/Prompt Cache 命中优化引擎实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建纯 C17 高性能提示词前缀对齐与 KV/Prompt Cache 命中优化引擎，在微秒级执行开销（$< 300\mu\text{s}$）内实现 Tools 字典序稳定排序、易变动态时戳与 UUID 智能下沉、Anthropic Claude 智能分块加权打点（至多 4 处 `cache_control: {"type": "ephemeral"}`）以及多协议响应 Usage 归一化解析，为公有云与私有化部署（vLLM / SGLang RadixAttention）带来 50%~90% 的上游计算成本缩减与高达 80% 的首字时延优化。

**Architecture:** 
1. 核心策略模块 (`src/policy/cache_optimizer.h / .c`)：实现 Tools 字典序就地稳定排序、System Prompt 动态时戳/UUID 识别下沉、Anthropic Ephemeral 断点加权注入以及 200 槽位内存环形快照缓冲与聚合指标统计；
2. 数据库与存储 (`schema/schema.sql`, `src/store/pg_store.c`)：提供 `cache_optimizer_rules` 表及 v15 迁移与对应的 CRUD C 原生接口；
3. 流水线挂载 (`src/core/pipeline_chat.c`, `src/core/aigate_core.c`)：在提示词压缩器之后、上游分发前挂载优化器，支持客户端 Header `X-Aigate-Prompt-Cache` 动态覆盖，解析上游返回的 `cached_tokens`，注入 `X-Aigate-Prompt-Cache-*` 响应头与 OTel 追踪属性；
4. 管理接口与可视化 (`src/server/admin_api.c`, `web/admin.html`)：提供 `/admin/v1/cache-optimizer/*` REST 接口以及包含 4 张 KPI 卡片、规则管理器与请求断点审查抽屉的 Web 看板。

**Tech Stack:** 纯 C17 标准, GCC/Clang, CMake, cJSON, POSIX Threads, PostgreSQL, OpenTelemetry TraceContext, Tailwind CSS, Vanilla JS.

---

### Task 1: 核心数据结构、Tools 字典序稳定重排与空白规范化

**Files:**
- Create: `src/policy/cache_optimizer.h`
- Create: `src/policy/cache_optimizer.c`
- Create: `tests/unit/policy/test_cache_optimizer.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/unit/run_tests.c`

- [x] **Step 1: 编写失败的单元测试 (测试 Tools 字典序重排与空白规范化)**

在 `tests/unit/policy/test_cache_optimizer.c` 中：
```c
#include "test_framework.h"
#include "policy/cache_optimizer.h"
#include <string.h>
#include <cjson/cJSON.h>

TEST_CASE(cache_optimizer_sort_tools_test)
{
    const char* json_str = "{"
        "\"model\":\"gpt-4o\","
        "\"tools\":["
        "  {\"type\":\"function\",\"function\":{\"name\":\"weather_lookup\"}},"
        "  {\"type\":\"function\",\"function\":{\"name\":\"calculator\"}},"
        "  {\"type\":\"function\",\"function\":{\"name\":\"database_query\"}}"
        "]"
    "}";

    cJSON* root = cJSON_Parse(json_str);
    TEST_ASSERT(root != NULL, "JSON parse failed");

    bool changed = cache_optimizer_sort_tools(root);
    TEST_ASSERT(changed == true, "Tools should have been reordered");

    cJSON* tools = cJSON_GetObjectItem(root, "tools");
    TEST_ASSERT(tools != NULL, "tools array missing");

    cJSON* t0 = cJSON_GetArrayItem(tools, 0);
    cJSON* t1 = cJSON_GetArrayItem(tools, 1);
    cJSON* t2 = cJSON_GetArrayItem(tools, 2);

    TEST_ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(t0, "function"), "name")->valuestring, "calculator");
    TEST_ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(t1, "function"), "name")->valuestring, "database_query");
    TEST_ASSERT_STR_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(t2, "function"), "name")->valuestring, "weather_lookup");

    cJSON_Delete(root);
}

TEST_CASE(cache_optimizer_normalize_whitespace_test)
{
    const char* raw = "  Hello   world \t from \n\n\n aigate  ";
    char out_buf[256];
    size_t out_len = cache_optimizer_normalize_whitespace(raw, strlen(raw), out_buf, sizeof(out_buf));
    TEST_ASSERT(out_len > 0, "Whitespace normalization failed");
    TEST_ASSERT(strstr(out_buf, "Hello world from\n\naigate") != NULL, "Normalization formatting unexpected");
}
```

- [x] **Step 2: 注册测试并运行验证失败**

在 `tests/unit/run_tests.c` 中注册 `cache_optimizer_sort_tools_test` 与 `cache_optimizer_normalize_whitespace_test`。
在 `CMakeLists.txt` 中添加 `src/policy/cache_optimizer.c` 与 `tests/unit/policy/test_cache_optimizer.c`。
执行：`cmake --build build -j`
预期：编译失败，提示 `cache_optimizer_sort_tools` 和 `cache_optimizer_normalize_whitespace` 未定义。

- [x] **Step 3: 实现 `src/policy/cache_optimizer.h` 与 `src/policy/cache_optimizer.c`**

在 `src/policy/cache_optimizer.h` 中定义数据结构与声明：
```c
#ifndef AIGATE_CACHE_OPTIMIZER_H
#define AIGATE_CACHE_OPTIMIZER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHE_OPTIMIZER_MAX_SNAPSHOTS 200
#define ANTHROPIC_CACHE_MIN_TOKENS    1024
#define ANTHROPIC_MAX_BREAKPOINTS     4

typedef struct {
    uint32_t id;
    char     model_pattern[64];
    bool     enabled;
    bool     sort_tools;
    bool     sink_dynamic_system;
    bool     inject_anthropic_breakpoints;
    uint32_t min_tokens_threshold;
    time_t   created_at;
    time_t   updated_at;
} cache_optimizer_rule_t;

typedef struct {
    bool     optimized;
    bool     tools_sorted;
    bool     dynamic_sunk;
    int      breakpoints_injected;
    char*    optimized_payload;
    size_t   optimized_len;
    uint32_t latency_us;
} cache_optimizer_result_t;

typedef struct {
    char     req_id[64];
    char     model[64];
    time_t   timestamp;
    bool     upstream_cache_hit;
    uint32_t prompt_tokens;
    uint32_t cached_tokens;
    double   cost_savings_usd;
    uint32_t latency_us;
    int      breakpoints_count;
    bool     dynamic_sunk;
    bool     tools_sorted;
} cache_optimizer_snapshot_t;

typedef struct {
    uint64_t total_optimized_requests;
    uint64_t upstream_cache_hit_requests;
    uint64_t total_prompt_tokens;
    uint64_t total_cached_tokens;
    double   total_savings_usd;
    uint64_t avg_latency_us;
} cache_optimizer_stats_t;

bool cache_optimizer_sort_tools(cJSON* root);
size_t cache_optimizer_normalize_whitespace(const char* in, size_t in_len, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_CACHE_OPTIMIZER_H */
```

在 `src/policy/cache_optimizer.c` 中实现：
- `cache_optimizer_sort_tools`: 提取 `tools` 数组中的子项，按 `function.name` 进行 ASCII 稳定排序，并重置 cJSON 链表；
- `cache_optimizer_normalize_whitespace`: 合并连续制表符和空格，将 3 个以上连续空行折叠为 2 个换行。

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（238/238 全部通过）。

- [x] **Step 5: 提交 Task 1 代码**

执行：`git add src/policy/cache_optimizer.h src/policy/cache_optimizer.c tests/unit/policy/test_cache_optimizer.c CMakeLists.txt tests/unit/run_tests.c && git commit -m "feat(cache_optimizer): implement tools sorting and whitespace normalization"`

---

### Task 2: 易变动态时戳与 UUID 智能检测与下沉 (Dynamic Content Sinking)

**Files:**
- Modify: `src/policy/cache_optimizer.h`
- Modify: `src/policy/cache_optimizer.c`
- Modify: `tests/unit/policy/test_cache_optimizer.c`

- [x] **Step 1: 编写失败的单元测试 (测试动态内容识别与下沉)**

在 `tests/unit/policy/test_cache_optimizer.c` 中：
```c
TEST_CASE(cache_optimizer_sink_dynamic_system_test)
{
    const char* raw_system = "Today is 2026-10-02 17:15:30. You are a senior AI coding assistant. Follow clean code principles.";
    char out_buf[1024];
    bool sunk = cache_optimizer_sink_dynamic_system(raw_system, strlen(raw_system), out_buf, sizeof(out_buf));
    TEST_ASSERT(sunk == true, "Expected dynamic timestamp to be detected and sunk");
    
    /* Static instructions must now be at the very beginning */
    TEST_ASSERT(strncmp(out_buf, "You are a senior AI coding assistant", 36) == 0, "Static rules should be moved to front");
    /* The dynamic timestamp must be appended at the end in a runtime context tag */
    TEST_ASSERT(strstr(out_buf, "Today is 2026-10-02 17:15:30") != NULL, "Dynamic timestamp must be preserved");
    TEST_ASSERT(strstr(out_buf, "[Runtime Context:") != NULL, "Runtime context block expected at end");

    /* Test that static text without dynamic elements remains unchanged */
    const char* static_system = "You are a senior AI coding assistant. Follow clean code principles.";
    bool sunk2 = cache_optimizer_sink_dynamic_system(static_system, strlen(static_system), out_buf, sizeof(out_buf));
    TEST_ASSERT(sunk2 == false, "Static system should not be modified");
}
```

- [x] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，提示 `cache_optimizer_sink_dynamic_system` 未声明。

- [x] **Step 3: 实现动态内容识别与下沉算法**

在 `src/policy/cache_optimizer.h` 中声明：
```c
bool cache_optimizer_sink_dynamic_system(const char* in, size_t in_len, char* out, size_t out_sz);
```

在 `src/policy/cache_optimizer.c` 中实现：
- 正则与特征模式匹配：
  - `Today is \d{4}[-/]\d{2}[-/]\d{2}`
  - `Current date/time: ...`
  - `\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}`
  - `Session ID: [0-9a-fA-F\-]+`
  - `Request ID: [0-9a-fA-F\-]+`
- 若首段匹配到上述动态前缀：
  - 截取该动态短语到标点或换行处；
  - 复制后续静态核心指令到 `out` 缓冲区头部；
  - 在尾部拼接 `\n\n[Runtime Context: %s]\n`；
  - 返回 true。

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS。

- [x] **Step 5: 提交 Task 2 代码**

执行：`git add src/policy/cache_optimizer.h src/policy/cache_optimizer.c tests/unit/policy/test_cache_optimizer.c && git commit -m "feat(cache_optimizer): implement dynamic timestamp and session sinking"`

---

### Task 3: Anthropic 智能断点注入、内存环形快照与聚合统计

**Files:**
- Modify: `src/policy/cache_optimizer.h`
- Modify: `src/policy/cache_optimizer.c`
- Modify: `tests/unit/policy/test_cache_optimizer.c`

- [x] **Step 1: 编写失败的单元测试 (测试 Anthropic 打点与快照统计)**

在 `tests/unit/policy/test_cache_optimizer.c` 中：
```c
TEST_CASE(cache_optimizer_inject_anthropic_breakpoints_test)
{
    const char* json_str = "{"
        "\"model\":\"claude-3-5-sonnet\","
        "\"messages\":["
        "  {\"role\":\"system\",\"content\":\"Large system instruction exceeding 1024 tokens...\"},"
        "  {\"role\":\"user\",\"content\":\"Hi\"},"
        "  {\"role\":\"assistant\",\"content\":\"Hello there!\"},"
        "  {\"role\":\"user\",\"content\":\"What is the capital of France?\"}"
        "]"
    "}";

    cJSON* root = cJSON_Parse(json_str);
    TEST_ASSERT(root != NULL, "JSON parse failed");

    int bp_count = cache_optimizer_inject_anthropic_breakpoints(root, 1024);
    TEST_ASSERT(bp_count >= 1, "Expected at least 1 breakpoint injected");

    /* Verify cache_control node exists in messages */
    char* printed = cJSON_PrintUnformatted(root);
    TEST_ASSERT(strstr(printed, "\"cache_control\":{\"type\":\"ephemeral\"}") != NULL, "Missing ephemeral cache_control");

    free(printed);
    cJSON_Delete(root);
}

TEST_CASE(cache_optimizer_cache_and_stats_test)
{
    cache_optimizer_cache_t* cache = cache_optimizer_cache_create(200);
    TEST_ASSERT(cache != NULL, "Cache creation failed");

    cache_optimizer_snapshot_t snap = {
        .req_id = "req-12345",
        .model = "claude-3-5-sonnet",
        .timestamp = time(NULL),
        .upstream_cache_hit = true,
        .prompt_tokens = 4096,
        .cached_tokens = 3072,
        .cost_savings_usd = 0.0092,
        .latency_us = 120,
        .breakpoints_count = 2,
        .dynamic_sunk = true,
        .tools_sorted = true
    };

    cache_optimizer_cache_record(cache, &snap);

    cache_optimizer_stats_t stats;
    cache_optimizer_cache_get_stats(cache, &stats);

    TEST_ASSERT_EQ(stats.total_optimized_requests, 1);
    TEST_ASSERT_EQ(stats.upstream_cache_hit_requests, 1);
    TEST_ASSERT_EQ(stats.total_cached_tokens, 3072);

    cache_optimizer_cache_destroy(cache);
}
```

- [x] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，未定义的断点函数与快照缓存结构体。

- [x] **Step 3: 实现 Anthropic 打点与快照缓存管理**

在 `src/policy/cache_optimizer.h` 中添加 `cache_optimizer_cache_t` 结构与 API 声明：
- `int cache_optimizer_inject_anthropic_breakpoints(cJSON* root, uint32_t min_tokens);`
- `cache_optimizer_cache_t* cache_optimizer_cache_create(size_t capacity);`
- `void cache_optimizer_cache_destroy(cache_optimizer_cache_t* cache);`
- `void cache_optimizer_cache_record(cache_optimizer_cache_t* cache, const cache_optimizer_snapshot_t* snap);`
- `void cache_optimizer_cache_get_stats(cache_optimizer_cache_t* cache, cache_optimizer_stats_t* out_stats);`
- `size_t cache_optimizer_cache_get_snapshots(cache_optimizer_cache_t* cache, cache_optimizer_snapshot_t* out_snaps, size_t max_snaps);`

在 `src/policy/cache_optimizer.c` 中实现：
- 加权打点分配：优先 Tools 数组末项、System 块末尾、倒数第 2 轮 Assistant 回复；
- 线程安全（`pthread_mutex_t`）环形缓冲区，容量 200，超过容量自动循环覆盖；
- 累加统计：`total_optimized_requests`、`upstream_cache_hit_requests`、`total_prompt_tokens`、`total_cached_tokens`、`total_savings_usd`。

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS。

- [x] **Step 5: 提交 Task 3 代码**

执行：`git add src/policy/cache_optimizer.h src/policy/cache_optimizer.c tests/unit/policy/test_cache_optimizer.c && git commit -m "feat(cache_optimizer): implement Anthropic ephemeral breakpoints and snapshot cache"`

---

### Task 4: PostgreSQL 存储持久化与数据库迁移 v15

**Files:**
- Modify: `schema/schema.sql`
- Modify: `src/store/schema_sql.h`
- Modify: `src/store/pg_store.h`
- Modify: `src/store/pg_store.c`
- Modify: `tests/unit/server/test_admin_api.c` (或相关 store 测试)

- [x] **Step 1: 编写持久化增删改查单元测试**

在 `tests/unit/policy/test_cache_optimizer.c` 中增加配置序列化与匹配测试：
```c
TEST_CASE(cache_optimizer_rule_match_test)
{
    cache_optimizer_rule_t rule = {
        .id = 1,
        .model_pattern = "claude-*",
        .enabled = true,
        .sort_tools = true,
        .sink_dynamic_system = true,
        .inject_anthropic_breakpoints = true,
        .min_tokens_threshold = 1024
    };

    TEST_ASSERT(cache_optimizer_rule_matches(&rule, "claude-3-5-sonnet") == true, "Pattern matching failed");
    TEST_ASSERT(cache_optimizer_rule_matches(&rule, "gpt-4o") == false, "Pattern matching should fail");
}
```

- [x] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，提示 `cache_optimizer_rule_matches` 未定义。

- [x] **Step 3: 更新 SQL Schema 与 pg_store**

在 `schema/schema.sql` 和 `src/store/schema_sql.h` 中追加：
```sql
-- Migration v15: cache optimizer rules
CREATE TABLE IF NOT EXISTS cache_optimizer_rules (
    id SERIAL PRIMARY KEY,
    model_pattern VARCHAR(64) NOT NULL UNIQUE,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    sort_tools BOOLEAN NOT NULL DEFAULT TRUE,
    sink_dynamic_system BOOLEAN NOT NULL DEFAULT TRUE,
    inject_anthropic_breakpoints BOOLEAN NOT NULL DEFAULT TRUE,
    min_tokens_threshold INT NOT NULL DEFAULT 1024,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_cache_optimizer_rules_model ON cache_optimizer_rules(model_pattern);
```

在 `src/store/pg_store.h` 和 `src/store/pg_store.c` 中增加：
- `pg_store_list_cache_optimizer_rules(...)`
- `pg_store_upsert_cache_optimizer_rule(...)`
- `pg_store_delete_cache_optimizer_rule(...)`
- 在 `pg_store_migrate()` 中增加 `v15` 迁移逻辑。

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS。

- [x] **Step 5: 提交 Task 4 代码**

执行：`git add schema/schema.sql src/store/schema_sql.h src/store/pg_store.h src/store/pg_store.c tests/unit/policy/test_cache_optimizer.c && git commit -m "feat(store): implement cache optimizer rules schema and postgres persistence"`

---

### Task 5: 请求流水线集成、上游 Usage 解析、响应头注入与 OTel 追踪

**Files:**
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `src/server/transport_civetweb.c`
- Modify: `tests/unit/core/test_aigate_core.c`

- [x] **Step 1: 编写流水线前缀优化与响应头断言测试**

在 `tests/unit/core/test_aigate_core.c` 中：
```c
TEST_CASE(pipeline_cache_optimizer_and_headers)
{
    /* Test that chat request correctly executes prefix optimization and injects X-Aigate-Prompt-Cache-* */
    chat_req_t chatq;
    memset(&chatq, 0, sizeof(chatq));
    chatq.cache_opt_result.optimized = true;
    chatq.cache_opt_result.tools_sorted = true;
    chatq.cache_opt_result.dynamic_sunk = true;
    chatq.cache_opt_result.breakpoints_injected = 2;
    chatq.upstream_cached_tokens = 3072;
    chatq.upstream_prompt_tokens = 4096;
    chatq.upstream_cache_savings_usd = 0.0092;

    struct civet_mock c1;
    civet_mock_init(&c1);
    aigate_inject_cache_optimizer_headers(&chatq, (void*)&c1);

    TEST_ASSERT(cap_has_header(&c1, "X-Aigate-Prompt-Cache-Hit: true"), "Missing cache hit header");
    TEST_ASSERT(cap_has_header(&c1, "X-Aigate-Prompt-Cache-Tokens: 3072"), "Missing cached tokens header");
    TEST_ASSERT(cap_has_header(&c1, "X-Aigate-Prompt-Cache-Savings: 0.0092"), "Missing savings header");
}
```

- [x] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，提示相关字段与注入函数未定义。

- [x] **Step 3: 流水线挂载与核心生命周期集成**

1. 在 `aigate_ctx` 中添加 `cache_optimizer_cache_t* cache_opt_cache;`；
2. 在 `chat_req` 中添加 `cache_optimizer_result_t cache_opt_result;`、`uint32_t upstream_cached_tokens;`、`double upstream_cache_savings_usd;`；
3. 在 `pipeline_chat.c` 中：
   - 在 `prompt_compressor` 之后调用 `cache_optimizer_process_request(...)`；
   - 提取上游响应中的 `usage` 节点：
     - OpenAI: `usage.prompt_tokens_details.cached_tokens`；
     - Anthropic: `usage.cache_read_input_tokens`；
   - 记录快照至 `ac->cache_opt_cache`；
4. 在 `transport_civetweb.c` 中放行 `X-Aigate-Prompt-Cache-*` 头。

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS。

- [x] **Step 5: 提交 Task 5 代码**

执行：`git add src/core/aigate_core_internal.h src/core/aigate_core.h src/core/aigate_core.c src/core/pipeline_chat.c src/server/transport_civetweb.c tests/unit/core/test_aigate_core.c && git commit -m "feat(core): integrate cache optimizer into pipeline with usage parsing and headers"`

---

### Task 6: Admin REST API 路由与处理器

**Files:**
- Modify: `src/server/admin_api.c`
- Modify: `tests/unit/server/test_admin_api.c`

- [x] **Step 1: 编写 Admin API 端点测试**

在 `tests/unit/server/test_admin_api.c` 中添加端点测试：
```c
TEST_CASE(admin_cache_optimizer_endpoints)
{
    struct aigate_ctx* ctx = test_get_app_ctx();
    TEST_ASSERT(ctx != NULL, "ctx is null");

    http_response_t resp;
    int rc = test_admin_request(ctx, "GET", "/admin/v1/cache-optimizer/rules", NULL, &resp);
    TEST_ASSERT_EQ(rc, 200);
    TEST_ASSERT(strstr(resp.body, "rules") != NULL, "expected rules array in response");
    http_response_free(&resp);

    rc = test_admin_request(ctx, "GET", "/admin/v1/cache-optimizer/stats", NULL, &resp);
    TEST_ASSERT_EQ(rc, 200);
    TEST_ASSERT(strstr(resp.body, "total_optimized_requests") != NULL, "expected total_optimized_requests");
    http_response_free(&resp);

    rc = test_admin_request(ctx, "GET", "/admin/v1/cache-optimizer/snapshots", NULL, &resp);
    TEST_ASSERT_EQ(rc, 200);
    TEST_ASSERT(strstr(resp.body, "snapshots") != NULL, "expected snapshots array");
    http_response_free(&resp);
}
```

- [x] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：返回 404 Not Found。

- [x] **Step 3: 实现 Admin API 路由与处理器**

在 `src/server/admin_api.c` 中注册并实现：
- `GET /admin/v1/cache-optimizer/rules`
- `POST /admin/v1/cache-optimizer/rules`
- `PUT /admin/v1/cache-optimizer/rules/{id}`
- `DELETE /admin/v1/cache-optimizer/rules/{id}`
- `GET /admin/v1/cache-optimizer/snapshots`
- `GET /admin/v1/cache-optimizer/stats`

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS。

- [x] **Step 5: 提交 Task 6 代码**

执行：`git add src/server/admin_api.c tests/unit/server/test_admin_api.c && git commit -m "feat(admin): implement REST endpoints for cache optimizer rules, snapshots, and stats"`

---

### Task 7: Web 控制台看板与断点审查抽屉

**Files:**
- Modify: `tests/unit/server/test_admin_ui.c`
- Modify: `web/admin.html`

- [x] **Step 1: 编写 Web UI 结构断言测试**

在 `tests/unit/server/test_admin_ui.c` 中添加：
```c
TEST_ASSERT(strstr(html, "data-tab=\"cache-optimizer\"") != NULL, "missing cache-optimizer tab button");
TEST_ASSERT(strstr(html, "id=\"tab-cache-optimizer\"") != NULL, "missing tab-cache-optimizer section");
TEST_ASSERT(strstr(html, "id=\"cacheOptKpiHitRate\"") != NULL, "missing cacheOptKpiHitRate");
TEST_ASSERT(strstr(html, "id=\"cacheOptKpiTokens\"") != NULL, "missing cacheOptKpiTokens");
TEST_ASSERT(strstr(html, "id=\"cacheOptKpiSavings\"") != NULL, "missing cacheOptKpiSavings");
TEST_ASSERT(strstr(html, "id=\"cacheOptKpiLatency\"") != NULL, "missing cacheOptKpiLatency");
TEST_ASSERT(strstr(html, "id=\"cacheOptRuleModal\"") != NULL, "missing cacheOptRuleModal");
TEST_ASSERT(strstr(html, "id=\"cacheOptDetailModal\"") != NULL, "missing cacheOptDetailModal");
```

- [x] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：FAIL，缺少 `tab-cache-optimizer` 元素。

- [x] **Step 3: 更新 `web/admin.html` 实现看板与审查模态框**

1. 侧边栏添加 `🎯 提示词缓存` 导航按钮 (`data-tab="cache-optimizer"`)；
2. 添加 `#tab-cache-optimizer` 主面板：
   - 4 张 KPI 卡片（命中率、缓存 Token 数、预估节省、优化平均耗时）；
   - 规则管理卡片与 `#cacheOptRuleModal`；
   - 实时请求流表格与 `#cacheOptDetailModal`（展示断点注入详情与动态下沉信息）；
3. 编写原生 JavaScript 交互函数：
   - `loadCacheOptimizerTab()`, `renderCacheOptStats()`, `renderCacheOptRules()`, `renderCacheOptSnapshots()`, `openCacheOptRuleModal()`, `openCacheOptDetailModal()`。

- [x] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（239/239 全部通过）。

- [x] **Step 5: 提交 Task 7 代码**

执行：`git add web/admin.html tests/unit/server/test_admin_ui.c && git commit -m "feat(ui): add prompt cache optimizer console with detail modal"`

---

### Task 8: 全量验证、Doxygen 文档检查、规范归档与合并至 master

**Files:**
- Modify: `docs/superpowers/specs/2026-10-02-prompt-cache-prefix-alignment-design.md`
- Modify: `docs/superpowers/plans/2026-10-02-prompt-cache-prefix-alignment.md`

- [x] **Step 1: 运行全量 CTest 套件与并发压测**

执行：`ctest --test-dir build --output-on-failure`
预期：6/6 全部通过（100%）。

- [x] **Step 2: 执行 Doxygen 静态文档扫描**

执行：`doxygen Doxyfile 2>&1 | grep -i warning || true`
预期：严格 0 Warnings。

- [x] **Step 3: 更新设计规范验收状态**

在 `docs/superpowers/specs/2026-10-02-prompt-cache-prefix-alignment-design.md` 中：
- 将状态更新为 `已实现并通过验证 (Implemented & Verified)`；
- 勾选所有验收标准复选框 `[x]`。

- [x] **Step 4: 提交规范更新并合并至 master**

执行：
```bash
git checkout master
git merge --no-ff feature/prompt-cache-prefix-alignment -m "feat: merge prompt cache and prefix alignment engine"
git branch -d feature/prompt-cache-prefix-alignment
```

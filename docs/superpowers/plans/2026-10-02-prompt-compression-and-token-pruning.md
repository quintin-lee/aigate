# 提示词自适应压缩与 Token 瘦身引擎 (Prompt Compression & Token Pruning Engine) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建纯 C17 高性能提示词自适应压缩与 Token 瘦身引擎，在请求流水线中以 `< 500μs` 极低开销实现多轮历史自适应滑动折叠、结构化格式去噪与句子密度重要度剪枝，在 100% 保障代码块、Tool 调用与最新提问安全的前提下，大幅削减 30%~60% 的 Token 成本并降低首字时延，配套完整响应头注入、OTel 追踪指标、PostgreSQL 持久化、Admin REST 接口与 Web 控制台 Side-by-Side 文本对比抽屉。

**Architecture:** 
1. 核心策略模块 (`src/policy/prompt_compressor.h / .c`)：实现微秒级 Token 预估、三阶混合剪枝（历史轮次折叠、空白/换行去噪、句子信息熵密度剪枝）与状态机安全沙箱（保护代码块、PII 占位符、Tool Calls 与最新 User 提问），配备 200 条容量的内存环形快照缓冲池与聚合统计；
2. 数据库与存储 (`schema/schema.sql`, `src/store/pg_store.c`)：提供 `compressor_rules` 表及对应的增删改查 C 原生接口；
3. 流水线挂载 (`src/core/pipeline_chat.c`, `src/core/aigate_core.c`)：在响应缓存未命中后、上游组装前执行剪枝，支持 `X-Aigate-Compress` 控制头覆盖，并注入 `X-Aigate-Compression-*` 响应头与 OTel 追踪 Span；
4. 管理接口与可视化 (`src/server/admin_api.c`, `web/admin.html`)：提供 `/admin/v1/compressor/*` REST 接口以及包含 4 张 KPI 卡片、规则管理器与双栏高亮 Diff 抽屉的 Web 看板。

**Tech Stack:** 纯 C17 标准, GCC/Clang, CMake, cJSON, POSIX Threads, PostgreSQL, OpenTelemetry TraceContext, Tailwind CSS, Vanilla JS.

---

### Task 1: 核心数据结构、极速 Token 估算与格式去噪 (Stage 2)

**Files:**
- Create: `src/policy/prompt_compressor.h`
- Create: `src/policy/prompt_compressor.c`
- Create: `tests/unit/policy/test_prompt_compressor.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写失败的单元测试 (测试 Token 估算与空白去噪)**

在 `tests/unit/policy/test_prompt_compressor.c` 中：
```c
#include "test_framework.h"
#include "policy/prompt_compressor.h"
#include <string.h>

TEST_CASE(compressor_fast_token_estimate)
{
    const char* text_en = "Hello world, this is a test prompt for token estimation.";
    uint32_t tokens_en = compressor_estimate_tokens(text_en, strlen(text_en));
    TEST_ASSERT(tokens_en >= 10 && tokens_en <= 15, "English token estimate out of bounds");

    const char* text_zh = "你好世界，这是一个中文提示词测试。";
    uint32_t tokens_zh = compressor_estimate_tokens(text_zh, strlen(text_zh));
    TEST_ASSERT(tokens_zh >= 10 && tokens_zh <= 25, "Chinese token estimate out of bounds");
}

TEST_CASE(compressor_whitespace_sanitization)
{
    const char* raw = "Line 1\n\n\n\nLine 2   with   extra   spaces\n```python\ndef foo():\n    # indented code\n\n\n    return 42\n```\nAfter code\n\n\nEnd";
    char out_buf[1024];
    size_t out_len = compressor_sanitize_whitespace(raw, strlen(raw), out_buf, sizeof(out_buf), true);
    TEST_ASSERT(out_len > 0, "Sanitization failed");

    /* Verifies that non-code consecutive newlines were collapsed */
    TEST_ASSERT(strstr(out_buf, "Line 1\n\nLine 2") != NULL, "Failed to collapse newlines outside code");
    /* Verifies that inside ```python, indentation and newlines are preserved */
    TEST_ASSERT(strstr(out_buf, "    # indented code\n\n\n    return 42") != NULL, "Code block corrupted");
    TEST_ASSERT(strstr(out_buf, "After code\n\nEnd") != NULL, "Trailing newlines not collapsed");
}
```

- [ ] **Step 2: 注册测试并运行验证失败**

在 `tests/unit/run_tests.c` 中注册 `compressor_fast_token_estimate` 与 `compressor_whitespace_sanitization`。
在 `CMakeLists.txt` 中添加 `src/policy/prompt_compressor.c` 与 `tests/unit/policy/test_prompt_compressor.c`。
执行：`cmake --build build -j`
预期：编译失败，提示 `compressor_estimate_tokens` 和 `compressor_sanitize_whitespace` 未定义。

- [ ] **Step 3: 实现 `src/policy/prompt_compressor.h` 与 `src/policy/prompt_compressor.c`**

在 `src/policy/prompt_compressor.h` 中定义数据结构：
`compressor_level_t`, `compressor_rule_t`, `compressor_result_t`, `compressor_snapshot_t`, `compressor_stats_t`，以及函数原型：
- `uint32_t compressor_estimate_tokens(const char* text, size_t len);`
- `size_t compressor_sanitize_whitespace(const char* src, size_t src_len, char* dst, size_t dst_cap, bool preserve_code);`

在 `src/policy/prompt_compressor.c` 中实现：
- Token 估算：按单词（空白/标点分隔）加多字节 UTF-8 字符加权：`tokens = words * 1.3 + (utf8_bytes / 2)`；
- 状态机空白去噪：
  - 维护 `bool in_code_block` 状态；
  - 当扫描到 ` ``` ` 行起始时切换 `in_code_block = !in_code_block`；
  - 在代码块内原样写入字符；
  - 在代码块外将连续超过 2 个换行 `\n\n\n+` 折叠为 `\n\n`，将单行连续空格/Tab 压缩为单个空格。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：2 个新测试与之前 229 个测试全部 PASS（共 231 个通过）。

- [ ] **Step 5: 检查 Doxygen 文档规范并提交**

执行：`doxygen Doxyfile 2>&1 | grep -i warning || true`（确保 0 警告）。
执行：`git add src/policy/prompt_compressor.h src/policy/prompt_compressor.c tests/unit/policy/test_prompt_compressor.c tests/unit/run_tests.c CMakeLists.txt && git commit -m "feat(compressor): implement token estimation and whitespace sanitizer with code block protection"`

---

### Task 2: 会话历史轮次自适应滑动保留与安全沙箱 (Stage 1)

**Files:**
- Modify: `src/policy/prompt_compressor.h`
- Modify: `src/policy/prompt_compressor.c`
- Modify: `tests/unit/policy/test_prompt_compressor.c`

- [ ] **Step 1: 编写失败的单元测试 (测试历史滑动保留与沙箱安全)**

在 `tests/unit/policy/test_prompt_compressor.c` 中添加：
```c
TEST_CASE(compressor_history_windowing_and_safety)
{
    const char* mock_payload = "{\n"
        "  \"model\": \"gpt-4o\",\n"
        "  \"messages\": [\n"
        "    {\"role\": \"system\", \"content\": \"You are a helpful assistant. Output in JSON format.\"},\n"
        "    {\"role\": \"user\", \"content\": \"My email is {{PII_EMAIL_1}} and turn 1.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"Turn 1 answer.\"},\n"
        "    {\"role\": \"user\", \"content\": \"Turn 2 question.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"Turn 2 answer.\"},\n"
        "    {\"role\": \"user\", \"content\": \"Turn 3 question.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"Turn 3 answer.\"},\n"
        "    {\"role\": \"user\", \"content\": \"Turn 4 question: show me python code.\"},\n"
        "    {\"role\": \"assistant\", \"content\": \"```python\\nprint('hello')\\n```\"},\n"
        "    {\"role\": \"user\", \"content\": \"Final user question: what is my email?\"}\n"
        "  ]\n"
        "}";

    compressor_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.enabled = true;
    rule.level = COMPRESS_LEVEL_MODERATE;
    rule.min_tokens = 10;
    rule.max_history_turns = 2; /* Only keep last 2 turns + system + final user */
    rule.preserve_system = true;
    rule.preserve_code = true;
    rule.preserve_tools = true;

    compressor_result_t res;
    bool ok = prompt_compressor_process_payload(mock_payload, strlen(mock_payload), &rule, &res);
    TEST_ASSERT(ok, "compressor process payload failed");
    TEST_ASSERT(res.compressed, "expected compression to occur");
    TEST_ASSERT(res.compressed_payload != NULL, "compressed payload is null");
    TEST_ASSERT(res.saved_tokens > 0, "expected tokens saved");

    /* System prompt preserved */
    TEST_ASSERT(strstr(res.compressed_payload, "Output in JSON format") != NULL, "System prompt lost");
    /* Final user question strictly preserved */
    TEST_ASSERT(strstr(res.compressed_payload, "Final user question: what is my email?") != NULL, "Final question lost");
    /* Python code block preserved */
    TEST_ASSERT(strstr(res.compressed_payload, "```python\\nprint('hello')\\n```") != NULL, "Code block lost");
    /* Older turn 1 question dropped or folded */
    TEST_ASSERT(strstr(res.compressed_payload, "turn 1.") == NULL, "Turn 1 should be pruned");

    prompt_compressor_result_cleanup(&res);
}
```

- [ ] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，提示 `prompt_compressor_process_payload` 与 `prompt_compressor_result_cleanup` 未声明。

- [ ] **Step 3: 实现多轮历史折叠与 Payload 重构**

在 `src/policy/prompt_compressor.h` 中声明：
- `bool prompt_compressor_process_payload(const char* payload, size_t payload_len, const compressor_rule_t* rule, compressor_result_t* out_result);`
- `void prompt_compressor_result_cleanup(compressor_result_t* res);`

在 `src/policy/prompt_compressor.c` 中：
- 使用 cJSON 解析 `payload`；
- 遍历 `messages` 数组，标记每个消息的索引与角色；
- 定位第一个 `system` 消息与最后一个 `user` 消息；
- 识别 `tool_calls` 或 `role="tool"` 消息原子配对；
- 统计历史会话轮次，若超过 `max_history_turns`，从中间历史消息开始裁剪或折叠为单条轻量上下文占位消息；
- 对保留消息的内容执行空白去噪（调用 Task 1 的 `compressor_sanitize_whitespace`）；
- 重新序列化 JSON 并对比长度与 Token，如果有效缩短则生成 `res->compressed_payload` 并计算节约指标；
- 若出现任何 JSON 解析或分配失败，安全回退并返回 `res->compressed = false`。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（232/232 通过）。

- [ ] **Step 5: 提交 Task 2 代码**

执行：`git add src/policy/prompt_compressor.h src/policy/prompt_compressor.c tests/unit/policy/test_prompt_compressor.c && git commit -m "feat(compressor): implement multi-turn history windowing and payload reconstruction"`

---

### Task 3: 句子级重要度密度剪枝 (Stage 3) 与快照环形缓存池

**Files:**
- Modify: `src/policy/prompt_compressor.h`
- Modify: `src/policy/prompt_compressor.c`
- Modify: `tests/unit/policy/test_prompt_compressor.c`

- [ ] **Step 1: 编写失败的单元测试 (测试激进模式句子剪枝与快照缓存)**

在 `tests/unit/policy/test_prompt_compressor.c` 中添加：
```c
TEST_CASE(compressor_sentence_density_pruning_and_cache)
{
    /* Text with polite fillers and low entropy sentences */
    const char* text_with_filler = 
        "As an AI assistant, I would be pleased to assist you with this comprehensive request. "
        "The server port is configured to 8080 and bind to 127.0.0.1. "
        "Please feel free to ask if you have any further questions or inquiries.";

    char pruned[512];
    size_t pruned_len = compressor_prune_sentence_density(text_with_filler, strlen(text_with_filler), 0.60, pruned, sizeof(pruned));
    TEST_ASSERT(pruned_len > 0, "Prune failed");
    /* Core config sentence preserved */
    TEST_ASSERT(strstr(pruned, "port is configured to 8080") != NULL, "Core sentence was pruned");

    /* Test Snapshot Cache & Stats */
    compressor_cache_t* cache = compressor_cache_create(200);
    TEST_ASSERT(cache != NULL, "cache create failed");

    compressor_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.req_id, sizeof(snap.req_id), "req-12345");
    snprintf(snap.model, sizeof(snap.model), "gpt-4o");
    snap.original_tokens = 3000;
    snap.compressed_tokens = 1500;
    snap.saved_tokens = 1500;
    snap.compression_ratio = 0.50;
    snap.elapsed_us = 210;

    compressor_cache_record(cache, &snap);

    compressor_stats_t stats;
    compressor_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.total_evaluated == 1, "evaluated mismatch");
    TEST_ASSERT(stats.total_saved_tokens == 1500, "saved mismatch");

    compressor_cache_destroy(cache);
}
```

- [ ] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，提示 `compressor_prune_sentence_density`、`compressor_cache_*` 未定义。

- [ ] **Step 3: 实现句子密度剪枝算法与环形快照缓存**

在 `src/policy/prompt_compressor.h` 与 `.c` 中：
- 实现 `compressor_prune_sentence_density`：
  - 按标点句末切分文本；
  - 判定保护句：若包含 `{{PII_`、约束词（"must", "json", "schema", "必须", "禁止"），或者为首末句，设置保护权重 `1.0`；
  - 对其余句子计算去重词数比例信息熵得分，淘汰低分填充句，直至总长度达到目标保留比例 `target_ratio`；
- 实现 `compressor_cache_t` 结构体：
  - 固定容量 200 的 `compressor_snapshot_t` 数组；
  - 互斥锁 `pthread_mutex_t lock`；
  - 维护 `head`, `count`, 以及全局累计指标 `compressor_stats_t stats`；
  - `compressor_cache_record`：将快照存入环形队列，原子累计总评估数、压缩数、Token 节省数与微秒耗时；
  - `compressor_cache_get_snapshots`：导出最近快照供 API 序列化；
  - `compressor_cache_get_stats`：计算并返回平均压缩比与预估节省金额。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（233/233 通过）。

- [ ] **Step 5: 提交 Task 3 代码**

执行：`git add src/policy/prompt_compressor.h src/policy/prompt_compressor.c tests/unit/policy/test_prompt_compressor.c && git commit -m "feat(compressor): implement sentence density pruning and snapshot circular cache"`

---

### Task 4: 数据库持久化存储与 SQL Schema

**Files:**
- Modify: `schema/schema.sql`
- Modify: `src/store/schema_sql.h`
- Modify: `src/store/pg_store.h`
- Modify: `src/store/pg_store.c`
- Modify: `tests/unit/policy/test_prompt_compressor.c`

- [ ] **Step 1: 编写数据库 CRUD 失败的单元测试**

在 `tests/unit/policy/test_prompt_compressor.c` 中添加规则结构序列化与存储测试：
```c
TEST_CASE(compressor_rule_serialization_and_match)
{
    compressor_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    snprintf(rule.id, sizeof(rule.id), "rule-c01");
    snprintf(rule.model_pattern, sizeof(rule.model_pattern), "gpt-4o*");
    rule.enabled = true;
    rule.level = COMPRESS_LEVEL_AGGRESSIVE;
    rule.min_tokens = 1024;
    rule.max_history_turns = 4;
    rule.target_ratio = 0.50;
    rule.preserve_system = true;
    rule.preserve_code = true;
    rule.preserve_tools = true;

    /* Matching test */
    TEST_ASSERT(compressor_rule_match(&rule, "gpt-4o", 2000) == true, "Should match gpt-4o above min_tokens");
    TEST_ASSERT(compressor_rule_match(&rule, "gpt-4o", 500) == false, "Should not match below min_tokens");
    TEST_ASSERT(compressor_rule_match(&rule, "claude-3-5-sonnet", 2000) == false, "Should not match different model");
}
```

- [ ] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，缺少 `compressor_rule_match`。

- [ ] **Step 3: 更新 SQL Schema 与 pg_store**

在 `schema/schema.sql` 与 `src/store/schema_sql.h` 中追加：
```sql
CREATE TABLE IF NOT EXISTS compressor_rules (
    id                  VARCHAR(36) PRIMARY KEY,
    model_pattern       VARCHAR(64) NOT NULL,
    enabled             BOOLEAN NOT NULL DEFAULT TRUE,
    level               INTEGER NOT NULL DEFAULT 1,
    min_tokens          INTEGER NOT NULL DEFAULT 2048,
    max_history_turns   INTEGER NOT NULL DEFAULT 6,
    target_ratio        DOUBLE PRECISION NOT NULL DEFAULT 0.60,
    preserve_system     BOOLEAN NOT NULL DEFAULT TRUE,
    preserve_code       BOOLEAN NOT NULL DEFAULT TRUE,
    preserve_tools      BOOLEAN NOT NULL DEFAULT TRUE,
    created_at          BIGINT NOT NULL,
    updated_at          BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_compressor_rules_model ON compressor_rules(model_pattern);
```

在 `src/store/pg_store.h` 与 `src/store/pg_store.c` 中实现：
- `bool pg_store_compressor_rules_load(pg_store_t* store, compressor_rule_t* out_rules, size_t max_rules, size_t* out_count);`
- `bool pg_store_compressor_rule_upsert(pg_store_t* store, const compressor_rule_t* rule);`
- `bool pg_store_compressor_rule_delete(pg_store_t* store, const char* rule_id);`
- 在 `src/policy/prompt_compressor.c` 中实现通配符与阈值匹配函数 `compressor_rule_match`。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（234/234 通过）。

- [ ] **Step 5: 提交 Task 4 代码**

执行：`git add schema/schema.sql src/store/schema_sql.h src/store/pg_store.h src/store/pg_store.c src/policy/prompt_compressor.c tests/unit/policy/test_prompt_compressor.c && git commit -m "feat(store): implement compressor rules schema and postgres persistence"`

---

### Task 5: 请求流水线集成、响应头注入与 OpenTelemetry 追踪

**Files:**
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `tests/unit/core/test_aigate_core.c`

- [ ] **Step 1: 编写流水线压缩与响应头注入的单元测试**

在 `tests/unit/core/test_aigate_core.c` 中添加集成测试：
```c
TEST_CASE(pipeline_prompt_compression_and_headers)
{
    /* Test that chat request triggers compression when rule matches,
       sets comp_result in chat_req_t, and injects headers on response */
    ...
}
```

- [ ] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：编译失败，缺少上下文字段与流水线 Hook。

- [ ] **Step 3: 流水线挂载与生命周期集成**

1. 在 `src/core/aigate_core_internal.h` 中：
   - 包含 `policy/prompt_compressor.h`；
   - `struct aigate_ctx` 新增 `compressor_cache_t* comp_cache` 与规则表；
   - `struct chat_req` 新增 `compressor_result_t comp_result`。
2. 在 `src/core/aigate_core.c` 中：
   - 初始化 `ac->comp_cache = compressor_cache_create(200)`；
   - 清理时 `compressor_cache_destroy(ac->comp_cache)`；
   - 在 `chat_req_cleanup()` 中调用 `prompt_compressor_result_cleanup(&q->comp_result)` 释放压缩生成的 payload。
3. 在 `src/core/pipeline_chat.c` 中：
   - 处于缓存未命中后、上游分发前：
   - 读取客户端请求头 `X-Aigate-Compress`；
   - 匹配规则并调用 `prompt_compressor_process_payload`；
   - 如果发生压缩：
     - 用 `q->comp_result.compressed_payload` 替换 `q->payload` 传给上游；
     - 开启并结束 OTel Span `prompt_compression`；
     - 记录快照至 `ac->comp_cache`；
   - 在响应发送或流式起始时注入 `X-Aigate-Compression-*` 响应头。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（235/235 全部通过）。

- [ ] **Step 5: 提交 Task 5 代码**

执行：`git add src/core/aigate_core_internal.h src/core/aigate_core.h src/core/aigate_core.c src/core/pipeline_chat.c tests/unit/core/test_aigate_core.c && git commit -m "feat(core): integrate prompt compressor into chat pipeline with headers and tracing"`

---

### Task 6: 管理后台 REST API 端点

**Files:**
- Modify: `src/server/admin_api.c`
- Modify: `tests/unit/server/test_admin_api.c`

- [ ] **Step 1: 编写 Admin API 单元测试**

在 `tests/unit/server/test_admin_api.c` 中添加端点测试：
```c
TEST_CASE(admin_compressor_endpoints)
{
    /* 1. GET /admin/v1/compressor/rules */
    /* 2. POST /admin/v1/compressor/rules */
    /* 3. PUT /admin/v1/compressor/rules/:id */
    /* 4. DELETE /admin/v1/compressor/rules/:id */
    /* 5. GET /admin/v1/compressor/snapshots */
    /* 6. GET /admin/v1/compressor/stats */
    ...
}
```

- [ ] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j`
预期：测试失败，返回 404 Not Found。

- [ ] **Step 3: 实现 Admin API 路由与处理器**

在 `src/server/admin_api.c` 中注册并实现：
- `GET /admin/v1/compressor/rules`：返回规则 JSON 数组；
- `POST /admin/v1/compressor/rules`：解析并保存新规则；
- `PUT /admin/v1/compressor/rules/:id`：更新已有规则；
- `DELETE /admin/v1/compressor/rules/:id`：删除规则；
- `GET /admin/v1/compressor/snapshots`：返回最近 200 条请求快照；
- `GET /admin/v1/compressor/stats`：返回全局聚合统计。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（236/236 全部通过）。

- [ ] **Step 5: 提交 Task 6 代码**

执行：`git add src/server/admin_api.c tests/unit/server/test_admin_api.c && git commit -m "feat(admin): implement REST endpoints for compressor rules, snapshots, and stats"`

---

### Task 7: Web 控制台看板与 Side-by-Side 左右双栏 Diff 抽屉

**Files:**
- Modify: `tests/unit/server/test_admin_ui.c`
- Modify: `web/admin.html`

- [ ] **Step 1: 编写 Web UI 结构断言测试**

在 `tests/unit/server/test_admin_ui.c` 中添加：
```c
TEST_ASSERT(strstr(html, "data-tab=\"compressor\"") != NULL, "missing compressor nav button");
TEST_ASSERT(strstr(html, "id=\"tab-compressor\"") != NULL, "missing tab-compressor pane");
TEST_ASSERT(strstr(html, "id=\"compressorKpiSavedTokens\"") != NULL, "missing compressorKpiSavedTokens");
TEST_ASSERT(strstr(html, "id=\"compressorKpiSavings\"") != NULL, "missing compressorKpiSavings");
TEST_ASSERT(strstr(html, "id=\"compressorKpiRatio\"") != NULL, "missing compressorKpiRatio");
TEST_ASSERT(strstr(html, "id=\"compressorKpiLatency\"") != NULL, "missing compressorKpiLatency");
TEST_ASSERT(strstr(html, "id=\"compressorRuleModal\"") != NULL, "missing compressorRuleModal");
TEST_ASSERT(strstr(html, "id=\"compressorDiffModal\"") != NULL, "missing compressorDiffModal");
```

- [ ] **Step 2: 运行测试验证失败**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：FAIL，缺少 `tab-compressor` 及其元素。

- [ ] **Step 3: 更新 `web/admin.html` 实现看板与 Diff 抽屉**

1. 侧边栏添加 `🗜️ 提示词压缩` 导航按钮 (`data-tab="compressor"`)；
2. 添加 `#tab-compressor` 主面板：
   - 4 张效益 KPI 卡片：累计节省 Token、预估节约费用、平均压缩率、平均剪枝耗时；
   - 压缩规则列表管理表格与新建弹窗 `#compressorRuleModal`；
   - 实时压缩记录数据流表格（包含时间戳、模型、原始/压缩 Token、节约徽章、查看对比按钮）；
3. 实现 Side-by-Side 左右双栏 Diff 模态框 `#compressorDiffModal`：
   - 顶部统计徽章（节省 Token 数、压缩率、处理耗时）；
   - 左栏：原始提示词（高亮显示将被修剪的冗余多轮历史与多余空白）；
   - 右栏：瘦身优化后的提示词（高亮显示保留的 System 指令、完整代码块、Tool 关联与最新 User 提问）；
4. 编写原生 JavaScript 交互函数：
   - `loadCompressorTab()`, `renderCompressorStats()`, `renderCompressorRules()`, `renderCompressorSnapshots()`, `openCompressorRuleModal()`, `openCompressorDiffModal()`。

- [ ] **Step 4: 编译并运行单元测试验证通过**

执行：`cmake --build build -j && ./build/tests/aigate_unit_tests`
预期：PASS（236/236 全部通过）。

- [ ] **Step 5: 提交 Task 7 代码**

执行：`git add web/admin.html tests/unit/server/test_admin_ui.c && git commit -m "feat(ui): add prompt compressor console with side-by-side diff modal"`

---

### Task 8: 全量回归测试、Doxygen 验证与分支合入

**Files:**
- Modify: `docs/superpowers/specs/2026-10-02-prompt-compression-and-token-pruning-design.md`

- [ ] **Step 1: 运行全量 CTest 套件与并发压测**

执行：`ctest --test-dir build --output-on-failure`
预期：6/6 全部通过（100%）。

- [ ] **Step 2: 执行 Doxygen 静态文档扫描**

执行：`doxygen Doxyfile 2>&1 | grep -i warning || true`
预期：严格 0 Warnings。

- [ ] **Step 3: 更新设计规范验收状态**

在 `docs/superpowers/specs/2026-10-02-prompt-compression-and-token-pruning-design.md` 中：
- 将状态更新为 `已实现并通过验证 (Implemented & Verified)`；
- 勾选所有验收标准复选框 `[x]`。

- [ ] **Step 4: 提交规范更新并合并至 master**

执行：
```bash
git add docs/superpowers/specs/2026-10-02-prompt-compression-and-token-pruning-design.md
git commit -m "docs(spec): mark prompt compression and token pruning as implemented and verified"
```
检出 `master` 分支并合入当前特性分支。

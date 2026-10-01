# OpenTelemetry 分布式追踪与全链路耗时瀑布图 (Distributed Tracing & Waterfall Profiling) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建符合 W3C TraceContext 与 OpenTelemetry GenAI Semantic Conventions 标准的全链路分布式追踪系统，支持毫秒级低开销异步批处理导出与控制台内置交互式耗时瀑布图。

**Architecture:** 请求生命周期内通过内嵌在 `chat_req_t` 中的 `trace_context_t`（零动态堆内存分配）记录各阶段（鉴权、风控脱敏、缓存查询、自适应选路、上游首字延迟 TTFT、流式 Chunk 聚合、出站逆向还原）单调时钟与 GenAI 语义属性；请求结束时运行智能尾部自适应采样（错误与慢请求 100% 捕获）；命中链路非阻塞推入容量 1024 的环形队列；由独立后台 Worker 线程批量（满 50 条或达 2 秒）序列化为标准 OTLP/HTTP JSON 上报远端 Collector（Jaeger / Tempo），同时在内存保留最近 500 条链路；通过 Admin REST API 与 `web/admin.html` 控制台提供交互式多轨道甘特耗时瀑布图。

**Tech Stack:** C17 (GCC/Clang), POSIX Threads & Clocks, W3C TraceContext Level 1, OpenTelemetry GenAI Semantics, Jansson JSON, CivetWeb, HTML5 / Tailwind CSS / Vanilla JS.

---

## File Structure

- **New Files:**
  - `src/observe/tracer.h`: 声明 W3C TraceContext 解析/注入、Span 打点结构与宏、自适应采样器、环形队列与后台 Worker 接口原型。
  - `src/observe/tracer.c`: 实现 W3C `traceparent` 解析与生成、微秒级单调打点、尾部自适应采样判定、无阻塞环形队列、OTLP/HTTP JSON 批处理序列化与后台上报线程、本地内存索引池。
  - `tests/unit/observe/test_tracer.c`: 覆盖 W3C 头解析/注入、Span 嵌套生命周期、自适应尾部采样、高并发环形缓冲入队出队、OTLP JSON 合规性等单元测试。
- **Modified Files:**
  - `CMakeLists.txt`: 将 `src/observe/tracer.c` 加入 `libaigate` 源码列表，将 `tests/unit/observe/test_tracer.c` 加入测试可执行文件构建列表。
  - `src/core/aigate_core_internal.h`: 在 `chat_req_t` 中内嵌 `trace_context_t trace_ctx`。
  - `src/core/aigate_core.c`: 在请求入口初始化 `trace_ctx`，解析入站 `traceparent`，在请求出口执行尾部采样并压入环形缓冲，并在清理时安全重置。
  - `src/core/pipeline_chat.c`: 在流水线关键阶段嵌入 Spans：`auth_and_limits`、`guardrails_inbound`、`cache_lookup`、`router_and_hedge`、`upstream_ttft`、`upstream_streaming`、`guardrails_outbound`，并注入标准 GenAI 属性。
  - `src/server/admin_api.c`: 新增 `GET /admin/v1/traces/:trace_id`、`GET /admin/v1/traces/config`、`PUT /admin/v1/traces/config` 路由与处理函数。
  - `tests/unit/server/test_admin_api.c`: 新增追踪管理端点的单元测试。
  - `tests/unit/run_tests.c`: 注册 `test_tracer` 专属测试用例集。
  - `web/admin.html`: 在审计日志中增加 Trace ID 徽章与「🔍 耗时瀑布图」操作，实现交互式多轨道耗时瀑布抽屉与追踪参数动态配置。

---

### Task 1: W3C TraceContext Data Structures, Parser & Injector

**Files:**
- Create: `src/observe/tracer.h`
- Create: `src/observe/tracer.c`
- Modify: `CMakeLists.txt`
- Create: `tests/unit/observe/test_tracer.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write unit tests for W3C `traceparent` parsing and Span lifecycle**

Create `tests/unit/observe/test_tracer.c`:
```c
#include "test_framework.h"
#include "observe/tracer.h"
#include <string.h>

TEST_CASE(test_w3c_traceparent_parsing)
{
    trace_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Valid W3C traceparent */
    const char* valid_header = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, valid_header) == true, "valid traceparent should parse");
    TEST_ASSERT(strcmp(ctx.trace_id, "4bf92f3577b34da6a3ce929d0e0e4736") == 0, "trace_id matches");
    TEST_ASSERT(strcmp(ctx.inbound_parent_id, "00f067aa0ba902b7") == 0, "parent_id matches");
    TEST_ASSERT(ctx.is_sampled == true, "flags 01 means sampled");

    /* Valid traceparent with sampled=00 */
    memset(&ctx, 0, sizeof(ctx));
    const char* unsampled_header = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00";
    TEST_ASSERT(tracer_parse_traceparent(&ctx, unsampled_header) == true, "unsampled traceparent should parse");
    TEST_ASSERT(ctx.is_sampled == false, "flags 00 means unsampled");

    /* Invalid formats */
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT(tracer_parse_traceparent(&ctx, "invalid-header") == false, "invalid format should fail");
    TEST_ASSERT(tracer_parse_traceparent(&ctx, "00-short-00f067aa0ba902b7-01") == false, "short trace id fails");
    TEST_ASSERT(tracer_parse_traceparent(&ctx, NULL) == false, "null header fails");

    /* Outbound format injection */
    char out_buf[128];
    tracer_format_traceparent(&ctx, "1122334455667788", out_buf, sizeof(out_buf));
    TEST_ASSERT(strncmp(out_buf, "00-4bf92f3577b34da6a3ce929d0e0e4736-1122334455667788-", 54) == 0, "formatted traceparent has valid prefix");
}

TEST_CASE(test_span_lifecycle_and_timing)
{
    trace_context_t ctx;
    tracer_context_init(&ctx, NULL, true);

    TEST_ASSERT(strlen(ctx.trace_id) == 32, "auto generated trace_id is 32 hex");
    TEST_ASSERT(strlen(ctx.root_span_id) == 16, "auto generated root_span_id is 16 hex");

    int span_idx = tracer_span_start(&ctx, "guardrails_inbound", SPAN_KIND_INTERNAL, ctx.root_span_id);
    TEST_ASSERT(span_idx >= 0, "span started successfully");
    TEST_ASSERT(ctx.span_count == 1, "span count is 1");

    tracer_span_set_attr(&ctx, "guardrails_inbound", "aigate.guardrails.pii_masked", "2");
    tracer_span_set_attr_int(&ctx, "guardrails_inbound", "gen_ai.usage.prompt_tokens", 128);

    tracer_span_end(&ctx, "guardrails_inbound", SPAN_STATUS_OK, NULL);
    TEST_ASSERT(ctx.spans[span_idx].status == SPAN_STATUS_OK, "span status is OK");
    TEST_ASSERT(ctx.spans[span_idx].end_time_ns >= ctx.spans[span_idx].start_time_ns, "monotonic end >= start");
    TEST_ASSERT(ctx.spans[span_idx].attr_count == 2, "2 attributes stored");
}
```
Register `test_w3c_traceparent_parsing` and `test_span_lifecycle_and_timing` in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because `observe/tracer.h` does not exist yet.

- [ ] **Step 3: Implement data structures, W3C parser and Span timing functions**

Create `src/observe/tracer.h`:
- Define `trace_span_t`, `trace_context_t`, `span_kind_t`, `span_status_t`.
- Declare:
  - `void tracer_context_init(trace_context_t* ctx, const char* inbound_header, bool default_sample);`
  - `void tracer_context_cleanup(trace_context_t* ctx);`
  - `bool tracer_parse_traceparent(trace_context_t* ctx, const char* header);`
  - `void tracer_format_traceparent(const trace_context_t* ctx, const char* span_id, char* buf, size_t buf_sz);`
  - `int tracer_span_start(trace_context_t* ctx, const char* name, span_kind_t kind, const char* parent_id);`
  - `void tracer_span_end(trace_context_t* ctx, const char* name, span_status_t status, const char* err_desc);`
  - `void tracer_span_set_attr(trace_context_t* ctx, const char* name, const char* key, const char* value);`
  - `void tracer_span_set_attr_int(trace_context_t* ctx, const char* name, const char* key, int64_t value);`

Create `src/observe/tracer.c`:
- Implement random 16/32-byte hex generator using cryptographically seeded pseudo-random bytes.
- Implement strict W3C `traceparent` validator: checks `version == "00"`, 32 hex chars (not all zero), 16 hex chars (not all zero), 2 hex chars flags.
- Implement `tracer_span_start` using `clock_gettime(CLOCK_MONOTONIC, ...)` and `tracer_span_end`.
- Add `src/observe/tracer.c` and `tests/unit/observe/test_tracer.c` to `CMakeLists.txt`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: All tests pass.

- [ ] **Step 5: Git commit Task 1**

Run: `git add CMakeLists.txt src/observe/ tests/unit/observe/ tests/unit/run_tests.c && git commit -m "feat(tracer): implement W3C TraceContext parsing, span lifecycle, and timing"`

---

### Task 2: Adaptive Tail-Sampling Engine & Thread-Safe Ring Buffer

**Files:**
- Modify: `src/observe/tracer.h`
- Modify: `src/observe/tracer.c`
- Modify: `tests/unit/observe/test_tracer.c`

- [ ] **Step 1: Write unit tests for tail sampling and ring buffer queue**

Add tests in `tests/unit/observe/test_tracer.c`:
```c
TEST_CASE(test_trace_tail_sampling_decision)
{
    tracer_config_t cfg = {
        .enabled = true,
        .sample_rate = 0.0, /* 0% regular sampling */
        .slow_threshold_ms = 1000
    };

    trace_context_t ctx;
    tracer_context_init(&ctx, NULL, false);
    ctx.is_sampled = false;

    /* Regular 200 OK within threshold should NOT be sampled */
    bool decision1 = tracer_should_sample(&ctx, &cfg, 200, 200);
    TEST_ASSERT(decision1 == false, "normal fast 200 is unsampled when rate is 0");

    /* HTTP 500 error MUST be tail-sampled */
    bool decision2 = tracer_should_sample(&ctx, &cfg, 500, 200);
    TEST_ASSERT(decision2 == true, "error 500 is tail-sampled");

    /* Slow query (> 1000ms) MUST be tail-sampled */
    bool decision3 = tracer_should_sample(&ctx, &cfg, 200, 1500);
    TEST_ASSERT(decision3 == true, "slow query > threshold is tail-sampled");

    /* Guardrail blocked (400) MUST be tail-sampled */
    bool decision4 = tracer_should_sample(&ctx, &cfg, 400, 50);
    TEST_ASSERT(decision4 == true, "blocked 400 is tail-sampled");
}

TEST_CASE(test_trace_ring_buffer_operations)
{
    trace_ring_buffer_t* rb = trace_ring_buffer_create(8);
    TEST_ASSERT(rb != NULL, "ring buffer created");

    trace_context_t sample_ctx;
    tracer_context_init(&sample_ctx, NULL, true);

    /* Push until full and beyond */
    for (int i = 0; i < 12; i++) {
        trace_ring_buffer_push(rb, &sample_ctx);
    }

    TEST_ASSERT(trace_ring_buffer_count(rb) == 8, "count is capped at capacity 8");
    TEST_ASSERT(trace_ring_buffer_dropped(rb) == 4, "4 overflow traces dropped without blocking");

    trace_context_t out_ctx;
    TEST_ASSERT(trace_ring_buffer_pop(rb, &out_ctx, 10) == true, "pop succeeds");
    TEST_ASSERT(trace_ring_buffer_count(rb) == 7, "count decremented to 7");

    trace_ring_buffer_destroy(rb);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because sampling decision and ring buffer functions are not yet defined.

- [ ] **Step 3: Implement adaptive tail-sampling and ring buffer**

In `src/observe/tracer.h`:
- Define `tracer_config_t`:
  ```c
  typedef struct {
      bool        enabled;
      double      sample_rate;       /* 0.0 to 1.0 */
      uint32_t    slow_threshold_ms; /* default 2000 */
      char        otlp_endpoint[256];/* e.g. "http://localhost:4318/v1/traces" */
  } tracer_config_t;
  ```
- Declare `bool tracer_should_sample(const trace_context_t* ctx, const tracer_config_t* cfg, int http_status, uint64_t elapsed_ms);`
- Declare `trace_ring_buffer_t` type and functions: `create`, `push`, `pop`, `destroy`, `count`, `dropped`.

In `src/observe/tracer.c`:
- Implement `tracer_should_sample`:
  - Check `ctx->is_sampled` (already sampled by inbound W3C or head sampling).
  - Check `http_status >= 400`.
  - Check `elapsed_ms >= cfg->slow_threshold_ms`.
  - Check random probability against `cfg->sample_rate`.
- Implement `trace_ring_buffer_t` using a fixed-capacity circular array protected by `pthread_mutex_t` and `pthread_cond_t`.
  - When buffer is full on push: overwrite the oldest slot (`head = (head + 1) % cap`), increment `dropped_count`, signal consumers.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: All tests pass.

- [ ] **Step 5: Git commit Task 2**

Run: `git add src/observe/ tests/unit/observe/ && git commit -m "feat(tracer): implement tail-sampling engine and ring buffer queue"`

---

### Task 3: Pipeline Instrumentation & GenAI Semantic Conventions Timing

**Files:**
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `tests/unit/core/test_aigate_core.c`

- [ ] **Step 1: Write integration tests verifying Spans generated during pipeline execution**

In `tests/unit/core/test_aigate_core.c`:
- Verify that a mocked chat request produces a populated `trace_context_t` with `root`, `auth_and_limits`, `guardrails_inbound`, `upstream_ttft` Spans.
- Verify that W3C `traceparent` is injected into upstream request headers.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`

- [ ] **Step 3: Embed `trace_context_t` into `chat_req_t` and instrument pipeline**

In `src/core/aigate_core_internal.h`:
```c
#include "observe/tracer.h"
...
struct chat_req {
    ...
    trace_context_t trace_ctx;
};
```

In `src/core/aigate_core.c`:
- In `aigate_handle_chat()`:
  - Extract `traceparent` from CivetWeb incoming HTTP headers.
  - Call `tracer_context_init(&req->trace_ctx, header_val, default_sample_rate)`.
  - Start `root` Span.
  - Start and end `auth_and_limits` Span around API key validation & quota check.
- When sending to upstream in `upstream_request()`:
  - Format `traceparent` with child span ID and inject into upstream request headers.
- In `chat_req_cleanup()`:
  - If `tracer_should_sample(...)` is true: push copy of `req->trace_ctx` into gateway global `trace_ring_buffer`.
  - Call `tracer_context_cleanup(&req->trace_ctx)`.

In `src/core/pipeline_chat.c`:
- Surround inbound guardrail checks with `tracer_span_start(&req->trace_ctx, "guardrails_inbound", ...)` and `tracer_span_end(...)`.
- Surround response cache lookup with `tracer_span_start(&req->trace_ctx, "cache_lookup", ...)`.
- Surround router selection with `tracer_span_start(&req->trace_ctx, "router_and_hedge", ...)`.
- On first chunk received in SSE / streaming: end `upstream_ttft` Span and record `aigate.latency.ttft_ms`, start `upstream_streaming` Span.
- On stream completion: end `upstream_streaming` Span.
- Surround outbound PII restoration with `tracer_span_start(&req->trace_ctx, "guardrails_outbound", ...)`.
- Set standard OpenTelemetry GenAI attributes: `gen_ai.system`, `gen_ai.request.model`, `gen_ai.usage.prompt_tokens`, `gen_ai.usage.completion_tokens`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% test pass.

- [ ] **Step 5: Git commit Task 3**

Run: `git add src/core/ tests/unit/core/ && git commit -m "feat(core): instrument request pipeline with OpenTelemetry GenAI spans and TTFT"`

---

### Task 4: OTLP/HTTP JSON Serializer & Background Exporter Worker

**Files:**
- Modify: `src/observe/tracer.h`
- Modify: `src/observe/tracer.c`
- Modify: `tests/unit/observe/test_tracer.c`

- [ ] **Step 1: Write unit tests for OTLP/HTTP JSON serialization**

In `tests/unit/observe/test_tracer.c`:
```c
TEST_CASE(test_otlp_json_serialization)
{
    trace_context_t ctx;
    tracer_context_init(&ctx, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", true);

    int s = tracer_span_start(&ctx, "upstream_ttft", SPAN_KIND_CLIENT, ctx.root_span_id);
    tracer_span_set_attr(&ctx, "upstream_ttft", "gen_ai.system", "openai");
    tracer_span_set_attr(&ctx, "upstream_ttft", "gen_ai.request.model", "gpt-4o");
    tracer_span_end(&ctx, "upstream_ttft", SPAN_STATUS_OK, NULL);

    char* json_str = tracer_serialize_otlp_json(&ctx, 1);
    TEST_ASSERT(json_str != NULL, "json string produced");

    /* Validate that JSON parses cleanly with Jansson */
    json_error_t jerr;
    json_t* root = json_loads(json_str, 0, &jerr);
    TEST_ASSERT(root != NULL, "valid json document");

    json_t* res_spans = json_object_get(root, "resourceSpans");
    TEST_ASSERT(json_is_array(res_spans) && json_array_size(res_spans) > 0, "contains resourceSpans array");

    json_decref(root);
    free(json_str);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`

- [ ] **Step 3: Implement OTLP/HTTP JSON serialization & exporter worker thread**

In `src/observe/tracer.h`:
- Declare `char* tracer_serialize_otlp_json(const trace_context_t* traces, int count);`
- Declare `tracer_manager_t* tracer_manager_create(const tracer_config_t* cfg);`
- Declare `void tracer_manager_destroy(tracer_manager_t* tm);`
- Declare `const trace_context_t* tracer_manager_lookup_trace(tracer_manager_t* tm, const char* trace_id);`

In `src/observe/tracer.c`:
- Implement `tracer_serialize_otlp_json`:
  - Build `resourceSpans` -> `resource` (`service.name: aigate`, `service.version`).
  - Build `scopeSpans` -> `spans` array:
    - `traceId`, `spanId`, `parentSpanId`, `name`, `kind`, `startTimeUnixNano`, `endTimeUnixNano`.
    - `attributes` array of `{ key, value: { stringValue / intValue } }`.
    - `status: { code: 1 (OK) or 2 (ERROR) }`.
- Implement recent traces cache: a circular array of 500 `trace_context_t` indexed by `trace_id` for instant query via Admin API.
- Implement `trace_exporter_worker` thread:
  - Loop while running:
    - Wait on condition variable with timeout (2000ms).
    - Pop up to 50 traces from ring buffer.
    - Store in local recent traces cache.
    - If `otlp_endpoint[0] != '\0'`:
      - Serialize to OTLP JSON.
      - Perform HTTP POST to endpoint.
      - On failure: log throttled warning, exponential backoff (1s, 2s, 4s, max 30s).
    - Free serialized JSON.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% test pass.

- [ ] **Step 5: Git commit Task 4**

Run: `git add src/observe/ tests/unit/observe/ && git commit -m "feat(tracer): implement OTLP/HTTP JSON serialization and background batch worker"`

---

### Task 5: Admin REST APIs for Tracing Configuration & Span Query

**Files:**
- Modify: `src/server/admin_api.c`
- Modify: `tests/unit/server/test_admin_api.c`

- [ ] **Step 1: Write unit tests for `/admin/v1/traces/:trace_id` and `/admin/v1/traces/config`**

In `tests/unit/server/test_admin_api.c`:
- Test `GET /admin/v1/traces/config` returns default config JSON.
- Test `PUT /admin/v1/traces/config` updates `sample_rate`, `slow_threshold_ms`, `otlp_endpoint`.
- Test `GET /admin/v1/traces/non_existent_id` returns 404.
- Test `GET /admin/v1/traces/:trace_id` returns 200 with full spans array, durations, and attributes when trace exists in cache.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`

- [ ] **Step 3: Implement tracing Admin API endpoints**

In `src/server/admin_api.c`:
- Implement `admin_traces_config_get()`:
  - Returns `{ "enabled": bool, "sample_rate": double, "slow_threshold_ms": int, "otlp_endpoint": string, "buffered_count": int, "dropped_count": int }`.
- Implement `admin_traces_config_put()`:
  - Parses JSON payload, updates global `tracer_config_t`.
- Implement `admin_trace_get_by_id()`:
  - Parses `:trace_id` from URL path `/admin/v1/traces/<trace_id>`.
  - Searches local recent traces cache via `tracer_manager_lookup_trace()`.
  - If not found: return 404 (`trace_not_found`).
  - If found: return full trace JSON:
    ```json
    {
      "trace_id": "...",
      "root_span_id": "...",
      "is_sampled": true,
      "total_duration_ms": 142.5,
      "spans": [
        {
          "span_id": "...",
          "parent_span_id": "...",
          "name": "upstream_ttft",
          "kind": "client",
          "start_offset_ms": 2.1,
          "duration_ms": 120.3,
          "status": "ok",
          "attributes": {
            "gen_ai.system": "openai",
            "gen_ai.request.model": "gpt-4o",
            "aigate.latency.ttft_ms": 120.3
          }
        }
      ]
    }
    ```
- Register routes in `admin_api_handle_request()`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: 100% test pass.

- [ ] **Step 5: Git commit Task 5**

Run: `git add src/server/admin_api.c tests/unit/server/test_admin_api.c && git commit -m "feat(admin): add trace query and config REST API endpoints"`

---

### Task 6: Web Console Interactive Waterfall Timeline UI

**Files:**
- Modify: `web/admin.html`

- [ ] **Step 1: Add Trace ID badges and waterfall inspection buttons in Audit Log**

In `web/admin.html`:
- In the Audit records table (`#tab-audit`), render a clickable `Trace ID` badge (`#4bf92f...`) next to Request ID.
- Add an action button in each audit row: `🔍 耗时瀑布图` (`onclick="openTraceWaterfallModal('${r.trace_id}')"`).

- [ ] **Step 2: Implement Interactive Waterfall Timeline Modal/Drawer**

In `web/admin.html`:
- Create modal `#traceWaterfallModal`:
  - **Header**:
    - Trace ID display with copy button.
    - Badges for Model, Status (HTTP 200 / 4xx / 5xx), and Total Duration.
  - **Summary Metrics Strip**:
    - `端到端总耗时`: e.g. `245.8 ms`
    - `首字延迟 (TTFT)`: e.g. `180.2 ms` (琥珀金高亮)
    - `输入/输出 Tokens`: e.g. `128 / 340`
    - `Span 节点数`: e.g. `8 阶段`
  - **Gantt Waterfall Timeline**:
    - Ruler track at top: `0ms` ... `N ms`.
    - Horizontal bars for each span:
      - Relative start offset: `left: (span.start_offset_ms / total_ms * 100)%`
      - Width: `width: Math.max((span.duration_ms / total_ms * 100), 1)%`
      - Distinct color styling per stage:
        - `auth_and_limits`: Sky blue
        - `guardrails_inbound`: Purple
        - `cache_lookup`: Teal
        - `router_and_hedge`: Cyan
        - `upstream_ttft`: Amber gold
        - `upstream_streaming`: Emerald green
        - `guardrails_outbound`: Violet
    - Clicking any span bar toggles an inline attributes inspector table showing all key-value tags.

- [ ] **Step 3: Implement Tracing Config Panel**

In `web/admin.html`:
- In `#tab-metrics` or inside a dedicated settings section:
  - Add "全链路追踪与 OTLP 导出配置" card.
  - Controls:
    - OTLP Endpoint URL input (e.g. `http://localhost:4318/v1/traces`).
    - Sample Rate slider / input (`0% ~ 100%`).
    - Slow Request Threshold input (`ms`).
    - "保存追踪配置" button calling `PUT /admin/v1/traces/config`.

- [ ] **Step 4: Implement JavaScript functions**

In `<script>` of `web/admin.html`:
- `openTraceWaterfallModal(traceId)`:
  - Fetches `GET /admin/v1/traces/${traceId}`.
  - Calculates relative start times and percentage widths.
  - Dynamically renders the waterfall tracks and span cards.
- `renderSpanAttributes(span)`: formats attributes into a clean key-value table.
- `fetchTraceConfig()` and `saveTraceConfig()`.

- [ ] **Step 5: Build and verify embedded HTML compilation**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: `admin_ui_html.h` is regenerated automatically and all tests pass.

- [ ] **Step 6: Git commit Task 6**

Run: `git add web/admin.html && git commit -m "feat(web): add interactive trace waterfall timeline and OTLP settings in web console"`

---

### Task 7: Full System Verification, Doxygen Audit & Master Merge

**Files:**
- Modify: `docs/superpowers/specs/2026-10-01-opentelemetry-distributed-tracing-design.md`

- [ ] **Step 1: Run complete CTest suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: 100% pass across all test suites.

- [ ] **Step 2: Audit Doxygen documentation warnings**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: Strictly `0` warnings. Fix any missing Doxygen comments in newly added header files.

- [ ] **Step 3: Update specification document status**

In `docs/superpowers/specs/2026-10-01-opentelemetry-distributed-tracing-design.md`:
- Change status to `- **状态**: 已实现并全量验证通过 (Implemented & Verified)`.
- Mark all acceptance criteria checkboxes as checked `[x]`.

- [ ] **Step 4: Git commit specification update and verify clean working tree**

Run: `git add docs/superpowers/specs/ && git commit -m "docs(spec): mark OpenTelemetry distributed tracing spec as implemented and verified"`
Run: `git status` -> clean working tree.

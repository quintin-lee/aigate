# 高性能响应缓存引擎实施计划 (Response Cache Engine Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 aigate 构建高性能分段锁内存 LRU 响应缓存引擎，支持规范化 SHA-256 请求指纹、流式与非流式全双向贯通回放、管理端可观测性与清空 API，并在运维大屏实时可视化呈现缓存命中。

**Architecture:** 
1. **分片 LRU 缓存核 (`src/response_cache.{c,h}`)**：16 分片哈希表（独立互斥锁），双向链表 LRU，128MB / 20,000 条目配额限制与自动淘汰，可选 Redis 适配接口；
2. **规范化指纹计算器**：提取 Model、Messages、System Prompt、Tools、Temperature 并生成唯一 SHA-256 Key，支持 `Cache-Control: no-cache` 绕过；
3. **数据面流式/非流式集成 (`src/aigate_core.c`)**：
   - 非流式：命中时 0ms 注入 `X-Cache: HIT` 返回完整 JSON，未命中时后置写入；
   - 流式 (SSE)：命中时平滑回放分块增量 SSE 事件，未命中透传时后台累加器组装入库；
4. **管理 API 与运维控制台 (`src/admin_api.c`, `web/admin.html`)**：`GET /admin/v1/cache/stats`、`POST /admin/v1/cache/purge`，实时大屏 KPI 卡片与瀑布流 `[CACHE HIT]` 标记。

**Tech Stack:** C17, CivetWeb 1.16, OpenSSL (SHA256), Jansson, POSIX Threads, Tailwind CSS, Pytest.

---

## 文件影响范围映射

- **新增核心源文件：**
  - `src/response_cache.h`, `src/response_cache.c`: 分片 LRU 缓存与指纹计算核心实现。
  - `tests/unit/test_response_cache.c`: 缓存模块独立单元测试。
- **修改现有核心源文件：**
  - `CMakeLists.txt`: 将 `response_cache.c` 加入 `libaigate`。
  - `src/aigate_core.h`, `src/aigate_core.c`: 数据面挂载缓存逻辑（非流式命中、流式回放、流式累加入库）。
  - `src/admin_api.h`, `src/admin_api.c`: 新增 `cache/stats` 与 `cache/purge` 管理端点。
  - `src/transport_civetweb.c`: 注入 `response_cache` 句柄至 `admin_ctx_t`。
  - `src/main.c`: 网关启动时初始化缓存管理器，优雅关机时安全释放。
  - `tests/unit/run_tests.c`: 注册 `test_response_cache` 测试套件。
- **修改前端单页源文件：**
  - `web/admin.html`: 实时大屏增加缓存命中率 KPI 卡、瀑布流 `[CACHE HIT]` 徽章、缓存管理面板。
- **自动化测试文件：**
  - `tests/integration/test_gateway.py`: 增加端到端精确缓存、流式互通、绕过与清除测试用例。

---

## 任务拆分列表

### Task 1: 实现核心响应缓存引擎 (`src/response_cache.{c,h}`) 与单元测试

**Files:**
- Create: `src/response_cache.h`
- Create: `src/response_cache.c`
- Create: `tests/unit/test_response_cache.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写 `src/response_cache.h` 接口定义**
  - 定义 `cache_entry_t` 结构体（`cache_key`, `model`, `response_body`, `response_len`, `status_code`, `prompt_tokens`, `completion_tokens`, `cost_usd`, `expires_at`, 双向链表指针）。
  - 定义 `response_cache_stats_t` 结构体（`hits`, `misses`, `entries_count`, `bytes_used`, `saved_tokens`, `saved_cost`）。
  - 声明操作函数：
    - `response_cache_t* response_cache_new(size_t max_bytes, size_t max_entries, long default_ttl_sec);`
    - `void response_cache_free(response_cache_t* rc);`
    - `int response_cache_fingerprint(const char* model, const char* json_body, size_t body_len, char out_key[65]);`
    - `cache_entry_t* response_cache_get(response_cache_t* rc, const char* key);`
    - `void response_cache_release_entry(cache_entry_t* entry);`
    - `int response_cache_set(response_cache_t* rc, const char* key, const char* model, const char* body, size_t len, long prompt_tokens, long completion_tokens, double cost_usd, long ttl_sec);`
    - `int response_cache_purge(response_cache_t* rc, const char* model_or_null, size_t* out_purged_entries, size_t* out_freed_bytes);`
    - `char* response_cache_get_stats_json(response_cache_t* rc);`

- [ ] **Step 2: 编写 `src/response_cache.c` 实现**
  - 实现 16 分片哈希表与 `pthread_mutex_t`；
  - 实现规范化请求指纹 `response_cache_fingerprint`：使用 Jansson 解析提取 `model`, `messages`, `system`, `tools`, `temperature`，经排序格式化后使用 OpenSSL `SHA256()` 生成 64 字节十六进制哈希；
  - 实现基于双向链表的 LRU 插入、访问移至表头 (MRU) 与超额淘汰 (Evict LRU)；
  - 实现安全并发引用计数或读复制机制，保证获取条目时不被其他线程并发逐出导致野指针；
  - 实现 TTL 判定与过期条目就地清理；
  - 实现按模型清空与全量清空 `response_cache_purge`；
  - 实现全局原子统计数据累加与 `response_cache_get_stats_json` 序列化。

- [ ] **Step 3: 将 `src/response_cache.c` 注册到 `CMakeLists.txt`**
  - 在 `add_library(libaigate ...)` 源文件列表中加入 `src/response_cache.c`。

- [ ] **Step 4: 编写 `tests/unit/test_response_cache.c` 单元测试**
  - 测试 1: 指纹一致性与微小空白无关性；
  - 测试 2: 基本 Set / Get / TTL 过期验证；
  - 测试 3: LRU 容量超额逐出验证；
  - 测试 4: Purge 按模型清除与全量清除；
  - 测试 5: 16 线程高并发存取压力测试（数据竞争与并发安全检查）。

- [ ] **Step 5: 在 `tests/unit/run_tests.c` 注册并运行单元测试**
  - 编译并执行 `ctest --test-dir .build -R unit --output-on-failure` 验证 100% 通过。

- [ ] **Step 6: Commit Task 1**
  ```bash
  git add CMakeLists.txt src/response_cache.h src/response_cache.c tests/unit/test_response_cache.c tests/unit/run_tests.c
  git commit -m "feat(cache): ⚡ implement sharded LRU response cache engine with canonical fingerprinting"
  ```

---

### Task 2: 实现 Admin REST API 缓存统计与清空接口

**Files:**
- Modify: `src/admin_api.h`
- Modify: `src/admin_api.c`
- Modify: `src/transport_civetweb.c`
- Modify: `tests/unit/test_admin_api.c`

- [ ] **Step 1: 在 `admin_ctx_t` 中持有 `response_cache_t* rc`**
  - 在 `src/admin_api.h` 的 `admin_ctx_t` 结构体中添加 `response_cache_t* rc;`。
  - 在 `src/transport_civetweb.c` 中初始化 `cw->adm.rc = ac->rc;`。

- [ ] **Step 2: 在 `src/admin_api.c` 中增加路由分发**
  - 路由 `GET /admin/v1/cache/stats` -> 调用 `response_cache_get_stats_json(adm->rc)` 返回 200 JSON；
  - 路由 `POST /admin/v1/cache/purge` -> 解析可选 `{"model": "..."}` 参数，调用 `response_cache_purge`，返回 `{ "purged_entries": N, "freed_bytes": B, "model": "..." }`。

- [ ] **Step 3: 在 `tests/unit/test_admin_api.c` 补充单元测试**
  - 验证未经鉴权返回 401；
  - 验证正确 Admin Token 访问 `GET /admin/v1/cache/stats` 返回包含 `hit_rate_percent`, `bytes_used` 等字段；
  - 验证调用 `POST /admin/v1/cache/purge` 清理成功并返回 200。

- [ ] **Step 4: 编译并执行单元测试**
  - `cmake --build .build -j$(nproc)`
  - `ctest --test-dir .build -R unit --output-on-failure`

- [ ] **Step 5: Commit Task 2**
  ```bash
  git add src/admin_api.h src/admin_api.c src/transport_civetweb.c tests/unit/test_admin_api.c
  git commit -m "feat(admin): 📊 expose /admin/v1/cache/stats and /admin/v1/cache/purge endpoints"
  ```

---

### Task 3: 数据面非流式请求缓存命中与自动写入集成

**Files:**
- Modify: `src/aigate_core.h`
- Modify: `src/aigate_core.c`
- Modify: `src/main.c`

- [ ] **Step 1: 在 `aigate_core_t` 中管理 `response_cache_t* rc` 的生命周期**
  - `aigate_core_init` 中创建 `ac->rc = response_cache_new(...)`；
  - `aigate_core_free` 中调用 `response_cache_free(ac->rc)`。
  - 启动与关机日志打印缓存初始化状态。

- [ ] **Step 2: 在 `handle_responses` 与 `handle_chat_completions` 等处理函数前置注入读缓存逻辑**
  - 检查客户端请求头：若包含 `Cache-Control: no-cache`、`max-age=0` 或 `X-Skip-Cache: true`，标记 `bypass_cache = true`；
  - 计算规范化指纹 `response_cache_fingerprint`；
  - 若 `!bypass_cache`：
    - 调用 `response_cache_get`；
    - 若命中：
      - 设置响应头 `X-Cache: HIT`、`X-Cache-Lookup-Time: 0.1ms`、`Age: <secs>`；
      - 设置 `Content-Type: application/json`；
      - 直接调用 `aigate_write_json(rc, 200, entry->response_body, entry->response_len)`；
      - 触发 `record_usage_and_event`（耗时传 0，provider 设为 `"cache"`），向事件总线广播 `cached: true`；
      - 释放条目引用并直接 `return 0`，完全不打扰上游！
  - 若未命中：
    - 设置响应头 `X-Cache: MISS`。

- [ ] **Step 3: 在非流式请求正常返回 200 时后置写入缓存**
  - 检查客户端是否携带 `Cache-Control: no-store`；
  - 若未禁用存储且状态码为 200，且响应字节数不超过 1MB：
    - 从解析出的 Usage 提取 `prompt_tokens`, `completion_tokens`, 计算 `cost_usd`；
    - 调用 `response_cache_set(ac->rc, key, model, ubody, ulen, ptok, ctok, cost, 3600)` 写入缓存。

- [ ] **Step 4: 编译并运行单元测试验证**
  - `cmake --build .build -j$(nproc)`
  - `ctest --test-dir .build -R unit --output-on-failure`

- [ ] **Step 5: Commit Task 3**
  ```bash
  git add src/aigate_core.h src/aigate_core.c src/main.c
  git commit -m "feat(core): ⚡ integrate non-streaming exact response cache lookup and storage"
  ```

---

### Task 4: 数据面流式 (SSE) 缓存平滑回放与异步累加组装入库

**Files:**
- Modify: `src/aigate_core.c`

- [ ] **Step 1: 实现流式缓存命中回放器 (`cache_stream_replay`)**
  - 当 `stream: true` 且命中 `cache_entry_t` 时：
    - 设置响应状态码 200；
    - 注入响应头：
      - `Content-Type: text/event-stream; charset=utf-8`
      - `Cache-Control: no-cache`
      - `Connection: keep-alive`
      - `Transfer-Encoding: chunked`
      - `X-Cache: HIT`
    - 解析缓存中的 JSON，提取 `id`, `model`, `content` 文本；
    - 分块平滑推送：按标点或 32 字符步长构造标准 SSE delta 格式事件，调用 `mg_send_chunk` 发送；
    - 发送携带真实 `usage` 的流结束事件以及 `data: [DONE]\n\n`；
    - 发送 `mg_send_chunk(conn, "", 0)` 完成流式关闭；
    - 发布事件至 `event_bus`（`cached: true`, `latency: 0`）。

- [ ] **Step 2: 实现流式透传时的异步累加组装入库器 (`stream_cache_accumulator`)**
  - 在 `cw_stream_chunk_cb`（或对应的流处理上下文）中：
    - 分配累加缓冲区（限制最大 512KB）；
    - 边透传 chunk 边解析并累加 `delta.content`，记录最后的 `usage`；
    - 当流正常关闭且状态码为 200 时，由组装器构造完整 OpenAI 响应 JSON；
    - 异步调用 `response_cache_set` 入库，使随后的流式或非流式请求立即享用缓存！

- [ ] **Step 3: 编译并运行单元测试**
  - `cmake --build .build -j$(nproc)`
  - `ctest --test-dir .build -R unit --output-on-failure`

- [ ] **Step 4: Commit Task 4**
  ```bash
  git add src/aigate_core.c
  git commit -m "feat(stream): 🌊 support smooth SSE stream cache replay and streaming chunk accumulator"
  ```

---

### Task 5: Web 控制台大屏与管理面板联动 (`web/admin.html`)

**Files:**
- Modify: `web/admin.html`
- Compile: `admin_ui_html.h`

- [ ] **Step 1: 实时大屏 (`#tab-live`) 增加缓存可观测性**
  - 顶部指标栏增加 **⚡ 缓存命中率** KPI 卡片（`#liveStatCacheHitRate`），显示实时命中率与已节省费用；
  - 实时请求瀑布流中：当事件携带 `cached: true` 时，状态码列旁边显示绿底白字 `[CACHE HIT]` 徽章，耗时显示绿色 `<1ms`，Provider 列显示 `cache`；
  - 点击行详情弹出框，展示缓存命中标记与节约的成本估算。

- [ ] **Step 2: 导航与管理面板集成**
  - 侧边栏/设置面板新增缓存管理卡片：
    - 展示内存占用仪表盘（例如 `14.2 MB / 128 MB`）、条目总数（`1,420 条`）、累计节约 Token 与成本；
    - 提供「🧹 一键清空所有缓存」按钮（带防误触确认弹窗）；
    - 提供按模型筛选清空输入框与执行按钮。
  - JS 绑定 `fetchCacheStats()` 与 `triggerCachePurge()`。

- [ ] **Step 3: 编译并重新生成内嵌头文件**
  - `cmake --build .build -j$(nproc)`

- [ ] **Step 4: Commit Task 5**
  ```bash
  git add web/admin.html
  git commit -m "feat(ui): 🖥️ add cache hit metrics to live ops dashboard and cache purge controls"
  ```

---

### Task 6: 端到端自动化集成测试与全量回归

**Files:**
- Modify: `tests/integration/test_gateway.py`

- [ ] **Step 1: 编写非流式精确匹配与命中测试**
  - `test_response_cache_exact_hit`:
    - 发起第一次请求，断言响应头 `X-Cache: MISS`，上游 Mock 记录请求；
    - 发起第二次完全相同请求，断言响应头 `X-Cache: HIT`，状态码 200，上游 Mock 依然只有 1 次请求（未穿透）；
    - 验证响应 JSON 内容完全一致。

- [ ] **Step 2: 编写缓存穿透控制测试**
  - `test_response_cache_bypass`:
    - 携带 `headers={"Cache-Control": "no-cache"}` 发起相同请求；
    - 断言请求穿透到上游 Mock，Mock 请求数增加。

- [ ] **Step 3: 编写流式与非流式双向贯通测试**
  - `test_response_cache_streaming_dual_replay`:
    - 先非流式请求写入缓存；
    - 以 `stream=True` 发起相同请求，验证客户端以 SSE 格式收到完整 chunks 流、`delta` 增量内容及 `[DONE]` 结束符；
    - 再反向验证：先流式请求写入缓存，后续非流式请求成功命中返回完整 JSON。

- [ ] **Step 4: 编写管理端 Stats 与 Purge 测试**
  - `test_admin_cache_stats_and_purge`:
    - `GET /admin/v1/cache/stats` 断言 `hits_total >= 1`，`entries_count >= 1`；
    - `POST /admin/v1/cache/purge` 清理全量；
    - 再次请求该内容，断言响应为 `X-Cache: MISS`。

- [ ] **Step 5: 运行全量测试套件验证交付**
  - C 单元测试：`ctest --test-dir .build -R unit --output-on-failure`（预期 100% 通过）；
  - Python E2E 测试：`pytest tests/integration/test_gateway.py -v`（预期 38+ 项全绿通过）。

- [ ] **Step 6: Commit Task 6**
  ```bash
  git add tests/integration/test_gateway.py
  git commit -m "test(cache): 🧪 add end-to-end integration tests for response caching engine"
  ```

---

## 阶段验收与质量准则

1. **测试覆盖**：
   - C 单元测试覆盖分片哈希、LRU 链表超额淘汰、TTL 清理、多线程并发安全无竞态；
   - Python E2E 测试覆盖真正 HTTP 客户端的非流式与流式双向命中、Header 穿透与控制台 Purge。
2. **零编译告警**：
   - 严格遵循 C17 `-Wall -Wextra -Werror` 标准，无未初始化变量与内存泄漏。
3. **单二进制零依赖**：
   - 保持 aigate 启动即用特性，默认纯内存高性能运行，无外部 Redis 强绑。

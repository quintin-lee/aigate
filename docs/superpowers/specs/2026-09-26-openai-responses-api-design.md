# `/v1/responses` 端点设计（OpenAI Responses API 透传）

**日期**：2026-09-26  
**状态**：待实现  
**范围**：全协议覆盖（文本 + 工具调用 + 内置工具 + `previous_response_id` 服务端状态）；严格 OpenAI 系透传，非 openai provider 返回 400。

---

## §1 背景与范围

OpenAI Responses API（`POST /v1/responses`）正在成为客户端生态的新默认端点，逐步取代 `chat/completions`。其核心新增能力：

- `input` 可为字符串或结构化消息数组（含 image、file 内容项）
- 内置工具（`web_search_preview`、`code_interpreter`、`file_search`）
- `previous_response_id`：服务端维护多轮对话状态（由 OpenAI 平台托管）
- `reasoning_tokens`：Chain-of-Thought 推理 token 计量
- SSE 事件类型与 `chat/completions` 不同（`response.created` / `response.output_text.delta` / `response.completed` 等）

**不在范围内**（首版）：
- 跨后端转换（Anthropic / Gemini 不支持 Responses API 语义）
- `DELETE /v1/responses/{id}`（对话状态删除）
- `GET /v1/responses/{id}`（状态查询）
- 文件上传端点

---

## §2 整体架构

```
客户端  POST /v1/responses
         │
         ▼
aigate_core.c — handle_responses()
  ├─ auth_key_resolve()                    ← 与 chat 完全相同
  ├─ rl_allow_request()                    ← QPS 门（相同）
  ├─ rl_remaining_daily()                  ← 配额预检（相同）
  ├─ model_router_resolve()                ← 解析 body.model 字段
  ├─ provider 类型检查                     ← ★ 新增
  │     └─ route.provider != "openai" → 400 unsupported_endpoint
  ├─ model_router_select_candidates()      ← 选目标（相同）
  ├─ cb_allow_request()                    ← 熔断门（相同）
  ├─ provider_openai_build_responses()     ← ★ 新增
  ├─ upstream_call_ext()                   ← 共用，含 SSE 流式回调
  │     └─ sse_callback_responses()        ← ★ 新增：识别 response.completed
  ├─ provider_openai_parse_responses_usage() ← ★ 新增
  ├─ cb_record_success / cb_record_failure ← 相同
  └─ rl_reserve_tokens()                   ← 相同
```

**关键约束：**
- `handle_responses()` 是独立函数，与 `handle_chat()` 平行，不共用主体逻辑（避免嵌套 if-else 污染热路径）
- 所有现有中间件（鉴权、限流、配额、熔断、计量）**零修改**，直接复用
- provider 类型检查在路由解析完成后、发起上游请求前执行；非 openai 系不发起任何网络请求

---

## §3 Provider 适配器（`src/provider_openai.c`）

### 3.1 `provider_openai_build_responses()`

```c
int provider_openai_build_responses(
    const upstream_target_t* target,
    const void*              body,
    size_t                   body_len,
    upstream_request_t*      out);
```

**行为：**
- 目标 URL：`{target.endpoint}/responses`（追加 `/responses`，与 `build_chat` 追加 `/chat/completions` 一致）
- Headers：`Authorization: Bearer {resolved_key}` + `Content-Type: application/json`
- Body：**原样转发**，不解析 `input` / `tools` / `previous_response_id` 等任何字段
- provider key 解密：与 `build_chat` 相同（`secrets_decrypt` + `master_key`）

### 3.2 `provider_openai_parse_responses_usage()`

```c
int provider_openai_parse_responses_usage(
    const char* body,
    size_t      len,
    long*       out_input_tokens,
    long*       out_output_tokens,
    long*       out_cached_tokens,
    long*       out_reasoning_tokens);
```

**非流式**：从顶层响应 JSON 提取：
- `usage.input_tokens` → `out_input_tokens`
- `usage.output_tokens` → `out_output_tokens`
- `usage.input_tokens_details.cached_tokens` → `out_cached_tokens`
- `usage.output_tokens_details.reasoning_tokens` → `out_reasoning_tokens`

**流式 SSE 回调** (`sse_callback_responses`)：
- 每帧解析 `event:` 类型
- 找到 `type: "response.completed"` 事件后，从嵌套 `response.usage` 字段提取上述四项
- 其余事件帧直接写回客户端，**不做任何解析或修改**
- 若流结束未见 `response.completed`：usage 四项全记为 0，写 `AIGATE_LOG_WARN`

---

## §4 Schema 变更

在现有迁移版本（v6 或 v7）追加一列，不影响已有行：

```sql
ALTER TABLE usage_requests
    ADD COLUMN IF NOT EXISTS reasoning_tokens BIGINT NOT NULL DEFAULT 0;
```

**`usage_requests` 完整字段：**

| 列 | 类型 | 说明 |
|---|---|---|
| `id` | BIGSERIAL PK | |
| `key_id` | BIGINT | API key |
| `model` | TEXT | 请求的 model 名 |
| `provider` | TEXT | 路由到的 provider（如 `openai`） |
| `endpoint` | TEXT | 上游完整 URL |
| `http_status` | INT | 上游响应 HTTP 状态码 |
| `prompt_tokens` | BIGINT | `input_tokens` |
| `completion_tokens` | BIGINT | `output_tokens` |
| `cached_tokens` | BIGINT | 缓存命中 token |
| `reasoning_tokens` | BIGINT | **新增**，CoT 推理 token（非 Responses API 请求记 0） |
| `latency_ns` | BIGINT | 端到端耗时（ns） |
| `ts` | TIMESTAMPTZ | 请求时间 |

**`usage_daily` 不变**：Responses API token 通过 `rl_reserve_tokens` 计入同一日配额桶，无需新增聚合列。

---

## §5 端点注册与路由

### `src/transport_civetweb.c`
新增 URI 匹配规则：`POST /v1/responses` → `handle_responses()`。

### `src/aigate_core.c`
新增函数 `handle_responses()`，在主分发函数 `aigate_handle_request()` 的路径匹配块中新增分支：

```c
if (strcmp(rq->path, "/v1/responses") == 0) {
    return handle_responses(ac, rq, rc);
}
```

### Provider 类型检查
`model_rec_t.provider` 字段已存在。检查逻辑：

```c
if (strcmp(route.provider, "openai") != 0) {
    aigate_write_error(rc, 400, "unsupported_endpoint",
        "/v1/responses requires an openai-compatible provider");
    return 0;
}
```

---

## §6 错误处理矩阵

| 场景 | HTTP 状态 | error.code | 备注 |
|---|---|---|---|
| `model` 字段缺失或为空 | 400 | `model_not_found` | 与 chat 相同 |
| model 路由到非 openai provider | 400 | `unsupported_endpoint` | **不发上游请求** |
| 上游返回 4xx（含无效 `previous_response_id`） | 原样透传 | — | 不触发熔断 |
| 上游返回 5xx / 超时 | 502/504 | — | 单跳重试，触发熔断计数 |
| 流式中缺失 `response.completed` | 透传完成 | — | usage 记 0，写 WARN |
| Redis 限流不可用（分布式模式） | 503 | `distributed_state_unavailable` | Fail-Closed，与 chat 相同 |
| 鉴权失败 | 401 | `auth_error` | 与 chat 相同 |
| QPS 超限 | 429 | `rate_limit` | 含 `Retry-After` header |
| 日配额耗尽 | 429 | `daily_quota_exceeded` | 含 `Retry-After` header |

---

## §7 测试策略

### 单元测试（`tests/unit/`）

**`test_provider_openai.c`**（新增用例）：
- `test_openai_responses_build`：验证 URL 拼接（`/responses`）、Authorization header 正确注入、body 原样透传（不修改任何字段）
- `test_openai_responses_parse_usage_nstream`：KAT — 给定标准非流式 Response JSON，断言四项 token 计数正确
- `test_openai_responses_parse_usage_stream`：KAT — 给定含 `response.completed` 的 SSE 事件序列，断言 usage 正确提取
- `test_openai_responses_parse_usage_missing_completed`：缺失 `response.completed` 时 usage 全零，函数返回 -1

**`test_aigate_core.c`**（新增用例）：
- `test_responses_non_openai_400`：mock model 路由到 anthropic provider，断言返回 400 且上游未被调用
- `test_responses_pipeline_200`：mock upstream 返回标准 Response 对象，断言端到端 200 + usage 计入
- `test_responses_streaming_200`：mock SSE 序列，验证流帧转发 + usage 提取

### 集成测试（`tests/integration/test_gateway.py`）

- `test_responses_openai_passthrough`：完整非流式端到端（mock upstream 返回 Response JSON）
- `test_responses_streaming_sse`：mock 返回标准 SSE 事件序列，验证客户端收到完整流帧 + usage 计入日志
- `test_responses_non_openai_400`：model 路由到 anthropic provider，验证 400 `unsupported_endpoint`

---

## §8 文件改动面汇总

| 文件 | 改动类型 | 说明 |
|---|---|---|
| `src/provider_openai.c` | 新增函数 | `build_responses`、`parse_responses_usage`、`sse_callback_responses` |
| `src/provider_openai.h` | 新增声明 | 对应三个函数签名 |
| `src/provider_adapter.c` | 新增分支 | `responses` 路径调用新函数 |
| `src/provider_adapter.h` | 无变化 | — |
| `src/aigate_core.c` | 新增函数 + 路由分支 | `handle_responses()`、路径匹配 |
| `src/transport_civetweb.c` | 新增 URI 注册 | `POST /v1/responses` |
| `src/schema_sql.h` | schema 版本升级 | 追加 `reasoning_tokens` 列 |
| `src/pg_store.c` | schema 迁移 | `ALTER TABLE usage_requests ADD COLUMN reasoning_tokens` |
| `tests/unit/test_provider_openai.c` | 新增用例 | 3 个 KAT |
| `tests/unit/test_aigate_core.c` | 新增用例 | 3 个用例 |
| `tests/integration/test_gateway.py` | 新增用例 | 3 个 E2E 用例 |

**不改动**：`ratelimit.c`、`circuit_breaker.c`、`admin_api.c`、`usage_meter.c`、`auth_key.c`、`model_router.c`

---

## §9 验收标准

- [ ] `POST /v1/responses` 对 openai 系 provider 端到端返回 200（非流式）
- [ ] 流式 SSE 帧原样转发，客户端收到完整事件序列
- [ ] `response.completed` 事件 usage 正确提取并写入 `usage_requests`（含 `reasoning_tokens`）
- [ ] 非 openai provider 返回 400 `unsupported_endpoint`，上游不被调用
- [ ] `rl_reserve_tokens` 使用 Responses API token 计数，日配额正确扣减
- [ ] 现有所有 ctest 单元测试 100% 通过（无回归）
- [ ] 新增 7 个单元测试用例全部通过

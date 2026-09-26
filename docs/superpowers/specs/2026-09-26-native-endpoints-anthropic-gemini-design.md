# 原生入站端点扩展设计规范：Anthropic Messages 与 Google Gemini

日期: 2026-09-26 · 场景: 内部自用与官方 SDK 直连 · 状态: Approved

---

## 1. 背景与目标 (Context & Motivation)

### 1.1 背景
当前 `aigate` 网关对客户端主要暴露 OpenAI 风格的端点（`POST /v1/chat/completions`、`POST /v1/responses`、`POST /v1/embeddings` 以及 `GET /v1/models`）。对于后端上游是 Anthropic 或 Gemini 的模型，网关依赖内部翻译适配器（如 `provider_anthropic_build`、`provider_gemini_build`）将 OpenAI 请求格式转换为上游格式，并将上游响应再转回 OpenAI 格式输出给客户端。

然而，企业与开发者日益倾向于使用厂商提供的官方 SDK（例如 Anthropic Claude SDK、Google GenAI SDK）。这导致客户端调用必须经过额外的协议适配层或只能使用通用客户端。

### 1.2 目标 (Goals)
1. **Anthropic Messages 协议原生入站**：
   - 暴露 `POST /v1/messages` 端点。
   - 接收原生 Anthropic Messages 请求体（JSON），原样直通至 Anthropic 上游。
   - 支持非流式和流式（SSE）传输，100% 保真传递 Anthropic 专有事件（thinking blocks、tool use、redaction、citations）。
2. **Google Gemini 原生入站**：
   - 暴露 `POST /v1beta/models/{model}:generateContent` 与 `POST /v1beta/models/{model}:streamGenerateContent`（同时兼容 `/v1/models/{model}:*` 前缀）。
   - 接收原生 Gemini GenerateContent 请求体，原样直通至 Gemini 上游。
   - 支持非流式和流式（SSE），完整保留 SafetySettings、Grounding、Citations 等高级字段。
3. **多凭证灵活鉴权**：
   - 支持标准 HTTP 标头 `Authorization: Bearer <key>`。
   - 支持 Anthropic 原生标头 `x-api-key: <key>`。
   - 支持 Gemini 原生标头 `x-goog-api-key: <key>` 以及 URL 查询参数 `?key=<key>`。
4. **同构安全守卫 (Strict Route Guard)**：
   - `/v1/messages` 端点路由的目标 Provider 必须为 `anthropic`。
   - `/v1beta/models/*` 端点路由的目标 Provider 必须为 `gemini` 或 `google`。
   - 若路由到异构 Provider，拦截并返回 HTTP 400 `unsupported_endpoint`。
5. **完整保留网关核心承重能力**：
   - API Key 哈希验证、每 Key 的 QPS 限流（本地及 Redis 集群）。
   - 每日配额检查（`daily_token_quota`）。
   - 上游故障转移（Failover）与熔断器（Circuit Breaker）统计。
   - 用量计量（`usage_daily` 与 `usage_requests` 明细审计）。
6. **协议级原生错误响应**：
   - 发生鉴权失败、限流超额、熔断不可用或参数非法时，输出符合当前官方 SDK 规范的原生错误 JSON。

### 1.3 非目标 (Non-Goals)
- **非跨协议双向转换**：不进行跨协议转码（例如不将客户端发来的 Anthropic 原生请求转交 OpenAI 运行并再转回 Anthropic 格式）。同构直通模式保证 100% 协议保真，零语义裁剪与极低延迟。
- **不新增底层 Schema 迁移**：完全复用现有的 Migration v8 数据表（`usage_requests`、`usage_daily`、`api_keys` 等）。

---

## 2. 架构设计与端点矩阵 (Architecture & Endpoints)

### 2.1 端点路由定义

| HTTP 方法 | 路径规范 | 目标上游要求 | 模型提取来源 | 传输模式 |
|---|---|---|---|---|
| `POST` | `/v1/messages` | `provider == "anthropic"` | 请求 JSON Body 中 `"model"` 字段 | 非流式 / SSE 流式 (`stream: true`) |
| `POST` | `/v1beta/models/{model}:generateContent` | `provider in ["gemini", "google"]` | URL Path 中的 `{model}` 占位符 | 非流式 |
| `POST` | `/v1beta/models/{model}:streamGenerateContent` | `provider in ["gemini", "google"]` | URL Path 中的 `{model}` 占位符 | SSE 流式 (`?alt=sse`) |
| `POST` | `/v1/models/{model}:generateContent` | 同上（兼容别名） | URL Path 中的 `{model}` | 非流式 |
| `POST` | `/v1/models/{model}:streamGenerateContent` | 同上（兼容别名） | URL Path 中的 `{model}` | SSE 流式 |

### 2.2 身份鉴权机制 (Multi-Credential Authentication)

网关提供统一凭据解析函数 `auth_extract_client_key()`，按优先级依次提取凭据：
1. `Authorization: Bearer <key>`（去除前缀 Bearer 与空白字符）
2. 标头 `x-api-key: <key>`（Anthropic SDK 常用）
3. 标头 `x-goog-api-key: <key>`（Google GenAI SDK 常用）
4. URL 查询字符串参数 `key=<key>`（Google GenAI SDK 备用）

提取成功后，按原有流程执行 SHA-256 哈希计算，检索 `api_keys` 表（或缓存），校验 key 是否有效、是否过期、是否在白名单、模型是否在 `allowed_models` 中。

---

## 3. 数据流与直通流水线 (Data Flow & Passive Sniffing Pipeline)

```
[Claude / Gemini SDK]
        │
        ▼ (POST /v1/messages 或 /v1beta/models/*)
   [aigate Transport (civetweb)]
        │
   [aigate_core: Auth & Route Guard]
        ├── 提取凭证 (Bearer / x-api-key / x-goog-api-key / ?key=)
        ├── QPS 令牌桶 & 日配额扣减 (Redis / Local)
        ├── 匹配路由 model_router
        └── 严格校验 provider 类型 (anthropic / gemini)
        │
        ▼
   [Upstream Client (curl)] ──── 替换上游 Key 标头 ────► [Anthropic / Gemini 上游]
        │                                                          │
        ◄──────────────────── 响应流 ──────────────────────────────┘
        │
   ┌────┴─────────────────────────────┐
   ▼                                   ▼
[非流式响应]                       [流式 SSE 响应]
- 原样写回 Client                   - 实时无缓冲写回 Client
- 嗅探 usage/usageMetadata         - 旁路轻量状态机监听 token 事件
- um_record_ext() 计量落库          - 连接结束时 um_record_ext() 计量落库
```

### 3.1 上游请求重写 (Header & URL Rewriting)

1. **Anthropic Messages 请求构造**：
   - 目标 URL：`{route->endpoint}/v1/messages`（若 endpoint 以 `/v1` 或 `/v1/` 结尾则正确拼接 `/messages`）。
   - 标头写入：
     - `x-api-key: {route->upstream_key}`（换为上游真实密钥）
     - `anthropic-version: {client_provided_or_"2023-06-01"}`
     - `anthropic-beta: {client_provided_if_any}`
     - `content-type: application/json`
   - 请求体：直接使用客户端入站原始 JSON Body。
2. **Gemini GenerateContent 请求构造**：
   - 目标 URL：
     - 非流式：`{base_ep}/v1beta/models/{mapped_model}:generateContent`
     - 流式：`{base_ep}/v1beta/models/{mapped_model}:streamGenerateContent?alt=sse`
   - 标头写入：
     - `x-goog-api-key: {route->upstream_key}`
     - `content-type: application/json`
   - 请求体：直接使用客户端入站原始 JSON Body。

### 3.2 响应处理与旁路 Usage 嗅探 (Passive Usage Sniffer)

为了实现极低延迟和 100% 协议保真，数据流采用**旁路只读嗅探模式**，不对响应内容做重新包装：

#### 3.2.1 非流式处理
- 接收上游完整响应体，直接通过 HTTP 200 原样输出给客户端。
- 同时通过轻量 JSON 提取器：
  - **Anthropic**：提取 `response["usage"]` 中的 `input_tokens`、`output_tokens`、`cache_read_input_tokens`、`cache_creation_input_tokens`。
  - **Gemini**：提取 `response["usageMetadata"]` 中的 `promptTokenCount`、`candidatesTokenCount`、`cachedContentTokenCount`。
- 调用 `um_record_ext()` 将用量记入 `usage_daily` 与 `usage_requests`。

#### 3.2.2 流式（SSE）旁路嗅探器
- 接收到第一个 Chunk 时，向下游客户端写出 SSE 响应头：
  `Content-Type: text/event-stream; charset=utf-8`
  `Cache-Control: no-cache`
  `Connection: keep-alive`
- **实时穿透**：每个上游 Chunk 直接写出至客户端连接。
- **旁路状态机（Sniffer State Machine）**：
  - **Anthropic 嗅探器 (`anthropic_sniff_feed`)**：
    - 行缓冲解析 SSE 行。
    - 监听 `event: message_start`：解析伴随的 `data: {"message": {"usage": {"input_tokens": N, ...}}}`。
    - 监听 `event: message_delta`：解析伴随的 `data: {"usage": {"output_tokens": N}}`。
  - **Gemini 嗅探器 (`gemini_sniff_feed`)**：
    - 行缓冲解析 SSE 行。
    - 监听 `data: {...}`，检索是否存在 `"usageMetadata"`。从末尾 Candidate 的 usage 数据中读取 `promptTokenCount` 与 `candidatesTokenCount`。
- **流结束归档**：
  - 当上游连接关闭或发送 `event: message_stop` / `data: [DONE]` 时，调用 `um_record_ext()` 记录本次请求的完整 Token 审计。

---

## 4. 容灾与错误处理 (Resilience & Native Error Formatting)

### 4.1 故障转移与熔断机制
- 若在向客户端写出任何 Header 之前，上游连接失败、超时或返回 5xx 状态码：
  - 熔断器记录对应 Candidate 失败（`cb_record_failure`）。
  - 网关轮转至该 Route 下的下一个可用 Candidate 重试请求。
- 若上游返回 4xx（如客户端请求格式非法、上游拒认等）：
  - 属于客户端请求语义错误，网关直接原样透传上游 4xx 状态码与上游 Body，不计入熔断器失败，不触发故障转移。

### 4.2 网关原生错误格式输出 (Protocol-Native Errors)

当错误发生于网关层自身时（如 API Key 认证失败、QPS 超限、日额度用尽、熔断器全开、或请求了异构模型），网关根据当前端点类型返回对应的官方错误规范：

#### Anthropic 错误格式
```json
{
  "type": "error",
  "error": {
    "type": "authentication_error" | "invalid_request_error" | "rate_limit_error" | "api_error",
    "message": "Detailed error explanation"
  }
}
```
- 401 ➜ `authentication_error`
- 400 ➜ `invalid_request_error`（如 `unsupported_endpoint`）
- 429 ➜ `rate_limit_error`
- 503 ➜ `api_error`

#### Gemini 错误格式
```json
{
  "error": {
    "code": 401,
    "message": "Detailed error explanation",
    "status": "UNAUTHENTICATED" | "INVALID_ARGUMENT" | "RESOURCE_EXHAUSTED" | "UNAVAILABLE"
  }
}
```
- 401 ➜ `code: 401, status: "UNAUTHENTICATED"`
- 400 ➜ `code: 400, status: "INVALID_ARGUMENT"`
- 429 ➜ `code: 429, status: "RESOURCE_EXHAUSTED"`
- 503 ➜ `code: 503, status: "UNAVAILABLE"`

---

## 5. 模块与文件变更面 (Module Changes)

| 文件 | 变更说明 |
|---|---|
| `src/auth_key.h` / `src/auth_key.c` | 扩展 `auth_extract_client_key()`，支持解析 `x-api-key`、`x-goog-api-key` 和 query param `?key=` |
| `src/provider_anthropic.h` / `.c` | 增加 Anthropic 响应与流式 Usage 旁路嗅探器：`anthropic_sniff_usage_json()` 与 `anthropic_sniff_feed()` |
| `src/provider_gemini.h` / `.c` | 增加 Gemini 响应与流式 Usage 旁路嗅探器：`gemini_sniff_usage_json()` 与 `gemini_sniff_feed()` |
| `src/aigate_core.h` / `src/aigate_core.c` | 增加 `handle_anthropic_messages()` 与 `handle_gemini_generate()` 流水线处理函数，增加协议原生错误输出辅助函数 |
| `src/transport_civetweb.c` | 注册 `/v1/messages`、`/v1beta/models/*` 以及 `/v1/models/*` 路由分发映射 |
| `tests/unit/test_auth_key.c` | 增加多凭证提取单测 |
| `tests/unit/test_provider_anthropic.c` | 增加 Anthropic 原生 usage 嗅探与流式嗅探单测 |
| `tests/unit/test_provider_gemini.c` | 增加 Gemini 原生 usage 嗅探与流式嗅探单测 |
| `tests/unit/test_aigate_core.c` | 增加端点守卫、异构拦截 400 及核心流程单元测试 |
| `tests/integration/mock_upstream.py` | 增加 Anthropic `/v1/messages` 与 Gemini `/v1beta/models/*` 的原生非流式与流式 Mock |
| `tests/integration/test_gateway.py` | 增加 Python 端到端集成测试用例 |

---

## 6. 测试与验证策略 (Testing Strategy)

1. **单元测试 (C17 / CTest)**:
   - 验证 Bearer、`x-api-key`、`x-goog-api-key`、`?key=` 的提取及容错（畸形标头、空串、特殊字符）。
   - 验证 Anthropic 与 Gemini 非流式响应解析（包含正向提取与缺少 usage 字段容错）。
   - 验证 Anthropic 与 Gemini 分段 Chunk 输入时，旁路状态机能准确聚合最终 `prompt_tokens`、`completion_tokens` 与 `cached_tokens`。
   - 验证向 `/v1/messages` 请求 OpenAI 模型时返回 HTTP 400 与 `unsupported_endpoint`。
2. **端到端集成测试 (Python / Pytest)**:
   - 测试 Claude SDK 风格请求（`POST /v1/messages`，标头带 `x-api-key`）：验证非流式与流式下网关 200 返回，并检查数据库 `usage_requests` 中精确记录了 token 与 provider。
   - 测试 Google GenAI SDK 风格请求（`POST /v1beta/models/{model}:generateContent` 与 `:streamGenerateContent?alt=sse`，标头带 `x-goog-api-key` 或 `?key=` 参数）：验证非流式与流式直通。
   - 验证异构 Provider 拦截返回的原生格式 JSON 错误体。
   - 验证 QPS 超限与配额超限触发的原生 429 响应。

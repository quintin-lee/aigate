# Reasoning Ecosystem v4 Design Specification

> **Date:** 2026-10-12  
> **Status:** Draft / Pending Review  
> **Target Version:** aigate v0.21.0  
> **Authors:** DeepMind / aigate Core Team  

---

## 1. Executive Summary & Motivation

In Reasoning Ecosystem v3, `aigate` established core reasoning governance foundations: Gemini 2.0 full-duplex thinking streaming, 3-tier output format resolution hierarchy (Client Header -> Client Body -> Model Config Default), dedicated reasoning token pricing, daily reasoning quota management with graceful degradation (`X-Aigate-Reasoning-Degraded: quota_exceeded`), and Admin REST API exposure.

However, production adoption has highlighted three immediate requirements for **Reasoning Ecosystem v4**:
1. **Claude 3.7 Sonnet Extended Thinking Deep Integration**:
   - Claude 3.7 Sonnet introduces extended thinking capabilities supporting budgets up to **128,000 tokens**.
   - Streaming responses emit structured SSE event blocks: `content_block_start` (`type: "thinking"` / `"redacted_thinking"`), `content_block_delta` (`thinking_delta` and `signature_delta`), and `content_block_stop`.
   - The cryptographic `signature` is mandatory for Anthropic multi-turn verification, requiring strict preservation across conversations.
   - When daily reasoning quotas are exceeded, thinking configuration must be cleanly suppressed without breaking non-reasoning completions.
2. **OpenAI o1 / o3-mini Protocol Bidirectional Bridging**:
   - OpenAI o-series models introduce `reasoning_effort` (`"low" | "medium" | "high"`) and require `max_completion_tokens` instead of `max_tokens`.
   - Streaming outputs across OpenAI-compatible providers (DeepSeek, vLLM, Ollama, Azure) vary between `choices[0].delta.reasoning_content` and `choices[0].delta.thought`.
   - Downstream clients need bidirectional normalization to either raw reasoning attributes or `<think>` emulation tags.
3. **Web Console Full-Stack Linkage & Observability Loop**:
   - The embedded Web Console (`web/admin.html`) currently lacks controls for model-level `tag_emulation_default` and `reasoning_mtok` pricing.
   - API Keys and Key Groups lack UI management for `daily_reasoning_quota` and visual warning badges for degraded states.
   - The Playground lacks deep reasoning controls (reasoning format selector, thinking budget controls, collapsible thinking chain component, and dynamic degraded alert banners).
   - The Admin Overview lacks dedicated reasoning ecosystem telemetry cards.

Reasoning Ecosystem v4 closes these gaps with a layered, decoupled full-duplex adapter and end-to-end Web Console linkage.

---

## 2. Architecture Overview & System Flow

```
                                  Client Request
            (OpenAI Client / Anthropic Client / Chat WebUI / Playground)
                                        │
                                        ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 1. Request Ingestion & Governance Filter                                     │
│    - Authenticate API Key & Check Rate Limits                               │
│    - Query Daily Reasoning Quota (Rate Limiter / Budget Enforcer)           │
│    - If Quota Exceeded: Set degraded flag & inject degradation context      │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 2. Upstream Protocol Adaptation & Thinking Mutation                          │
│    ┌──────────────────────────────┐       ┌───────────────────────────────┐ │
│    │ Claude 3.7 Adapter           │       │ OpenAI o1 / o3-mini Adapter   │ │
│    │ - Budget <= 128,000 tokens   │       │ - Map reasoning_effort        │ │
│    │ - Auto-adjust max_tokens     │       │ - Map max_completion_tokens  │ │
│    │ - Strip thinking if degraded │       │ - Clear effort if degraded    │ │
│    └──────────────────────────────┘       └───────────────────────────────┘ │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 3. Streaming Response State Machine & 3-Tier Resolution                      │
│    - Tier 1: Client Header / Body (tag_emulation vs raw)                    │
│    - Tier 2: Model Route Default (tag_emulation_default)                    │
│    - Tier 3: Gateway Default (Raw reasoning_content)                        │
│                                                                             │
│    Stream Chunk Parser:                                                     │
│      Anthropic: thinking_delta + signature_delta + redacted_thinking        │
│      OpenAI:    delta.reasoning_content / delta.thought                     │
│                                                                             │
│    Formatter:                                                               │
│      Tag Emulation => Inject <think> ... </think> into choices[0].delta.content│
│      Raw Mode      => choices[0].delta.reasoning_content + signature        │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 4. Client Response & Web Console Observability                               │
│    - Headers: X-Aigate-Reasoning-Format, X-Aigate-Reasoning-Degraded         │
│    - Playground: Render collapsible thinking cards + Alert banners          │
│    - Web Console: Models, Keys, Groups & Status Telemetry Synchronization   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Upstream Protocol Adaptation Details

### 3.1 Claude 3.7 Sonnet (`src/upstream/provider_anthropic.c`)

1. **Inbound Request Construction**:
   - Accepts Anthropic Extended Thinking configuration:
     ```json
     "thinking": {
       "type": "enabled",
       "budget_tokens": 1024
     }
     ```
   - Maximum allowed budget tokens expanded from 64,000 to **128,000**.
   - **Constraint Enforcement**: Anthropic enforces `max_tokens > budget_tokens`. When mutating requests, if `max_tokens <= budget_tokens`, `max_tokens` is automatically elevated to `budget_tokens + 1024` (or clamped to the upstream model context limit).
   - **Degradation Suppression**: When `req_ctx->reasoning_degraded == true` (quota exceeded), the `"thinking"` block is cleanly removed from the outbound Anthropic payload, returning to standard non-thinking generation.

2. **Streaming Event Parsing**:
   - `content_block_start`:
     - If `block.type == "thinking"`, enter reasoning block. If tag emulation is active, emit `<think>\n` into downstream `choices[0].delta.content`.
     - If `block.type == "redacted_thinking"`, record redacted state and emit appropriate sanitized reasoning block or tag indicator.
   - `content_block_delta`:
     - If `delta.type == "thinking_delta"`, extract `delta.thinking`. In tag emulation mode, emit text into `choices[0].delta.content`. In raw mode, emit chunk as `choices[0].delta.reasoning_content`.
     - If `delta.type == "signature_delta"`, extract `delta.signature` and accumulate for multi-turn session preservation.
   - `content_block_stop`:
     - Exit reasoning block. If tag emulation is active, emit `\n</think>\n` into `choices[0].delta.content`.

3. **Stream Safety & State Machine**:
   - In `streaming_reasoning_state_t`:
     ```c
     typedef struct {
         bool in_reasoning_block;
         bool emitted_start_tag;
         bool emitted_end_tag;
         size_t accumulated_reasoning_bytes;
         char signature_buffer[512];
     } streaming_reasoning_state_t;
     ```
   - Guaranteed single start tag emission and automated closing tag injection if connection disconnects or truncates unexpectedly.

### 3.2 OpenAI o1 / o3-mini Protocol Bridging (`src/upstream/provider_openai.c`)

1. **Inbound Parameter Mapping**:
   - Support `reasoning_effort`: validates allowed values (`"low"`, `"medium"`, `"high"`).
   - Automatically remap client `max_tokens` to `max_completion_tokens` for OpenAI o-series endpoints.
   - If degraded, strip `reasoning_effort` from request payload.

2. **Outbound Stream Normalization**:
   - Ingest chunks from OpenAI and third-party compatible providers (DeepSeek, vLLM, Ollama):
     - Check `choices[0].delta.reasoning_content`.
     - Fallback to `choices[0].delta.thought`.
   - Apply 3-Tier format resolution:
     - Tag Emulation: Emit `<think>...</think>` within `choices[0].delta.content`.
     - Raw Mode: Emit standard `choices[0].delta.reasoning_content`.

---

## 4. Web Console UI/UX & Observability Linkage (`web/admin.html`)

### 4.1 Models Configuration (`#modelModal`)
1. **Tag Emulation Default Switch**:
   - Checkbox `mFormTagEmulationDefault` (`tag_emulation_default`): "默认开启 `<think>` 标签流式仿真封装".
   - Persisted via `POST /admin/v1/models` and `PATCH /admin/v1/models/{name}`.
2. **Dedicated Reasoning Pricing**:
   - Number input `mFormPricingReasoning` (`pricing.reasoning_mtok`): "思考价格 ($/Mtok)".
   - Loaded and saved as part of model pricing structure.
3. **Budget Presets**:
   - Extend thinking budget preset buttons to include `64K` and `128K`.

### 4.2 Keys & Groups Quota Governance
1. **API Keys Modal & Table**:
   - Field `kFormReasoningQuota` (`daily_reasoning_quota`): "每日推理 Token 限额 (0 为不限)".
   - Keys list view displays a badge: `<span class="badge-reasoning">${k.daily_reasoning_quota > 0 ? k.daily_reasoning_quota.toLocaleString() : "不限"}</span>`.
   - If key status has `degraded: true`, render `⚠️ 已降级` indicator.
2. **Groups Modal & Table**:
   - Field `gFormDailyReasoningQuota` (`daily_reasoning_quota`): "组级每日推理配额 (0 为不限)".
   - Displayed in Groups table columns.

### 4.3 Playground Deep Reasoning Workbench
1. **Configuration Column (Col 1)**:
   - Reasoning Format Selector (`playReasoningFormat`):
     - `default`: 网关/模型默认
     - `tag_emulation`: `<think>` 标签仿真封装
     - `raw`: 原始 `reasoning_content`
   - Thinking Budget input/slider with support up to 128k.
2. **Chat Dialog Column (Col 2)**:
   - **Collapsible Thinking Accordion**:
     - When receiving thinking blocks (either `<think>` tags or `reasoning_content`), render inside a collapsible card `<details class="thinking-box">` with an animated pulse indicator and token count estimate.
     - Users can expand/collapse the thought process to keep conversation legible.
   - **Degradation Alert Banner**:
     - Detects response header `X-Aigate-Reasoning-Degraded: quota_exceeded`.
     - Renders an amber banner: `⚠️ 每日推理配额已超限，本次交互已自动降级为标准模式生成 (X-Aigate-Reasoning-Degraded: quota_exceeded)`.
3. **Telemetry Column (Col 3)**:
   - Add telemetry metric: `playStatReasoningTokens` (Reasoning Tokens count and percentage of total completion).
   - Display key remaining reasoning quota水位.

### 4.4 Dashboard Overview Integration
- Add Reasoning Ecosystem Summary Card to Overview tab:
  - Total reasoning-enabled models.
  - Today's cumulative reasoning tokens and cost.
  - Active degraded key count.

---

## 5. Data Contracts & Error Handling

### 5.1 Request / Response Headers & Body Schemas
```http
Client -> Gateway:
POST /v1/chat/completions HTTP/1.1
X-Reasoning-Format: tag_emulation | raw (optional)
Content-Type: application/json

{
  "model": "claude-3-7-sonnet-20250219",
  "messages": [...],
  "thinking": {
    "type": "enabled",
    "budget_tokens": 8192
  },
  "tag_emulation": true
}
```

```http
Gateway -> Client:
HTTP/1.1 200 OK
Content-Type: text/event-stream
X-Aigate-Reasoning-Format: tag_emulation
X-Aigate-Reasoning-Degraded: quota_exceeded (only when degraded)

data: {"choices":[{"delta":{"content":"<think>\n"}}]}
data: {"choices":[{"delta":{"content":"Thinking step 1..."}}]}
data: {"choices":[{"delta":{"content":"\n</think>\nHere is the answer."}}]}
data: [DONE]
```

### 5.2 Error Handling & Resiliency
1. **Clamping & Boundaries**:
   - Budget tokens < 1024 coerced to 1024 if thinking is enabled.
   - Budget tokens > 128,000 clamped to 128,000.
2. **Tag Emulation Guarantee**:
   - State machine maintains `emitted_start_tag` and `emitted_end_tag`.
   - On connection abort or upstream EOF, any unclosed `<think>` tag is safely closed with `\n</think>\n`.
3. **Cryptographic Signature Preservation**:
   - `signature_delta` chunks accumulated; in multi-turn conversations, signatures are retained in context buffers for session validation.

---

## 6. Testing & Quality Assurance Plan

### 6.1 Unit Tests
1. `tests/unit/upstream/test_provider_anthropic.c`:
   - Claude 3.7 128k budget serialization.
   - `max_tokens > budget_tokens` constraint verification.
   - `thinking_delta`, `signature_delta`, `redacted_thinking` SSE parsing.
   - Tag emulation vs raw `reasoning_content` output assertion.
   - Degradation stripping assertion.
2. `tests/unit/upstream/test_provider_openai.c`:
   - `reasoning_effort` and `max_completion_tokens` mapping.
   - `delta.reasoning_content` and fallback `delta.thought` extraction.
   - Tag emulation wrapping under OpenAI SSE flow.
3. `tests/unit/server/test_admin_ui.c`:
   - Ensure `web/admin.html` compiles into C headers without error.
   - Validate presence of `tag_emulation_default`, `daily_reasoning_quota`, `playReasoningFormat`, thinking accordion markup, and degradation banner.

### 6.2 Regression & Build Verification
- Zero compiler warnings under `-Wall -Wextra -Werror`.
- Full pass on `ctest --output-on-failure`.
- Formatted git commits following project conventions (`feat(anthropic): ...`, `feat(admin-ui): ...`).

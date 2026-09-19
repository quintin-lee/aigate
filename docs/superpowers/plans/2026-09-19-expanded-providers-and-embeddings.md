# aigate Implementation Plan 3: Expanded Providers (Gemini, DeepSeek) & Embeddings Pipeline

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand `aigate` with a modular Provider VTable abstraction layer, a Google Gemini protocol adapter (chat completions, SSE streaming bridge, embeddings), DeepSeek reasoning content preservation and Prompt Cache token metering, and a dual-mode `/v1/embeddings` vector pipeline.

**Architecture:** 
- A lightweight `provider_adapter_t` interface in `src/provider_adapter.h` decouples the core request pipeline (`aigate_core.c`) from specific upstream provider protocols.
- `provider_openai.c` is extended to preserve DeepSeek `reasoning_content` and extract prompt cache hits (`prompt_cache_hit_tokens`, `prompt_tokens_details.cached_tokens`).
- `provider_gemini.c/.h` translates OpenAI chat completions and embeddings to Google Gemini REST endpoints (`generateContent`, `:streamGenerateContent?alt=sse`, `:embedContent`), using secure `x-goog-api-key` headers and an SSE event translation bridge.
- `/v1/embeddings` is registered as a first-class data plane endpoint in `transport_civetweb.c` and `aigate_core.c`, supporting both direct pass-through (OpenAI, Ollama, SiliconFlow) and Gemini native translation.

**Tech Stack:** C17, CMake, civetweb, jansson, libcurl, libpq, OpenSSL, hdr_histogram, Python/pytest.

**Spec:** `docs/superpowers/specs/2026-09-19-expanded-providers-and-embeddings-design.md`

---

## File Structure

```
aigate/
├── src/
│   ├── provider_adapter.h        # [NEW] Provider VTable interface & registry definition
│   ├── provider_adapter.c        # [NEW] Registry implementation & provider_find()
│   ├── provider_openai.h/.c      # OpenAI + DeepSeek reasoning preservation & cache parsing
│   ├── provider_anthropic.h/.c   # Anthropic adapter adapted to provider_adapter_t
│   ├── provider_gemini.h/.c      # [NEW] Google Gemini chat, stream bridge & embeddings
│   ├── aigate_core.h/.c          # Core pipeline dispatching via provider_adapter_t
│   ├── transport_civetweb.c      # Add POST /v1/embeddings route registration
│   ├── usage_meter.h/.c          # Add cached_prompt_tokens tracking & Prometheus metric
│   ├── pg_store.h/.c             # Schema migration v2 & cached_prompt_tokens persistence
│   ├── admin_api.c               # Update /admin/v1/usage query for cached tokens
│   └── schema_sql.h              # Idempotent migration v2
├── schema/
│   └── schema.sql                # Add cached_prompt_tokens to usage_daily
├── tests/unit/
│   ├── test_provider_gemini.c    # [NEW] Unit tests for Gemini request/response translation
│   ├── test_gemini_stream.c      # [NEW] Unit tests for Gemini SSE streaming bridge
│   ├── test_provider_deepseek.c  # [NEW] Unit tests for reasoning_content & cache tokens
│   └── test_embeddings.c         # [NEW] Unit tests for /v1/embeddings (Direct & Gemini)
└── tests/integration/
    ├── mock_upstream.py          # Add Gemini and DeepSeek mock endpoints
    └── test_gateway.py           # Integration scenarios for Gemini, DeepSeek, Embeddings
```

---

## Task 1: Provider VTable Interface & Adapter Registry

**Files:**
- Create: `src/provider_adapter.h`, `src/provider_adapter.c`
- Modify: `src/provider_openai.h`, `src/provider_openai.c`, `src/provider_anthropic.h`, `src/provider_anthropic.c`, `src/aigate_core.c`, `CMakeLists.txt`

- [x] **Step 1: Define `provider_adapter_t` in `src/provider_adapter.h`**
  Declare function pointer types:
  - `supports(const char* provider)`
  - `build_chat(...)`
  - `parse_chat_response(...)`
  - `stream_bridge_new(...)`, `stream_bridge_feed(...)`, `stream_bridge_finish(...)`, `stream_bridge_get_tokens(...)`, `stream_bridge_free(...)`
  - `build_embeddings(...)`, `parse_embeddings_response(...)`
  - Declare `const provider_adapter_t* provider_find(const char* provider);`

- [x] **Step 2: Implement Registry in `src/provider_adapter.c`**
  - Register `g_provider_openai`, `g_provider_anthropic`, `g_provider_gemini`.
  - Implement `provider_find`: iterate over list, match `adapter->supports(provider)`.

- [x] **Step 3: Export Adapters from `provider_openai.c` and `provider_anthropic.c`**
  - Define `extern const provider_adapter_t g_provider_openai;` and `extern const provider_adapter_t g_provider_anthropic;`.
  - Wrap existing build/parse/stream functions to conform to the VTable signatures.

- [x] **Step 4: Refactor `aigate_core.c` Dispatch**
  - In `aigate_handle_request`, lookup `adapter = provider_find(route.provider)`.
  - Replace hardcoded `if (is_openai) ... else if (is_anthropic)` with `adapter->build_chat()` and streaming bridge calls.
  - Compile and run `ctest` to ensure zero regressions on existing tests.

---

## Task 2: DeepSeek Extensions & Prompt Cache Token Accounting

**Files:**
- Modify: `schema/schema.sql`, `src/schema_sql.h`, `src/pg_store.h`, `src/pg_store.c`, `src/usage_meter.h`, `src/usage_meter.c`, `src/metrics.c`, `src/provider_openai.c`, `src/admin_api.c`
- Create: `tests/unit/test_provider_deepseek.c`

- [x] **Step 1: Schema Migration v2 in `schema.sql` & `pg_store.c`**
  - Add column `cached_prompt_tokens BIGINT NOT NULL DEFAULT 0` to `usage_daily`.
  - Record migration version 2 in `schema_migrations`.
  - Update `pg_store_ops` usage upsert query to include `cached_prompt_tokens`.

- [x] **Step 2: Extend Usage Metering & Prometheus Metrics**
  - Update `um_record(...)` signature to accept `long cached_prompt_tokens`.
  - Add Prometheus metric `aigate_tokens_cached_total` in `metrics.c`.
  - Update `admin_api.c` `/admin/v1/usage` to return `cached_tokens` in JSON response.

- [x] **Step 3: Extract Cache Tokens & Preserve `reasoning_content` in `provider_openai.c`**
  - In streaming accumulator and non-streaming response parser, detect `usage.prompt_tokens_details.cached_tokens` or `usage.prompt_cache_hit_tokens`.
  - Pass `cached_prompt_tokens` to `um_record`.
  - In `choices[0].message`, preserve `reasoning_content` field when parsing/forwarding responses.

- [x] **Step 4: Unit Test `test_provider_deepseek.c`**
  - Verify `reasoning_content` preservation in non-streaming response.
  - Verify extraction of cache hit tokens from DeepSeek usage payload.
  - Add test to CMakeLists.txt and verify `ctest` passes.

---

## Task 3: Google Gemini Protocol Adapter (Non-Streaming)

**Files:**
- Create: `src/provider_gemini.h`, `src/provider_gemini.c`, `tests/unit/test_provider_gemini.c`
- Modify: `CMakeLists.txt`

- [x] **Step 1: Header Declaration (`src/provider_gemini.h`)**
  - Declare `provider_gemini_supports(const char* provider)` (matches `"gemini"`, `"google"`).
  - Declare `provider_gemini_build(...)`.
  - Declare `provider_gemini_parse_response(...)`.
  - Declare external adapter descriptor `extern const provider_adapter_t g_provider_gemini;`.

- [x] **Step 2: Implement Request Translation (`provider_gemini_build`)**
  - Construct target URL: `{endpoint}/v1beta/models/{model}:generateContent`.
  - Add header `x-goog-api-key: <key>` to `extra_headers`.
  - Translate `messages`:
    - Role `"system"` -> `systemInstruction: {parts: [{text: ...}]}`.
    - Role `"user"` -> `contents: [{role: "user", parts: [{text: ...}]}]`.
    - Role `"assistant"` -> `contents: [{role: "model", parts: [{text: ...}]}]`.
  - Map `generationConfig`: `temperature`, `maxOutputTokens`, `topP`, `stopSequences`.

- [x] **Step 3: Implement Non-Streaming Response Translation (`provider_gemini_parse_response`)**
  - Parse `candidates[0].content.parts[0].text`.
  - Map `finishReason`: `STOP` -> `"stop"`, `MAX_TOKENS` -> `"length"`, `SAFETY` -> `"content_filter"`.
  - Extract `usageMetadata`: `promptTokenCount`, `candidatesTokenCount`, `totalTokenCount`.
  - Construct standard OpenAI chat completion JSON payload.

- [x] **Step 4: Unit Test `test_provider_gemini.c`**
  - Test simple request conversion, multi-turn conversation, and system prompt extraction.
  - Test response conversion and token counts extraction.
  - Verify error response wrapping on 400 `INVALID_ARGUMENT`.

---

## Task 4: Google Gemini SSE Streaming Bridge

**Files:**
- Modify: `src/provider_gemini.h`, `src/provider_gemini.c`
- Create: `tests/unit/test_gemini_stream.c`
- Modify: `CMakeLists.txt`

- [x] **Step 1: Design `gemini_bridge_t` State Machine**
  - Define line accumulation buffer (4096 bytes).
  - Track `headers_sent`, `prompt_tokens`, `completion_tokens`, `model_name`.
  - URL for streaming: `{endpoint}/v1beta/models/{model}:streamGenerateContent?alt=sse`.

- [x] **Step 2: Implement `gemini_bridge_feed`**
  - Split incoming bytes on `\n`.
  - When line starts with `data: `, parse JSON.
  - Extract delta text from `candidates[0].content.parts[0].text`.
  - On first content, set SSE headers (`text/event-stream; charset=utf-8`, `no-cache`, `keep-alive`).
  - Format and write OpenAI chunk: `data: {"id":"chatcmpl-gemini-...","choices":[{"index":0,"delta":{"content":"..."},"finish_reason":null}]}\n\n`.
  - Extract `usageMetadata` and `finishReason` on candidate completion.

- [x] **Step 3: Implement `gemini_bridge_finish`**
  - If `finishReason` was observed, emit final chunk with finish_reason and usage object.
  - Emit terminal `data: [DONE]\n\n`.

- [x] **Step 4: Unit Test `test_gemini_stream.c`**
  - Mock Gemini SSE streams with single-part and multi-part chunks.
  - Test stream fragmented across arbitrary byte packet boundaries.
  - Verify token counts and terminal `[DONE]` frame.

---

## Task 5: `/v1/embeddings` Dual-Mode Pipeline

**Files:**
- Modify: `src/provider_openai.h`, `src/provider_openai.c`, `src/provider_gemini.h`, `src/provider_gemini.c`, `src/aigate_core.c`, `src/transport_civetweb.c`
- Create: `tests/unit/test_embeddings.c`
- Modify: `CMakeLists.txt`

- [x] **Step 1: Mode A (Direct Pass-Through in `provider_openai.c`)**
  - Implement `provider_openai_build_embeddings`: route to `{endpoint}/v1/embeddings`.
  - Implement `provider_openai_parse_embeddings`: parse OpenAI embedding response and extract `usage.prompt_tokens`.

- [x] **Step 2: Mode B (Gemini Translation in `provider_gemini.c`)**
  - Implement `provider_gemini_build_embeddings`:
    - Single input -> `{endpoint}/v1beta/models/{model}:embedContent` with `{"content": {"parts": [{"text": str}]}}`.
    - Batch input -> `{endpoint}/v1beta/models/{model}:batchEmbedContents` with `{"requests": [...]}`.
  - Implement `provider_gemini_parse_embeddings`:
    - Parse `embedding.values` or `embeddings[].values`.
    - Construct OpenAI embedding list JSON response with prompt tokens.

- [x] **Step 3: Pipeline Integration in `transport_civetweb.c` & `aigate_core.c`**
  - In `transport_civetweb.c`, bind route `/v1/embeddings` to `aigate_handle_request`.
  - In `aigate_core.c`, handle `path == "/v1/embeddings"`:
    - Validate auth & allowlist.
    - Check rate limit (`rl_allow_request`).
    - Resolve model route and adapter.
    - Call `adapter->build_embeddings` -> `upstream_call_ext` -> `adapter->parse_embeddings_response`.
    - Reserve token quota (`rl_reserve_tokens`) and record usage (`um_record`).
    - Write JSON response to client.

- [x] **Step 4: Unit Test `test_embeddings.c`**
  - Verify Mode A direct pass-through parsing and usage tracking.
  - Verify Mode B Gemini single and batch embedding translation.
  - Verify quota deduction on embeddings requests.

---

## Task 6: End-to-End Integration Tests & Verification

**Files:**
- Modify: `tests/integration/mock_upstream.py`, `tests/integration/test_gateway.py`

- [x] **Step 1: Add Mock Endpoints in `mock_upstream.py`**
  - Add Gemini `:generateContent`, `:streamGenerateContent?alt=sse`, and `:embedContent`.
  - Add DeepSeek reasoning content mock and prompt cache usage mock.
  - Add OpenAI embeddings mock `/v1/embeddings`.

- [x] **Step 2: Add Pytest Scenarios in `test_gateway.py`**
  - Test Gemini non-streaming chat completion.
  - Test Gemini streaming chat completion (`stream: true`).
  - Test DeepSeek streaming response with `reasoning_content` and prompt cache usage.
  - Test `/v1/embeddings` with OpenAI upstream.
  - Test `/v1/embeddings` with Gemini upstream.

- [x] **Step 3: Execute Test Suites & Smoke Test**
  - Run `ctest --test-dir build --output-on-failure`.
  - Run `pytest tests/integration/test_gateway.py`.
  - Verify 100% test pass rate.


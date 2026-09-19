# aigate Plan 3: Expanded Providers (Gemini, DeepSeek) & Embeddings Pipeline — Design Spec

Date: 2026-09-19  
Status: Approved  

---

## 1. Overview & Goals

Building upon the working C17 core gateway (Plan 1) and the production-grade SSE streaming pass-through and Anthropic adapter (Plan 2), this specification defines **Plan 3** of the `aigate` AI Gateway. Plan 3 expands the gateway's provider ecosystem, adds deep support for modern reasoning models and prompt caching, introduces high-performance vector embeddings, and refactors provider dispatch into a clean, zero-overhead VTable interface.

### Key Deliverables
1. **Provider VTable Abstraction Layer (`provider_adapter_t`)**:
   - Refactor hardcoded `if-else` provider branching in `aigate_core.c` into a lightweight, extensible C interface table.
   - Unified interface for chat request building, response parsing, streaming bridge lifecycle, and embeddings translation.
2. **Google Gemini Protocol Adapter (`provider_gemini.c/.h`)**:
   - Bi-directional translation between OpenAI Chat Completions protocol and Google Gemini REST API (`generateContent` and `:streamGenerateContent?alt=sse`).
   - Request transformation: OpenAI messages to Gemini contents (`role: "user"` / `"model"`) and top-level `systemInstruction`.
   - Streaming SSE bridge: state machine converting Gemini SSE chunks into OpenAI-compatible `data: {"choices":[{"delta":...}]}` and `data: [DONE]`.
   - Usage token accounting and secure `x-goog-api-key` header authentication.
3. **DeepSeek Special Features & Prompt Cache Metering**:
   - Zero-loss preservation and pass-through of reasoning chain tokens (`reasoning_content`) in both streaming and non-streaming responses.
   - Detection and extraction of prompt cache hit tokens (`prompt_cache_hit_tokens` and `prompt_tokens_details.cached_tokens`).
   - Database schema migration v2: persist `cached_prompt_tokens` in `usage_daily`.
   - Exposition of `aigate_tokens_cached_total` in Prometheus metrics.
4. **`/v1/embeddings` Dual-Mode Vector Pipeline**:
   - First-class data plane endpoint `POST /v1/embeddings` with full authentication, rate limiting, and token quota deduction.
   - **Mode A (Direct Pass-Through)**: Native OpenAI-compatible upstream pass-through (supporting OpenAI, Ollama, SiliconFlow, vLLM).
   - **Mode B (Protocol Translation)**: Translation to Google Gemini `:embedContent` (single input) and `:batchEmbedContents` (batch inputs).

---

## 2. Architecture & Provider VTable Abstraction

### 2.1 Component Flow Diagram

```
+-------------------------------------------------------------------------+
|                           Client HTTP Request                           |
|                  /v1/chat/completions  |  /v1/embeddings                |
+-------------------------------------------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                  CivetWeb Transport (transport_civetweb.c)              |
+-------------------------------------------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                      Core Pipeline (aigate_core.c)                      |
|  1. Bearer Token Auth -> key_rec                                        |
|  2. Model Allowlist Check (key_allows_model)                            |
|  3. Token Bucket Rate Limit Check (rl_allow_request)                    |
|  4. Model Route Resolution (model_router_resolve)                       |
|  5. Provider Adapter Lookup: provider_find(route.provider)              |
+-------------------------------------------------------------------------+
                                     |
                +--------------------+--------------------+
                |                    |                    |
                v                    v                    v
      +------------------+  +------------------+  +------------------+
      | provider_openai  |  |provider_anthropic|  | provider_gemini  |
      | (OpenAI/DeepSeek)|  | (Claude Messages)|  | (Google Gemini)  |
      +------------------+  +------------------+  +------------------+
                |                    |                    |
                +--------------------+--------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                        upstream_client (libcurl)                        |
|  - Non-streaming: upstream_call_ext()                                   |
|  - Streaming:     upstream_stream_call() (30s silence timeout)          |
+-------------------------------------------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                  Response Writing & Usage Accounting                    |
|  - Non-streaming: adapter->parse_*() -> write_json()                    |
|  - Streaming:     adapter->stream_bridge_*() -> chunk writes -> [DONE]  |
|  - Usage & Quotas: um_record(..., ptok, ctok, cached_tok)               |
|                    rl_reserve_tokens(..., ptok + ctok)                  |
+-------------------------------------------------------------------------+
```

### 2.2 Provider Interface Definition (`src/provider_adapter.h`)

```c
#ifndef PROVIDER_ADAPTER_H
#define PROVIDER_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include "aigate_core.h"
#include "model_router.h"

typedef struct stream_bridge stream_bridge_t;

typedef struct provider_adapter {
    const char* name;

    /* Returns true if the provider name is handled by this adapter */
    bool (*supports)(const char* provider);

    /* 1. /v1/chat/completions request building */
    int (*build_chat)(const model_rec_t* route,
                      const char*        req_body,
                      char*              out_url,
                      size_t             url_cap,
                      const char*        extra_headers[4][2],
                      int*               n_extra_headers,
                      char**             out_body,
                      size_t*            out_len);

    /* 2. Chat non-streaming response parsing */
    int (*parse_chat_response)(const char* raw_body,
                               size_t      raw_len,
                               int*        http_status,
                               char**      out_body,
                               size_t*     out_len,
                               long*       out_ptok,
                               long*       out_ctok,
                               long*       out_cached_tok);

    /* 3. SSE streaming bridge lifecycle */
    stream_bridge_t* (*stream_bridge_new)(aigate_response_ctx* rc, const char* model);
    int  (*stream_bridge_feed)(stream_bridge_t* b, const void* chunk, size_t len);
    int  (*stream_bridge_finish)(stream_bridge_t* b);
    void (*stream_bridge_get_tokens)(stream_bridge_t* b, long* ptok, long* ctok, long* cached_tok);
    void (*stream_bridge_free)(stream_bridge_t* b);

    /* 4. /v1/embeddings request building (NULL if unsupported) */
    int (*build_embeddings)(const model_rec_t* route,
                            const char*        req_body,
                            char*              out_url,
                            size_t             url_cap,
                            const char*        extra_headers[4][2],
                            int*               n_extra_headers,
                            char**             out_body,
                            size_t*            out_len);

    /* 5. Embeddings non-streaming response parsing (NULL if unsupported) */
    int (*parse_embeddings_response)(const char* raw_body,
                                     size_t      raw_len,
                                     int*        http_status,
                                     char**      out_body,
                                     size_t*     out_len,
                                     long*       out_ptok);
} provider_adapter_t;

/* Registry lookup */
const provider_adapter_t* provider_find(const char* provider);

#endif /* PROVIDER_ADAPTER_H */
```

---

## 3. Google Gemini Protocol Adapter (`provider_gemini.c/.h`)

### 3.1 Endpoint & Authentication Specification
* **Base Endpoint**: Defaults to `https://generativelanguage.googleapis.com` if `route->endpoint` is empty or a generic slash.
* **URL Construction**:
  * Chat non-streaming: `{endpoint}/v1beta/models/{model}:generateContent`
  * Chat streaming: `{endpoint}/v1beta/models/{model}:streamGenerateContent?alt=sse`
  * Embeddings single: `{endpoint}/v1beta/models/{model}:embedContent`
  * Embeddings batch: `{endpoint}/v1beta/models/{model}:batchEmbedContents`
* **Authentication**: Pass API key in header `x-goog-api-key: <upstream_key>`, keeping URLs clean and secure.

### 3.2 Request Translation (OpenAI -> Gemini)
1. **System Instruction**:
   - Extract messages with `role == "system"`. If multiple system messages exist, join their contents with `\n\n`.
   - Set in top-level JSON: `systemInstruction: {"parts": [{"text": "..."}]}`.
2. **Contents Array**:
   - For `role == "user"`, create `{"role": "user", "parts": [{"text": content}]}`.
   - For `role == "assistant"`, create `{"role": "model", "parts": [{"text": content}]}`.
3. **GenerationConfig**:
   - `temperature` -> `generationConfig.temperature` (float)
   - `max_tokens` -> `generationConfig.maxOutputTokens` (integer)
   - `top_p` -> `generationConfig.topP` (float)
   - `stop`: if string, wrapped into array `generationConfig.stopSequences: [stop]`; if array, mapped directly.

### 3.3 Non-Streaming Response Translation (Gemini -> OpenAI)
* Parse Gemini candidate: `candidates[0].content.parts[0].text`.
* Finish reason mapping:
  - `STOP` -> `"stop"`
  - `MAX_TOKENS` -> `"length"`
  - `SAFETY` / `RECITATION` -> `"content_filter"`
  - Default -> `"stop"`
* Usage extraction:
  - `usageMetadata.promptTokenCount` -> `usage.prompt_tokens`
  - `usageMetadata.candidatesTokenCount` -> `usage.completion_tokens`
  - `usageMetadata.totalTokenCount` -> `usage.total_tokens`
* Construct standard OpenAI chat completion JSON response.

### 3.4 SSE Stream Bridge State Machine (`gemini_bridge_t`)
* Accumulate chunks in line buffer split on `\n`.
* When line starts with `data: `, extract JSON payload.
* Extract incremental text: `candidates[0].content.parts[0].text`.
* On first chunk with content, send SSE response headers (`Content-Type: text/event-stream; charset=utf-8`, `Cache-Control: no-cache`, `Connection: keep-alive`).
* Write formatted OpenAI SSE chunk:
  ```
  data: {"id":"chatcmpl-gemini-...","object":"chat.completion.chunk","created":...,"model":"...","choices":[{"index":0,"delta":{"content":"..."},"finish_reason":null}]}\n\n
  ```
* When `finishReason` appears:
  - Translate to OpenAI finish_reason.
  - Extract final `usageMetadata`.
  - Emit final chunk with `finish_reason` and `usage` object (if client supports `stream_options.include_usage`).
  - Emit `data: [DONE]\n\n`.

---

## 4. DeepSeek Protocol Extensions & Prompt Cache Metering

### 4.1 Reasoning Content Preservation (`reasoning_content`)
* **Non-Streaming**: In `provider_openai.c`, ensure any `reasoning_content` field on `choices[0].message` is strictly preserved during response pass-through.
* **Streaming**: The line-buffer SSE chunk streamer passes through all delta payload attributes unchanged. Frontend clients (Chatbox, Cherry Studio, OpenWebUI) receive `delta.reasoning_content` in real-time.

### 4.2 Prompt Cache Tokens Metering
* **Detection Logic**:
  Inspect the `usage` object in responses (both non-streaming and final streaming chunk):
  1. `usage.prompt_tokens_details.cached_tokens` (OpenAI & Anthropic standard)
  2. `usage.prompt_cache_hit_tokens` (DeepSeek direct field)
  3. `usage.cache_read_input_tokens` (Anthropic prompt cache)
* Store `cached_prompt_tokens` in `stream_accum_t` and `provider_openai_parse_response`.

---

## 5. `/v1/embeddings` Dual-Mode Pipeline

### 5.1 Pipeline Integration
* Route `POST /v1/embeddings` in `transport_civetweb.c` to `aigate_handle_request`.
* Request validation:
  - Check Bearer token authentication.
  - Parse `"model"` from request body.
  - Check model allowlist (`key_allows_model`).
  - Check rate limit (`rl_allow_request`).
  - Resolve model route (`model_router_resolve`).
  - Lookup adapter: `adapter = provider_find(route.provider)`.
  - Reject with 400 if `adapter->build_embeddings == NULL`.

### 5.2 Mode A: OpenAI-Compatible Direct Pass-Through
* For `openai`, `deepseek`, `siliconflow`, `ollama`, `vllm`:
  - Target URL: `{endpoint}/v1/embeddings` (or `{endpoint}/embeddings` if path already specified).
  - Forward JSON payload including `input`, `model`, `dimensions`, `encoding_format`.
  - Parse response: extract `usage.prompt_tokens`.
  - Deduct quota via `rl_reserve_tokens(rl, key_id, quota, prompt_tokens)`.
  - Record usage: `um_record(um, key_id, model, 200, prompt_tokens, 0, 0, lat, provider)`.

### 5.3 Mode B: Google Gemini Protocol Translation
* For `gemini` / `google`:
  - If `input` is a single string:
    - Target URL: `{endpoint}/v1beta/models/{model}:embedContent`
    - Request payload: `{"content": {"parts": [{"text": input_string}]}}`
    - Response payload: `{"embedding": {"values": [...]}}`
  - If `input` is an array of strings:
    - Target URL: `{endpoint}/v1beta/models/{model}:batchEmbedContents`
    - Request payload: `{"requests": [{"model": "models/{model}", "content": {"parts": [{"text": str}]}}, ...]}`
    - Response payload: `{"embeddings": [{"values": [...]}, ...]}`
  - Translate to OpenAI standard embedding format:
    ```json
    {
      "object": "list",
      "data": [
        {
          "object": "embedding",
          "index": 0,
          "embedding": [0.0023, -0.0093, ...]
        }
      ],
      "model": "<model>",
      "usage": {
        "prompt_tokens": <calculated_tokens>,
        "total_tokens": <calculated_tokens>
      }
    }
    ```

---

## 6. Error Handling & Normalization

1. **Normalized Error Schema**:
   All upstream errors (4xx, 5xx) are transformed into OpenAI error objects:
   ```json
   {
     "error": {
       "message": "Error description from upstream",
       "type": "upstream_error",
       "code": 400
     }
   }
   ```
2. **Gemini Error Unwrapping**:
   Extract message from `{"error": {"code": 400, "message": "...", "status": "INVALID_ARGUMENT"}}`.
3. **Response Headers**:
   Always set `X-Upstream-Provider: <provider>` on all responses (success or error).
4. **Streaming Silence Timeout**:
   Monotonic 30-second silence timeout enforced on all streaming connections. On timeout, emit SSE error frame and `data: [DONE]`.

---

## 7. Database Migration & Observability

### 7.1 PostgreSQL Schema Migration (v2)
In `schema/schema.sql` and `pg_store.c`:
```sql
ALTER TABLE usage_daily ADD COLUMN IF NOT EXISTS cached_prompt_tokens BIGINT NOT NULL DEFAULT 0;
INSERT INTO schema_migrations(version) VALUES (2) ON CONFLICT (version) DO NOTHING;
```

### 7.2 Metrics & Prometheus Exposition
* New counter: `aigate_tokens_cached_total{provider="...", model="..."}`.
* Updated `usage_daily` queries and `/admin/v1/usage` to include `cached_prompt_tokens`.

---

## 8. Testing Strategy & Verification Plan

1. **Unit Tests (`tests/unit/`)**:
   - `test_provider_gemini.c`: Request building, response translation, finish reasons, error payloads.
   - `test_gemini_stream.c`: Streaming chunks, split TCP packets, usageMetadata extraction, `[DONE]` frame.
   - `test_provider_deepseek.c`: `reasoning_content` pass-through, prompt cache hit token extraction.
   - `test_embeddings.c`: `/v1/embeddings` in Mode A (direct) and Mode B (Gemini `:embedContent` / `:batchEmbedContents`).
2. **Integration Tests (`tests/integration/`)**:
   - Update `mock_upstream.py` with mock endpoints for Gemini `:generateContent`, `:streamGenerateContent`, `:embedContent`, and DeepSeek reasoning responses.
   - Run `test_gateway.py` covering end-to-end HTTP requests against running `aigate` binary.
3. **Regression Testing**:
   - Run full test suite via `ctest --test-dir build --output-on-failure` ensuring 100% test pass rate across all existing Plan 1 and Plan 2 test suites.

# aigate Plan 2: SSE Streaming & Anthropic Protocol Adapter — Design Spec

Date: 2026-09-19
Status: Approved

## 1. Overview & Goals

This specification details the architecture and implementation for **Plan 2** of the `aigate` AI Gateway. Building upon the working C17 core gateway (Plan 1), Plan 2 adds:

1. **Production-grade SSE Streaming Pass-Through (`stream: true`)**:
   - Zero-buffering, real-time chunk pumping to client connections via the transport seam.
   - 30-second inter-chunk silence timeout enforcement using monotonic time.
   - Streaming token usage extraction (auto-injecting `stream_options.include_usage: true` where applicable, line-buffer JSON scanning) to ensure accurate metering in Prometheus and deduction against per-key daily token quotas.
2. **Anthropic (Claude) Protocol Translation Adapter**:
   - Bi-directional protocol translation allowing OpenAI-compatible clients to communicate transparently with Anthropic `/v1/messages`.
   - Request transformation: extraction of `system` messages to the top-level field, automatic `max_tokens` default insertion, parameter normalization (`temperature`, `stop_sequences`).
   - Non-streaming response conversion: wrapping Anthropic text content blocks into OpenAI choice objects and translating token usage.
   - Streaming SSE event bridge: state machine converting Anthropic SSE event types (`content_block_delta`, `message_delta`, etc.) to OpenAI-compatible `data: {"choices":[{"delta":...}]}` chunks and `data: [DONE]`.

---

## 2. Architecture & Data Flow

```
+-------------------------------------------------------------+
|                      Client (HTTP/SSE)                      |
+-------------------------------------------------------------+
                              |
                     /v1/chat/completions (stream: true / false)
                              v
+-------------------------------------------------------------+
|             CivetWeb Transport (transport_civetweb.c)       |
+-------------------------------------------------------------+
                              |
                    aigate_request_ctx (stream: true)
                              v
+-------------------------------------------------------------+
|               Core Pipeline (aigate_core.c)                 |
|   1. Auth (Bearer -> key_rec)                               |
|   2. Allowlist Check (key_allows_model)                     |
|   3. Rate Limit Check (rl_allow_request)                    |
|   4. Model Route Resolution (model_router_resolve)          |
+-------------------------------------------------------------+
                              |
              +---------------+---------------+
              |                               |
    provider == "openai"            provider == "anthropic"
              |                               |
              v                               v
+---------------------------+   +---------------------------+
|  provider_openai_build    |   |  provider_anthropic_build |
|  - Inject include_usage   |   |  - System prompt extract  |
|  - Merge default params   |   |  - max_tokens fallback    |
|  - Pass-through / URL     |   |  - Target: /v1/messages   |
+---------------------------+   +---------------------------+
              \                               /
               \                             /
                v                           v
+-------------------------------------------------------------+
|                 upstream_client (libcurl)                   |
|  Non-streaming: upstream_call()                             |
|  Streaming:     upstream_stream_call()                      |
|                 - CURLOPT_WRITEFUNCTION (real-time chunk)   |
|                 - Monotonic 30s inter-chunk silence timeout |
+-------------------------------------------------------------+
                              |
                   Progressive Chunk Stream
                              |
        +---------------------+---------------------+
        |                                           |
provider == "openai"                      provider == "anthropic"
        |                                           |
        v                                           v
[Direct Chunk Pass]                       [SSE Event Bridge]
  - Scan usage line                         - Parse event & data
  - rc->write(chunk)                        - Convert to OpenAI chunk
        \                                           /
         \                                         /
          v                                       v
+-------------------------------------------------------------+
|                    Client HTTP Response                     |
|  Content-Type: text/event-stream; charset=utf-8             |
|  Cache-Control: no-cache                                    |
|  Connection: keep-alive                                     |
+-------------------------------------------------------------+
                              |
                       Stream Finished
                              v
+-------------------------------------------------------------+
|                Usage Accounting & Quota                     |
|  - um_record(um, key_id, model, status, ptok, ctok, lat)   |
|  - rl_reserve_tokens(rl, key_id, quota, ptok + ctok)       |
+-------------------------------------------------------------+
```

---

## 3. Streaming Upstream Client (`upstream_client.h/.c`)

### 3.1 Interface Specification

In addition to `upstream_call()` for synchronous non-streaming requests, `upstream_client` exposes:

```c
typedef int (*upstream_chunk_fn)(void* user_data, const void* chunk, size_t len);

int upstream_stream_call(const char*       url,
                         const char*       upstream_key,
                         const char*       upstream_headers_kv[][2],
                         int               n_headers,
                         const char*       body_json,
                         size_t            body_len,
                         long              silence_timeout_ms,
                         upstream_chunk_fn on_chunk,
                         void*             user_data,
                         int*              out_status);
```

### 3.2 Real-time Chunk Delivery
- `libcurl` option `CURLOPT_WRITEFUNCTION` receives network chunks as soon as TCP buffers arrive.
- Each chunk is immediately forwarded to `on_chunk(user_data, ptr, size * nmemb)`.
- No global buffering is performed; Time To First Token (TTFT) and subsequent token intervals are preserved with sub-millisecond gateway overhead.

### 3.3 Inter-chunk Silence Timeout (30 Seconds)
- The timeout counter tracks elapsed time between consecutive chunks using monotonic clock (`clock_gettime(CLOCK_MONOTONIC, ...)`).
- `CURLOPT_XFERINFOFUNCTION` (progress callback) is registered:
  - Called by libcurl frequently during I/O.
  - If `now_mono_ns - last_chunk_mono_ns > silence_timeout_ms * 1,000,000ull`, the callback returns non-zero, aborting the curl transfer immediately.
  - Return code `-110` (`ETIMEDOUT`) is returned by `upstream_stream_call`.

---

## 4. Streaming Token Usage Extraction & Metering

### 4.1 Upstream Request Auto-Enhancement
For OpenAI-compatible providers, `provider_openai_build` ensures that:
```json
"stream_options": { "include_usage": true }
```
is merged into the outbound request body when `"stream": true` is requested, guaranteeing that the upstream server sends a terminal usage event before `data: [DONE]`.

### 4.2 Stream Parser State Machine
A lightweight chunk parser accumulator (`sse_stream_ctx_t`) handles TCP fragmentation:
1. Maintains a line accumulation buffer (capacity 4096 bytes).
2. Reads incoming chunks line by line (`\n` delimiter).
3. If a line matches `data: ` and contains `"usage"`, parses the JSON object to extract `prompt_tokens` and `completion_tokens`.
4. When `upstream_stream_call` completes:
   - If usage was captured, records exact `prompt_tokens` and `completion_tokens`.
   - If usage was not provided by upstream, falls back to `prompt_tokens = 0, completion_tokens = 0`.
   - Invokes `um_record(...)` and `rl_reserve_tokens(...)` to update latency histograms and deduct from daily quotas.

### 4.3 Error Handling During Streaming
- **Early Failure (Status >= 400 before stream starts)**:
  - If the upstream immediately returns HTTP 4xx or 5xx, the `on_chunk` handler does NOT send 200 HTTP headers to the client.
  - The gateway falls back to writing a standard OpenAI JSON error response (`aigate_write_error(rc, 502, ...)`).
- **Mid-stream Interruption / Timeout**:
  - If the stream is broken or hits 30s silence timeout after headers have been sent:
    1. A synthesized error event is emitted to the client:
       ```
       data: {"error":{"message":"stream interrupted: silence timeout","type":"upstream_error","code":502}}

       data: [DONE]

       ```
    2. The core records the error in `usage_meter` (`status = 502`, increments error counter).

---

## 5. Anthropic Provider Adapter (`provider_anthropic.h/.c`)

### 5.1 Protocol Differences Summary

| Feature | OpenAI Format (`/v1/chat/completions`) | Anthropic Format (`/v1/messages`) |
|---|---|---|
| **Path** | `/chat/completions` | `/messages` |
| **Authentication** | `Authorization: Bearer <key>` | `x-api-key: <key>` |
| **API Version** | N/A | `anthropic-version: 2023-06-01` |
| **System Prompt** | Element in `messages` (`role: "system"`) | Top-level string field `"system": "..."` |
| **Max Tokens** | Optional (`max_tokens`) | Required (`max_tokens`) |
| **Stop Sequences** | `stop` (string or array) | `stop_sequences` (array of strings) |
| **Response Content** | `choices[0].message.content` | `content[0].text` |
| **Token Usage** | `usage: {prompt_tokens, completion_tokens}` | `usage: {input_tokens, output_tokens}` |
| **Streaming Events** | `data: {"choices":[{"delta":{"content":...}}]}` | `event: content_block_delta` / `event: message_delta` |

### 5.2 Request Translation (`provider_anthropic_build`)
1. Parses inbound OpenAI JSON request body.
2. Extracts all messages where `role == "system"`:
   - Concatenates multiple system messages separated by double newlines into the top-level `"system"` string.
3. Filters non-system messages:
   - Validates that roles are strictly `"user"` or `"assistant"`.
   - Converts `content` strings to Anthropic format.
4. Ensures `max_tokens`:
   - If present in the request, copies it.
   - If missing, sets default to `4096`.
5. Maps parameters:
   - `temperature`, `top_p` mapped directly if present.
   - `stop` converted to `stop_sequences` array.
   - `stream` preserved.
6. Headers:
   - Adds `x-api-key: <upstream_key>`.
   - Adds `anthropic-version: 2023-06-01`.

### 5.3 Non-Streaming Response Translation
Converts Anthropic response:
```json
{
  "id": "msg_013Zva2CMHLNnxPQCdQUqGsE",
  "type": "message",
  "role": "assistant",
  "model": "claude-3-5-sonnet-20241022",
  "content": [{"type": "text", "text": "Hello!"}],
  "stop_reason": "end_turn",
  "usage": {"input_tokens": 12, "output_tokens": 8}
}
```
Into OpenAI wire format:
```json
{
  "id": "chatcmpl-msg_013Zva2CMHLNnxPQCdQUqGsE",
  "object": "chat.completion",
  "created": 1726700000,
  "model": "claude-3-5-sonnet-20241022",
  "choices": [{
    "index": 0,
    "message": {"role": "assistant", "content": "Hello!"},
    "finish_reason": "stop"
  }],
  "usage": {"prompt_tokens": 12, "completion_tokens": 8, "total_tokens": 20}
}
```

### 5.4 Streaming Event Bridge State Machine
Anthropic emits discrete event lines followed by JSON data lines:
- `event: message_start` -> contains `message.usage.input_tokens`.
- `event: content_block_start` -> block index tracking.
- `event: content_block_delta` -> contains `delta.text`. Translated and emitted immediately as:
  ```
  data: {"id":"chatcmpl-...","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"..."}}]}

  ```
- `event: message_delta` -> contains `usage.output_tokens` and `delta.stop_reason`.
- `event: message_stop` -> triggers terminal emission:
  ```
  data: [DONE]

  ```

---

## 6. Testing & Verification Strategy

### 6.1 Unit Tests (C Runner)
1. **`test_upstream_streaming`**:
   - Local socket/mock server sending progressive chunks with delays.
   - Asserts chunks arrive in real time.
   - Tests 30s silence timeout triggering abortion.
2. **`test_anthropic_translator`**:
   - Request translation: verifies extraction of system prompt, default `max_tokens`, role conversion.
   - Response translation: verifies JSON structure and token counts.
   - Event bridge: feeds raw Anthropic SSE event sequence, verifies emitted OpenAI chunks and final `[DONE]`.

### 6.2 Integration Tests (Python Pytest)
1. **Streaming E2E**:
   - Mock upstream emits multi-chunk SSE.
   - Gateway verifies `text/event-stream` headers and progressive delivery.
   - Verifies usage token persistence in `usage_daily` after stream finishes.
2. **Anthropic E2E**:
   - Mock Anthropic server receiving translated `/v1/messages`.
   - Client sends OpenAI requests, verifies transparent completion and streaming output.

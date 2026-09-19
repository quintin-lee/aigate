# aigate Implementation Plan 2: SSE Streaming & Anthropic Protocol Adapter

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `aigate` with production-grade SSE streaming pass-through (`stream: true`) with zero buffering, monotonic 30s silence timeouts, and token usage accounting; build a bi-directional Anthropic (Claude) protocol translation adapter (non-streaming and streaming).

**Architecture:** In streaming mode, `upstream_client` delivers chunks progressively via `CURLOPT_WRITEFUNCTION`. `aigate_core` handles `is_streaming`, auto-injects `"stream_options":{"include_usage":true}` for OpenAI upstreams, scans line-buffered events for token counts, and streams chunks through `rc->write`. The Anthropic adapter (`provider_anthropic.c/h`) translates OpenAI `/v1/chat/completions` requests to Anthropic `/v1/messages` (extracting system prompts, setting `max_tokens` default), and bridges Anthropic SSE events to OpenAI-compatible chunks in real time.

**Tech Stack:** C17, CMake, civetweb, jansson, libcurl, libpq, OpenSSL, hdr_histogram.

**Spec:** `docs/superpowers/specs/2026-09-19-sse-streaming-and-anthropic-design.md`

---

## File Structure

```
aigate/
├── src/
│   ├── upstream_client.h/.c      # Add upstream_stream_call + 30s silence timeout
│   ├── aigate_core.h/.c          # Core pipeline stream routing & usage accumulator
│   ├── transport_civetweb.h/.c   # text/event-stream headers & chunk flush
│   ├── provider_openai.h/.c      # Add stream_options injection
│   └── provider_anthropic.h/.c   # [NEW] Anthropic request/response/SSE adapter
├── tests/unit/
│   ├── mock_upstream.h/.c        # Add streaming chunk generator
│   ├── test_upstream_streaming.c # [NEW] Test upstream_stream_call & silence timeout
│   ├── test_stream_pipeline.c    # [NEW] Test core streaming pipeline
│   └── test_provider_anthropic.c # [NEW] Test Anthropic translation & event bridge
└── tests/integration/
    ├── mock_upstream.py          # Add streaming & Anthropic endpoints
    └── test_streaming.py         # [NEW] E2E Python pytest streaming tests
```

---

## Task 1: Upstream Streaming Client (`upstream_stream_call`)

**Files:**
- Modify: `src/upstream_client.h`, `src/upstream_client.c`, `tests/unit/mock_upstream.h`, `tests/unit/mock_upstream.c`
- Create: `tests/unit/test_upstream_streaming.c`

- [ ] **Step 1: Interface in `upstream_client.h`**
  Declare:
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

- [ ] **Step 2: Implementation in `upstream_client.c`**
  - Implement `CURLOPT_WRITEFUNCTION`: forward each chunk to `on_chunk(user_data, chunk, len)`.
  - Update `last_chunk_mono_ns` on each write.
  - Implement `CURLOPT_XFERINFOFUNCTION` (progress callback):
    If `mono_ns() - last_chunk_mono_ns > silence_timeout_ms * 1000000ull`, return non-zero to abort transfer.
  - On abort due to silence timeout, return `-110` (`ETIMEDOUT`).

- [ ] **Step 3: Update `mock_upstream.c` for streaming**
  - Add endpoint `/mock/stream`: emits 3 SSE chunks separated by 10ms delays.
  - Add endpoint `/mock/stream-slow`: emits 1 chunk then sleeps 100ms (for testing timeout).

- [ ] **Step 4: Unit tests in `test_upstream_streaming.c`**
  - Test normal chunk reception: 3 chunks received in order.
  - Test silence timeout: with `silence_timeout_ms = 50`, `/mock/stream-slow` aborts and returns `-110`.
  - Register in `run_tests.c`.

- [ ] **Step 5: Build, verify and commit**
  - Run `./build/tests/aigate_unit_tests`.
  - `git commit -m "feat(upstream): add upstream_stream_call with silence timeout"`

---

## Task 2: Core Streaming Pipeline Seam & Usage Accumulator

**Files:**
- Modify: `src/aigate_core.h`, `src/aigate_core.c`, `src/provider_openai.h`, `src/provider_openai.c`
- Create: `tests/unit/test_stream_pipeline.c`

- [ ] **Step 1: Provider auto-inject `stream_options` in `provider_openai.c`**
  - When request body contains `"stream": true`, merge `"stream_options": {"include_usage": true}` into upstream body.

- [ ] **Step 2: Core streaming dispatch in `aigate_core.c`**
  - Detect `"stream": true` from request JSON.
  - If streaming:
    - Invoke `upstream_stream_call`.
    - Provide `stream_chunk_handler`:
      - Maintains 4KB line buffer to reassemble split lines.
      - Scans for `data: ` line containing `"usage"`.
      - Parses `prompt_tokens` and `completion_tokens`.
      - Calls `rc->write(rc->impl, chunk, len, false)`.
    - After stream finishes:
      - Call `rc->write(rc->impl, "", 0, true)`.
      - Call `um_record` and `rl_reserve_tokens`.
    - If status >= 400 before stream starts:
      - Call `aigate_write_error(rc, 502, "upstream_error", ...)` (headers not sent yet).
    - If error during stream (e.g. timeout):
      - Push SSE error message + `data: [DONE]`.
      - Record error in `um_record`.

- [ ] **Step 3: Unit tests in `test_stream_pipeline.c`**
  - Happy path streaming: client receives SSE chunks, usage is recorded, tokens deducted from quota.
  - Upstream 500 before stream starts: returns 502 JSON error.
  - Mid-stream timeout: emits error event and `[DONE]`.
  - Register in `run_tests.c`.

- [ ] **Step 4: Build, verify and commit**
  - `git commit -m "feat(core): streaming pipeline seam with usage extraction"`

---

## Task 3: CivetWeb Transport Streaming Integration

**Files:**
- Modify: `src/transport_civetweb.c`

- [ ] **Step 1: Streaming response adapter in `transport_civetweb.c`**
  - When `is_streaming` is detected or `rc->set_header("Content-Type", "text/event-stream")` is called:
    - Set `Content-Type: text/event-stream; charset=utf-8`.
    - Set `Cache-Control: no-cache`.
    - Set `Connection: keep-alive`.
    - Flush headers immediately on first chunk.
    - Each `cw_write` call flushes data directly to client socket via `mg_write` without buffering.

- [ ] **Step 2: Build, verify and commit**
  - `git commit -m "feat(transport): wire civetweb text/event-stream chunk flush"`

---

## Task 4: Anthropic Protocol Translator (Non-Streaming)

**Files:**
- Create: `src/provider_anthropic.h`, `src/provider_anthropic.c`, `tests/unit/test_provider_anthropic.c`
- Modify: `src/aigate_core.c`

- [ ] **Step 1: Request builder in `provider_anthropic.c`**
  - Endpoint path: `/v1/messages`.
  - Headers: `x-api-key: <key>`, `anthropic-version: 2023-06-01`.
  - Request body transformation:
    - Concatenate `role == "system"` messages into top-level `"system": "..."`.
    - Convert remaining messages to Anthropic format (`role: "user" | "assistant"`).
    - If `max_tokens` is missing, inject default `4096`.
    - Map `temperature`, `top_p`, `stop` -> `stop_sequences`.

- [ ] **Step 2: Non-streaming response parser in `provider_anthropic.c`**
  - Parse Anthropic response JSON:
    - Extract `content[0].text`.
    - Extract `usage.input_tokens` and `usage.output_tokens`.
    - Transform into OpenAI wire format:
      `{"id":"chatcmpl-...","object":"chat.completion","choices":[{"message":{"content":"..."}}],"usage":{"prompt_tokens":...,"completion_tokens":...}}`.

- [ ] **Step 3: Core pipeline wiring in `aigate_core.c`**
  - Support `route.provider == "anthropic"`.
  - Route through `provider_anthropic_build` and response translation.

- [ ] **Step 4: Unit tests in `test_provider_anthropic.c`**
  - Test system prompt extraction and concatenation.
  - Test `max_tokens` default insertion.
  - Test non-streaming response JSON conversion.
  - Register in `run_tests.c`.

- [ ] **Step 5: Build, verify and commit**
  - `git commit -m "feat(anthropic): request translation and non-streaming response parser"`

---

## Task 5: Anthropic SSE Event Bridge (Streaming)

**Files:**
- Modify: `src/provider_anthropic.h`, `src/provider_anthropic.c`, `src/aigate_core.c`, `tests/unit/test_provider_anthropic.c`

- [ ] **Step 1: Event Bridge State Machine in `provider_anthropic.c`**
  - Parses Anthropic raw SSE lines:
    - `event: message_start` -> extracts `input_tokens`.
    - `event: content_block_delta` -> extracts `delta.text`, formats as:
      `data: {"id":"chatcmpl-...","object":"chat.completion.chunk","choices":[{"delta":{"content":"..."}}]}\n\n`.
    - `event: message_delta` -> extracts `output_tokens`.
    - `event: message_stop` -> formats `data: [DONE]\n\n`.
  - Feeds converted chunks into `rc->write`.

- [ ] **Step 2: Core pipeline streaming integration**
  - Connect Anthropic event bridge when `route.provider == "anthropic"` and `is_streaming == true`.

- [ ] **Step 3: Unit tests in `test_provider_anthropic.c`**
  - Feed mock Anthropic SSE stream (message_start, content_block_delta, message_delta, message_stop).
  - Verify emitted OpenAI chunks, token usage extraction, and terminal `[DONE]`.

- [ ] **Step 4: Build, verify and commit**
  - `git commit -m "feat(anthropic): sse event bridge for streaming Claude completions"`

---

## Task 6: End-to-End Integration Tests

**Files:**
- Modify: `tests/integration/mock_upstream.py`, `tests/integration/test_gateway.py`

- [ ] **Step 1: Mock upstream streaming endpoints in `mock_upstream.py`**
  - Support `/v1/chat/completions` streaming (emit chunks with 10ms delay).
  - Support `/v1/messages` Anthropic mock (non-streaming + SSE streaming).

- [ ] **Step 2: Integration tests in `test_gateway.py`**
  - Test OpenAI streaming: client receives stream chunks progressively, usage persists in DB.
  - Test Anthropic non-streaming: OpenAI request to Claude model returns translated 200 response.
  - Test Anthropic streaming: OpenAI request to Claude model streams chunks and finishes with `[DONE]`.

- [ ] **Step 3: Run full suite & commit**
  - Run all 45+ unit tests and Python integration tests.
  - `git commit -m "test(integration): end-to-end streaming and anthropic scenarios"`

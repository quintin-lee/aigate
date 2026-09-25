# OpenAI Responses API (`/v1/responses`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the OpenAI Responses API (`POST /v1/responses`) endpoint in aigate with strict OpenAI-family passthrough, SSE streaming token extraction, reasoning tokens tracking, and comprehensive unit/integration test coverage.

**Architecture:** Add a new `handle_responses()` dispatch branch in `aigate_core.c` parallel to `handle_chat()`, enforcing that the resolved route uses an OpenAI-compatible provider (rejecting others with HTTP 400 `unsupported_endpoint`). Integrate `provider_openai_build_responses` and `provider_openai_parse_responses_usage` into `provider_openai.c`, extend `usage_requests` with `reasoning_tokens`, and reuse all rate limiting, quota, circuit breaking, and streaming pipeline machinery.

**Tech Stack:** C17, Jansson JSON, libpq (PostgreSQL), CivetWeb HTTP, Python 3 / Pytest / Requests.

---

### File Structure Map

| File | Responsibility |
|---|---|
| `schema/schema.sql` & `src/schema_sql.h` | Migration v8: add `reasoning_tokens BIGINT NOT NULL DEFAULT 0` to `usage_requests` |
| `src/pg_store.h` & `src/pg_store.c` | Add `reasoning_tokens` to `usage_request_row_t`, update `pq_flush_requests` & `pq_query_requests` |
| `src/usage_meter.h` & `src/usage_meter.c` | Add `um_record_ext()` accepting `reasoning_tokens`, update worker ring buffer |
| `src/admin_api.c` | Expose `reasoning_tokens` in `GET /admin/v1/usage` JSON response |
| `src/provider_openai.h` & `src/provider_openai.c` | `build_responses()` (target URL + raw body), `parse_responses_usage()`, SSE usage parser |
| `src/provider_adapter.h` & `src/provider_adapter.c` | Add `build_responses` & `parse_responses` function pointers to `provider_adapter_t` |
| `src/aigate_core.c` | `handle_responses()` branch: auth, rate limits, provider check, dispatch, metrics |
| `src/transport_civetweb.c` | Route `POST /v1/responses` to `aigate_handle_request` |
| `tests/unit/test_pg_store.c` | Verify migration v8 and `reasoning_tokens` flush & query |
| `tests/unit/test_provider_openai.c` | KAT tests for responses request build and non-streaming/streaming usage parsing |
| `tests/unit/test_aigate_core.c` | Unit tests for 400 non-openai rejection, 200 passthrough, and streaming |
| `tests/integration/mock_upstream.py` | Add `/v1/responses` mock handler (JSON and SSE) |
| `tests/integration/test_gateway.py` | E2E integration tests for non-streaming, streaming, and non-openai rejection |

---

### Task 1: Schema Migration v8 & Data Structures (reasoning_tokens)

**Files:**
- Modify: `schema/schema.sql`
- Modify: `src/schema_sql.h`
- Modify: `src/pg_store.h`
- Modify: `src/pg_store.c`
- Modify: `src/usage_meter.h`
- Modify: `src/usage_meter.c`
- Modify: `src/admin_api.c`
- Modify: `tests/unit/test_pg_store.c`
- Modify: `tests/unit/test_admin_api.c`

- [ ] **Step 1: Write failing test in test_pg_store.c for reasoning_tokens**

Add test case verifying `reasoning_tokens` in `usage_request_row_t` is recorded and queried:

```c
TEST_CASE(test_pg_fake_reasoning_tokens)
{
    pg_ops_t    ops;
    pg_store_t* ps = pg_store_open_fake(&ops);
    TEST_ASSERT(ps != NULL, "open fake");

    usage_request_row_t rr = {
        .key_id = 1,
        .model_name = "o3-mini",
        .provider = "openai",
        .http_status = 200,
        .prompt_tokens = 20,
        .completion_tokens = 30,
        .cached_prompt_tokens = 10,
        .reasoning_tokens = 15,
        .latency_ns = 50000000,
        .ts = 1700000000,
    };
    TEST_ASSERT(ops.flush_usage_requests(ops.ctx, &rr, 1) == 0, "flush reasoning row");

    usage_request_row_t out[4];
    int n = 0;
    TEST_ASSERT(ops.query_usage_requests(ops.ctx, 1, 0, out, 4, &n) == 0, "query");
    TEST_ASSERT(n == 1, "found 1 row");
    TEST_ASSERT(out[0].reasoning_tokens == 15, "reasoning_tokens preserved");

    pg_store_close(ps);
}
```

- [ ] **Step 2: Run test to verify compilation/test failure**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: Compile failure on `.reasoning_tokens` field not existing in `usage_request_row_t`.

- [ ] **Step 3: Update schema.sql, schema_sql.h, and pg_store.h**

In `schema/schema.sql` and `src/schema_sql.h`, add Migration v8:
```sql
-- Migration v8: reasoning tokens for OpenAI Responses API / CoT models
ALTER TABLE usage_requests ADD COLUMN IF NOT EXISTS reasoning_tokens BIGINT NOT NULL DEFAULT 0;
INSERT INTO schema_migrations(version) VALUES (8) ON CONFLICT (version) DO NOTHING;
```

In `src/pg_store.h`, add `long reasoning_tokens;` to `struct usage_request_row`:
```c
typedef struct usage_request_row {
    long     key_id;
    char     model_name[128];
    char     provider[32];
    int      http_status;
    long     prompt_tokens;
    long     completion_tokens;
    long     cached_prompt_tokens;
    long     reasoning_tokens;
    uint64_t latency_ns;
    time_t   ts;
} usage_request_row_t;
```

- [ ] **Step 4: Update pg_store.c, usage_meter.h, usage_meter.c, and admin_api.c**

In `src/pg_store.c`:
1. In `pq_flush_requests`: update SQL insert columns to include `reasoning_tokens` (10 parameters per row instead of 9):
```c
"INSERT INTO usage_requests(key_id, model_name, provider, "
"http_status, prompt_tokens, completion_tokens, "
"cached_prompt_tokens, reasoning_tokens, latency_ns, ts) VALUES "
```
Format `r->reasoning_tokens` into string buffer array and bind parameter `$8`.
2. In `pq_query_requests`: update SELECT query:
```c
"SELECT key_id, model_name, provider, http_status, prompt_tokens, "
"completion_tokens, cached_prompt_tokens, reasoning_tokens, latency_ns, ts "
"FROM usage_requests ..."
```
And parse column 7 into `out[i].reasoning_tokens`.

In `src/usage_meter.h`:
Add declaration for `um_record_ext`:
```c
void um_record_ext(usage_meter_t* um,
                   long           key_id,
                   const char*    model,
                   int            http_status,
                   long           prompt_tokens,
                   long           completion_tokens,
                   long           cached_prompt_tokens,
                   long           reasoning_tokens,
                   uint64_t       latency_ns,
                   const char*    provider);
```

In `src/usage_meter.c`:
Implement `um_record_ext` filling `rr->reasoning_tokens = reasoning_tokens;`, and make `um_record` call `um_record_ext(..., 0, latency_ns, provider)`.

In `src/admin_api.c`:
In `format_usage_requests` (around lines 1782 & 1860):
```c
json_object_set_new(o, "reasoning_tokens", json_integer(rows[i].reasoning_tokens));
```

In `tests/unit/test_pg_store.c` and `tests/unit/test_admin_api.c`:
Update `fake_flush_requests` and `fake_query_requests` to copy `reasoning_tokens`. Register `test_pg_fake_reasoning_tokens` in `tests/unit/run_tests.c`.

- [ ] **Step 5: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 6: Commit Task 1**

```bash
git add schema/schema.sql src/schema_sql.h src/pg_store.h src/pg_store.c src/usage_meter.h src/usage_meter.c src/admin_api.c tests/unit/test_pg_store.c tests/unit/test_admin_api.c tests/unit/run_tests.c
git commit -m "feat(store): ✨ add reasoning_tokens to usage_requests and migration v8"
```

---

### Task 2: Provider OpenAI Responses Adapter (`build_responses` & `parse_responses_usage`)

**Files:**
- Modify: `src/provider_openai.h`
- Modify: `src/provider_openai.c`
- Modify: `src/provider_adapter.h`
- Modify: `src/provider_adapter.c`
- Modify: `tests/unit/test_provider_openai.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write KAT unit tests for build_responses and parse_responses_usage**

In `tests/unit/test_provider_openai.c`:
```c
TEST_CASE(test_openai_responses_build)
{
    model_rec_t route = {
        .name = "gpt-4o",
        .provider = "openai",
        .endpoint = "https://api.openai.com/v1",
        .upstream_key = "sk-test-secret-12345",
    };
    char url[512];
    const char* extra_headers[4][2];
    int n_extra = 0;
    char* body = NULL;
    size_t body_len = 0;

    const char* in_body = "{\"model\":\"gpt-4o\",\"input\":\"Hello world\"}";
    int rc = provider_openai_build_responses(&route, in_body, url, sizeof(url),
                                            extra_headers, &n_extra, &body, &body_len);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(strcmp(url, "https://api.openai.com/v1/responses") == 0, "url ends with /responses");
    TEST_ASSERT(n_extra == 2, "2 headers");
    TEST_ASSERT(strcmp(extra_headers[0][0], "Authorization") == 0, "auth header");
    TEST_ASSERT(strcmp(extra_headers[0][1], "Bearer sk-test-secret-12345") == 0, "bearer key");
    TEST_ASSERT(strcmp(extra_headers[1][0], "Content-Type") == 0, "content-type");
    TEST_ASSERT(body != NULL && strcmp(body, in_body) == 0, "raw body preserved");
    free(body);
}

TEST_CASE(test_openai_responses_parse_usage_nonstream)
{
    const char* json_resp =
        "{\"id\":\"resp_123\",\"object\":\"response\",\"status\":\"completed\","
        "\"usage\":{\"input_tokens\":12,\"output_tokens\":25,"
        "\"input_tokens_details\":{\"cached_tokens\":4},"
        "\"output_tokens_details\":{\"reasoning_tokens\":7}}}";

    long ptok = 0, ctok = 0, cached = 0, reasoning = 0;
    int rc = provider_openai_parse_responses_usage(json_resp, strlen(json_resp),
                                                   &ptok, &ctok, &cached, &reasoning);
    TEST_ASSERT(rc == 0, "parse ok");
    TEST_ASSERT(ptok == 12, "input_tokens 12");
    TEST_ASSERT(ctok == 25, "output_tokens 25");
    TEST_ASSERT(cached == 4, "cached_tokens 4");
    TEST_ASSERT(reasoning == 7, "reasoning_tokens 7");
}

TEST_CASE(test_openai_responses_parse_usage_stream)
{
    const char* sse_stream =
        "event: response.created\ndata: {\"type\":\"response.created\"}\n\n"
        "event: response.output_text.delta\ndata: {\"delta\":\"Hello\"}\n\n"
        "event: response.completed\ndata: {\"type\":\"response.completed\","
        "\"response\":{\"usage\":{\"input_tokens\":15,\"output_tokens\":40,"
        "\"input_tokens_details\":{\"cached_tokens\":5},"
        "\"output_tokens_details\":{\"reasoning_tokens\":10}}}}\n\n";

    long ptok = 0, ctok = 0, cached = 0, reasoning = 0;
    int rc = provider_openai_parse_responses_usage(sse_stream, strlen(sse_stream),
                                                   &ptok, &ctok, &cached, &reasoning);
    TEST_ASSERT(rc == 0, "stream parse ok");
    TEST_ASSERT(ptok == 15, "stream ptok 15");
    TEST_ASSERT(ctok == 40, "stream ctok 40");
    TEST_ASSERT(cached == 5, "stream cached 5");
    TEST_ASSERT(reasoning == 10, "stream reasoning 10");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: Compile failure (symbols `provider_openai_build_responses` and `provider_openai_parse_responses_usage` undefined).

- [ ] **Step 3: Implement build_responses and parse_responses_usage in provider_openai.c**

In `src/provider_openai.h`:
Declare functions:
```c
int provider_openai_build_responses(const model_rec_t* route,
                                    const char*        in_body,
                                    char*              url_out,
                                    size_t             url_cap,
                                    const char*        extra_headers[4][2],
                                    int*               n_extra_headers,
                                    char**             out_body,
                                    size_t*            out_body_len);

int provider_openai_parse_responses_usage(const char* body,
                                          size_t      len,
                                          long*       out_input_tokens,
                                          long*       out_output_tokens,
                                          long*       out_cached_tokens,
                                          long*       out_reasoning_tokens);
```

In `src/provider_openai.c`:
1. `provider_openai_build_responses`:
   - URL: `snprintf(url_out, url_cap, "%s%s/responses", route->endpoint, ...)` handling trailing slash if any.
   - Headers: Set `extra_headers[0]` to `"Authorization"`, `"Bearer {route->upstream_key}"`.
   - Set `extra_headers[1]` to `"Content-Type"`, `"application/json"`.
   - Set `*n_extra_headers = 2`.
   - Body: pass-through `in_body` via `strdup` into `*out_body` and set `*out_body_len`.
2. `provider_openai_parse_responses_usage`:
   - If `body` contains `event:` or `data:`, scan lines for `"response.completed"` or `"usage"`. Find the `{` following `data:`. Parse with `json_loads`.
   - Extract from `response.usage` or `usage`: `input_tokens`, `output_tokens`, `cached_tokens` (from `input_tokens_details.cached_tokens`), and `reasoning_tokens` (from `output_tokens_details.reasoning_tokens`).
   - If not streaming, parse `body` directly as JSON object and read from `usage`.
3. In `openai_stream_process_line`:
   - Also check for `"response"` -> `"usage"` and extract `reasoning_tokens`.
   - Add `long reasoning_tokens;` to `openai_bridge_t`.

In `src/provider_adapter.h` & `src/provider_adapter.c`:
- Add `build_responses` and `parse_responses_response` function pointers to `provider_adapter_t`.
- Populate them in `g_provider_openai`.

Register new test cases in `tests/unit/run_tests.c`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 5: Commit Task 2**

```bash
git add src/provider_openai.h src/provider_openai.c src/provider_adapter.h src/provider_adapter.c tests/unit/test_provider_openai.c tests/unit/run_tests.c
git commit -m "feat(provider): ✨ add build_responses and parse_responses_usage to openai provider"
```

---

### Task 3: Core Gateway Pipeline Integration (`handle_responses`)

**Files:**
- Modify: `src/aigate_core.c`
- Modify: `src/transport_civetweb.c`
- Modify: `tests/unit/test_aigate_core.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write unit tests in test_aigate_core.c**

Add unit tests for `/v1/responses`:
1. `test_responses_non_openai_400`:
   - Setup route with `provider = "anthropic"`.
   - Send `POST /v1/responses` with valid key and body `{"model": "claude-3"}`.
   - Assert response status is 400 and error code is `unsupported_endpoint`.
2. `test_responses_pipeline_200`:
   - Setup route with `provider = "openai"`.
   - Mock upstream returns 200 with Response JSON.
   - Assert response status is 200 and body matches upstream.
3. `test_responses_missing_model_400`:
   - Send `POST /v1/responses` with body `{}`.
   - Assert response status is 400 `model_not_found`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: FAIL (path `/v1/responses` not handled, returns 404 or fails assertion).

- [ ] **Step 3: Implement handle_responses in aigate_core.c & transport_civetweb.c**

In `src/aigate_core.c`:
Implement `static int handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)`:
1. `auth_key_resolve(&ac->keys, rq->bearer, &krec)`.
2. Rate limit QPS: `rl_allow_request(...)`. If retry_ms == -1 return 503 `distributed_state_unavailable`.
3. Daily quota check: `rl_remaining_daily(...)`. If LONG_MIN return 503 `distributed_state_unavailable`. If <= 0 return 429 `daily_quota_exceeded`.
4. Parse `model` from `rq->body`. If missing or not in `key_allows_model`: return 400 / 403.
5. Resolve model: `model_router_resolve(ac->router, model, &route)`.
6. **Strict provider check**:
   ```c
   if (strcmp(route.provider, "openai") != 0) {
       aigate_write_error(rc, 400, "unsupported_endpoint",
           "/v1/responses requires an openai-compatible provider");
       key_rec_free(&krec);
       return 0;
   }
   ```
7. Select target candidate via `model_router_select_candidates` & `cb_allow_request`.
8. Call `provider_openai_build_responses(&cur_route, rq->body, ...)` to generate upstream request.
9. Execute via `upstream_call_ext` with responses bridge if streaming or buffer if non-streaming.
10. On success / failure:
    - Update circuit breaker (`cb_record_success` or `cb_record_failure`).
    - Parse usage tokens (including `reasoning_tokens`) via `provider_openai_parse_responses_usage`.
    - Record in usage meter via `um_record_ext(ac->um, krec.key_id, model, status, ptok, ctok, cached, reasoning, latency_ns, target->provider)`.
    - Reserve tokens in rate limiter `rl_reserve_tokens(...)`.
11. In `aigate_handle_request`:
    Add route match:
    ```c
    if (rq->path != NULL && strcmp(rq->path, "/v1/responses") == 0) {
        return handle_responses(ac, rq, rc);
    }
    ```

In `src/transport_civetweb.c`:
Ensure handler for `/v1/responses` maps to `aigate_handle_request`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 5: Commit Task 3**

```bash
git add src/aigate_core.c src/transport_civetweb.c tests/unit/test_aigate_core.c tests/unit/run_tests.c
git commit -m "feat(core): ✨ add /v1/responses endpoint with strict openai validation and usage tracking"
```

---

### Task 4: Integration E2E Tests (Mock Upstream & Pytest)

**Files:**
- Modify: `tests/integration/mock_upstream.py`
- Modify: `tests/integration/test_gateway.py`

- [ ] **Step 1: Add /v1/responses mock handler to mock_upstream.py**

In `tests/integration/mock_upstream.py`:
Add `/responses` path handler in `do_POST`:
1. If streaming (`stream: true` in body):
   Emit SSE events:
   - `event: response.created\ndata: {"type":"response.created","response":{"id":"resp_mock_1"}}\n\n`
   - `event: response.output_text.delta\ndata: {"type":"response.output_text.delta","delta":"Hello from Responses API!"}\n\n`
   - `event: response.completed\ndata: {"type":"response.completed","response":{"id":"resp_mock_1","usage":{"input_tokens":10,"output_tokens":15,"input_tokens_details":{"cached_tokens":3},"output_tokens_details":{"reasoning_tokens":5}}}}\n\n`
2. If non-streaming:
   Return JSON Response object:
   ```json
   {
     "id": "resp_mock_sync_1",
     "object": "response",
     "status": "completed",
     "output": [
       {
         "type": "message",
         "role": "assistant",
         "content": [{"type": "text", "text": "Hello from mock responses!"}]
       }
     ],
     "usage": {
       "input_tokens": 12,
       "output_tokens": 18,
       "input_tokens_details": {"cached_tokens": 4},
       "output_tokens_details": {"reasoning_tokens": 6}
     }
   }
   ```

- [ ] **Step 2: Add test cases to test_gateway.py**

In `tests/integration/test_gateway.py`:
1. `test_responses_openai_passthrough`:
   - Create model with `provider: openai` pointing to mock upstream.
   - Send `POST /v1/responses` with `input: "Hello"`.
   - Verify HTTP 200, response body has `status: completed`.
   - Query `GET /admin/v1/usage` to verify `prompt_tokens == 12`, `completion_tokens == 18`, `reasoning_tokens == 6`.
2. `test_responses_streaming_sse`:
   - Send `POST /v1/responses` with `stream: true`.
   - Verify HTTP 200 with `Content-Type: text/event-stream`.
   - Verify events `response.created`, `response.output_text.delta`, `response.completed` received in order.
3. `test_responses_non_openai_400`:
   - Create model with `provider: anthropic`.
   - Send `POST /v1/responses`.
   - Verify HTTP 400 and error code `unsupported_endpoint`.

- [ ] **Step 3: Run integration test and verify**

Run: `pytest tests/integration/test_gateway.py -k "responses" -v`
Expected: All 3 tests PASS.

- [ ] **Step 4: Run full unit + integration test suite**

Run:
```bash
cmake --build .build -j && ctest --test-dir .build --output-on-failure
pytest tests/integration/test_gateway.py -v
```
Expected: All tests PASS.

- [ ] **Step 5: Commit Task 4**

```bash
git add tests/integration/mock_upstream.py tests/integration/test_gateway.py
git commit -m "test(integration): 🧪 add end-to-end integration tests for /v1/responses endpoint"
```

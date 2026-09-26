# Native Endpoints (Anthropic Messages & Gemini GenerateContent) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add native inbound endpoints for Anthropic Messages (`POST /v1/messages`) and Google Gemini (`POST /v1beta/models/*:generateContent` / `:streamGenerateContent`), with multi-credential authentication (`Authorization: Bearer`, `x-api-key`, `x-goog-api-key`, `?key=`), strict homogeneous provider guards (400 `unsupported_endpoint`), zero-copy streaming with passive usage sniffing, and protocol-native error responses.

**Architecture:** Transport routes `/v1/messages` and `/v1beta/models/*` requests to dedicated core gateway handlers. Authentication extracts credentials flexibly from vendor headers or query parameters. Upstream calls substitute vendor authentication keys while preserving client request payloads verbatim. Non-streaming responses and streaming SSE chunks are passed through directly to clients, while lightweight passive sniffers extract token usage metrics for PostgreSQL audit recording.

**Tech Stack:** C17, CivetWeb, libcurl, Jansson, PostgreSQL (libpq), Python 3 / Pytest.

---

### Task 1: Multi-Credential Authentication Support

**Files:**
- Modify: `src/transport_civetweb.c:100-125`
- Modify: `tests/unit/test_auth_key.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write unit tests in test_auth_key.c for credential extraction**

Add unit tests verifying credential extraction from Bearer tokens, `x-api-key`, `x-goog-api-key`, and URL query string `key=...`:

```c
/* In tests/unit/test_auth_key.c */

TEST_CASE(test_credential_extraction_variants)
{
    /* 1. Bearer token in Authorization header */
    const char* b1 = "Bearer my-secret-key-123";
    const char* k1 = extract_credential_from_headers(b1, NULL, NULL, NULL);
    TEST_ASSERT(k1 != NULL && strcmp(k1, "my-secret-key-123") == 0);

    /* 2. x-api-key header (Anthropic) */
    const char* k2 = extract_credential_from_headers(NULL, "sk-ant-test-456", NULL, NULL);
    TEST_ASSERT(k2 != NULL && strcmp(k2, "sk-ant-test-456") == 0);

    /* 3. x-goog-api-key header (Gemini) */
    const char* k3 = extract_credential_from_headers(NULL, NULL, "AIzaSyTest789", NULL);
    TEST_ASSERT(k3 != NULL && strcmp(k3, "AIzaSyTest789") == 0);

    /* 4. Query string key= (Gemini) */
    const char* k4 = extract_credential_from_headers(NULL, NULL, NULL, "alt=sse&key=AIzaSyQuery999&pretty=true");
    TEST_ASSERT(k4 != NULL && strcmp(k4, "AIzaSyQuery999") == 0);

    /* 5. Empty / missing fallback */
    const char* k5 = extract_credential_from_headers(NULL, NULL, NULL, NULL);
    TEST_ASSERT(k5 != NULL && k5[0] == '\0');
}
```

- [ ] **Step 2: Run test to verify compilation/test failure**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: Compile failure (`extract_credential_from_headers` undefined).

- [ ] **Step 3: Implement credential extraction in transport_civetweb.c and declare in auth_key.h**

In `src/auth_key.h`:
```c
/** @brief Extract client key from HTTP headers and query string.
 * Checks Authorization (Bearer), x-api-key, x-goog-api-key, and ?key=... in order.
 * @return non-null token string (empty string "" if none found). */
const char* extract_credential_from_headers(const char* auth_header,
                                            const char* x_api_key,
                                            const char* x_goog_api_key,
                                            const char* query_string);
```

In `src/auth_key.c`:
Implement `extract_credential_from_headers`.

In `src/transport_civetweb.c`:
Update `extract_bearer(struct mg_connection* conn)`:
```c
static const char*
extract_bearer(struct mg_connection* conn)
{
    const char*                   auth = mg_get_header(conn, "Authorization");
    const char*                   x_api_key = mg_get_header(conn, "x-api-key");
    const char*                   x_goog_key = mg_get_header(conn, "x-goog-api-key");
    const struct mg_request_info* ri = mg_get_request_info(conn);
    const char*                   qs = ri != NULL ? ri->query_string : NULL;
    return extract_credential_from_headers(auth, x_api_key, x_goog_key, qs);
}
```

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 5: Commit Task 1**

```bash
git add src/auth_key.h src/auth_key.c src/transport_civetweb.c tests/unit/test_auth_key.c tests/unit/run_tests.c
git commit -m "feat(auth): ✨ add multi-credential authentication support for anthropic and gemini headers"
```

---

### Task 2: Anthropic Passive Usage Sniffer & Provider Helpers

**Files:**
- Modify: `src/provider_anthropic.h`
- Modify: `src/provider_anthropic.c`
- Modify: `tests/unit/test_provider_anthropic.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write KAT unit tests for Anthropic usage sniffing**

In `tests/unit/test_provider_anthropic.c`:
```c
TEST_CASE(test_anthropic_sniff_usage_json)
{
    const char* json_resp =
        "{\"id\":\"msg_123\",\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"text\",\"text\":\"Hello\"}],"
        "\"usage\":{\"input_tokens\":25,\"output_tokens\":40,"
        "\"cache_creation_input_tokens\":10,\"cache_read_input_tokens\":5}}";

    long ptok = 0, ctok = 0, cached = 0;
    int rc = anthropic_sniff_usage_json(json_resp, &ptok, &ctok, &cached);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ptok == 25);
    TEST_ASSERT(ctok == 40);
    TEST_ASSERT(cached == 5);
}

TEST_CASE(test_anthropic_sniff_streaming_sse)
{
    anthropic_sniffer_t sniffer;
    anthropic_sniffer_init(&sniffer);

    const char* chunk1 =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01\",\"usage\":{\"input_tokens\":30,\"cache_read_input_tokens\":12}}}\n\n";

    const char* chunk2 =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n";

    const char* chunk3 =
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":15}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

    anthropic_sniffer_feed(&sniffer, chunk1, strlen(chunk1));
    anthropic_sniffer_feed(&sniffer, chunk2, strlen(chunk2));
    anthropic_sniffer_feed(&sniffer, chunk3, strlen(chunk3));

    long ptok = 0, ctok = 0, cached = 0;
    anthropic_sniffer_get_tokens(&sniffer, &ptok, &ctok, &cached);
    TEST_ASSERT(ptok == 30);
    TEST_ASSERT(ctok == 15);
    TEST_ASSERT(cached == 12);
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: Compile failure (`anthropic_sniff_usage_json`, `anthropic_sniffer_t` undefined).

- [ ] **Step 3: Implement Anthropic passive usage sniffer in provider_anthropic.c**

In `src/provider_anthropic.h`:
```c
int anthropic_sniff_usage_json(const char* json_str, long* out_ptok, long* out_ctok, long* out_cached);

typedef struct anthropic_sniffer {
    char   line_buf[8192];
    size_t line_len;
    char   current_event[64];
    long   input_tokens;
    long   output_tokens;
    long   cached_tokens;
} anthropic_sniffer_t;

void anthropic_sniffer_init(anthropic_sniffer_t* s);
int  anthropic_sniffer_feed(anthropic_sniffer_t* s, const void* chunk, size_t len);
void anthropic_sniffer_get_tokens(const anthropic_sniffer_t* s, long* out_ptok, long* out_ctok, long* out_cached);
```

In `src/provider_anthropic.c`:
Implement `anthropic_sniff_usage_json`, `anthropic_sniffer_init`, `anthropic_sniffer_feed`, and `anthropic_sniffer_get_tokens`.
Register test cases in `tests/unit/run_tests.c`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 5: Commit Task 2**

```bash
git add src/provider_anthropic.h src/provider_anthropic.c tests/unit/test_provider_anthropic.c tests/unit/run_tests.c
git commit -m "feat(provider): ✨ add passive usage sniffer for anthropic messages"
```

---

### Task 3: Gemini Passive Usage Sniffer & Provider Helpers

**Files:**
- Modify: `src/provider_gemini.h`
- Modify: `src/provider_gemini.c`
- Modify: `tests/unit/test_provider_gemini.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write KAT unit tests for Gemini usage sniffing**

In `tests/unit/test_provider_gemini.c`:
```c
TEST_CASE(test_gemini_sniff_usage_json)
{
    const char* json_resp =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello World\"}],\"role\":\"model\"}}],"
        "\"usageMetadata\":{\"promptTokenCount\":18,\"candidatesTokenCount\":25,\"totalTokenCount\":43,\"cachedContentTokenCount\":8}}";

    long ptok = 0, ctok = 0, cached = 0;
    int rc = gemini_sniff_usage_json(json_resp, &ptok, &ctok, &cached);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ptok == 18);
    TEST_ASSERT(ctok == 25);
    TEST_ASSERT(cached == 8);
}

TEST_CASE(test_gemini_sniff_streaming_sse)
{
    gemini_sniffer_t sniffer;
    gemini_sniffer_init(&sniffer);

    const char* chunk1 =
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello\"}]}}]}\n\n";

    const char* chunk2 =
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\" World\"}]}}],"
        "\"usageMetadata\":{\"promptTokenCount\":22,\"candidatesTokenCount\":33,\"cachedContentTokenCount\":6}}\n\n";

    gemini_sniffer_feed(&sniffer, chunk1, strlen(chunk1));
    gemini_sniffer_feed(&sniffer, chunk2, strlen(chunk2));

    long ptok = 0, ctok = 0, cached = 0;
    gemini_sniffer_get_tokens(&sniffer, &ptok, &ctok, &cached);
    TEST_ASSERT(ptok == 22);
    TEST_ASSERT(ctok == 33);
    TEST_ASSERT(cached == 6);
}
```

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: Compile failure (`gemini_sniff_usage_json`, `gemini_sniffer_t` undefined).

- [ ] **Step 3: Implement Gemini passive usage sniffer in provider_gemini.c**

In `src/provider_gemini.h`:
```c
int gemini_sniff_usage_json(const char* json_str, long* out_ptok, long* out_ctok, long* out_cached);

typedef struct gemini_sniffer {
    char   line_buf[8192];
    size_t line_len;
    long   prompt_tokens;
    long   candidates_tokens;
    long   cached_tokens;
} gemini_sniffer_t;

void gemini_sniffer_init(gemini_sniffer_t* s);
int  gemini_sniffer_feed(gemini_sniffer_t* s, const void* chunk, size_t len);
void gemini_sniffer_get_tokens(const gemini_sniffer_t* s, long* out_ptok, long* out_ctok, long* out_cached);
```

In `src/provider_gemini.c`:
Implement `gemini_sniff_usage_json`, `gemini_sniffer_init`, `gemini_sniffer_feed`, and `gemini_sniffer_get_tokens`.
Register test cases in `tests/unit/run_tests.c`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 5: Commit Task 3**

```bash
git add src/provider_gemini.h src/provider_gemini.c tests/unit/test_provider_gemini.c tests/unit/run_tests.c
git commit -m "feat(provider): ✨ add passive usage sniffer for gemini generateContent"
```

---

### Task 4: Core Gateway Pipeline Integration & Strict Route Guards

**Files:**
- Modify: `src/aigate_core.h`
- Modify: `src/aigate_core.c`
- Modify: `src/transport_civetweb.c`
- Modify: `tests/unit/mock_upstream.c`
- Modify: `tests/unit/test_aigate_core.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: Write unit tests in test_aigate_core.c for native endpoints**

Add unit tests:
1. `test_anthropic_native_pipeline_200`:
   - Send `POST /v1/messages` with model `"claude-test"` and Anthropic JSON body.
   - Assert response status is 200, Content-Type is `application/json`, response body matches upstream.
2. `test_anthropic_native_non_anthropic_400`:
   - Send `POST /v1/messages` with model `"gpt-4o"` (OpenAI provider).
   - Assert response status is 400, body contains `"type":"error"`, `"type":"invalid_request_error"`.
3. `test_gemini_native_pipeline_200`:
   - Send `POST /v1beta/models/gemini-1.5-flash:generateContent` with Gemini JSON body.
   - Assert response status is 200, Content-Type is `application/json`, response matches upstream.
4. `test_gemini_native_non_gemini_400`:
   - Send `POST /v1beta/models/gpt-4o:generateContent`.
   - Assert response status is 400, body contains `"error":{"code":400,"status":"INVALID_ARGUMENT"}`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build .build -j && ctest --test-dir .build -R unit`
Expected: Test failures (paths `/v1/messages` and `/v1beta/models/*` return 404 or unhandled).

- [ ] **Step 3: Implement handle_anthropic_messages and handle_gemini_generate**

In `src/aigate_core.c`:
1. Implement error formatters:
   - `aigate_write_anthropic_error(rc, http_status, error_type, message)`
   - `aigate_write_gemini_error(rc, http_status, status_str, message)`
2. Implement `handle_anthropic_messages(ac, rq, rc)`:
   - Auth via `auth_key_resolve`
   - QPS rate limit & daily token quota check
   - Extract model from JSON body `{"model": "..."}`
   - Route resolution via `mr_resolve_candidate`
   - **Route Guard**: verify `route.provider == "anthropic"`, else return 400 `aigate_write_anthropic_error` (`invalid_request_error`, `unsupported_endpoint`)
   - Candidate loop: rewrite headers with `x-api-key: {upstream_key}`, pass raw client body
   - If streaming (`stream: true` in body):
     - SSE write loop with `anthropic_sniffer_feed`
     - On completion, record `um_record_ext`
   - If non-streaming:
     - Execute upstream call, sniff tokens via `anthropic_sniff_usage_json`, record `um_record_ext`
3. Implement `handle_gemini_generate(ac, rq, rc)`:
   - Auth via `auth_key_resolve`
   - QPS rate limit & daily token quota check
   - Extract model from URL path (`/v1beta/models/{model}:generateContent` or `:streamGenerateContent`)
   - Route resolution via `mr_resolve_candidate`
   - **Route Guard**: verify `route.provider in ["gemini", "google"]`, else return 400 `aigate_write_gemini_error` (`INVALID_ARGUMENT`, `unsupported_endpoint`)
   - Candidate loop: rewrite header `x-goog-api-key: {upstream_key}`, pass raw client body
   - If streaming (path contains `:streamGenerateContent`):
     - SSE write loop with `gemini_sniffer_feed`
     - On completion, record `um_record_ext`
   - If non-streaming:
     - Execute upstream call, sniff tokens via `gemini_sniff_usage_json`, record `um_record_ext`
4. In `aigate_handle_request`:
   - Dispatch `/v1/messages` to `handle_anthropic_messages`
   - Dispatch `/v1beta/models/*` and `/v1/models/*:generateContent` to `handle_gemini_generate`
5. In `src/transport_civetweb.c`:
   - Register `/v1beta/` handler with `mg_set_request_handler(cw->ctx, "/v1beta/", handle_v1, cw);`
6. In `tests/unit/mock_upstream.c`:
   - Support mock endpoints for `/messages`, `/v1/messages`, and `/v1beta/models/*`.

- [ ] **Step 4: Run tests and verify they pass**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% tests passed.

- [ ] **Step 5: Commit Task 4**

```bash
git add src/aigate_core.h src/aigate_core.c src/transport_civetweb.c tests/unit/mock_upstream.c tests/unit/test_aigate_core.c tests/unit/run_tests.c
git commit -m "feat(core): ✨ add native anthropic and gemini endpoints with strict provider guard and error rendering"
```

---

### Task 5: End-to-End Integration Tests (Mock Upstream & Pytest)

**Files:**
- Modify: `tests/integration/mock_upstream.py`
- Modify: `tests/integration/test_gateway.py`

- [ ] **Step 1: Add native mock handlers to mock_upstream.py**

In `tests/integration/mock_upstream.py`:
1. In `do_POST`:
   - Handle `/v1/messages` and `/messages`:
     - If `"stream": true`: stream Anthropic SSE chunks (`message_start`, `content_block_start`, `content_block_delta`, `message_delta`, `message_stop`).
     - Else: return Anthropic message JSON with `usage`.
   - Handle `/v1beta/models/{model}:generateContent` and `:streamGenerateContent`:
     - If `:streamGenerateContent`: stream Gemini SSE chunks with `usageMetadata` on the final chunk.
     - Else: return Gemini generateContent JSON with `usageMetadata`.

- [ ] **Step 2: Add integration tests in test_gateway.py**

In `tests/integration/test_gateway.py` (before `test_admin_lockout_429`):
1. `test_anthropic_native_passthrough`:
   - Seed Anthropic model `"claude-3-5-sonnet"` with provider `"anthropic"`.
   - Send `POST /v1/messages` with `x-api-key: <gateway_key>`, body `{"model": "claude-3-5-sonnet", "messages": [{"role": "user", "content": "hi"}]}`.
   - Assert HTTP 200, response matches Anthropic schema.
   - Query `/admin/v1/usage/requests` to verify `prompt_tokens`, `completion_tokens`, and provider `"anthropic"` are recorded.
2. `test_anthropic_native_streaming_sse`:
   - Send `POST /v1/messages` with `stream: true`.
   - Read SSE chunks and verify events (`message_start`, `content_block_delta`, etc.).
   - Verify usage row in PostgreSQL.
3. `test_anthropic_native_non_anthropic_400`:
   - Send `POST /v1/messages` with model `"gpt-4o"` (OpenAI provider).
   - Assert HTTP 400 and Anthropic error format `{"type": "error", "error": {"type": "invalid_request_error"}}`.
4. `test_gemini_native_passthrough`:
   - Seed Gemini model `"gemini-1.5-flash"` with provider `"gemini"`.
   - Send `POST /v1beta/models/gemini-1.5-flash:generateContent` with `x-goog-api-key: <gateway_key>`.
   - Assert HTTP 200 and Gemini response schema.
   - Query `/admin/v1/usage/requests` to verify usage.
5. `test_gemini_native_streaming_sse`:
   - Send `POST /v1beta/models/gemini-1.5-flash:streamGenerateContent?alt=sse&key=<gateway_key>`.
   - Read SSE stream and verify chunks.
   - Verify usage row in PostgreSQL.
6. `test_gemini_native_non_gemini_400`:
   - Send `POST /v1beta/models/gpt-4o:generateContent`.
   - Assert HTTP 400 and Gemini error format `{"error": {"code": 400, "status": "INVALID_ARGUMENT"}}`.

- [ ] **Step 3: Run integration tests**

Run: `pytest tests/integration/test_gateway.py -k "native" -v`
Expected: All native endpoint tests PASS.

- [ ] **Step 4: Run full unit + integration test suite**

Run:
```bash
ctest --test-dir .build --output-on-failure
pytest tests/integration/test_gateway.py -v
```
Expected: 100% tests PASS across both suites.

- [ ] **Step 5: Commit Task 5**

```bash
git add tests/integration/mock_upstream.py tests/integration/test_gateway.py
git commit -m "test(integration): 🧪 add end-to-end integration tests for native anthropic and gemini endpoints"
```

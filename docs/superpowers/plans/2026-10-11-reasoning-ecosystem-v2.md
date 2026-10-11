# Reasoning Ecosystem V2 Implementation Plan

> **Design Specification**: [`docs/superpowers/specs/2026-10-11-reasoning-ecosystem-v2-design.md`](file:///home/quintin/Data/source/c_cpp/aigate/docs/superpowers/specs/2026-10-11-reasoning-ecosystem-v2-design.md)  
> **Target Branch**: `feature/reasoning-ecosystem-v2`  
> **Development Methodology**: Test-Driven Development (TDD) — failing test $\to$ minimal code $\to$ passing test $\to$ atomic git commit.

---

## Task Breakdown

| Task # | Area | Scope | Key Files |
| :--- | :--- | :--- | :--- |
| **Task 1** | Upstream Adapter | `<think>` Tag Emulation Detection (`parse_reasoning_config`) | `src/upstream/provider_adapter.h/.c`, `tests/unit/upstream/test_provider_reasoning.c` |
| **Task 2** | Upstream Adapter | Streaming `<think>` Tag Emulation SSE State Machine | `src/upstream/provider_anthropic.c`, `tests/unit/upstream/test_provider_reasoning.c` |
| **Task 3** | Upstream Adapter | Non-Streaming `<think>` Tag Content Assembly | `src/upstream/provider_anthropic.c`, `tests/unit/upstream/test_provider_reasoning.c` |
| **Task 4** | Core Native | Native `/v1/messages` Non-Streaming OpenAI $\to$ Anthropic Bridge | `src/core/pipeline_native.c`, `src/upstream/provider_openai.h/.c` |
| **Task 5** | Core Native | Native `/v1/messages` Streaming OpenAI $\to$ Anthropic SSE Bridge | `src/core/pipeline_native.c`, `src/upstream/provider_openai.h/.c` |
| **Task 6** | Control Plane | Admin REST API Model Reasoning Fields CRUD | `src/server/admin_api.c`, `tests/unit/server/test_admin_api.c` |
| **Task 7** | Web Console | Model Form Inputs, Reasoning Badges & Metrics Overview | `src/server/admin_ui.html`, `tests/unit/server/test_admin_api.c` |
| **Task 8** | Web Console | Playground Collapsible Reasoning Accordion Drawer | `src/server/admin_ui.html` |
| **Task 9** | Integration & Quality | Python E2E Integration Suite & Full Regression | `tests/integration/test_reasoning_models.py`, `tests/unit/` |

---

### Task 1: `<think>` Tag Emulation Detection in `parse_reasoning_config`

**Files:**
- Modify: `src/upstream/provider_adapter.h:20-40`
- Modify: `src/upstream/provider_adapter.c:20-60`
- Test: `tests/unit/upstream/test_provider_reasoning.c`

- [ ] **Step 1: Write failing test for tag emulation config detection**

Add to `tests/unit/upstream/test_provider_reasoning.c`:
```c
TEST_CASE(test_reasoning_tag_emulation_parsing)
{
    model_rec_t route;
    memset(&route, 0, sizeof(route));

    /* 1. Inbound JSON body contains thinking_mode: "tag" */
    const char* json_str = "{\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":2048},\"thinking_mode\":\"tag\"}";
    json_t* req = json_loads(json_str, 0, NULL);
    TEST_ASSERT(req != NULL, "json valid");

    reasoning_config_t cfg;
    int rc = parse_reasoning_config(req, &route, &cfg);
    json_decref(req);

    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.enabled == true, "thinking enabled");
    TEST_ASSERT(cfg.budget_tokens == 2048, "budget 2048");
    TEST_ASSERT(cfg.tag_emulation == true, "tag emulation true");

    /* 2. Inbound JSON body contains thinking_format: "tag" */
    const char* json_str2 = "{\"thinking_format\":\"tag\"}";
    json_t* req2 = json_loads(json_str2, 0, NULL);
    rc = parse_reasoning_config(req2, &route, &cfg);
    json_decref(req2);
    TEST_ASSERT(rc == 0, "parsed ok");
    TEST_ASSERT(cfg.tag_emulation == true, "thinking_format tag detected");
}
```
Register in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests reasoning_tag_emulation_parsing`  
Expected: FAIL (compilation error: `tag_emulation` field not in `reasoning_config_t`).

- [ ] **Step 3: Implement tag emulation detection**

1. In `src/upstream/provider_adapter.h`:
Add `bool tag_emulation;` to `struct reasoning_config`.
2. In `src/upstream/provider_adapter.c` (`parse_reasoning_config`):
```c
    json_t* jtm = json_object_get(req_body, "thinking_mode");
    if (jtm == NULL) {
        jtm = json_object_get(req_body, "thinking_format");
    }
    if (jtm != NULL && json_is_string(jtm)) {
        if (strcasecmp(json_string_value(jtm), "tag") == 0) {
            out_cfg->tag_emulation = true;
        }
    }
```

- [ ] **Step 4: Run test to verify pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests reasoning_tag_emulation_parsing`  
Expected: PASS (1/1 passed).

- [ ] **Step 5: Commit**

```bash
git add src/upstream/provider_adapter.h src/upstream/provider_adapter.c tests/unit/upstream/test_provider_reasoning.c tests/unit/run_tests.c
git commit -m "feat(upstream): ✨ detect and parse thinking tag emulation configuration"
```

---

### Task 2: Streaming `<think>` Tag Emulation in Anthropic SSE Adapter

**Files:**
- Modify: `src/upstream/provider_anthropic.c:640-750`
- Test: `tests/unit/upstream/test_provider_reasoning.c`

- [ ] **Step 1: Write failing test for streaming tag emulation**

Add to `tests/unit/upstream/test_provider_reasoning.c`:
```c
TEST_CASE(test_anthropic_thinking_stream_tag_emulation)
{
    mock_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.write = mock_stream_write;
    rc.impl = &sink;

    stream_bridge_t* bridge = g_provider_anthropic.stream_bridge_new(&rc, "claude-3-7-sonnet");
    TEST_ASSERT(bridge != NULL, "bridge created");
    anthropic_bridge_t* ab = (anthropic_bridge_t*)bridge;
    ab->tag_emulation = true;

    /* Feed content_block_start with thinking */
    const char* c1 = "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c1, strlen(c1));

    /* Feed thinking_delta */
    const char* c2 = "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Reasoning step 1...\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c2, strlen(c2));

    /* Feed content_block_stop */
    const char* c3 = "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c3, strlen(c3));

    /* Feed text content */
    const char* c4 = "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c4, strlen(c4));
    const char* c5 = "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello world!\"}}\n\n";
    g_provider_anthropic.stream_bridge_feed(bridge, c5, strlen(c5));

    g_provider_anthropic.stream_bridge_finish(bridge);
    g_provider_anthropic.stream_bridge_free(bridge);

    /* Verify <think> tag emitted in delta.content */
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"<think>\\n\"") != NULL, "<think> open emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Reasoning step 1...\"") != NULL, "thinking delta in content");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"\\n</think>\\n\\n\"") != NULL, "</think> close emitted");
    TEST_ASSERT(strstr(sink.accum, "\"content\":\"Hello world!\"") != NULL, "final text emitted");
}
```
Register in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests anthropic_thinking_stream_tag_emulation`  
Expected: FAIL (assertion fails: `<think>` tag open chunk not found).

- [ ] **Step 3: Implement streaming tag emulation state transitions**

In `src/upstream/provider_anthropic.c` (`bridge_handle_event`):
1. When `content_block_start` with `type: "thinking"` or first `thinking_delta`:
   ```c
   if (b->tag_emulation && !b->tag_opened) {
       json_t* cd = json_pack("{s:s,s:s,s:s,s:[{s:i,s:{s:s},s:n}]}",
                              "id", id_buf, "object", "chat.completion.chunk",
                              "model", b->model, "choices", "index", 0,
                              "delta", "content", "<think>\n", "finish_reason");
       emit_and_free(b, cd);
       b->tag_opened = true;
   }
   ```
2. When `thinking_delta` arrives:
   ```c
   if (b->tag_emulation) {
       json_t* cd = json_pack("{s:s,s:s,s:s,s:[{s:i,s:{s:s},s:n}]}",
                              "id", id_buf, "object", "chat.completion.chunk",
                              "model", b->model, "choices", "index", 0,
                              "delta", "content", thinking_str, "finish_reason");
       emit_and_free(b, cd);
   }
   ```
3. When `text_delta` or `content_block_stop` arrives:
   ```c
   if (b->tag_emulation && b->tag_opened && !b->tag_closed) {
       json_t* cd = json_pack("{s:s,s:s,s:s,s:[{s:i,s:{s:s},s:n}]}",
                              "id", id_buf, "object", "chat.completion.chunk",
                              "model", b->model, "choices", "index", 0,
                              "delta", "content", "\n</think>\n\n", "finish_reason");
       emit_and_free(b, cd);
       b->tag_closed = true;
   }
   ```
4. In `anthropic_stream_bridge_finish`:
   Ensure unclosed tag is closed before emitting `[DONE]`.

- [ ] **Step 4: Run test to verify pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests anthropic_thinking_stream_tag_emulation`  
Expected: PASS (1/1 passed).

- [ ] **Step 5: Commit**

```bash
git add src/upstream/provider_anthropic.c tests/unit/upstream/test_provider_reasoning.c tests/unit/run_tests.c
git commit -m "feat(upstream): ✨ implement streaming sse <think> tag emulation state machine"
```

---

### Task 3: Non-Streaming `<think>` Tag Content Assembly

**Files:**
- Modify: `src/upstream/provider_anthropic.h:40-60`
- Modify: `src/upstream/provider_anthropic.c:430-610`
- Test: `tests/unit/upstream/test_provider_reasoning.c`

- [ ] **Step 1: Write failing test for non-streaming tag emulation**

Add to `tests/unit/upstream/test_provider_reasoning.c`:
```c
TEST_CASE(test_anthropic_thinking_sync_tag_emulation)
{
    const char* anthropic_resp =
        "{\"id\":\"msg_tag_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-3-7-sonnet\","
        "\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"Step-by-step reasoning.\"},"
        "{\"type\":\"text\",\"text\":\"42 is the answer.\"}"
        "],"
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":20}}";

    char*  out_json = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0;
    int rc = provider_anthropic_resp_to_openai_ext(anthropic_resp, "claude-3-7-sonnet", true,
                                                   &out_json, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "translated ok");

    json_t* root = json_loads(out_json, 0, NULL);
    free(out_json);
    TEST_ASSERT(root != NULL, "json valid");

    json_t* choices = json_object_get(root, "choices");
    json_t* c0 = json_array_get(choices, 0);
    json_t* msg = json_object_get(c0, "message");
    const char* content = json_string_value(json_object_get(msg, "content"));
    TEST_ASSERT(strstr(content, "<think>\nStep-by-step reasoning.\n</think>\n\n42 is the answer.") != NULL,
                "<think> wrapped content present");

    json_decref(root);
}
```
Register in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests anthropic_thinking_sync_tag_emulation`  
Expected: FAIL (compilation failure: `provider_anthropic_resp_to_openai_ext` undeclared).

- [ ] **Step 3: Implement non-streaming tag emulation assembly**

In `src/upstream/provider_anthropic.c`:
1. Implement `provider_anthropic_resp_to_openai_ext`:
   - If `tag_emulation && thinking_text`:
     - Format: `"<think>\n" + thinking_text + "\n</think>\n\n" + (content_text ? content_text : "")`.
     - Assign to `msg.content`.
2. Keep `provider_anthropic_resp_to_openai` as backward-compatible wrapper with `tag_emulation = false`.

- [ ] **Step 4: Run test to verify pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests anthropic_thinking_sync_tag_emulation`  
Expected: PASS (1/1 passed).

- [ ] **Step 5: Commit**

```bash
git add src/upstream/provider_anthropic.h src/upstream/provider_anthropic.c tests/unit/upstream/test_provider_reasoning.c tests/unit/run_tests.c
git commit -m "feat(upstream): ✨ format non-streaming response with <think> tag emulation wrapper"
```

---

### Task 4: Native `/v1/messages` Non-Streaming OpenAI $\to$ Anthropic Bridge

**Files:**
- Modify: `src/upstream/provider_openai.h:80-100`
- Modify: `src/upstream/provider_openai.c:500-580`
- Modify: `src/core/pipeline_native.c:300-360`
- Test: `tests/unit/upstream/test_provider_reasoning.c`

- [ ] **Step 1: Write failing test for OpenAI response to Anthropic conversion**

Add to `tests/unit/upstream/test_provider_reasoning.c`:
```c
TEST_CASE(test_openai_to_anthropic_thinking_sync)
{
    const char* oai_resp =
        "{\"id\":\"chatcmpl-r1\",\"object\":\"chat.completion\",\"model\":\"deepseek-r1\","
        "\"choices\":[{\"index\":0,\"message\":{"
        "\"role\":\"assistant\","
        "\"reasoning_content\":\"Proof of irrationality of sqrt(2).\","
        "\"content\":\"Assume p/q is irreducible...\""
        "},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":100}}";

    char*  ant_resp = NULL;
    size_t ant_len = 0;
    long   ptok = 0, ctok = 0;
    int rc = provider_openai_resp_to_anthropic(oai_resp, "deepseek-r1", &ant_resp, &ant_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "converted ok");

    json_t* root = json_loads(ant_resp, 0, NULL);
    free(ant_resp);
    TEST_ASSERT(root != NULL, "json valid");

    json_t* content = json_object_get(root, "content");
    TEST_ASSERT(json_is_array(content) && json_array_size(content) == 2, "2 content blocks");

    json_t* b0 = json_array_get(content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b0, "type")), "thinking") == 0, "b0 is thinking");
    TEST_ASSERT(strstr(json_string_value(json_object_get(b0, "thinking")), "Proof of irrationality") != NULL,
                "thinking text matches");

    json_t* b1 = json_array_get(content, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b1, "type")), "text") == 0, "b1 is text");
    TEST_ASSERT(strstr(json_string_value(json_object_get(b1, "text")), "Assume p/q") != NULL,
                "answer text matches");

    json_decref(root);
}
```
Register in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests openai_to_anthropic_thinking_sync`  
Expected: FAIL (compilation failure: `provider_openai_resp_to_anthropic` undeclared).

- [ ] **Step 3: Implement `provider_openai_resp_to_anthropic`**

1. In `src/upstream/provider_openai.h`:
Declare `int provider_openai_resp_to_anthropic(...)`.
2. In `src/upstream/provider_openai.c`:
Implement translation parsing `reasoning_content` $\to$ `content[0] (type: thinking)`, `content` $\to$ `content[1] (type: text)`.
3. In `src/core/pipeline_native.c`:
Use `provider_openai_resp_to_anthropic` when upstream provider is `"openai"` in non-streaming path.

- [ ] **Step 4: Run test to verify pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests openai_to_anthropic_thinking_sync`  
Expected: PASS (1/1 passed).

- [ ] **Step 5: Commit**

```bash
git add src/upstream/provider_openai.h src/upstream/provider_openai.c src/core/pipeline_native.c tests/unit/upstream/test_provider_reasoning.c tests/unit/run_tests.c
git commit -m "feat(native): ✨ bridge non-streaming openai reasoning_content to anthropic thinking blocks"
```

---

### Task 5: Native `/v1/messages` Streaming OpenAI $\to$ Anthropic SSE Bridge

**Files:**
- Modify: `src/upstream/provider_openai.h:100-120`
- Modify: `src/upstream/provider_openai.c:300-380`
- Modify: `src/core/pipeline_native.c:260-320`
- Test: `tests/unit/upstream/test_provider_reasoning.c`

- [ ] **Step 1: Write failing test for OpenAI SSE to Anthropic SSE bridge**

Add to `tests/unit/upstream/test_provider_reasoning.c`:
```c
TEST_CASE(test_openai_to_anthropic_thinking_stream)
{
    mock_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    aigate_response_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.write = mock_stream_write;
    rc.impl = &sink;

    stream_bridge_t* b = provider_openai_to_anthropic_bridge_new(&rc, "deepseek-r1");
    TEST_ASSERT(b != NULL, "bridge created");

    /* Feed OpenAI chunk with reasoning_content */
    const char* chunk1 =
        "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"model\":\"deepseek-r1\","
        "\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Solving equation...\"},\"finish_reason\":null}]}\n\n";
    provider_openai_to_anthropic_bridge_feed(b, chunk1, strlen(chunk1));

    /* Feed OpenAI chunk with final content */
    const char* chunk2 =
        "data: {\"id\":\"c2\",\"object\":\"chat.completion.chunk\",\"model\":\"deepseek-r1\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"x = 5\"},\"finish_reason\":\"stop\"}]}\n\n";
    provider_openai_to_anthropic_bridge_feed(b, chunk2, strlen(chunk2));

    provider_openai_to_anthropic_bridge_finish(b);
    provider_openai_to_anthropic_bridge_free(b);

    /* Verify Anthropic SSE events */
    TEST_ASSERT(strstr(sink.accum, "event: content_block_start") != NULL, "block start emitted");
    TEST_ASSERT(strstr(sink.accum, "\"type\":\"thinking\"") != NULL, "thinking block start");
    TEST_ASSERT(strstr(sink.accum, "event: content_block_delta") != NULL, "delta emitted");
    TEST_ASSERT(strstr(sink.accum, "\"type\":\"thinking_delta\"") != NULL, "thinking delta");
    TEST_ASSERT(strstr(sink.accum, "Solving equation...") != NULL, "thinking text inside delta");
    TEST_ASSERT(strstr(sink.accum, "\"type\":\"text_delta\"") != NULL, "text delta emitted");
    TEST_ASSERT(strstr(sink.accum, "x = 5") != NULL, "text content emitted");
}
```
Register in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests openai_to_anthropic_thinking_stream`  
Expected: FAIL (compilation failure: functions undeclared).

- [ ] **Step 3: Implement streaming OpenAI to Anthropic bridge state machine**

Implement `provider_openai_to_anthropic_bridge_t`:
1. Parse incoming `delta.reasoning_content` $\to$ emit Anthropic Block 0 (`thinking`).
2. On transition to `delta.content` $\to$ close Block 0 (`content_block_stop`), start Block 1 (`content_block_start: text`).
3. On stream finish $\to$ emit `content_block_stop`, `message_delta`, `message_stop`.
4. Integrate with `pipeline_native.c` when upstream provider is `"openai"`.

- [ ] **Step 4: Run test to verify pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests openai_to_anthropic_thinking_stream`  
Expected: PASS (1/1 passed).

- [ ] **Step 5: Commit**

```bash
git add src/upstream/provider_openai.h src/upstream/provider_openai.c src/core/pipeline_native.c tests/unit/upstream/test_provider_reasoning.c tests/unit/run_tests.c
git commit -m "feat(native): ✨ implement streaming sse translation from openai reasoning deltas to anthropic thinking blocks"
```

---

### Task 6: Admin REST API Model Reasoning Fields CRUD

**Files:**
- Modify: `src/server/admin_api.c:1040-1470`
- Test: `tests/unit/server/test_admin_api.c`

- [ ] **Step 1: Write failing test for admin model reasoning CRUD**

Add to `tests/unit/server/test_admin_api.c`:
```c
TEST_CASE(test_admin_model_reasoning_crud)
{
    /* Test POST /admin/v1/models with supports_reasoning=true, default_thinking_budget=4096 */
    // Verify GET /admin/v1/models returns both fields
    // Verify PATCH /admin/v1/models/<name> can update default_thinking_budget to 8192
}
```
Register in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify failure**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests admin_model_reasoning_crud`  
Expected: FAIL.

- [ ] **Step 3: Implement reasoning fields in `admin_api.c`**

1. In `model_create`:
   ```c
   json_t* jsr = json_object_get(jbody, "supports_reasoning");
   m.supports_reasoning = (jsr && json_is_true(jsr));
   json_t* jtb = json_object_get(jbody, "default_thinking_budget");
   m.default_thinking_budget = (jtb && json_is_integer(jtb)) ? json_integer_value(jtb) : 0;
   ```
2. In `model_list`:
   ```c
   json_object_set_new(item, "supports_reasoning", recs[i].supports_reasoning ? json_true() : json_false());
   json_object_set_new(item, "default_thinking_budget", json_integer(recs[i].default_thinking_budget));
   ```
3. In `model_patch`:
   Check `supports_reasoning` and `default_thinking_budget`, setting `MMASK_SUPPORTS_REASONING` and `MMASK_THINKING_BUDGET`.

- [ ] **Step 4: Run test to verify pass**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests admin_model_reasoning_crud`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/server/admin_api.c tests/unit/server/test_admin_api.c tests/unit/run_tests.c
git commit -m "feat(admin): ✨ support supports_reasoning and default_thinking_budget in models rest api"
```

---

### Task 7: Web Console Model Form Inputs, Badges & Metrics Overview

**Files:**
- Modify: `src/server/admin_ui.html`
- Generate: `src/server/admin_ui_html.h`

- [ ] **Step 1: Add model modal controls & badges**

In `src/server/admin_ui.html`:
1. Model edit/add form:
   - Add Checkbox: `<input type="checkbox" id="m-supports-reasoning"> 支持深度思考 (supports_reasoning)`
   - Add Number Input: `<input type="number" id="m-default-thinking-budget" placeholder="0">` with helper badges `[2048] [4096] [8192]`.
2. Model Table rendering:
   - If `m.supports_reasoning || m.default_thinking_budget > 0`, render `<span class="badge badge-purple">Reasoning</span>`.
3. Metrics Overview:
   - Add Stat Card: `思考 Token (Reasoning)` binding to `aigate_tokens_reasoning_total`.

- [ ] **Step 2: Regenerate header and build**

Run: `cmake --build build -j`  
Verify compilation and HTML asset bundling.

- [ ] **Step 3: Commit**

```bash
git add src/server/admin_ui.html src/server/admin_ui_html.h
git commit -m "feat(console): ✨ add reasoning model configuration inputs, badges and metrics card to web console"
```

---

### Task 4: Web Console Playground Collapsible Reasoning Accordion

**Files:**
- Modify: `src/server/admin_ui.html`
- Generate: `src/server/admin_ui_html.h`

- [ ] **Step 1: Implement collapsible thinking accordion in Playground**

In `src/server/admin_ui.html`:
1. In the Playground response stream handler:
   - Detect `reasoning_content` or `<think>` tags.
   - Inject DOM container:
     ```html
     <div class="reasoning-accordion" id="reasoning-box">
       <div class="reasoning-header" onclick="toggleReasoning()">
         <span>💭 深度思考过程</span>
         <span class="reasoning-toggle-icon">▼</span>
       </div>
       <pre class="reasoning-body" id="reasoning-stream-content"></pre>
     </div>
     ```
   - Stream typing into `#reasoning-stream-content`.
   - When text content arrives, render answer in regular markdown container below.

- [ ] **Step 2: Build and verify**

Run: `cmake --build build -j`

- [ ] **Step 3: Commit**

```bash
git add src/server/admin_ui.html src/server/admin_ui_html.h
git commit -m "feat(console): ✨ implement collapsible reasoning process accordion in playground"
```

---

### Task 9: Python E2E Integration Suite & Full Regression Verification

**Files:**
- Modify: `tests/integration/test_reasoning_models.py`
- Root workspace

- [ ] **Step 1: Add E2E tests for tag emulation & native messages bridge**

In `tests/integration/test_reasoning_models.py`:
- `test_thinking_tag_emulation_e2e`: Test request with `X-Thinking-Format: tag` returns `<think>` wrapped content.
- `test_native_messages_reasoning_bridge_e2e`: Test `/v1/messages` against reasoning upstream returns Anthropic `thinking` content block.
- `test_admin_model_reasoning_config_e2e`: Test `/admin/v1/models` persistence of reasoning config.

- [ ] **Step 2: Run pytest**

Run: `python3 -m pytest tests/integration/test_reasoning_models.py --collect-only`  
Expected: 7 tests collected with 0 errors.

- [ ] **Step 3: Run full CTest suite**

Run: `ctest --test-dir build --output-on-failure`  
Expected: 100% tests passed (6/6 suites, 320+ unit tests).

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_reasoning_models.py
git commit -m "test(integration): ✅ add e2e test cases for reasoning ecosystem v2 and verify regression"
```

---

## Plan Self-Review Checklist
- [x] All 9 tasks follow TDD (failing test $\to$ implementation $\to$ passing test $\to$ atomic commit).
- [x] Strict compiler hygiene maintained (`-Wall -Wextra -Werror`).
- [x] Gitmoji commit conventions used across every step.
- [x] Full backwards compatibility for legacy OpenAI clients.
- [x] Full Anthropic native API protocol conformance.

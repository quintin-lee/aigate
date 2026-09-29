# Tool Calling Protocol Translation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Anthropic 和 Gemini 适配器完整实现 OpenAI Tool Calling 协议双向翻译（入站请求 + 出站响应 + 多轮对话历史），使 Agent 框架可以通过 aigate 透明地使用工具调用。

**Architecture:** Per-adapter inline extension — 在 `provider_anthropic.c` 和 `provider_gemini.c` 内各自扩展 build / parse / bridge 三个函数，不引入新文件、不改 vtable 签名（`provider_adapter_t` 和 `aigate_core.c` 零改动）。新增两个单测文件，追加两个集成测试用例。

**Tech Stack:** C17、jansson（JSON）、libcurl、CivetWeb SSE、CTest、pytest（集成测试）

---

## File Map

| 文件 | 操作 | 职责 |
|---|---|---|
| `src/upstream/provider_anthropic.c` | Modify | 扩展 build/parse/bridge（Tool Calling 支持）|
| `src/upstream/provider_anthropic.h` | Modify | `anthropic_bridge_t` 新增 5 个 tool 字段 |
| `src/upstream/provider_gemini.c` | Modify | 扩展 build/parse/bridge_process_line（Tool Calling 支持）|
| `tests/unit/upstream/test_provider_anthropic_tools.c` | Create | Anthropic Tool Calling 单元测试（6 用例）|
| `tests/unit/upstream/test_provider_gemini_tools.c` | Create | Gemini Tool Calling 单元测试（6 用例）|
| `tests/unit/run_tests.c` | Modify | 注册 12 个新测试函数 |
| `tests/integration/test_gateway.py` | Modify | 新增 2 个集成测试 |

---

## Task 1: Anthropic bridge struct — tool 字段

**Files:**
- Modify: `src/upstream/provider_anthropic.h`

- [ ] **Step 1: 在 `anthropic_bridge_t` 结构体末尾添加 5 个 tool 字段**

找到 `provider_anthropic.h` 中 `anthropic_bridge_t` 定义（含 `bool done_emitted;` 的那行之后），追加：

```c
    /* Tool calling state (one active tool_use block at a time) */
    bool   in_tool_use;       /**< 正在累积一个 tool_use content block */
    char   tool_id[64];       /**< 当前 tool call id (content_block_start 时记录) */
    char   tool_name[128];    /**< 当前 tool call name */
    char*  tool_args_buf;     /**< malloc 动态累积 input_json_delta，NULL 表示空 */
    size_t tool_args_len;     /**< tool_args_buf 已使用字节数 */
    int    tool_index;        /**< tool_calls 数组下标（多工具时递增）*/
```

- [ ] **Step 2: 确认编译通过**

```bash
cd /home/quintin/Data/source/c_cpp/aigate
cmake --build .build --target aigate_unit_tests 2>&1 | tail -5
```

Expected: 编译通过，`tool_args_buf` 新增字段不影响现有测试。

---

## Task 2: Anthropic build — tools / tool_choice / 消息历史扩展

**Files:**
- Modify: `src/upstream/provider_anthropic.c`（`provider_anthropic_build` 函数，行 ~60–200）

- [ ] **Step 1: 扩展 `provider_anthropic_build` 的 messages 循环，处理 content-as-array**

在现有的 messages 循环内，`jcontent` 取出后，把当前「只读 string」的逻辑改为同时支持数组：

```c
/* 从 content 字段提取纯文本（兼容 string 和 content-part 数组） */
static char* extract_text_content(json_t* jcontent)
{
    if (jcontent == NULL) return strdup("");
    if (json_is_string(jcontent)) return strdup(json_string_value(jcontent));
    if (!json_is_array(jcontent)) return strdup("");
    /* content part 数组：拼接所有 type=="text" 的 part */
    size_t idx; json_t* part;
    char*  buf = NULL; size_t buf_len = 0;
    json_array_foreach(jcontent, idx, part) {
        json_t* jtype = json_object_get(part, "type");
        if (!jtype || !json_is_string(jtype)) continue;
        if (strcmp(json_string_value(jtype), "text") == 0) {
            json_t* jt = json_object_get(part, "text");
            if (jt && json_is_string(jt)) {
                const char* t = json_string_value(jt);
                size_t tlen = strlen(t);
                char* nb = realloc(buf, buf_len + tlen + 1);
                if (nb) { buf = nb; memcpy(buf + buf_len, t, tlen); buf_len += tlen; buf[buf_len] = '\0'; }
            }
        } else if (strcmp(json_string_value(jtype), "image_url") == 0) {
            AIGATE_LOG_WARN("image_url content part ignored (vision not supported)");
        }
    }
    return buf ? buf : strdup("");
}
```

将该 static helper 放在 `provider_anthropic_build` 函数之前。在循环内用 `extract_text_content(jcontent)` 替换原来的 `json_string_value(jcontent)` 读取（注意 `free` 返回的 malloc 字符串）。

- [ ] **Step 2: 扩展 messages 循环，处理 role=="assistant" + tool_calls**

在 messages 循环里，对 `ant_role == "assistant"` 分支（现在只做 `json_string(content)`），改为：

```c
json_t* jtool_calls = json_object_get(item, "tool_calls");
if (jtool_calls != NULL && json_is_array(jtool_calls) && json_array_size(jtool_calls) > 0) {
    /* Build Anthropic content array: optional text block + N tool_use blocks */
    json_t* ant_content = json_array();
    char* txt = extract_text_content(jcontent);
    if (txt && txt[0] != '\0') {
        json_t* tb = json_object();
        json_object_set_new(tb, "type", json_string("text"));
        json_object_set_new(tb, "text", json_string(txt));
        json_array_append_new(ant_content, tb);
    }
    free(txt);
    size_t ti; json_t* tc;
    json_array_foreach(jtool_calls, ti, tc) {
        json_t* jfn = json_object_get(tc, "function");
        const char* tc_id   = json_string_value(json_object_get(tc, "id"));
        const char* fn_name = jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
        const char* fn_args = jfn ? json_string_value(json_object_get(jfn, "arguments")) : NULL;
        if (!tc_id || !fn_name) continue;
        json_t* input = (fn_args && fn_args[0]) ? json_loads(fn_args, 0, NULL) : json_object();
        if (!input) input = json_object();
        json_t* tub = json_object();
        json_object_set_new(tub, "type", json_string("tool_use"));
        json_object_set_new(tub, "id",   json_string(tc_id));
        json_object_set_new(tub, "name", json_string(fn_name));
        json_object_set_new(tub, "input", input);
        json_array_append_new(ant_content, tub);
    }
    json_t* m = json_object();
    json_object_set_new(m, "role", json_string("assistant"));
    json_object_set_new(m, "content", ant_content);
    json_array_append_new(ant_msgs, m);
} else {
    /* plain text assistant message (existing path) */
    char* txt = extract_text_content(jcontent);
    json_t* m = json_object();
    json_object_set_new(m, "role", json_string("assistant"));
    json_object_set_new(m, "content", json_string(txt ? txt : ""));
    free(txt);
    json_array_append_new(ant_msgs, m);
}
```

- [ ] **Step 3: 扩展 messages 循环，处理 role=="tool" → tool_result**

在 `strcmp(role, "system") == 0` / `strcmp(role, "assistant") == 0` 的 if-else 链里加 tool 分支：

```c
} else if (strcmp(role, "tool") == 0) {
    json_t* jtool_call_id = json_object_get(item, "tool_call_id");
    const char* tcid = (jtool_call_id && json_is_string(jtool_call_id))
                       ? json_string_value(jtool_call_id) : "";
    char* txt = extract_text_content(jcontent);
    json_t* tr = json_object();
    json_object_set_new(tr, "type",        json_string("tool_result"));
    json_object_set_new(tr, "tool_use_id", json_string(tcid));
    json_object_set_new(tr, "content",     json_string(txt ? txt : ""));
    free(txt);
    json_t* tr_content = json_array();
    json_array_append_new(tr_content, tr);
    json_t* m = json_object();
    json_object_set_new(m, "role",    json_string("user"));
    json_object_set_new(m, "content", tr_content);
    json_array_append_new(ant_msgs, m);
}
```

- [ ] **Step 4: 在 `provider_anthropic_build` 中追加 tools / tool_choice 转换**

在 messages 循环结束、`max_tokens` 处理之前，加：

```c
/* tools */
json_t* jtools = json_object_get(in_req, "tools");
if (jtools != NULL && json_is_array(jtools) && json_array_size(jtools) > 0) {
    json_t* ant_tools = json_array();
    size_t ti; json_t* tool;
    json_array_foreach(jtools, ti, tool) {
        json_t* jfn = json_object_get(tool, "function");
        if (!jfn) continue;
        json_t* at = json_object();
        json_t* jname = json_object_get(jfn, "name");
        json_t* jdesc = json_object_get(jfn, "description");
        json_t* jparm = json_object_get(jfn, "parameters");
        if (jname) json_object_set(at, "name", jname);
        if (jdesc) json_object_set(at, "description", jdesc);
        if (jparm) json_object_set_new(at, "input_schema", json_deep_copy(jparm));
        json_array_append_new(ant_tools, at);
    }
    json_object_set_new(out, "tools", ant_tools);
}

/* tool_choice */
json_t* jtc = json_object_get(in_req, "tool_choice");
if (jtc != NULL) {
    json_t* ant_tc = NULL;
    if (json_is_string(jtc)) {
        const char* s = json_string_value(jtc);
        if      (strcmp(s, "auto")     == 0) ant_tc = json_pack("{ss}", "type", "auto");
        else if (strcmp(s, "required") == 0) ant_tc = json_pack("{ss}", "type", "any");
        else if (strcmp(s, "none")     == 0) ant_tc = json_pack("{ss}", "type", "none");
    } else if (json_is_object(jtc)) {
        json_t* jfn = json_object_get(jtc, "function");
        const char* fname = jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
        if (fname) ant_tc = json_pack("{ssss}", "type", "tool", "name", fname);
    }
    if (ant_tc) json_object_set_new(out, "tool_choice", ant_tc);
}
```

- [ ] **Step 5: 编译确认**

```bash
cmake --build .build --target libaigate 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: 零 error，零 warning（-Werror 严格模式）。

---

## Task 3: Anthropic parse — tool_use response → OpenAI tool_calls

**Files:**
- Modify: `src/upstream/provider_anthropic.c`（`provider_anthropic_resp_to_openai` 函数，行 ~200–320）

- [ ] **Step 1: 扩展 stop_reason 映射，加 tool_use → tool_calls**

在现有的 `finish_reason` 判断处（`strcmp(sr, "max_tokens")`），追加：

```c
} else if (strcmp(sr, "tool_use") == 0) {
    finish_reason = "tool_calls";
}
```

- [ ] **Step 2: 扩展 content 扫描，同时收集 text 和 tool_use block**

把现有 content 遍历改为同时处理两种 block：

```c
char*   content_text = NULL;
size_t  ct_len = 0;
json_t* tool_calls_arr = json_array();

json_t* jcontent = json_object_get(root, "content");
if (jcontent != NULL && json_is_array(jcontent)) {
    size_t idx; json_t* block;
    json_array_foreach(jcontent, idx, block) {
        json_t* jtype = json_object_get(block, "type");
        if (!jtype || !json_is_string(jtype)) continue;
        const char* btype = json_string_value(jtype);

        if (strcmp(btype, "text") == 0) {
            json_t* jt = json_object_get(block, "text");
            if (jt && json_is_string(jt)) {
                const char* t = json_string_value(jt);
                size_t tlen = strlen(t);
                char* nbuf = realloc(content_text, ct_len + tlen + 1);
                if (nbuf) { content_text = nbuf; memcpy(content_text + ct_len, t, tlen); ct_len += tlen; content_text[ct_len] = '\0'; }
            }
        } else if (strcmp(btype, "tool_use") == 0) {
            json_t* jtid   = json_object_get(block, "id");
            json_t* jtname = json_object_get(block, "name");
            json_t* jtinput= json_object_get(block, "input");
            const char* tid   = (jtid   && json_is_string(jtid))   ? json_string_value(jtid)   : "";
            const char* tname = (jtname && json_is_string(jtname)) ? json_string_value(jtname) : "";
            char* args_str = jtinput ? json_dumps(jtinput, JSON_COMPACT) : strdup("{}");
            json_t* tc = json_object();
            json_object_set_new(tc, "id",   json_string(tid));
            json_object_set_new(tc, "type", json_string("function"));
            json_t* fn = json_object();
            json_object_set_new(fn, "name",      json_string(tname));
            json_object_set_new(fn, "arguments", json_string(args_str ? args_str : "{}"));
            free(args_str);
            json_object_set_new(tc, "function", fn);
            json_array_append_new(tool_calls_arr, tc);
        }
    }
}
```

- [ ] **Step 3: 在构建 OpenAI message 时加入 tool_calls 字段**

找到现有的 `json_object_set_new(msg, "content", ...)` 处，改为：

```c
json_t* msg = json_object();
json_object_set_new(msg, "role", json_string("assistant"));
/* content: null when pure tool_calls, text otherwise */
if (content_text && content_text[0] != '\0') {
    json_object_set_new(msg, "content", json_string(content_text));
} else {
    json_object_set_new(msg, "content", json_null());
}
if (json_array_size(tool_calls_arr) > 0) {
    json_object_set_new(msg, "tool_calls", tool_calls_arr);
} else {
    json_decref(tool_calls_arr);
}
```

在函数末尾 `free(content_text)` 保持不变（已经有）。

- [ ] **Step 4: 编译确认**

```bash
cmake --build .build --target libaigate 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: 零 error，零 warning。

---

## Task 4: Anthropic SSE bridge — tool_use 流式处理

**Files:**
- Modify: `src/upstream/provider_anthropic.c`（`anthropic_bridge_init` 及 bridge feed/finish/free 函数）

- [ ] **Step 1: 初始化新字段**

在 `anthropic_bridge_init` 函数末尾追加：

```c
b->in_tool_use   = false;
b->tool_id[0]    = '\0';
b->tool_name[0]  = '\0';
b->tool_args_buf = NULL;
b->tool_args_len = 0;
b->tool_index    = 0;
```

- [ ] **Step 2: 在 bridge_free（或 stream_bridge_free vtable 函数）中释放 tool_args_buf**

找到释放 bridge 内存的函数（通常是 `static void adapter_anthropic_bridge_free(stream_bridge_t* b)`），在 `free(b)` 之前加：

```c
anthropic_bridge_t* ab = (anthropic_bridge_t*)b;
free(ab->tool_args_buf);
```

- [ ] **Step 3: 在 `anthropic_bridge_feed` 事件处理中加入 tool_use 三个事件分支**

找到现有的 `content_block_start` / `content_block_delta` / `message_delta` 等 `strcmp(b->current_event, ...)` 分支块。

**content_block_start：** 在现有处理后（发 role chunk 后），检测 tool_use：

```c
/* 在现有的 content_block_start 分支末尾追加 */
json_t* jcb_type = json_object_get(data, "content_block");
if (jcb_type) {
    json_t* jt = json_object_get(jcb_type, "type");
    if (jt && json_is_string(jt) && strcmp(json_string_value(jt), "tool_use") == 0) {
        b->in_tool_use = true;
        json_t* jid   = json_object_get(jcb_type, "id");
        json_t* jname = json_object_get(jcb_type, "name");
        snprintf(b->tool_id,   sizeof b->tool_id,   "%s",
                 (jid   && json_is_string(jid))   ? json_string_value(jid)   : "");
        snprintf(b->tool_name, sizeof b->tool_name, "%s",
                 (jname && json_is_string(jname)) ? json_string_value(jname) : "");
        free(b->tool_args_buf);
        b->tool_args_buf = NULL;
        b->tool_args_len = 0;
    } else {
        b->in_tool_use = false;
    }
}
```

**content_block_delta：** 在现有 `text` delta 分支后加：

```c
} else if (strcmp(json_string_value(jdtype), "input_json_delta") == 0) {
    if (b->in_tool_use) {
        json_t* jpj = json_object_get(jdel, "partial_json");
        if (jpj && json_is_string(jpj)) {
            const char* pj = json_string_value(jpj);
            size_t pjlen = strlen(pj);
            if (b->tool_args_len + pjlen < 65536) {
                char* nb = realloc(b->tool_args_buf, b->tool_args_len + pjlen + 1);
                if (nb) {
                    b->tool_args_buf = nb;
                    memcpy(b->tool_args_buf + b->tool_args_len, pj, pjlen);
                    b->tool_args_len += pjlen;
                    b->tool_args_buf[b->tool_args_len] = '\0';
                }
            } else {
                AIGATE_LOG_WARN("tool_args_buf overflow for model %s (>64KB), truncating", b->model);
            }
        }
    }
```

**content_block_stop：** 新增处理（在现有事件分支链末尾加 `else if`）：

```c
} else if (strcmp(b->current_event, "content_block_stop") == 0) {
    if (b->in_tool_use && b->tool_id[0] != '\0') {
        /* Emit one tool_calls SSE chunk */
        const char* args = b->tool_args_buf ? b->tool_args_buf : "{}";
        json_t* chunk = json_object();
        json_object_set_new(chunk, "id",     json_string(b->msg_id[0] ? b->msg_id : "chatcmpl-tool"));
        json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
        json_object_set_new(chunk, "model",  json_string(b->model));
        json_t* choices = json_array();
        json_t* choice  = json_object();
        json_object_set_new(choice, "index", json_integer(0));
        json_t* delta = json_object();
        json_t* tc_arr = json_array();
        json_t* tc = json_object();
        json_object_set_new(tc, "index", json_integer(b->tool_index));
        json_object_set_new(tc, "id",    json_string(b->tool_id));
        json_object_set_new(tc, "type",  json_string("function"));
        json_t* fn = json_object();
        json_object_set_new(fn, "name",      json_string(b->tool_name));
        json_object_set_new(fn, "arguments", json_string(args));
        json_object_set_new(tc, "function", fn);
        json_array_append_new(tc_arr, tc);
        json_object_set_new(delta, "tool_calls", tc_arr);
        json_object_set_new(choice, "delta", delta);
        json_object_set_new(choice, "finish_reason", json_null());
        json_array_append_new(choices, choice);
        json_object_set_new(chunk, "choices", choices);
        char* packed = json_dumps(chunk, JSON_COMPACT);
        json_decref(chunk);
        if (packed) {
            char sse[8192];
            snprintf(sse, sizeof sse, "data: %s\n\n", packed);
            free(packed);
            bridge_send_chunk(b, sse);
        }
        b->in_tool_use = false;
        b->tool_index++;
    }
```

**message_delta：** 在现有 `stop_reason` 映射里加 `tool_use → tool_calls`：

```c
if (jsr && json_is_string(jsr)) {
    const char* sr = json_string_value(jsr);
    if      (strcmp(sr, "max_tokens") == 0) finish_reason = "length";
    else if (strcmp(sr, "tool_use")   == 0) finish_reason = "tool_calls";
}
```

- [ ] **Step 4: 编译确认**

```bash
cmake --build .build --target libaigate 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: 零 error，零 warning。

---

## Task 5: Anthropic 单元测试

**Files:**
- Create: `tests/unit/upstream/test_provider_anthropic_tools.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 创建测试文件**

```c
/** @file test_provider_anthropic_tools.c
 *  @brief Unit tests for Anthropic Tool Calling protocol translation.
 */
#include "run_tests.h"
#include "aigate_core.h"
#include "provider_anthropic.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */
static model_rec_t make_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name,         sizeof r.name,         "claude-3-5-sonnet-20241022");
    snprintf(r.provider,     sizeof r.provider,     "anthropic");
    snprintf(r.endpoint,     sizeof r.endpoint,     "http://127.0.0.1:8080");
    snprintf(r.upstream_key, sizeof r.upstream_key, "sk-ant-test");
    return r;
}

/* ----------------------------------------------------- Test 1: tools build */
TEST_CASE(test_anthropic_tools_request_build)
{
    model_rec_t route = make_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"What's the weather?\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{"
        "\"name\":\"get_weather\","
        "\"description\":\"Get weather for a location\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}}],"
        "\"tool_choice\":\"auto\"}";

    char url[512]; const char* hdrs[4][2]; int n = 0;
    char* body = NULL; size_t blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "output is valid json");

    json_t* jtools = json_object_get(out, "tools");
    TEST_ASSERT(jtools && json_is_array(jtools) && json_array_size(jtools) == 1,
                "tools array has 1 entry");
    json_t* t0 = json_array_get(jtools, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(t0, "name")), "get_weather") == 0,
                "tool name");
    TEST_ASSERT(json_object_get(t0, "input_schema") != NULL, "input_schema present");

    json_t* jtc = json_object_get(out, "tool_choice");
    TEST_ASSERT(jtc && json_is_object(jtc), "tool_choice is object");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jtc, "type")), "auto") == 0,
                "tool_choice type=auto");

    json_decref(out); free(body);
}

/* --------------------------------------------- Test 2: tool_use response */
TEST_CASE(test_anthropic_tool_use_response_parse)
{
    const char* ant_resp =
        "{\"id\":\"msg_01\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\","
        "\"stop_reason\":\"tool_use\","
        "\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_01\","
        "\"name\":\"get_weather\",\"input\":{\"location\":\"Beijing\"}}],"
        "\"usage\":{\"input_tokens\":30,\"output_tokens\":10}}";

    char* oai = NULL; size_t olen = 0; long ptok = 0, ctok = 0;
    int rc = provider_anthropic_resp_to_openai(ant_resp, "claude-3-5-sonnet-20241022",
                                               &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    TEST_ASSERT(oai != NULL, "output not null");

    json_t* out = json_loads(oai, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");
    json_t* choices = json_object_get(out, "choices");
    json_t* c0 = json_array_get(choices, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(c0, "finish_reason")), "tool_calls") == 0,
                "finish_reason=tool_calls");
    json_t* msg = json_object_get(c0, "message");
    json_t* tc_arr = json_object_get(msg, "tool_calls");
    TEST_ASSERT(tc_arr && json_is_array(tc_arr) && json_array_size(tc_arr) == 1,
                "tool_calls array has 1 entry");
    json_t* tc0 = json_array_get(tc_arr, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tc0, "id")), "toolu_01") == 0, "id");
    json_t* fn = json_object_get(tc0, "function");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fn, "name")), "get_weather") == 0, "name");
    const char* args = json_string_value(json_object_get(fn, "arguments"));
    json_t* parsed_args = json_loads(args, 0, NULL);
    TEST_ASSERT(parsed_args != NULL, "arguments is valid json");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(parsed_args, "location")), "Beijing") == 0,
                "location arg");
    json_decref(parsed_args); json_decref(out); free(oai);
}

/* ----------------------------------------- Test 3: mixed text + tool_use */
TEST_CASE(test_anthropic_mixed_text_and_tool_use)
{
    const char* ant_resp =
        "{\"id\":\"msg_02\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\","
        "\"stop_reason\":\"tool_use\","
        "\"content\":["
        "{\"type\":\"text\",\"text\":\"Let me check the weather.\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_02\",\"name\":\"get_weather\","
        "\"input\":{\"location\":\"Shanghai\"}}],"
        "\"usage\":{\"input_tokens\":40,\"output_tokens\":15}}";

    char* oai = NULL; size_t olen = 0; long ptok = 0, ctok = 0;
    int rc = provider_anthropic_resp_to_openai(ant_resp, "claude-3-5-sonnet-20241022",
                                               &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    json_t* out = json_loads(oai, 0, NULL);
    json_t* msg = json_object_get(json_array_get(json_object_get(out, "choices"), 0), "message");
    json_t* jcontent = json_object_get(msg, "content");
    TEST_ASSERT(jcontent && json_is_string(jcontent) &&
                strcmp(json_string_value(jcontent), "Let me check the weather.") == 0,
                "text content preserved");
    TEST_ASSERT(json_array_size(json_object_get(msg, "tool_calls")) == 1,
                "tool_calls has 1 entry");
    json_decref(out); free(oai);
}

/* --------------------------------------- Test 4: tool result message build */
TEST_CASE(test_anthropic_tool_result_message_build)
{
    model_rec_t route = make_route();
    /* Conversation: user → assistant(tool_calls) → tool(result) */
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":\"What's the weather in Beijing?\"},"
        "{\"role\":\"assistant\",\"content\":null,"
        "\"tool_calls\":[{\"id\":\"toolu_01\",\"type\":\"function\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\\\"Beijing\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"toolu_01\","
        "\"content\":\"Sunny, 25°C\"}"
        "]}";

    char url[512]; const char* hdrs[4][2]; int n = 0;
    char* body = NULL; size_t blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    json_t* msgs = json_object_get(out, "messages");
    TEST_ASSERT(json_is_array(msgs) && json_array_size(msgs) == 3, "3 messages");

    /* Check assistant message has tool_use content */
    json_t* asst = json_array_get(msgs, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(asst, "role")), "assistant") == 0,
                "assistant role");
    json_t* asst_content = json_object_get(asst, "content");
    TEST_ASSERT(json_is_array(asst_content), "assistant content is array");
    json_t* tu = json_array_get(asst_content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tu, "type")), "tool_use") == 0,
                "tool_use type");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tu, "id")), "toolu_01") == 0, "id");

    /* Check tool result message */
    json_t* tr_msg = json_array_get(msgs, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr_msg, "role")), "user") == 0,
                "tool result role=user");
    json_t* tr_content = json_object_get(tr_msg, "content");
    TEST_ASSERT(json_is_array(tr_content) && json_array_size(tr_content) == 1, "content array");
    json_t* tr = json_array_get(tr_content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr, "type")), "tool_result") == 0,
                "type=tool_result");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr, "tool_use_id")), "toolu_01") == 0,
                "tool_use_id");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr, "content")), "Sunny, 25°C") == 0,
                "content value");

    json_decref(out); free(body);
}

/* ------------------------------------------- Test 5: SSE tool_call stream */
/* Lightweight mock response context for SSE output capture */
static char g_sse_buf[65536];
static size_t g_sse_len;

static int mock_write(void* impl, const void* data, size_t len)
{
    (void)impl;
    if (g_sse_len + len < sizeof g_sse_buf) {
        memcpy(g_sse_buf + g_sse_len, data, len);
        g_sse_len += len;
        g_sse_buf[g_sse_len] = '\0';
    }
    return 0;
}
static int mock_set_header(void* impl, const char* k, const char* v) { (void)impl;(void)k;(void)v; return 0; }
static int mock_begin_stream(void* impl) { (void)impl; return 0; }
static int mock_flush(void* impl) { (void)impl; return 0; }

TEST_CASE(test_anthropic_sse_tool_call_stream)
{
    g_sse_len = 0; g_sse_buf[0] = '\0';

    aigate_response_ctx rc_ctx;
    memset(&rc_ctx, 0, sizeof rc_ctx);
    rc_ctx.write       = mock_write;
    rc_ctx.set_header  = mock_set_header;
    rc_ctx.begin_stream= mock_begin_stream;
    rc_ctx.flush       = mock_flush;

    /* Initialize bridge */
    anthropic_bridge_t b;
    anthropic_bridge_init(&b, &rc_ctx);
    snprintf(b.model,  sizeof b.model,  "claude-3-5-sonnet-20241022");
    snprintf(b.msg_id, sizeof b.msg_id, "msg_01");

    /* Feed: message_start */
    const char* s1 =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\","
        "\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n";
    anthropic_bridge_feed(&b, s1, strlen(s1));

    /* Feed: content_block_start (tool_use) */
    const char* s2 =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01\","
        "\"name\":\"get_weather\",\"input\":{}}}\n\n";
    anthropic_bridge_feed(&b, s2, strlen(s2));

    /* Feed: two input_json_delta chunks */
    const char* s3 =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"loc\"}}\n\n";
    anthropic_bridge_feed(&b, s3, strlen(s3));

    const char* s4 =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"ation\\\":\\\"Beijing\\\"}\"}}\n\n";
    anthropic_bridge_feed(&b, s4, strlen(s4));

    /* Feed: content_block_stop — should emit tool_calls SSE chunk */
    const char* s5 =
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    anthropic_bridge_feed(&b, s5, strlen(s5));

    /* Feed: message_delta (stop_reason=tool_use) */
    const char* s6 =
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},"
        "\"usage\":{\"output_tokens\":20}}\n\n";
    anthropic_bridge_feed(&b, s6, strlen(s6));

    /* message_stop */
    const char* s7 = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    anthropic_bridge_finish(&b);
    anthropic_bridge_feed(&b, s7, strlen(s7));

    free(b.tool_args_buf);

    /* Verify SSE output contains tool_calls chunk */
    TEST_ASSERT(strstr(g_sse_buf, "tool_calls") != NULL, "output contains tool_calls");
    TEST_ASSERT(strstr(g_sse_buf, "get_weather") != NULL, "output contains get_weather");
    TEST_ASSERT(strstr(g_sse_buf, "toolu_01") != NULL, "output contains toolu_01");
    TEST_ASSERT(strstr(g_sse_buf, "Beijing") != NULL, "output contains Beijing");
    TEST_ASSERT(strstr(g_sse_buf, "tool_calls") != NULL, "finish_reason chunk present");
}

/* ----------------------------- Test 6: tool_choice required→any mapping */
TEST_CASE(test_anthropic_tool_choice_required_mapping)
{
    model_rec_t route = make_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"Call a tool\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{"
        "\"name\":\"fn\",\"description\":\"d\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}],"
        "\"tool_choice\":\"required\"}";

    char url[512]; const char* hdrs[4][2]; int n = 0;
    char* body = NULL; size_t blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");
    json_t* out = json_loads(body, 0, NULL);
    json_t* jtc = json_object_get(out, "tool_choice");
    TEST_ASSERT(jtc && strcmp(json_string_value(json_object_get(jtc, "type")), "any") == 0,
                "required maps to any");
    json_decref(out); free(body);
}
```

- [ ] **Step 2: 在 `tests/unit/run_tests.c` 注册 6 个新测试**

找到已有的 `test_anthropic_sniff_streaming_sse` 注册行之后，追加：

```c
    extern void test_anthropic_tools_request_build(void);
    extern void test_anthropic_tool_use_response_parse(void);
    extern void test_anthropic_mixed_text_and_tool_use(void);
    extern void test_anthropic_tool_result_message_build(void);
    extern void test_anthropic_sse_tool_call_stream(void);
    extern void test_anthropic_tool_choice_required_mapping(void);
    test_register("anthropic_tools_request_build",       test_anthropic_tools_request_build);
    test_register("anthropic_tool_use_response_parse",   test_anthropic_tool_use_response_parse);
    test_register("anthropic_mixed_text_and_tool_use",   test_anthropic_mixed_text_and_tool_use);
    test_register("anthropic_tool_result_message_build", test_anthropic_tool_result_message_build);
    test_register("anthropic_sse_tool_call_stream",      test_anthropic_sse_tool_call_stream);
    test_register("anthropic_tool_choice_required_mapping", test_anthropic_tool_choice_required_mapping);
```

- [ ] **Step 3: 构建并运行单元测试**

```bash
cmake --build .build --target aigate_unit_tests 2>&1 | tail -5
cd .build && ctest -R "anthropic_tools|anthropic_tool_use|anthropic_mixed|anthropic_tool_result|anthropic_sse_tool|anthropic_tool_choice" --output-on-failure
```

Expected: 6/6 PASSED。

- [ ] **Step 4: 确认全量测试不退步**

```bash
cd .build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: 全部 PASS。

- [ ] **Step 5: 提交**

```bash
cd /home/quintin/Data/source/c_cpp/aigate
git add src/upstream/provider_anthropic.h src/upstream/provider_anthropic.c \
        tests/unit/upstream/test_provider_anthropic_tools.c tests/unit/run_tests.c
git commit -m "feat(upstream): 🔧 add Anthropic tool calling protocol translation"
```

---

## Task 6: Gemini build — tools / tool_choice / 消息历史扩展

**Files:**
- Modify: `src/upstream/provider_gemini.c`（`provider_gemini_build` 函数）

- [ ] **Step 1: 添加 `gemini_lookup_tool_name` helper**

在 `provider_gemini_build` 函数之前加：

```c
/** @brief 向上回溯在 messages 数组中找 tool_call_id 对应的 function.name。
 *  未找到时返回 tool_call_id 本身（fallback）。out 为调用者栈上 buf[128]。 */
static void
gemini_lookup_tool_name(json_t* msgs, const char* tool_call_id, char* out, size_t out_cap)
{
    if (!msgs || !json_is_array(msgs) || !tool_call_id) {
        snprintf(out, out_cap, "%s", tool_call_id ? tool_call_id : "unknown");
        return;
    }
    /* Scan backwards for nearest assistant message */
    int n = (int)json_array_size(msgs);
    for (int i = n - 1; i >= 0; i--) {
        json_t* m = json_array_get(msgs, i);
        json_t* jrole = json_object_get(m, "role");
        if (!jrole || !json_is_string(jrole)) continue;
        if (strcmp(json_string_value(jrole), "assistant") != 0) continue;
        json_t* jtcs = json_object_get(m, "tool_calls");
        if (!jtcs || !json_is_array(jtcs)) continue;
        size_t ti; json_t* tc;
        json_array_foreach(jtcs, ti, tc) {
            json_t* jid = json_object_get(tc, "id");
            if (!jid || !json_is_string(jid)) continue;
            if (strcmp(json_string_value(jid), tool_call_id) == 0) {
                json_t* jfn = json_object_get(tc, "function");
                json_t* jname = jfn ? json_object_get(jfn, "name") : NULL;
                if (jname && json_is_string(jname)) {
                    snprintf(out, out_cap, "%s", json_string_value(jname));
                    return;
                }
            }
        }
    }
    AIGATE_LOG_WARN("gemini_lookup_tool_name: no match for tool_call_id=%s, using id as name",
                    tool_call_id);
    snprintf(out, out_cap, "%s", tool_call_id);
}
```

- [ ] **Step 2: 扩展 messages 循环，处理 role=="assistant" + tool_calls**

在现有 Gemini messages 循环的 `else`（user/model 分支）中，对 `role=="assistant"` 分支改为：

```c
json_t* jtool_calls = json_object_get(m, "tool_calls");
if (strcmp(role, "assistant") == 0 && jtool_calls != NULL &&
    json_is_array(jtool_calls) && json_array_size(jtool_calls) > 0) {
    const char* gemini_role = "model";
    json_t* entry = json_object();
    json_object_set_new(entry, "role", json_string(gemini_role));
    json_t* parts = json_array();
    /* Optional text part */
    if (json_is_string(jcontent) && json_string_value(jcontent)[0] != '\0') {
        json_t* tp = json_object();
        json_object_set_new(tp, "text", json_string(json_string_value(jcontent)));
        json_array_append_new(parts, tp);
    }
    /* functionCall parts */
    size_t ti; json_t* tc;
    json_array_foreach(jtool_calls, ti, tc) {
        json_t* jfn = json_object_get(tc, "function");
        const char* fname = jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
        const char* fargs = jfn ? json_string_value(json_object_get(jfn, "arguments")) : NULL;
        if (!fname) continue;
        json_t* args = (fargs && fargs[0]) ? json_loads(fargs, 0, NULL) : json_object();
        if (!args) args = json_object();
        json_t* fc = json_object();
        json_object_set_new(fc, "name", json_string(fname));
        json_object_set_new(fc, "args", args);
        json_t* fcp = json_object();
        json_object_set_new(fcp, "functionCall", fc);
        json_array_append_new(parts, fcp);
    }
    json_object_set_new(entry, "parts", parts);
    json_array_append_new(contents, entry);
} else if (strcmp(role, "tool") == 0) {
    /* functionResponse */
    json_t* jtcid = json_object_get(m, "tool_call_id");
    const char* tcid = (jtcid && json_is_string(jtcid)) ? json_string_value(jtcid) : "";
    char fname[128];
    gemini_lookup_tool_name(msgs, tcid, fname, sizeof fname);
    const char* result_str = (jcontent && json_is_string(jcontent))
                             ? json_string_value(jcontent) : "";
    /* Try to parse result as JSON; if not, wrap as {output: "..."} */
    json_t* result_obj = json_loads(result_str, 0, NULL);
    if (result_obj == NULL) {
        result_obj = json_object();
        json_object_set_new(result_obj, "output", json_string(result_str));
    }
    json_t* fr = json_object();
    json_object_set_new(fr, "name",     json_string(fname));
    json_object_set_new(fr, "response", result_obj);
    json_t* frp = json_object();
    json_object_set_new(frp, "functionResponse", fr);
    json_t* parts = json_array();
    json_array_append_new(parts, frp);
    json_t* entry = json_object();
    json_object_set_new(entry, "role",  json_string("user"));
    json_object_set_new(entry, "parts", parts);
    json_array_append_new(contents, entry);
} else {
    /* 普通 user/model 消息（原有逻辑）*/
    ...
}
```

**注意**：原有的 `entry`/`parts`/`part` 构建逻辑移入最终的 `else` 分支，保持不变。

- [ ] **Step 3: 在 generationConfig 之后追加 tools / toolConfig**

```c
/* tools → functionDeclarations */
json_t* jtools = json_object_get(in_req, "tools");
if (jtools && json_is_array(jtools) && json_array_size(jtools) > 0) {
    json_t* fn_decls = json_array();
    size_t ti; json_t* tool;
    json_array_foreach(jtools, ti, tool) {
        json_t* jfn = json_object_get(tool, "function");
        if (!jfn) continue;
        json_t* fd = json_object();
        json_t* jname = json_object_get(jfn, "name");
        json_t* jdesc = json_object_get(jfn, "description");
        json_t* jparm = json_object_get(jfn, "parameters");
        if (jname) json_object_set(fd, "name", jname);
        if (jdesc) json_object_set(fd, "description", jdesc);
        if (jparm) json_object_set_new(fd, "parameters", json_deep_copy(jparm));
        json_array_append_new(fn_decls, fd);
    }
    json_t* tools_wrapper = json_object();
    json_object_set_new(tools_wrapper, "functionDeclarations", fn_decls);
    json_t* tools_arr = json_array();
    json_array_append_new(tools_arr, tools_wrapper);
    json_object_set_new(out_req, "tools", tools_arr);
}

/* tool_choice → toolConfig */
json_t* jtc = json_object_get(in_req, "tool_choice");
if (jtc) {
    const char* mode = "AUTO";
    json_t* allowed = NULL;
    if (json_is_string(jtc)) {
        const char* s = json_string_value(jtc);
        if      (strcmp(s, "required") == 0) mode = "ANY";
        else if (strcmp(s, "none")     == 0) mode = "NONE";
        /* "auto" → "AUTO" (default) */
    } else if (json_is_object(jtc)) {
        json_t* jfn = json_object_get(jtc, "function");
        const char* fname = jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
        mode = "ANY";
        if (fname) {
            allowed = json_array();
            json_array_append_new(allowed, json_string(fname));
        }
    }
    json_t* fcc = json_object();
    json_object_set_new(fcc, "mode", json_string(mode));
    if (allowed) json_object_set_new(fcc, "allowedFunctionNames", allowed);
    json_t* tool_cfg = json_object();
    json_object_set_new(tool_cfg, "functionCallingConfig", fcc);
    json_object_set_new(out_req, "toolConfig", tool_cfg);
}
```

- [ ] **Step 4: 编译确认**

```bash
cmake --build .build --target libaigate 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: 零 error，零 warning。

---

## Task 7: Gemini parse — functionCall response → OpenAI tool_calls

**Files:**
- Modify: `src/upstream/provider_gemini.c`（`provider_gemini_resp_to_openai` 函数）

- [ ] **Step 1: 扩展 parts 遍历，同时处理 text 和 functionCall**

现有代码只取 `parts[0].text`，改为遍历所有 parts：

```c
/* Scan all parts: collect text and functionCalls */
char*   text_buf = NULL;
size_t  text_len = 0;
json_t* fc_arr   = json_array();   /* OpenAI tool_calls */
int     fc_index = 0;

json_t* candidates = json_object_get(root, "candidates");
if (candidates && json_is_array(candidates) && json_array_size(candidates) > 0) {
    json_t* c0 = json_array_get(candidates, 0);

    json_t* jfinish = json_object_get(c0, "finishReason");
    if (jfinish && json_is_string(jfinish)) {
        finish_reason = map_gemini_finish_reason(json_string_value(jfinish));
    }

    json_t* content = json_object_get(c0, "content");
    json_t* parts   = content ? json_object_get(content, "parts") : NULL;
    if (parts && json_is_array(parts)) {
        size_t pi; json_t* p;
        json_array_foreach(parts, pi, p) {
            json_t* jtext = json_object_get(p, "text");
            json_t* jfc   = json_object_get(p, "functionCall");
            if (jtext && json_is_string(jtext)) {
                const char* t = json_string_value(jtext);
                size_t tlen = strlen(t);
                char* nb = realloc(text_buf, text_len + tlen + 1);
                if (nb) { text_buf = nb; memcpy(text_buf + text_len, t, tlen); text_len += tlen; text_buf[text_len] = '\0'; }
            } else if (jfc && json_is_object(jfc)) {
                const char* fname = json_string_value(json_object_get(jfc, "name"));
                json_t*     fargs = json_object_get(jfc, "args");
                if (!fname) continue;
                char* args_str = fargs ? json_dumps(fargs, JSON_COMPACT) : strdup("{}");
                /* Synthesize a stable id */
                char tc_id[64];
                snprintf(tc_id, sizeof tc_id, "call_%s_%ld", fname, (long)time(NULL) + fc_index);
                json_t* tc = json_object();
                json_object_set_new(tc, "id",   json_string(tc_id));
                json_object_set_new(tc, "type", json_string("function"));
                json_t* fn = json_object();
                json_object_set_new(fn, "name",      json_string(fname));
                json_object_set_new(fn, "arguments", json_string(args_str ? args_str : "{}"));
                free(args_str);
                json_object_set_new(tc, "function", fn);
                json_array_append_new(fc_arr, tc);
                finish_reason = "tool_calls";
                fc_index++;
            }
        }
    }
}

const char* text = text_buf ? text_buf : "";
```

在函数末尾构建 message 时：

```c
json_t* msg = json_object();
json_object_set_new(msg, "role", json_string("assistant"));
if (text_buf && text_buf[0] != '\0') {
    json_object_set_new(msg, "content", json_string(text_buf));
} else {
    json_object_set_new(msg, "content", json_null());
}
if (json_array_size(fc_arr) > 0) {
    json_object_set_new(msg, "tool_calls", fc_arr);
} else {
    json_decref(fc_arr);
}
/* ... finish_reason, choices assembly ... */
free(text_buf);
```

- [ ] **Step 2: 编译确认**

```bash
cmake --build .build --target libaigate 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: 零 error，零 warning。

---

## Task 8: Gemini SSE bridge — functionCall 流式处理

**Files:**
- Modify: `src/upstream/provider_gemini.c`（`gemini_bridge_process_line` 函数）

- [ ] **Step 1: 在 parts 遍历中加入 functionCall 检测**

现有 `gemini_bridge_process_line` 中解析 `delta_text` 的逻辑在 `parts[0]` 处：

```c
/* 在现有 parts 遍历（或取 parts[0] 处）之后，追加 functionCall 检测 */
if (parts && json_is_array(parts)) {
    size_t pi; json_t* p;
    json_array_foreach(parts, pi, p) {
        json_t* jfc = json_object_get(p, "functionCall");
        if (jfc && json_is_object(jfc)) {
            const char* fname = json_string_value(json_object_get(jfc, "name"));
            json_t*     fargs = json_object_get(jfc, "args");
            if (!fname) continue;
            char* args_str = fargs ? json_dumps(fargs, JSON_COMPACT) : strdup("{}");
            char tc_id[64];
            snprintf(tc_id, sizeof tc_id, "call_%s_%ld", fname, (long)time(NULL));

            /* Ensure headers sent first */
            if (!b->headers_sent) {
                b->rc->set_header(b->rc->impl, "Content-Type", "text/event-stream");
                b->rc->set_header(b->rc->impl, "Cache-Control", "no-cache");
                b->rc->begin_stream(b->rc->impl);
                b->headers_sent = true;
            }

            json_t* chunk = json_object();
            json_object_set_new(chunk, "id",     json_string(b->msg_id));
            json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
            json_object_set_new(chunk, "model",  json_string(b->model));
            json_t* choices = json_array();
            json_t* choice  = json_object();
            json_object_set_new(choice, "index", json_integer(0));
            json_t* delta = json_object();
            json_t* tc_arr = json_array();
            json_t* tc = json_object();
            json_object_set_new(tc, "index", json_integer(0));
            json_object_set_new(tc, "id",    json_string(tc_id));
            json_object_set_new(tc, "type",  json_string("function"));
            json_t* fn = json_object();
            json_object_set_new(fn, "name",      json_string(fname));
            json_object_set_new(fn, "arguments", json_string(args_str ? args_str : "{}"));
            free(args_str);
            json_object_set_new(tc, "function", fn);
            json_array_append_new(tc_arr, tc);
            json_object_set_new(delta, "tool_calls", tc_arr);
            json_object_set_new(choice, "delta", delta);
            json_object_set_new(choice, "finish_reason", json_null());
            json_array_append_new(choices, choice);
            json_object_set_new(chunk, "choices", choices);
            char* packed = json_dumps(chunk, JSON_COMPACT);
            json_decref(chunk);
            if (packed) {
                char sse_line[8192];
                snprintf(sse_line, sizeof sse_line, "data: %s\n\n", packed);
                free(packed);
                gemini_bridge_send_chunk(b, sse_line);
            }
            /* Update finish_reason for final chunk */
            snprintf(b->finish_reason, sizeof b->finish_reason, "tool_calls");
        }
    }
}
```

- [ ] **Step 2: 编译确认**

```bash
cmake --build .build --target libaigate 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: 零 error，零 warning。

---

## Task 9: Gemini 单元测试

**Files:**
- Create: `tests/unit/upstream/test_provider_gemini_tools.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 创建测试文件**

```c
/** @file test_provider_gemini_tools.c
 *  @brief Unit tests for Gemini Tool Calling protocol translation.
 */
#include "run_tests.h"
#include "provider_gemini.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static model_rec_t make_gemini_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name,         sizeof r.name,         "gemini-1.5-pro");
    snprintf(r.provider,     sizeof r.provider,     "gemini");
    snprintf(r.endpoint,     sizeof r.endpoint,     "https://generativelanguage.googleapis.com");
    snprintf(r.upstream_key, sizeof r.upstream_key, "AIzaSyTest");
    return r;
}

/* ------------------------------------------------ Test 1: tools build */
TEST_CASE(test_gemini_tools_request_build)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"What is the weather?\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{"
        "\"name\":\"get_weather\",\"description\":\"Get weather\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}}],"
        "\"tool_choice\":\"auto\"}";

    char url[512]; const char* hdrs[4][2]; int n = 0;
    char* body = NULL; size_t blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* jtools = json_object_get(out, "tools");
    TEST_ASSERT(jtools && json_is_array(jtools) && json_array_size(jtools) == 1, "tools array");
    json_t* t0 = json_array_get(jtools, 0);
    json_t* fn_decls = json_object_get(t0, "functionDeclarations");
    TEST_ASSERT(fn_decls && json_is_array(fn_decls) && json_array_size(fn_decls) == 1,
                "functionDeclarations");
    json_t* fd0 = json_array_get(fn_decls, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd0, "name")), "get_weather") == 0,
                "name");
    TEST_ASSERT(json_object_get(fd0, "parameters") != NULL, "parameters present");

    json_t* jtc = json_object_get(out, "toolConfig");
    json_t* fcc = jtc ? json_object_get(jtc, "functionCallingConfig") : NULL;
    TEST_ASSERT(fcc && strcmp(json_string_value(json_object_get(fcc, "mode")), "AUTO") == 0,
                "toolConfig mode=AUTO");

    json_decref(out); free(body);
}

/* ----------------------------------------- Test 2: functionCall response */
TEST_CASE(test_gemini_function_call_response_parse)
{
    const char* gemini_resp =
        "{\"candidates\":[{"
        "\"content\":{\"role\":\"model\",\"parts\":[{"
        "\"functionCall\":{\"name\":\"get_weather\",\"args\":{\"location\":\"Beijing\"}}"
        "}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":20,\"candidatesTokenCount\":10}}";

    char* oai = NULL; size_t olen = 0; long ptok = 0, ctok = 0;
    int rc = provider_gemini_resp_to_openai(gemini_resp, strlen(gemini_resp),
                                            "gemini-1.5-pro", 200, &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    json_t* out = json_loads(oai, 0, NULL);
    json_t* c0  = json_array_get(json_object_get(out, "choices"), 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(c0, "finish_reason")), "tool_calls") == 0,
                "finish_reason=tool_calls");
    json_t* msg    = json_object_get(c0, "message");
    json_t* tc_arr = json_object_get(msg, "tool_calls");
    TEST_ASSERT(tc_arr && json_array_size(tc_arr) == 1, "1 tool call");
    json_t* tc0 = json_array_get(tc_arr, 0);
    json_t* fn  = json_object_get(tc0, "function");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fn, "name")), "get_weather") == 0, "name");
    const char* args = json_string_value(json_object_get(fn, "arguments"));
    json_t* pargs = json_loads(args, 0, NULL);
    TEST_ASSERT(pargs && strcmp(json_string_value(json_object_get(pargs, "location")), "Beijing") == 0,
                "location arg");
    json_decref(pargs); json_decref(out); free(oai);
}

/* -------------------------------------- Test 3: tool_result name lookup */
TEST_CASE(test_gemini_tool_result_name_lookup)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":\"Weather in Beijing?\"},"
        "{\"role\":\"assistant\",\"content\":null,"
        "\"tool_calls\":[{\"id\":\"call_001\",\"type\":\"function\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"location\\\":\\\"Beijing\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"call_001\",\"content\":\"Sunny 25C\"}"
        "]}";

    char url[512]; const char* hdrs[4][2]; int n = 0;
    char* body = NULL; size_t blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out      = json_loads(body, 0, NULL);
    json_t* contents = json_object_get(out, "contents");
    TEST_ASSERT(json_is_array(contents) && json_array_size(contents) == 3, "3 contents");

    /* Third entry should be user role with functionResponse */
    json_t* tr_entry = json_array_get(contents, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(tr_entry, "role")), "user") == 0, "user");
    json_t* tr_parts = json_object_get(tr_entry, "parts");
    json_t* trp0     = json_array_get(tr_parts, 0);
    json_t* fr       = json_object_get(trp0, "functionResponse");
    TEST_ASSERT(fr != NULL, "functionResponse present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fr, "name")), "get_weather") == 0,
                "function name looked up correctly");

    json_decref(out); free(body);
}

/* ---------------------------------- Test 4: tool_result name fallback */
TEST_CASE(test_gemini_tool_result_name_missing_fallback)
{
    model_rec_t route = make_gemini_route();
    /* No preceding assistant message — must fall back to tool_call_id */
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":["
        "{\"role\":\"user\",\"content\":\"Hello\"},"
        "{\"role\":\"tool\",\"tool_call_id\":\"orphan_id\",\"content\":\"result\"}"
        "]}";

    char url[512]; const char* hdrs[4][2]; int n = 0;
    char* body = NULL; size_t blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok (no crash on missing id)");

    json_t* out      = json_loads(body, 0, NULL);
    json_t* contents = json_object_get(out, "contents");
    json_t* tr_entry = json_array_get(contents, 1);
    json_t* tr_parts = json_object_get(tr_entry, "parts");
    json_t* fr       = json_object_get(json_array_get(tr_parts, 0), "functionResponse");
    /* Fallback: name == tool_call_id */
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fr, "name")), "orphan_id") == 0,
                "fallback name = tool_call_id");

    json_decref(out); free(body);
}

/* --------------------------------------- Test 5: multi tool_calls */
TEST_CASE(test_gemini_multi_tool_calls_response)
{
    const char* gemini_resp =
        "{\"candidates\":[{"
        "\"content\":{\"role\":\"model\",\"parts\":["
        "{\"functionCall\":{\"name\":\"fn_a\",\"args\":{\"x\":1}}},"
        "{\"functionCall\":{\"name\":\"fn_b\",\"args\":{\"y\":2}}}"
        "]},"
        "\"finishReason\":\"STOP\"}]}";

    char* oai = NULL; size_t olen = 0; long ptok = 0, ctok = 0;
    int rc = provider_gemini_resp_to_openai(gemini_resp, strlen(gemini_resp),
                                            "gemini-1.5-pro", 200, &oai, &olen, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "parse ok");
    json_t* out = json_loads(oai, 0, NULL);
    json_t* msg = json_object_get(
        json_array_get(json_object_get(out, "choices"), 0), "message");
    json_t* tc_arr = json_object_get(msg, "tool_calls");
    TEST_ASSERT(tc_arr && json_array_size(tc_arr) == 2, "2 tool calls");
    TEST_ASSERT(strcmp(json_string_value(
        json_object_get(json_object_get(json_array_get(tc_arr, 0), "function"), "name")),
        "fn_a") == 0, "first tool=fn_a");
    TEST_ASSERT(strcmp(json_string_value(
        json_object_get(json_object_get(json_array_get(tc_arr, 1), "function"), "name")),
        "fn_b") == 0, "second tool=fn_b");
    json_decref(out); free(oai);
}

/* ---------------------------------- Test 6: SSE functionCall stream */
static char g_gemini_sse_buf[65536];
static size_t g_gemini_sse_len;
static int gemini_mock_write(void* impl, const void* data, size_t len) {
    (void)impl;
    if (g_gemini_sse_len + len < sizeof g_gemini_sse_buf) {
        memcpy(g_gemini_sse_buf + g_gemini_sse_len, data, len);
        g_gemini_sse_len += len;
        g_gemini_sse_buf[g_gemini_sse_len] = '\0';
    }
    return 0;
}
static int gemini_mock_set_header(void* impl, const char* k, const char* v) { (void)impl;(void)k;(void)v; return 0; }
static int gemini_mock_begin_stream(void* impl) { (void)impl; return 0; }
static int gemini_mock_flush(void* impl) { (void)impl; return 0; }

TEST_CASE(test_gemini_sse_function_call_stream)
{
    g_gemini_sse_len = 0; g_gemini_sse_buf[0] = '\0';

    aigate_response_ctx rc_ctx;
    memset(&rc_ctx, 0, sizeof rc_ctx);
    rc_ctx.write        = gemini_mock_write;
    rc_ctx.set_header   = gemini_mock_set_header;
    rc_ctx.begin_stream = gemini_mock_begin_stream;
    rc_ctx.flush        = gemini_mock_flush;

    /* Use the vtable to create and feed the bridge */
    extern const provider_adapter_t g_provider_gemini;
    stream_bridge_t* bridge = g_provider_gemini.stream_bridge_new(&rc_ctx, "gemini-1.5-pro");
    TEST_ASSERT(bridge != NULL, "bridge created");

    /* Gemini SSE line with functionCall */
    const char* sse_line =
        "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{"
        "\"functionCall\":{\"name\":\"get_weather\",\"args\":{\"location\":\"Shanghai\"}}"
        "}]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":20,\"candidatesTokenCount\":5}}\n\n";

    g_provider_gemini.stream_bridge_feed(bridge, sse_line, strlen(sse_line));
    g_provider_gemini.stream_bridge_finish(bridge);
    g_provider_gemini.stream_bridge_free(bridge);

    TEST_ASSERT(strstr(g_gemini_sse_buf, "tool_calls") != NULL,
                "SSE output contains tool_calls");
    TEST_ASSERT(strstr(g_gemini_sse_buf, "get_weather") != NULL,
                "SSE output contains get_weather");
    TEST_ASSERT(strstr(g_gemini_sse_buf, "Shanghai") != NULL,
                "SSE output contains Shanghai");
}
```

- [ ] **Step 2: 在 `tests/unit/run_tests.c` 注册 6 个新测试**

找到 `test_gemini_sniff_streaming_sse` 注册行之后，追加：

```c
    extern void test_gemini_tools_request_build(void);
    extern void test_gemini_function_call_response_parse(void);
    extern void test_gemini_tool_result_name_lookup(void);
    extern void test_gemini_tool_result_name_missing_fallback(void);
    extern void test_gemini_multi_tool_calls_response(void);
    extern void test_gemini_sse_function_call_stream(void);
    test_register("gemini_tools_request_build",              test_gemini_tools_request_build);
    test_register("gemini_function_call_response_parse",     test_gemini_function_call_response_parse);
    test_register("gemini_tool_result_name_lookup",          test_gemini_tool_result_name_lookup);
    test_register("gemini_tool_result_name_missing_fallback",test_gemini_tool_result_name_missing_fallback);
    test_register("gemini_multi_tool_calls_response",        test_gemini_multi_tool_calls_response);
    test_register("gemini_sse_function_call_stream",         test_gemini_sse_function_call_stream);
```

- [ ] **Step 3: 构建并运行 Gemini 单元测试**

```bash
cmake --build .build --target aigate_unit_tests 2>&1 | tail -5
cd .build && ctest -R "gemini_tools|gemini_function|gemini_tool_result|gemini_multi|gemini_sse_function" --output-on-failure
```

Expected: 6/6 PASSED。

- [ ] **Step 4: 全量测试确认不退步**

```bash
cd .build && ctest --output-on-failure 2>&1 | tail -5
```

Expected: 全部 PASS。

- [ ] **Step 5: 提交**

```bash
cd /home/quintin/Data/source/c_cpp/aigate
git add src/upstream/provider_gemini.c \
        tests/unit/upstream/test_provider_gemini_tools.c tests/unit/run_tests.c
git commit -m "feat(upstream): 🔧 add Gemini tool calling protocol translation"
```

---

## Task 10: 集成测试

**Files:**
- Modify: `tests/integration/test_gateway.py`

- [ ] **Step 1: 定位 mock upstream fixture 和现有集成测试结构**

打开 `tests/integration/test_gateway.py`，找到 mock server 的设置方式（通常是 `@pytest.fixture` 或测试类的 `setUp`）。新测试复用同样的 mock upstream 机制。

- [ ] **Step 2: 新增 `test_anthropic_tool_calling_non_streaming`**

在文件末尾（`test_admin_lockout_429` 之前）追加：

```python
def test_anthropic_tool_calling_non_streaming(gw, mock_upstream):
    """Anthropic tool calling: verify request translation and response translation."""
    # Anthropic mock response with tool_use content block
    mock_upstream.set_response(
        status=200,
        body=json.dumps({
            "id": "msg_tc_01",
            "type": "message",
            "role": "assistant",
            "model": "claude-3-5-sonnet-20241022",
            "stop_reason": "tool_use",
            "content": [
                {"type": "tool_use", "id": "toolu_01",
                 "name": "get_weather", "input": {"location": "Beijing"}}
            ],
            "usage": {"input_tokens": 30, "output_tokens": 10}
        }),
        headers={"Content-Type": "application/json"}
    )

    resp = gw.post("/anthropic/v1/messages", json={
        "model": "claude-3-5-sonnet-20241022",
        "max_tokens": 1024,
        "messages": [{"role": "user", "content": "What is the weather in Beijing?"}],
        "tools": [{
            "type": "function",
            "function": {
                "name": "get_weather",
                "description": "Get weather for a location",
                "parameters": {
                    "type": "object",
                    "properties": {"location": {"type": "string"}},
                    "required": ["location"]
                }
            }
        }],
        "tool_choice": "auto"
    })
    assert resp.status_code == 200
    body = resp.json()

    # Verify tool_calls in response
    choices = body.get("choices", [])
    assert len(choices) == 1
    msg = choices[0]["message"]
    assert "tool_calls" in msg
    assert len(msg["tool_calls"]) == 1
    tc = msg["tool_calls"][0]
    assert tc["function"]["name"] == "get_weather"
    args = json.loads(tc["function"]["arguments"])
    assert args.get("location") == "Beijing"
    assert choices[0]["finish_reason"] == "tool_calls"

    # Verify the request forwarded to upstream contained tools/input_schema
    fwd = mock_upstream.last_request_body()
    assert "tools" in fwd
    assert fwd["tools"][0].get("input_schema") is not None

    # Round 2: send tool_result message, verify tool_result forwarded
    mock_upstream.set_response(
        status=200,
        body=json.dumps({
            "id": "msg_tc_02", "type": "message", "role": "assistant",
            "model": "claude-3-5-sonnet-20241022", "stop_reason": "end_turn",
            "content": [{"type": "text", "text": "The weather in Beijing is sunny, 25°C."}],
            "usage": {"input_tokens": 60, "output_tokens": 20}
        }),
        headers={"Content-Type": "application/json"}
    )
    resp2 = gw.post("/anthropic/v1/messages", json={
        "model": "claude-3-5-sonnet-20241022",
        "max_tokens": 1024,
        "messages": [
            {"role": "user", "content": "What is the weather in Beijing?"},
            {"role": "assistant", "content": None,
             "tool_calls": [{"id": "toolu_01", "type": "function",
                             "function": {"name": "get_weather",
                                          "arguments": "{\"location\": \"Beijing\"}"}}]},
            {"role": "tool", "tool_call_id": "toolu_01", "content": "Sunny, 25°C"}
        ]
    })
    assert resp2.status_code == 200
    fwd2 = mock_upstream.last_request_body()
    msgs2 = fwd2.get("messages", [])
    # Last message should be user role with tool_result content block
    last_msg = msgs2[-1]
    assert last_msg["role"] == "user"
    content_arr = last_msg.get("content", [])
    assert any(b.get("type") == "tool_result" for b in content_arr if isinstance(b, dict))


def test_gemini_tool_calling_streaming(gw, mock_upstream):
    """Gemini tool calling: verify functionDeclarations forwarded, functionCall SSE translated."""
    # Gemini SSE response with functionCall
    gemini_sse = (
        'data: {"candidates":[{"content":{"role":"model","parts":[{'
        '"functionCall":{"name":"get_weather","args":{"location":"Shanghai"}}}]},'
        '"finishReason":"STOP"}],'
        '"usageMetadata":{"promptTokenCount":25,"candidatesTokenCount":8}}\n\n'
    )
    mock_upstream.set_response(
        status=200,
        body=gemini_sse,
        headers={"Content-Type": "text/event-stream"}
    )

    resp = gw.post("/v1/chat/completions", stream=True, json={
        "model": "gemini-1.5-pro",
        "messages": [{"role": "user", "content": "What is the weather in Shanghai?"}],
        "tools": [{
            "type": "function",
            "function": {
                "name": "get_weather",
                "description": "Get weather",
                "parameters": {
                    "type": "object",
                    "properties": {"location": {"type": "string"}},
                    "required": ["location"]
                }
            }
        }],
        "stream": True
    })
    assert resp.status_code == 200

    # Collect SSE chunks
    tool_calls_found = False
    for line in resp.iter_lines():
        if not line or not line.startswith("data:"):
            continue
        data_str = line[len("data:"):].strip()
        if data_str == "[DONE]":
            break
        chunk = json.loads(data_str)
        for choice in chunk.get("choices", []):
            delta = choice.get("delta", {})
            if "tool_calls" in delta and delta["tool_calls"]:
                tc = delta["tool_calls"][0]
                fn = tc.get("function", {})
                if fn.get("name") == "get_weather":
                    args = json.loads(fn.get("arguments", "{}"))
                    if args.get("location") == "Shanghai":
                        tool_calls_found = True
    assert tool_calls_found, "SSE stream should contain get_weather tool call for Shanghai"

    # Verify forwarded request had functionDeclarations
    fwd = mock_upstream.last_request_body()
    assert "tools" in fwd
    tools_arr = fwd["tools"]
    assert any("functionDeclarations" in t for t in tools_arr)
```

- [ ] **Step 3: 运行集成测试（需要 gateway 进程已启动）**

按项目现有集成测试运行方式执行（通常 `pytest tests/integration/ -k "tool_calling" -v`）。

Expected: 2 个新测试 PASSED（或 SKIP 如果 mock upstream fixture 需要额外配置——以现有 fixture 为准）。

- [ ] **Step 4: 全量单元测试最终确认**

```bash
cd .build && ctest --output-on-failure 2>&1 | tail -10
```

Expected: 全部 PASS。

- [ ] **Step 5: 提交**

```bash
cd /home/quintin/Data/source/c_cpp/aigate
git add tests/integration/test_gateway.py
git commit -m "test(upstream): 🧪 add Anthropic and Gemini tool calling integration tests"
```

---

## 自审清单（实施者检查）

在开始实施前，确认：

1. **Task 1 必须先于 Task 2–4**（bridge struct 字段变更影响所有 bridge 函数）
2. **Task 6–7–8 顺序**：build → parse → bridge，编译后测试
3. **`extract_text_content` helper**（Task 2 Step 1）定义在 `provider_anthropic_build` 之前，且声明为 `static`
4. **`gemini_lookup_tool_name` helper**（Task 6 Step 1）同样为 `static`，放在 `provider_gemini_build` 之前
5. **`-Wall -Wextra -Werror`**：每个 task 结束后必须无 warning，未使用变量 / 隐式类型转换都是错误
6. **`free(tool_args_buf)`**（Task 4 Step 2）：确认在 `free(b)` 之前执行，防止内存泄漏
7. **集成测试 `test_admin_lockout_429` 必须最后运行**：新测试应插在它之前

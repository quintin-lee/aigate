# Tool Calling Protocol Translation Design

**Date:** 2026-09-28  
**Scope:** `src/upstream/provider_anthropic.c`, `src/upstream/provider_gemini.c`  
**Status:** Approved

---

## 1. Problem Statement

aigate 现有的 Anthropic 和 Gemini 适配器仅处理纯文本消息转换。OpenAI Function Calling / Tool Use 协议（`tools`、`tool_choice`、`role:"assistant"` + `tool_calls`、`role:"tool"` 结果消息）在入站时被静默丢弃，导致任何依赖工具调用的 Agent 框架在走 Anthropic / Gemini 后端时完全无法工作。

**目标**：完整实现三个方向的协议双向翻译：
1. 入站请求转换（OpenAI → Anthropic / Gemini 原生格式）
2. 出站响应转换（Anthropic / Gemini 原生格式 → OpenAI）
3. 多轮对话历史（tool result 消息）转换

---

## 2. Architecture Decision: Per-adapter Inline Extension

选择在 `provider_anthropic.c` 和 `provider_gemini.c` 各自内联扩展，不引入新文件或新抽象层。

**理由：**
- vtable（`provider_adapter_t`）签名不变，`aigate_core.c` 零感知
- 两个适配器独立修改、独立测试，互不影响
- OpenAI `tools[].parameters` 已是 JSON Schema，与 Anthropic `input_schema` / Gemini `parameters` 结构相同，可直接透传 jansson 对象引用，无需共享转换层（YAGNI）

**被否决的方案：**
- 共享 `tool_schema.c`：JSON Schema 透传使共享层收益趋零
- vtable 新增 `build_tool_result` slot：`build_chat` 已处理完整 messages 数组，拆分 vtable 增加调用复杂度

---

## 3. Protocol Mapping

### 3.1 Tools Declaration (Request)

```
OpenAI:
  tools: [{
    type: "function",
    function: { name, description, parameters(JSON Schema) }
  }]

→ Anthropic:
  tools: [{
    name, description,
    input_schema: <parameters 直接透传>
  }]

→ Gemini:
  tools: [{
    functionDeclarations: [{
      name, description,
      parameters: <parameters 直接透传>
    }]
  }]
```

### 3.2 Tool Choice (Request)

| OpenAI `tool_choice` | Anthropic | Gemini `toolConfig` |
|---|---|---|
| `"auto"` | `{type:"auto"}` | `{functionCallingConfig:{mode:"AUTO"}}` |
| `"required"` | `{type:"any"}` | `{functionCallingConfig:{mode:"ANY"}}` |
| `"none"` | `{type:"none"}` | `{functionCallingConfig:{mode:"NONE"}}` |
| `{type:"function",function:{name}}` | `{type:"tool",name}` | `{mode:"ANY",allowedFunctionNames:[name]}` |
| 缺省（未设置） | 不设置 `tool_choice` 字段 | 不设置 `toolConfig` 字段 |

### 3.3 Message History: Assistant with Tool Calls (Request)

```
OpenAI:
  { role: "assistant", content: "optional text",
    tool_calls: [{ id, type:"function", function:{name, arguments} }] }

→ Anthropic:
  { role: "assistant", content: [
      {type:"text", text: content},          // 仅当 content 非空时
      {type:"tool_use", id, name, input: JSON.parse(arguments)}
  ]}

→ Gemini:
  { role: "model", parts: [
      {text: content},                       // 仅当 content 非空时
      {functionCall: {name, args: JSON.parse(arguments)}}
  ]}
```

### 3.4 Message History: Tool Result (Request)

```
OpenAI:
  { role: "tool", tool_call_id, content }

→ Anthropic:
  { role: "user", content: [
      {type:"tool_result", tool_use_id: tool_call_id, content}
  ]}

→ Gemini:
  { role: "user", parts: [
      {functionResponse: {
          name: <lookup_name(tool_call_id)>,
          response: {content: JSON.parse(content) or {output: content}}
      }}
  ]}
```

**Gemini name lookup**：遍历 `messages` 数组，向上回溯找最近的 `role:"assistant"` 消息，在其 `tool_calls` 中匹配 `id == tool_call_id` 取出 `function.name`。若未找到，以 `tool_call_id` 字符串作为 name 并写 WARN 日志。时间复杂度 O(n×k)，可接受。

### 3.5 Message: Content as Array (Request)

OpenAI 允许 `content` 为 content part 数组 `[{type:"text"|"image_url", ...}]`。本次扩展范围：

- `type:"text"` part → 拼入文本
- `type:"image_url"` → 写 WARN 日志后跳过（视觉功能暂缓）

### 3.6 Response: Tool Use (Non-Streaming)

```
Anthropic response:
  { stop_reason: "tool_use",
    content: [
      {type:"text", text},                          // 可能无
      {type:"tool_use", id, name, input: {object}}
    ]
  }

→ OpenAI response:
  { finish_reason: "tool_calls",
    choices:[{message:{
      role:"assistant",
      content: text or null,
      tool_calls: [{id, type:"function", function:{name, arguments: JSON.dumps(input)}}]
    }}]
  }
```

```
Gemini response:
  { candidates:[{
      finishReason:"STOP",
      content:{parts:[
        {functionCall:{name, args:{object}}}
      ]}
  }]}

→ OpenAI response:
  { finish_reason: "tool_calls",
    choices:[{message:{
      role:"assistant", content: null,
      tool_calls: [{id:"call_<name>_<ts>", type:"function", function:{name, arguments: JSON.dumps(args)}}]
    }}]
  }
```

---

## 4. SSE Streaming Translation

### 4.1 Anthropic SSE Tool Calling

Anthropic 流式 tool_use 分散在多个事件中：

```
content_block_start  → type=="tool_use": 记录 id / name，重置 args buffer
content_block_delta  → type=="input_json_delta": realloc 累积 partial_json（上限 64KB）
content_block_stop   → 若 in_tool_use: 发出完整 tool_calls chunk（单次，非增量）
message_delta        → stop_reason=="tool_use": 发 finish_reason:"tool_calls" chunk
```

**`anthropic_bridge_t` 新增字段：**

```c
bool   in_tool_use;       /* 正在累积 tool_use block */
char   tool_id[64];       /* 当前 tool call id */
char   tool_name[128];    /* 当前 tool call name */
char*  tool_args_buf;     /* malloc 动态累积 partial_json */
size_t tool_args_len;
int    tool_index;        /* tool_calls 数组下标（多工具递增）*/
```

**发出的 OpenAI SSE chunk（content_block_stop 时）：**

```json
data: {"id":"chatcmpl-...","object":"chat.completion.chunk","model":"claude-...","choices":[{
  "index":0,
  "delta":{"tool_calls":[{"index":0,"id":"toolu_01","type":"function","function":{"name":"fn","arguments":"{...}"}}]},
  "finish_reason":null
}]}
```

**bridge_free 时释放 `tool_args_buf`。**

### 4.2 Gemini SSE Tool Calling

Gemini SSE 每行为完整 JSON，`gemini_bridge_process_line` 解析 parts 时增加：

```c
json_t* jfc = json_object_get(p, "functionCall");
if (jfc) {
    /* 构造 tool_calls chunk + finish_reason:"tool_calls" */
}
```

Gemini 不做参数增量推送，检测到 `functionCall` 时直接发出完整 tool_calls chunk。

### 4.3 Multi-Tool Calls (N > 1)

- Anthropic：content array 含多个 `tool_use` block，`tool_index` 计数器逐 block 递增，每个 `content_block_stop` 发一个 chunk
- Gemini：parts array 含多个 `functionCall`，遍历后全部追加进同一 `tool_calls` 数组，单次发出

### 4.4 Mixed Content (text + tool_use)

- 非流式：先扫全部 content/parts，text 拼 `message.content`，tool_use/functionCall 追加 `tool_calls`
- 流式（Anthropic）：text delta chunk 和 tool_use chunk 按事件自然顺序发出，客户端按 `index` 区分
- 流式（Gemini）：单次 JSON 内含 text + functionCall parts，分别发对应 chunk

---

## 5. Files Changed

| 文件 | 变更 | 估算行数 |
|---|---|---|
| `src/upstream/provider_anthropic.c` | 扩展 build/parse/bridge | +~220 |
| `src/upstream/provider_anthropic.h` | bridge struct 新增字段注释 | +~10 |
| `src/upstream/provider_gemini.c` | 扩展 build/parse/bridge_process_line | +~180 |
| `tests/unit/upstream/test_provider_anthropic_tools.c` | 新文件，6 个单测 | ~160 |
| `tests/unit/upstream/test_provider_gemini_tools.c` | 新文件，6 个单测 | ~160 |
| `tests/integration/test_gateway.py` | 2 个集成测试 | +~80 |
| `CMakeLists.txt` | 2 个单测 target | +~10 |

`aigate_core.c` / `admin_api.c` / `provider_adapter.h` vtable — **零改动**。

---

## 6. Unit Tests

### `test_provider_anthropic_tools.c`

| 用例 | 验证点 |
|---|---|
| `tools_request_build` | tools/tool_choice → Anthropic JSON 正确 |
| `tool_use_response_parse` | tool_use block → OpenAI tool_calls；finish_reason |
| `mixed_text_tool_response` | text + tool_use → content 有文本 + tool_calls |
| `tool_result_message_build` | role:tool → tool_result content block |
| `assistant_tool_calls_message` | role:assistant + tool_calls → tool_use block |
| `sse_tool_call_stream` | 喂入 content_block_start/delta×N/stop/message_delta → SSE chunks 正确 |

### `test_provider_gemini_tools.c`

| 用例 | 验证点 |
|---|---|
| `tools_request_build` | functionDeclarations；toolConfig mode 映射 |
| `function_call_response_parse` | functionCall part → OpenAI tool_calls |
| `tool_result_name_lookup` | tool role + 前置 assistant → functionResponse 含正确 name |
| `tool_result_name_missing` | tool_call_id 无匹配 → fallback + WARN，不 crash |
| `multi_tool_calls` | N 个 functionCall parts → tool_calls 数组含 N 项 |
| `sse_function_call_stream` | functionCall SSE 行 → tool_calls chunk 正确 |

---

## 7. Integration Tests

`tests/integration/test_gateway.py` 新增：

**`test_anthropic_tool_calling_non_streaming`**
- 检查转发到 mock Anthropic 的请求含 `tools` / `input_schema`
- mock 返回 `tool_use` block → 检查回给客户端的 OpenAI `tool_calls` 正确
- 第二轮携带 `tool_result` 消息 → 检查转发含 `tool_result` content block

**`test_gemini_tool_calling_streaming`**
- 检查转发到 mock Gemini 含 `functionDeclarations`
- mock 返回 `functionCall` SSE 行 → 检查客户端收到 `tool_calls` delta chunk
- 第二轮 tool role 消息 → 检查 `functionResponse.name` 正确（name lookup）

---

## 8. Out of Scope

- 工具 Schema args 超 64KB（截断 + WARN，正确性不保证）
- 图像 / 多模态 content part（写 WARN 后跳过）
- Anthropic extended thinking / Claude 3.7 `thinking` block（独立功能项）
- OpenAI `/v1/responses` 端点（独立功能项，见 gap 分析 P1-3）
- 新供应商工具调用（新增供应商时按本设计模式实现）

# Design Spec: DeepSeek-R1 / Qwen-QwQ & Anthropic Extended Thinking Reasoning Ecosystem

**Author:** AI Pair Programmer & Antigravity  
**Status:** Approved  
**Date:** 2026-10-11  
**Target Milestone:** v2.1 Reasoning & Extended Thinking Interoperability  

---

## 1. Executive Summary & Goals

随着以 DeepSeek-R1、Qwen-QwQ 为代表的开源深度思考模型与以 Claude 3.7 Sonnet (Extended Thinking)、Gemini 2.0 Flash Thinking 为代表的商业闭源推理模型的普及，企业客户端在调用推理大模型时面临显著的**协议格式割裂**与**思考预算管理痛点**：

1. **协议表达差异显著**：
   - OpenAI 生态主流采用 `reasoning_content`（DeepSeek / Qwen 风格），独立于正文 `content`；
   - Anthropic 原生采用 `content: [{type: "thinking", thinking: "..."}, {type: "text", text: "..."}]` 与流式 `thinking_delta`；
   - 老旧纯文本客户端或未适配 SDK 仅支持 `<thinking>...</thinking>` 文本块包裹。
2. **思考预算缺乏统一调控**：
   - 客户端可能传入 OpenAI 风格 `reasoning_effort: "low" | "medium" | "high"` 或 Anthropic 风格 `budget_tokens`；
   - Anthropic 协议强制要求 `max_tokens > budget_tokens`，若不自适应调优极易触发上游 HTTP 400 校验阻断。
3. **安全风控与计量缺失**：
   - 思考链中可能生成极端违规内容或泄露隐私，需要安全风控（Guardrails 与越狱检测）无死角覆盖；
   - 隐写水印需精准避免污染思考链结构，且思考 Token 需作为独立维度进行计量监控与成本核算。

本设计说明书通过建立网关内核的**统一推理抽象中间层 (Unified Reasoning Abstraction Layer)**，消除各供应商之间的思考链协议摩擦，实现无感双向跨协议调用、自适应预算防护、思考过程安全审计与思考 Token 独立监控。

---

## 2. Architecture & Data Structures

### 2.1 统一内部数据模型

在 [`src/upstream/provider_adapter.h`](file:///home/quintin/Data/source/c_cpp/aigate/src/upstream/provider_adapter.h) 中定义统一数据结构：

```c
/** @brief 深度思考预算与工作模式配置 */
typedef struct reasoning_config {
    bool     enabled;         /**< 是否开启深度思考 */
    long     budget_tokens;   /**< 思考 Token 预算上限 (0 = 不限/上游默认) */
    char     effort[16];      /**< low | medium | high | none */
} reasoning_config_t;

/** @brief 跨协议统一思考分块 (用于流式增量与非流式聚合转换) */
typedef struct reasoning_chunk {
    const char* thinking_text;      /**< 思考过程增量内容 (NULL 表示无思考增量) */
    size_t      thinking_len;       /**< 思考内容字节数 */
    const char* content_text;       /**< 正式回答增量内容 (NULL 表示真正文尚未开始) */
    size_t      content_len;        /**< 正式回答字节数 */
    bool        is_thinking_block;  /**< 当前分块是否属于思考阶段 */
    long        reasoning_tokens;   /**< 累计思考 Token (若当前分块包含 usage) */
} reasoning_chunk_t;

/** @brief 流式 SSE 思考状态机状态 */
typedef enum stream_thinking_state {
    THINK_STATE_INIT = 0,     /**< 初始等待首包 */
    THINK_STATE_THINKING,     /**< 正在接收/发送思考过程 */
    THINK_STATE_CONTENT,      /**< 思考结束，正在接收/发送正文回答 */
    THINK_STATE_DONE          /**< 流结束 */
} stream_thinking_state_t;
```

### 2.2 存储与模型路由元数据扩展

在数据库 `models` 表与模型路由结构体 [`model_rec_t`](file:///home/quintin/Data/source/c_cpp/aigate/src/upstream/model_router.h) 中增加属性：

```c
typedef struct model_rec {
    // ... 现有字段
    long default_thinking_budget; /**< 模型默认思考 Token 预算 (0 = 未预设) */
    bool supports_reasoning;      /**< 是否支持深度思考模型 */
} model_rec_t;
```

SQL 架构更新（Schema Migration v19）：
```sql
ALTER TABLE models ADD COLUMN IF NOT EXISTS default_thinking_budget BIGINT NOT NULL DEFAULT 0;
ALTER TABLE models ADD COLUMN IF NOT EXISTS supports_reasoning BOOLEAN NOT NULL DEFAULT FALSE;
```

---

## 3. Inbound Parameter Extraction & Budget Policy

### 3.1 客户端思考参数解析

在 [`src/upstream/provider_adapter.c`](file:///home/quintin/Data/source/c_cpp/aigate/src/upstream/provider_adapter.c) 中实现：

```c
int parse_reasoning_config(json_t* req_body, const model_rec_t* route, reasoning_config_t* out_cfg);
```

提取优先级规则：
1. **客户端显式指定**：
   - 若请求体包含 `thinking: {type: "enabled", budget_tokens: N}` 或 `max_thinking_tokens: N`，直接解析 `budget_tokens = N`，`enabled = true`。
   - 若包含 OpenAI 标准参数 `reasoning_effort: "low" | "medium" | "high"`：
     - `low` $\to$ `budget_tokens = 1024`
     - `medium` $\to$ `budget_tokens = 4096`
     - `high` $\to$ `budget_tokens = 16384`
     - 设 `enabled = true`。
2. **网关模型路由回退**：
   - 若客户端未传入上述任何参数，但目标模型在网关中配置了 `supports_reasoning = true` 且 `default_thinking_budget > 0`：
     - 自动回退注入 `budget_tokens = route->default_thinking_budget`，`enabled = true`。

### 3.2 上游目标自适应注入与安全约束守护

针对不同上游提供商进行适配：

* **转发至 Anthropic (`provider_anthropic.c`)**：
  - 构造 `{"type": "enabled", "budget_tokens": cfg->budget_tokens}` 并赋给顶层 `thinking` 字段；
  - **关键约束**：Anthropic 强制要求 `max_tokens > budget_tokens`。若客户端给出的 `max_tokens <= cfg->budget_tokens`，网关自动将上游请求中的 `max_tokens` 动态修正为 `cfg->budget_tokens + 4096`，杜绝上游 HTTP 400 失败。
* **转发至 Gemini (`provider_gemini.c`)**：
  - 在 `generationConfig` 中注入 `thinkingConfig: {"thinkingBudget": cfg->budget_tokens}`。
* **转发至 DeepSeek / OpenAI (`provider_openai.c`)**：
  - 透传 `reasoning_effort`；若为数字预算，映射为最接近的 effort 级别或原样保留。

---

## 4. Outbound Response & Streaming SSE State Machine

### 4.1 非流式（同步）多向互转

1. **Anthropic 响应 $\to$ OpenAI 格式**：
   - 扫描 Anthropic 返回的 `content` 数组：
     - `type: "thinking"`：将其 `thinking` 字符串提取并赋予 OpenAI 响应中的 `choices[0].message.reasoning_content`；
     - `type: "text"`：将其 `text` 字符串赋予 `choices[0].message.content`；
   - 提取 Anthropic usage，并在 `usage.completion_tokens_details.reasoning_tokens` 中填报思考消耗。
2. **DeepSeek 响应 $\to$ Anthropic 原生 `/v1/messages` 格式**：
   - 检查 `choices[0].message.reasoning_content`：
     - 若存在，先生成 `{"type": "thinking", "thinking": reasoning_content}` 内容块；
     - 再附加正文 `{"type": "text", "text": content}` 内容块。

### 4.2 流式 SSE 分块状态机

在 [`src/upstream/provider_adapter.c`](file:///home/quintin/Data/source/c_cpp/aigate/src/upstream/provider_adapter.c) 中统一流式增量转换状态流：

```
       [ 接收上游 SSE 分块 ]
                 │
                 ▼
     ┌───────────────────────┐
     │  识别当前块类型 ?     │
     └───────────────────────┘
          │              │
    thinking 分块   正文 text 分块
          │              │
          ▼              ▼
  [ 处于思考状态 ]  [ 处于正文状态 ]
          │              │
          ▼              ▼
OpenAI 客户端下发:   OpenAI 客户端下发:
delta: {             delta: {
  "reasoning_content": "..."   "content": "..."
}                    }
Anthropic 客户端下发: Anthropic 客户端下发:
content_block_delta(thinking) content_block_delta(text)
```

#### 老旧客户端纯文本标签兼容模式 (Tag Emulation)
当请求头携带 `X-Aigate-Thinking-Format: tag` 或配置开启标签兼容时：
- 流式输出在进入 `THINK_STATE_THINKING` 时，首先发送包含 `<thinking>\n` 的 `delta.content`；
- 思考过程内容直接输出在 `delta.content` 中；
- 思考过程结束切换至真正文前，发送包含 `\n</thinking>\n\n` 的 `delta.content`。

---

## 5. Dual-Track Safety, Watermarking & Token Accounting

### 5.1 双轨安全协同

* **安全风控拦截（Guardrails & Jailbreak）**：
  - 出站过滤流水线 [`filter_chain_execute_outbound`](file:///home/quintin/Data/source/c_cpp/aigate/src/policy/filter_chain.c) 对 `reasoning_content` 与 `content` 进行双轨扫描；
  - 若模型在思考阶段生成违法违规、极端有害信息或触发敏感词阻断策略，网关立即阻断并上报安全审计事件。
* **零宽隐写水印（Zero-Width Watermarking）**：
  - 水印注入状态机 [`watermark_stream_feed`](file:///home/quintin/Data/source/c_cpp/aigate/src/policy/watermark_engine.c) **仅绑定到正文 `content` 增量**；
  - 严格跳过 `reasoning_content`，保证思考链的纯净与数学逻辑/格式的绝对完整。

### 5.2 Token 计量与成本审计

1. 数据库存储：核心链路在结算时，通过 [`record_usage_and_event`](file:///home/quintin/Data/source/c_cpp/aigate/src/core/aigate_core.c) 写入 `usage_requests.reasoning_tokens`。
2. 额度管理：`daily_token_quota` 与 `monthly_token_budget` 按照 `completion_tokens`（已包含 `reasoning_tokens`）扣减。
3. 指标暴露：
   - 在 [`src/observe/metrics.c`](file:///home/quintin/Data/source/c_cpp/aigate/src/observe/metrics.c) 中新增指标：
     `aigate_tokens_reasoning_total{model="...", key="..."}`，支持 Prometheus 采集并在 Web 控制台展示思考占比。

---

## 6. Verification & Test Plan

### 6.1 C 单元测试矩阵

在 `tests/unit/upstream/` 增加并注册以下用例：

1. `test_anthropic_thinking_inbound_budget`：
   - 验证传入 `reasoning_effort: "medium"` 映射为 4096 tokens；
   - 验证 `max_tokens` 自动上调保护（如 `max_tokens=2000`, `budget=4000` $\to$ `max_tokens=8096`）。
2. `test_anthropic_thinking_outbound_sync`：
   - 验证 Anthropic 原生思考块转换为 OpenAI `reasoning_content` 与正文 `content`，以及 `reasoning_tokens` 的提取。
3. `test_anthropic_thinking_outbound_stream`：
   - 验证流式 SSE 从 `thinking_delta` 平滑转换为 `delta.reasoning_content`，再无缝切换至 `delta.content`。
4. `test_deepseek_to_anthropic_native_thinking`：
   - 验证 DeepSeek-R1 思考响应转换为 Anthropic 原生 `/v1/messages` 接口下的 `thinking` 块格式。
5. `test_thinking_watermark_clean_pass`：
   - 验证隐写水印仅注入正文，思考链不受零宽字符干扰。

### 6.2 Python E2E 集成测试

在 `tests/integration/test_reasoning_models_and_thinking.py` 中编写真实端到端测试：
- 通过模拟 upstream 返回带有 `reasoning_content` 和 Anthropic `thinking` 块的响应；
- 测试流式客户端接收思考过程的实时分块；
- 验证 `/metrics` 接口中 `aigate_tokens_reasoning_total` 指标计数器正确递增；
- 验证出站风控阻断触发与水印纯净性。

---

## 7. Quality Gates

- 编译要求：遵循 `-Wall -Wextra -Werror`，严格保持 0 警告、0 错误。
- 现有测试兼容性：保证全量 306 个现有单元测试与 11 个集成测试 100% PASS，无回归。

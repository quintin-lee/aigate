# 中间件过滤器链与动态 Prompt 注入设计规范

- **状态**: Approved
- **日期**: 2026-09-30
- **目标**: 为 aigate 构建统一的请求中间件过滤器链（Filter Chain），并实现企业级动态 Prompt 模板引擎，支持在 Model 路由级和 API Key 级多级绑定、动态插值变量展开（`${date}`, `${model}`, `${key_name}` 等），以及灵活的 System Prompt 注入与合并模式（prepend / append / override）。

---

## 1. 背景与业务价值

在企业级 AI 应用网关场景中，各部门或各 API Key 调用方常有统一的安全规则、角色人设或合规声明注入诉求（例如要求所有请求均包含内部合规免责声明、企业知识库角色定义或审计上下文）。目前 aigate 仅支持基于精确匹配的 Guardrails 内容审查，缺少对提示词的动态装配与请求前置中间件扩展能力。

本次设计将完成两大核心目标：
1. **统一的前置中间件过滤器链 (Filter Chain)**：将原本分散在各处理阶段的安全审核、敏感词过滤与请求修改逻辑抽象为统一、解耦的中间件调用链，便于后续无缝扩充。
2. **企业级动态 Prompt 模板注入**：支持在模型级与 API Key 级配置 Prompt 模板，在请求进入大模型推理前自动完成动态变量插值与 System 消息的原子化合并。

---

## 2. 总体架构与数据流转

### 2.1 请求流水线中的位置

```text
HTTP Client (/v1/chat/completions)
                │
                ▼
┌───────────────────────────────────────┐
│        1. Auth & Quota Gate           │  --> 校验 API Key、QPS 与月度预算
└───────────────────────────────────────┘
                │
                ▼
┌───────────────────────────────────────┐
│        2. Model Route Resolve         │  --> 匹配目标模型及上游候选 Targets
└───────────────────────────────────────┘
                │
                ▼
┌───────────────────────────────────────┐
│      Middleware Filter Chain          │
│                                       │
│  [Filter 1] Guardrails Moderation     │  --> 敏感词检测、PII 扫描脱敏
│  [Filter 2] Prompt Template Injection │  --> 动态变量插值、合并 System Prompt
└───────────────────────────────────────┘
                │
                ▼
┌───────────────────────────────────────┐
│       3. Upstream Dispatch            │  --> 上游适配转换 (OpenAI / Claude / Gemini)
└───────────────────────────────────────┘
```

### 2.2 模块组织规划

```text
src/
└── policy/
    ├── filter_chain.h          # 中间件过滤器链定义与注册
    ├── filter_chain.c          # 中间件调度与执行器
    ├── prompt_template.h       # Prompt 模板模型、枚举与对外 API
    └── prompt_template.c       # 变量插值引擎与 Jansson 报文合并逻辑
```

---

## 3. 中间件过滤器链设计 (`filter_chain.h` / `.c`)

### 3.1 过滤器抽象接口

```c
typedef enum {
    FILTER_CONTINUE = 0, /* 继续执行后续 Filter */
    FILTER_STOP     = 1  /* 拦截中断请求（已向客户端返回错误） */
} filter_action_t;

/** @brief 请求前置过滤器回调函数指针 */
typedef filter_action_t (*req_filter_fn)(chat_req_t* q);

typedef struct {
    const char*   name;
    req_filter_fn execute;
} filter_entry_t;

/** @brief 执行全量入站过滤器链 */
filter_action_t filter_chain_execute_inbound(chat_req_t* q);
```

### 3.2 默认执行序列

1. **`filter_guardrails`**：执行敏感词匹配与 PII 脱敏（若触发 `block` 动作，向客户端返回 400 并返回 `FILTER_STOP`）。
2. **`filter_prompt_template`**：检查 API Key 或 Model 上是否挂载 Prompt 模板。若挂载，执行变量插值并重写 `q->eff_body`；若无挂载则直接放行。

---

## 4. Prompt 模板引擎设计 (`prompt_template.h` / `.c`)

### 4.1 数据结构与合并模式

```c
typedef enum {
    PROMPT_MODE_PREPEND  = 0, /* 默认：在客户端原有 system prompt 前拼接 */
    PROMPT_MODE_APPEND   = 1, /* 在客户端原有 system prompt 后拼接 */
    PROMPT_MODE_OVERRIDE = 2  /* 强行覆盖客户端原有的 system prompt */
} prompt_inject_mode_t;

typedef struct {
    char*                system_template;    /* System Prompt 模板正文 */
    prompt_inject_mode_t mode;               /* 合并模式 */
    char*                prefix_user_prompt; /* 可选：附加在最新 user 消息前的前缀 */
    char*                suffix_user_prompt; /* 可选：附加在最新 user 消息后的后缀 */
} prompt_template_t;
```

### 4.2 动态插值宏展开

支持以下动态占位符（单趟扫描、动态扩容）：

- **`${date}`**：当前 UTC 年-月-日（如 `2026-09-30`）
- **`${time}`**：当前 UTC 时:分:秒（如 `19:20:00`）
- **`${timestamp}`**：当前 Unix 秒级时间戳（如 `1790767200`）
- **`${model}`**：请求解析出的模型名称（如 `gpt-4o`）
- **`${key_name}`**：鉴权通过的 API Key 标识名称（如 `finance-corp-key`）

未识别的 `${...}` 占位符保持原文不动，避免意外丢失。

### 4.3 Jansson 消息合并规则

1. 解析当前有效请求体 `q->eff_body` 中的 `messages` JSON 数组。
2. 查找已存在的首个 `role == "system"` 对象：
   - **已存在**：
     - `PROMPT_MODE_PREPEND`：`content = <expanded_tmpl> + "\n\n" + <orig_content>`
     - `PROMPT_MODE_APPEND`：`content = <orig_content> + "\n\n" + <expanded_tmpl>`
     - `PROMPT_MODE_OVERRIDE`：`content = <expanded_tmpl>`
   - **不存在**：
     - 创建新对象 `{"role": "system", "content": <expanded_tmpl>}`，并使用 `json_array_insert_new(messages, 0, new_sys)` 插入到数组首位。
3. （若配置）处理最新一条 `role == "user"` 消息，附加 `prefix_user_prompt` 与 `suffix_user_prompt`。
4. 序列化修改后的 JSON，赋予 `q->sanitized_body`，并重定向 `q->eff_body`。在请求销毁时由 `chat_req_cleanup(q)` 统一安全释放，杜绝内存泄漏。

---

## 5. 配置优先级与持久化

### 5.1 生效优先级

1. **API Key 级模板**：最高优先级，若当前 API Key 配置了有效 `system_prompt`，优先应用此模板。
2. **Model 路由级模板**：次优先级，若 API Key 未配置但目标 Model 配置了，应用 Model 模板。
3. **未配置**：两者均为空时，过滤器直接跳过。

### 5.2 数据库 Schema 迁移 (`schema_sql.h`)

在 `models` 与 `api_keys` 表中新增可选列：

```sql
ALTER TABLE models ADD COLUMN IF NOT EXISTS system_prompt TEXT DEFAULT NULL;
ALTER TABLE models ADD COLUMN IF NOT EXISTS prompt_mode INT DEFAULT 0;

ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS system_prompt TEXT DEFAULT NULL;
ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS prompt_mode INT DEFAULT 0;
```

### 5.3 Admin API 扩展

在 `/admin/v1/models` 和 `/admin/v1/keys` 中新增字段解析与返回：
- `system_prompt`：字符串（最大 4096 字符）
- `prompt_mode`：字符串（`"prepend"`, `"append"`, `"override"`），缺省为 `"prepend"`

---

## 6. 验证与测试规范

1. **单元测试 (`tests/unit/policy/test_prompt_template.c`)**：
   - 验证插值引擎（`${date}`, `${model}`, `${key_name}` 替换正确性）；
   - 验证三种模式（`PREPEND`, `APPEND`, `OVERRIDE`）对包含与不包含原有 system prompt 的处理；
   - 验证异常输入（畸形 JSON、空模板、超长模板）的容错性。
2. **端到端集成测试 (`tests/integration/test_prompt_template.py`)**：
   - 通过 Admin API 为模型注册含 `${model}` 和 `${date}` 的模板；
   - 发起 Chat 请求，在 Mock 上游中捕获收到的最终请求报文，断言包含展开后的企业级 System Prompt。
3. **AddressSanitizer 验证**：
   - 在 `./scripts/run_chaos_asan.sh` 中跑通全部测试，确保新增的字符串拷贝与 JSON 报文操作 0 内存泄漏。

# 分层混合安全风控与外部 Webhook 审查插件实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 AIGate 构建分层混合安全风控体系（Tiered Hybrid Moderation），将外部 HTTP Webhook 审核插件集成至风控引擎与中间件过滤链，支持全链路双向审查（入站 Prompt 拦截 + 出站模型响应审查）、动态降级策略（Fail-Open / Fail-Closed）、毫秒级超时控制，并在 Admin API 及 Web 控制台提供可视化管理与连通性测试。

**Architecture:**
- **L1 本地极速通道 (<0.1ms):** Aho-Corasick 多关键词模式树匹配与正则表达式敏感信息脱敏（零额外网络开销）。
- **L2 外部 Webhook 深度审查插件:** 基于 libcurl 异步/同步 HTTP 客户端构建标准化交互协议（打包 `phase`, `model`, `key_id`, `content`, `messages`），解析 `pass` / `block` / `mask` 决策，支持配置降级策略（`fail_mode`: `open` 优先放行 vs `closed` 阻断）与超时控制。
- **数据库驱动 (Migration v11):** 扩展 `guardrails_rules` 表，新增 `webhook_secret`, `timeout_ms`, `fail_mode`, `phase` 字段，实现规则统一持久化与热重载。
- **管理端一体化:** 扩展 `/admin/v1/guardrails` RESTful 端点支持 `rule_type == 'webhook'`，并提供 `/admin/v1/guardrails/webhook/test` 连通性探测接口；在 `web/admin.html` 嵌入式界面增加 Webhook 规则配置与即时测试探测工具。

**Tech Stack:** C17, libcurl, Jansson (JSON), PostgreSQL / libpq, HTML5 / Tailwind CSS, Python 3, Pytest.

---

## 文件影响范围映射

- **数据库与数据访问层：**
  - `src/store/schema_sql.h`: 增加 Migration v11 SQL 迁移脚本。
  - `src/store/pg_store.h`: 扩展 `guardrail_rule_t` 结构体字段。
  - `src/store/pg_store.c`: 更新 `pq_list_guardrails_rules`, `pq_create_guardrails_rule`, `pq_update_guardrails_rule` 查询与字段映射。
- **策略引擎与过滤链：**
  - `src/policy/guardrails.h`, `src/policy/guardrails.c`: 实现 Webhook 规则结构体、libcurl HTTP 发送器、报文序列化/解析、降级处理与出入站审查函数。
  - `src/policy/filter_chain.h`, `src/policy/filter_chain.c`: 入站接入 L2 Webhook 审查；新增出站过滤链接口 `filter_chain_execute_outbound`。
- **核心网关流程：**
  - `src/core/pipeline_chat.c`: 在非流式响应交付及缓存前调用出站风控链。
- **管理 API 与 Web 控制台：**
  - `src/server/admin_api.c`: 支持 `webhook` 规则类型、字段解析与 `/admin/v1/guardrails/webhook/test` 探测接口。
  - `web/admin.html`: 增加 Webhook 规则表单、表格徽章与连通性即时测试模态交互。
- **集成测试：**
  - `tests/integration/test_guardrails_webhook.py`: 编写端到端自动化测试套件（覆盖 pass, block, mask, timeout, fail-open/closed 及 Admin API）。

---

## 任务拆分列表

### Task 1: 数据库迁移与数据访问层扩展 (Migration v11)

**Files:**
- Modify: `src/store/schema_sql.h:150-165`
- Modify: `src/store/pg_store.h:105-120`
- Modify: `src/store/pg_store.c:1890-1995`

- [x] **Step 1: 在 `schema_sql.h` 中编写 Migration v11**

在 `src/store/schema_sql.h` 的末尾添加：
```sql
-- Migration v11: external webhook moderation plugin support
ALTER TABLE guardrails_rules
  ADD COLUMN IF NOT EXISTS webhook_secret TEXT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS timeout_ms INT NOT NULL DEFAULT 500,
  ADD COLUMN IF NOT EXISTS fail_mode VARCHAR(16) NOT NULL DEFAULT 'open',
  ADD COLUMN IF NOT EXISTS phase VARCHAR(16) NOT NULL DEFAULT 'inbound';

INSERT INTO schema_migrations(version) VALUES (11) ON CONFLICT (version) DO NOTHING;
```

- [x] **Step 2: 在 `pg_store.h` 中扩展 `guardrail_rule_t` 结构体**

在 `src/store/pg_store.h` 的 `typedef struct guardrail_rule` 中增加：
```c
    char   webhook_secret[256]; /**< Optional Bearer token */
    int    timeout_ms;          /**< Webhook HTTP timeout in ms (default: 500) */
    char   fail_mode[16];       /**< "open" (fail-open) | "closed" (fail-closed) */
    char   phase[16];           /**< "inbound" | "outbound" | "both" */
```

- [x] **Step 3: 更新 `pg_store.c` 中规则读取、插入与更新逻辑**

在 `src/store/pg_store.c` 中：
1. `pq_list_guardrails_rules`:
   - SQL 查询扩展：
     ```sql
     "SELECT id, rule_type, pattern, action, category, enabled, "
     "EXTRACT(EPOCH FROM created_at)::bigint, "
     "COALESCE(webhook_secret, ''), COALESCE(timeout_ms, 500), "
     "COALESCE(fail_mode, 'open'), COALESCE(phase, 'inbound') "
     "FROM guardrails_rules ORDER BY id"
     ```
   - 读取字段：`webhook_secret`, `timeout_ms`, `fail_mode`, `phase`。
2. `pq_create_guardrails_rule`:
   - SQL 插入扩展为 9 个参数：包含 `webhook_secret`, `timeout_ms`, `fail_mode`, `phase`。
3. `pq_update_guardrails_rule`:
   - SQL 更新扩展为 10 个参数：包含 4 个新字段更新。

- [x] **Step 4: 编译并验证单元测试**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 编译无 warning，201 个单元测试全量通过。

- [x] **Step 5: Commit Task 1**

```bash
git add src/store/schema_sql.h src/store/pg_store.h src/store/pg_store.c
git commit -m "feat(store): 🗄️ add migration v11 and guardrails webhook rule fields"
```

---

### Task 2: Webhook 审查 HTTP 发送器与风控引擎核心 (Guardrails Engine)

**Files:**
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`

- [x] **Step 1: 在 `guardrails.h` 中定义 Webhook 规则结构与审查接口**

在 `src/policy/guardrails.h` 中添加：
```c
/** @brief Webhook rule runtime definition. */
typedef struct guardrail_webhook_rule {
    long id;
    char url[512];
    char secret[256];
    int  timeout_ms;
    char fail_mode[16]; /**< "open" | "closed" */
    char phase[16];     /**< "inbound" | "outbound" | "both" */
} guardrail_webhook_rule_t;

/** @brief Inbound Webhook inspection. */
guardrails_action_t guardrails_inspect_webhook_inbound(
    guardrails_ctx_t* ctx,
    const char*       model,
    long              key_id,
    const char*       raw_body,
    size_t            raw_len,
    char**            sanitized_body,
    size_t*           sanitized_len,
    char*             block_reason,
    size_t            block_reason_sz);

/** @brief Outbound Webhook inspection on LLM response. */
guardrails_action_t guardrails_inspect_webhook_outbound(
    guardrails_ctx_t* ctx,
    const char*       model,
    long              key_id,
    const char*       response_body,
    size_t            response_len,
    char**            sanitized_body,
    size_t*           sanitized_len,
    char*             block_reason,
    size_t            block_reason_sz);

/** @brief Single standalone probe helper for Webhook test endpoint. */
int guardrails_webhook_probe(const char* url,
                             const char* secret,
                             int         timeout_ms,
                             char*       out_err,
                             size_t      err_sz,
                             double*     out_latency_ms);
```

- [x] **Step 2: 在 `guardrails.c` 中实现 Webhook 发送与决策解析**

1. 在 `guardrails_ctx` 结构体中添加 `guardrail_webhook_rule_t webhooks[16]; size_t webhook_count;`。
2. 在 `guardrails_load_rules` 中识别 `strcmp(rule->rule_type, "webhook") == 0` 并载入 webhook 规则数组。
3. 实现 HTTP POST 通信函数 `perform_webhook_request()`：
   - 提取 `content`（从 chat completion body 提取最后一条 user 消息或整个正文）。
   - 构造 JSON 载荷：`{"phase": ..., "model": ..., "key_id": ..., "content": ..., "messages": ...}`。
   - 配置 libcurl：`CURLOPT_TIMEOUT_MS`、设置 `Authorization: Bearer <secret>`、接收响应 JSON。
   - 解析响应 `{"action": "pass" | "block" | "mask", "reason": "...", "masked_content": "..."}`。
   - 若遭遇网络超时或 HTTP 错误：
     - 若 `strcmp(rule->fail_mode, "closed") == 0`，返回 `GUARDRAILS_BLOCKED`（原因：`moderation_timeout_fail_closed`）。
     - 若为 `"open"`，记录警告日志并安全返回 `GUARDRAILS_PASS`。
4. 实现 `guardrails_inspect_webhook_inbound` 与 `guardrails_inspect_webhook_outbound`。
5. 实现 `guardrails_webhook_probe` 用于管理端测试连通性。

- [x] **Step 3: 编译与单元测试验证**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 编译无 warning，单元测试通过。

- [x] **Step 4: Commit Task 2**

```bash
git add src/policy/guardrails.h src/policy/guardrails.c
git commit -m "feat(guardrails): 🛡️ implement webhook moderation client and inspection engine"
```

---

### Task 3: 中间件过滤链双向集成 (Filter Chain Bidirectional Integration)

**Files:**
- Modify: `src/policy/filter_chain.h`
- Modify: `src/policy/filter_chain.c`
- Modify: `src/core/pipeline_chat.c`

- [x] **Step 1: 在 `filter_chain.c` 中集成 L2 入站 Webhook 审查**

在 `filter_guardrails(chat_req_t* q)` 中：
1. 先执行现有的 L1（关键词 AC 自动机 + 正则 PII 脱敏）。
2. 若 L1 返回 `GUARDRAILS_BLOCKED`，直接中止。
3. 若 L1 通过（或脱敏完成），检查 `q->ac->gr` 中是否有启用的入站 Webhook 规则：
   - 调用 `guardrails_inspect_webhook_inbound(q->ac->gr, q->model, q->krec.key_id, q->eff_body, q->eff_len, ...)`。
   - 若返回 `GUARDRAILS_BLOCKED`：写出 400 `content_policy_violation` 错误，记录用量并返回 `FILTER_STOP`。
   - 若返回 `GUARDRAILS_MASKED`：替换 `q->eff_body`，标记 `q->guardrail_act = "masked"`。

- [x] **Step 2: 在 `filter_chain.h` 与 `filter_chain.c` 中添加出站审查接口**

在 `filter_chain.h` 增加：
```c
filter_action_t filter_chain_execute_outbound(
    chat_req_t* q,
    const char* resp_body,
    size_t      resp_len,
    char**      out_body,
    size_t*     out_len);
```
在 `filter_chain.c` 中实现出站逻辑：
- 调用 `guardrails_inspect_webhook_outbound(q->ac->gr, q->model, q->krec.key_id, resp_body, resp_len, ...)`。
- 若返回 `GUARDRAILS_BLOCKED`：写出 400 阻断错误（或安全合规声明），返回 `FILTER_STOP`。
- 若返回 `GUARDRAILS_MASKED`：用脱敏内容替换 `out_body`。
- 否则返回 `FILTER_CONTINUE`。

- [x] **Step 3: 在 `pipeline_chat.c` 的非流式响应交付链路中接入出站审查**

在 `src/core/pipeline_chat.c` 的 `handle_chat_sync` 中（大约在 `adapter->parse_chat_response` 成功后）：
```c
char*  filtered_resp = NULL;
size_t filtered_len = 0;
if (filter_chain_execute_outbound(q, parsed_body, parsed_len, &filtered_resp, &filtered_len) == FILTER_STOP) {
    free(parsed_body);
    chat_req_cleanup(q);
    return 0;
}
if (filtered_resp != NULL) {
    free(parsed_body);
    parsed_body = filtered_resp;
    parsed_len = filtered_len;
}
```

- [x] **Step 4: 编译并验证核心链路**

Run: `cmake --build .build -j$(nproc)`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 编译 0 warning，所有单测全部通过。

- [x] **Step 5: Commit Task 3**

```bash
git add src/policy/filter_chain.h src/policy/filter_chain.c src/core/pipeline_chat.c
git commit -m "feat(filter): 🔗 wire bidirectional L1+L2 webhook moderation into filter chain"
```

---

### Task 4: 管理端 API 扩展与 Webhook 测试探测接口

**Files:**
- Modify: `src/server/admin_api.c:2830-3050`

- [x] **Step 1: 扩展 `/admin/v1/guardrails` 规则校验与序列化**

在 `src/server/admin_api.c` 中：
1. 校验放行 `rule_type == "webhook"`。
2. 在返回规则列表与单条规则时，序列化新字段：`webhook_secret`, `timeout_ms`, `fail_mode`, `phase`。
3. 在 `guardrails_rule_create` 和 `guardrails_rule_update` 中：
   - 提取 `webhook_secret`（可选字符串）。
   - 提取 `timeout_ms`（默认 500）。
   - 提取 `fail_mode`（默认 `"open"`，校验只能为 `"open"` 或 `"closed"`）。
   - 提取 `phase`（默认 `"inbound"`，校验只能为 `"inbound"`, `"outbound"`, `"both"`）。

- [x] **Step 2: 增加 `POST /admin/v1/guardrails/webhook/test` 连通性探测接口**

在 `src/server/admin_api.c` 中：
1. 路由注册：当路径为 `guardrails/webhook/test` 且方法为 `POST` 时分发给 `guardrails_webhook_test_handler`。
2. 实现 `guardrails_webhook_test_handler`:
   - 接收 JSON: `{"url": "...", "webhook_secret": "...", "timeout_ms": 1000}`。
   - 调用 `guardrails_webhook_probe(...)`。
   - 成功返回：`{"status": "ok", "latency_ms": 15.3, "reachable": true}`。
   - 失败返回：`{"status": "error", "error": err_msg, "reachable": false}`。

- [x] **Step 3: 编译与测试验证**

Run: `cmake --build .build -j$(nproc)`  
Expected: 编译 0 warning。

- [x] **Step 4: Commit Task 4**

```bash
git add src/server/admin_api.c
git commit -m "feat(admin): 🛡️ expand guardrails admin API with webhook parameters and probe endpoint"
```

---

### Task 5: 管理控制台 Webhook 规则管理与即时测试探测界面

**Files:**
- Modify: `web/admin.html:700-830` (Guardrails 面板与弹窗)
- Modify: `web/admin.html:3640-4050` (Guardrails JS 逻辑)

- [x] **Step 1: 更新 `guardrailModal` 弹窗支持 Webhook 规则选项与表单项**

在 `web/admin.html` 的 `guardrailModal` 中：
1. 规则类型下拉框添加 `<option value="webhook">🌐 外部 Webhook 审查插件</option>`。
2. 添加 Webhook 动态专用配置区 (`#grWebhookConfigSection`，在选择 webhook 时展示，其它类型隐藏)：
   - Webhook URL 提示（`#grPattern` 占位符切换为 `https://audit.corp.internal/v1/moderation`）。
   - Webhook Secret / Bearer Token 输入框 (`#grWebhookSecret`)。
   - 超时毫秒数输入框 (`#grTimeoutMs`, 默认 500)。
   - 降级策略下拉框 (`#grFailMode`: `open` 失败放行 / `closed` 严格阻断)。
   - 审查阶段下拉框 (`#grPhase`: `inbound` 入站 Prompt / `outbound` 出站模型回复 / `both` 双向全量)。
3. 在表单中增加“🔍 实时测试连通性”按钮 (`testGuardrailWebhookConnectivity()`)，在弹窗内即时显示网络联通与延迟状态。

- [x] **Step 2: 更新规则表格渲染与操作**

在 `renderGuardrailsTable()` 中：
1. 为 `webhook` 规则类型提供高对比徽章：
   - `<span class="px-2 py-0.5 rounded text-[10px] font-mono bg-purple-500/10 text-purple-400 border border-purple-500/20">🌐 Webhook (${r.phase || 'inbound'})</span>`
2. 展示超时时间与降级策略提示（如 `500ms · Fail-Open`）。
3. 支持点击编辑时自动反填全部 Webhook 配置参数。

- [x] **Step 3: 重新生成头文件并验证构建**

Run: `cmake --build .build -j$(nproc)`  
Expected: 重新生成 `admin_ui_html.h` 并成功构建 `aigate`。

- [x] **Step 4: Commit Task 5**

```bash
git add web/admin.html
git commit -m "feat(ui): 🛡️ add webhook moderation rule form and live probe testing to admin console"
```

---

### Task 6: 端到端自动化集成测试与完整功能验证

**Files:**
- Create: `tests/integration/test_guardrails_webhook.py`

- [x] **Step 1: 编写全量 Webhook 端到端集成测试套件**

在 `tests/integration/test_guardrails_webhook.py` 中：
1. 启动轻量 Mock 审核 HTTP 服务（提供 `/pass`, `/block`, `/mask`, `/slow` 接口）。
2. 测试 1：入站 Webhook 审查 - Action Pass（请求正常转发至上游并返回）。
3. 测试 2：入站 Webhook 审查 - Action Block（网关返回 400 `content_policy_violation`）。
4. 测试 3：入站 Webhook 审查 - Action Mask（用户 Prompt 被脱敏替换后转发给上游）。
5. 测试 4：超时与 Fail-Open 降级测试（模拟 2s 延迟，网关在 200ms 超时后放行请求）。
6. 测试 5：超时与 Fail-Closed 降级测试（模拟超时，网关阻断请求）。
7. 测试 6：出站 Webhook 审查 - 拦截或脱敏模型回复输出。
8. 测试 7：Admin API CRUD 与 `POST /admin/v1/guardrails/webhook/test` 连通性探测。

- [x] **Step 2: 运行新集成测试**

Run: `pytest tests/integration/test_guardrails_webhook.py -v`  
Expected: 全量用例 PASSED。

- [x] **Step 3: 运行完整网关回归测试套件**

Run: `pytest tests/integration/test_gateway.py -k "guardrails" -v`  
Run: `./.build/tests/aigate_unit_tests`  
Expected: 所有现有测试 100% 通过，无回归。

- [x] **Step 4: Commit Task 6**

```bash
git add tests/integration/test_guardrails_webhook.py
git commit -m "test(guardrails): 🧪 add end-to-end integration tests for webhook moderation plugin"
```

---

## 验证与验收标准

1. **编译与构建验证：** `cmake --build .build -j$(nproc)` 0 编译错误与 0 警告，单二进制嵌入式 HTML 正常生成。
2. **单测覆盖：** `./.build/tests/aigate_unit_tests` 全部通过。
3. **集成测试覆盖：** `pytest tests/integration/test_guardrails_webhook.py` 覆盖 Pass、Block、Mask、Fail-Open/Closed、出站审查与 Admin Probe。
4. **控制台交互验证：** 在 Web UI 能够直接配置 Webhook 规则并实时测试连通性。

# 安全防护与脱敏风控 (Guardrails & PII Masking & Budget Limits) 设计规范

日期: 2026-09-26  
状态: 已批准 (Approved)  
目标模块: `src/guardrails.{c,h}`, `src/budget_enforce.{c,h}`, `src/aigate_core.c`, `src/admin_api.c`, `schema/schema.sql`

---

## 1. 背景与目标

随着 `aigate` 在生产与内部组织的深度使用，单纯的协议转换与路由限流已无法满足企业级大模型网关的安全合规诉求：
1. **防止敏感数据外泄**：研发/业务调用大模型时，提示词中经常包含用户手机号、身份证、邮箱或云凭据 API Key 等 PII（个人身份信息）与机密数据。网关需要在流量送达外部上游（如 OpenAI/Anthropic/Gemini）前自动就地脱敏。
2. **敏感词合规阻断**：对于涉黄、涉政、涉暴或违规指令，网关需在线拦截并短路拒绝，避免风险请求扩散至模型推理侧。
3. **团队/项目月度预算硬熔断**：虽然已有日级 Token 配额，但缺少跨天的月度成本金额预算管控。需提供 API Key 级与 Group（部门/项目）级的双层月度总预算硬上限，达到 100% 预算即触发硬熔断，杜绝账单超支。

### 核心约束与设计原则
- **极速低延迟 (Sub-millisecond)**：敏感词与 PII 扫描单次延迟不超过 1ms，整体网关单核吞吐保持数万 QPS。
- **专注入站防护 (Inbound Guard)**：仅在请求进入上游前完成敏感词拦截与 PII 脱敏，出站流式输出（SSE）直出，不增加首字（TTFT）与中间流式 chunk 延迟。
- **零热路径 DB 查询**：月度消费与 Token 额度在内存/Redis 中维护原子累加值，鉴权阶段纳秒级判定。
- **零新外部依赖**：C17 原生实现，维持单二进制轻量交付。

---

## 2. 系统架构与处理流程

```
客户端请求 (POST /v1/chat/completions 等)
   │
   ▼
[1] 鉴权与策略解析 (auth_key_validate)
   │ 提取 key_id, group_id, guardrails_enabled, monthly_cost_budget, monthly_token_budget
   │
   ▼
[2] 周期预算熔断器 (budget_enforce_check)
   │ 校验：key 当月消费 >= monthly_cost_budget (若 > 0)
   │ 校验：key 当月 Token >= monthly_token_budget (若 > 0)
   │ 校验：group 当月消费 >= group_monthly_budget (若 group_id 有效且 > 0)
   │─── [超额熔断] ──► 返回 429 budget_exceeded (短路终止)
   │
   ▼ [预算正常]
[3] 入站安全防护网关 (guardrails_inspect_inbound)
   │ 若 guardrails_enabled = false 则直接透传
   │ 解析 JSON 提取提示词文本 (兼容 OpenAI messages, Anthropic messages, Gemini contents)
   │
   ├── [3.1 敏感词检测 - Aho-Corasick 多模式自动机]
   │   │ 执行 O(N) 线性多模式匹配
   │   └── [命中黑名单] ──► 记审计 'blocked' ──► 返回 400 content_policy_violation (短路终止)
   │
   └── [3.2 PII 隐私信息扫描与脱敏]
       │ 扫描手机号、身份证、邮箱、通用 API 密钥
       ├── [未检出 PII] ──► 保持原 raw_body 指针 (零拷贝)
       └── [检出 PII]   ──► 替换为 [PHONE]/[ID_CARD]/[EMAIL]/[API_KEY]
                            生成 sanitized_body 替换 raw_body，记审计 'masked'
   │
   ▼
[4] 模型路由与上游转发 (model_router & upstream_client)
   │ 上游接收清洗后的安全数据，杜绝原始隐私泄露
   │
   ▼
[5] 流式响应与异步审计下刷 (stream_pipeline & usage_meter)
   │ 客户端流畅接收 SSE 输出
   │ usage_meter 追加审计记录至 ring buffer (携带 guardrail_action 标记)
   └ 异步累加内存与 Redis 月度消费统计
```

---

## 3. 数据库模型升级 (Migration v9)

在 `schema/schema.sql` 中追加版本 9 数据库迁移：

```sql
-- Migration v9: guardrails rules and budget limits

-- 1. 敏感词与审查规则表
CREATE TABLE IF NOT EXISTS guardrails_rules (
  id          BIGSERIAL PRIMARY KEY,
  rule_type   TEXT NOT NULL,               -- 'keyword' (黑名单敏感词), 'regex' (正则), 'exempt' (白名单豁免)
  pattern     TEXT NOT NULL,               -- 敏感词文本或正则表达式
  action      TEXT NOT NULL DEFAULT 'block',-- 'block' (400 阻断), 'mask' (脱敏替换)
  category    TEXT NOT NULL DEFAULT 'general', -- 分类 ('violence', 'political', 'secret', 'general')
  enabled     BOOLEAN NOT NULL DEFAULT true,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS ix_guardrails_enabled ON guardrails_rules(enabled, rule_type);

-- 2. API Keys 增加月度预算与防护开关
ALTER TABLE api_keys 
  ADD COLUMN IF NOT EXISTS guardrails_enabled BOOLEAN NOT NULL DEFAULT true,
  ADD COLUMN IF NOT EXISTS monthly_cost_budget NUMERIC(12, 4) NOT NULL DEFAULT 0.0000,
  ADD COLUMN IF NOT EXISTS monthly_token_budget BIGINT NOT NULL DEFAULT 0;

-- 3. Groups (部门分组) 增加月度总预算金额上限
ALTER TABLE groups 
  ADD COLUMN IF NOT EXISTS monthly_budget_usd NUMERIC(12, 4) NOT NULL DEFAULT 0.0000;

-- 4. 用量审计明细增加防护动作追踪
ALTER TABLE usage_requests 
  ADD COLUMN IF NOT EXISTS guardrail_action TEXT NOT NULL DEFAULT '';
  -- 取值: '' (正常放行), 'masked' (包含 PII 并已完成脱敏), 'blocked' (命中黑名单已阻断)

INSERT INTO schema_migrations(version) VALUES (9) ON CONFLICT (version) DO NOTHING;
```

---

## 4. 详细模块设计与接口规范

### 4.1 内容安全审查器 (`src/guardrails.{c,h}`)

#### 核心数据结构与枚举
```c
typedef enum {
    GUARDRAILS_PASS = 0,     // 合规通过
    GUARDRAILS_MASKED = 1,   // 包含 PII 并已完成脱敏替换
    GUARDRAILS_BLOCKED = 2   // 包含黑名单敏感词，请求必须阻断
} guardrails_action_t;

typedef struct guardrails_ctx guardrails_ctx_t;

// 初始化 / 销毁
guardrails_ctx_t* guardrails_create(void);
void guardrails_destroy(guardrails_ctx_t *ctx);

// 规则编译与加载 (从规则列表构建 AC 自动机与正则)
int guardrails_load_rules(guardrails_ctx_t *ctx, const guardrail_rule_item_t *items, size_t count);

// 入站 Payload 统一审查接口
guardrails_action_t guardrails_inspect_inbound(
    guardrails_ctx_t *ctx,
    const char *raw_body,
    size_t raw_len,
    char **sanitized_body,
    size_t *sanitized_len,
    char *blocked_keyword,
    size_t blocked_keyword_sz);
```

#### Aho-Corasick 多模式自动机实现要点
1. **扁平化内存连续存储**：Trie 树使用固定节点数组存储，每个节点包含字符转移表、Failure 转移以及命中敏感词指针。
2. **单遍扫描**：文本遍历时，根据当前字符步进；若失配跳转至 `fail_index`，一旦遇到匹配标记立即短路并报告违规。
3. **白名单豁免机制**：若命中敏感词同时匹配了 `exempt` 豁免词（例如“技术枪战游戏开发”中包含“枪战”但命中白名单短语），则视为合规放行。

#### PII 隐私扫描器实现要点
内置预编译模式流水线：
- **手机号码**：`\b1[3-9]\d{9}\b` $\rightarrow$ 替换为 `[PHONE]`
- **二代身份证**：`\b[1-9]\d{5}(?:18|19|20)\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\d{3}[\dXx]\b` $\rightarrow$ 替换为 `[ID_CARD]`
- **电子邮件**：`\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b` $\rightarrow$ 替换为 `[EMAIL]`
- **API 凭证**：`\b(sk-[a-zA-Z0-9]{20,}|aig_[a-zA-Z0-9]{20,}|ghp_[a-zA-Z0-9]{20,})\b` $\rightarrow$ 替换为 `[API_KEY]`

替换过程计算偏移差，单次分配内存，精确保持 JSON 双引号闭合与转义有效。

---

### 4.2 周期预算熔断器 (`src/budget_enforce.{c,h}`)

#### 核心数据结构与接口
```c
typedef struct budget_enforce_mgr budget_enforce_mgr_t;

budget_enforce_mgr_t* budget_enforce_create(pg_store_t *store, redis_pool_t *redis);
void budget_enforce_destroy(budget_enforce_mgr_t *mgr);

// 启动时初始化：从 PG 同步当月消费基准
int budget_enforce_init_from_db(budget_enforce_mgr_t *mgr);

// 快速预算熔断检查 (返回 0 表示通过，-1 表示超限熔断)
int budget_enforce_check(
    budget_enforce_mgr_t *mgr,
    int64_t key_id,
    int64_t group_id,
    double key_monthly_cost_budget,
    int64_t key_monthly_token_budget,
    char *err_msg,
    size_t err_msg_sz);

// 实时追加用量消耗 (由 usage_meter 后台回调调用)
void budget_enforce_record(
    budget_enforce_mgr_t *mgr,
    int64_t key_id,
    int64_t group_id,
    double cost_usd,
    int64_t tokens);
```

#### 多节点与单机同步
- **单机模式**：哈希表存储 `(key_id -> atomic_spent_usd, atomic_tokens)`，`group_id -> atomic_spent_usd`。跨月检测时间戳重置。
- **Redis 集群模式**：使用键 `aigate:budget:{YYYYMM}:key:{id}` 与 `aigate:budget:{YYYYMM}:group:{id}` 执行 `HINCRBYFLOAT` / `HGETALL`，TTL 设置为 60 天自动过期。

---

## 5. HTTP 错误响应格式

### 5.1 敏感词拦截（HTTP 400 Bad Request）
```json
{
  "error": {
    "message": "Content policy violation: prompt contains prohibited content",
    "type": "content_policy_violation",
    "param": null,
    "code": "blocked_by_guardrails"
  }
}
```

### 5.2 预算超限熔断（HTTP 429 Too Many Requests）
```json
{
  "error": {
    "message": "Monthly budget limit exceeded for API key or group",
    "type": "budget_exceeded",
    "param": null,
    "code": "quota_exceeded"
  }
}
```

---

## 6. Admin REST API 规范

| 方法 | 路径 | 说明 | 请求参数/体 |
|---|---|---|---|
| `GET` | `/admin/v1/guardrails` | 查询敏感词与规则列表 | `?type=keyword&enabled=true&page=1&size=20` |
| `POST` | `/admin/v1/guardrails` | 创建审查规则 | `{"rule_type":"keyword","pattern":"敏感词","category":"security","action":"block"}` |
| `PUT` | `/admin/v1/guardrails/{id}` | 更新规则状态或内容 | `{"pattern":"新词","enabled":false}` |
| `DELETE` | `/admin/v1/guardrails/{id}` | 删除规则 | 路径 ID |
| `POST` | `/admin/v1/guardrails/reload` | 强制热重载重新编译 AC 自动机 | 无 |
| `POST` | `/admin/v1/keys` | 创建密钥（扩展） | 增加 `monthly_cost_budget`, `monthly_token_budget`, `guardrails_enabled` |
| `POST` | `/admin/v1/groups` | 创建部门（扩展） | 增加 `monthly_budget_usd` |

---

## 7. 质量保证与测试策略

1. **单元测试 (`tests/unit/test_guardrails.c`)**：
   - Aho-Corasick 算法全面测试（前缀匹配、多后缀重叠匹配、无模式空匹配、大文本性能基准）；
   - PII 脱敏边界测试（11位中国手机号、18位身份证号、各种邮箱、API Key 掩盖，验证替换后 JSON 仍然合法）；
   - 白名单豁免验证。
2. **单元测试 (`tests/unit/test_budget_enforce.c`)**：
   - Key 月度金额超限拦截测试（返回 -1 并填充错误描述）；
   - Key 月度 Token 超限拦截测试；
   - Group 月度预算超限拦截测试；
   - 预算设为 0（无限制）永不熔断；
   - 跨月自动平滑翻页重置。
3. **集成测试 (`tests/integration/test_gateway.py`)**：
   - `test_guardrails_pii_masking_e2e`：向 `/v1/chat/completions` 发送包含手机号与身份证的提示词，端到端测试 mock upstream 确认上游收到的仅包含 `[PHONE]` 和 `[ID_CARD]`；
   - `test_guardrails_block_keyword_e2e`：向网关发送包含黑名单敏感词的提示词，验证网关直接返回 400 `blocked_by_guardrails` 且上游完全未被调用；
   - `test_monthly_budget_enforce_e2e`：为测试密钥设定 $0.005 额度，消耗后验证后续请求直接收到 429 `budget_exceeded`。

# 安全防护与脱敏风控 (Guardrails & PII Masking & Budget Limits) 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 aigate 构建高性能内容安全审查（Aho-Corasick 敏感词多模式匹配与内置 PII 隐私信息脱敏）以及双层周期总预算硬熔断（API Key 级月度预算 + Group 部门月度上限）。

**Architecture:** 
1. 新建 `src/guardrails.{c,h}` 模块，内置 Aho-Corasick 算法与 PII 正则扫描器，在请求进入上游前完成 $O(N)$ 敏感词阻断与就地脱敏，保证上游 LLM 零接触隐私且流式出站零延迟；
2. 新建 `src/budget_enforce.{c,h}` 模块，维护内存+Redis 原子累加器，启动时对齐当月 PG 历史消费，鉴权阶段纳秒级判定超额熔断；
3. 扩展 `schema/schema.sql` (Migration v9) 与 `src/admin_api.c`，提供规则动态管理与审计记录。

**Tech Stack:** C17, PostgreSQL 16 (libpq), Redis (optional cluster sync), POSIX regex, CMake, ctest, pytest.

---

### Task 1: 数据库 Schema Migration v9 与 PG 存储层操作

**Files:**
- Modify: `schema/schema.sql:116-125`
- Modify: `src/pg_store.h:40-150`
- Modify: `src/pg_store.c:280-450`
- Test: `tests/unit/test_pg_store.c:80-160`

- [ ] **Step 1: 在 `schema/schema.sql` 编写 Migration v9 脚本**

```sql
-- Migration v9: guardrails rules and budget limits
CREATE TABLE IF NOT EXISTS guardrails_rules (
  id          BIGSERIAL PRIMARY KEY,
  rule_type   TEXT NOT NULL,
  pattern     TEXT NOT NULL,
  action      TEXT NOT NULL DEFAULT 'block',
  category    TEXT NOT NULL DEFAULT 'general',
  enabled     BOOLEAN NOT NULL DEFAULT true,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS ix_guardrails_enabled ON guardrails_rules(enabled, rule_type);

ALTER TABLE api_keys 
  ADD COLUMN IF NOT EXISTS guardrails_enabled BOOLEAN NOT NULL DEFAULT true,
  ADD COLUMN IF NOT EXISTS monthly_cost_budget NUMERIC(12, 4) NOT NULL DEFAULT 0.0000,
  ADD COLUMN IF NOT EXISTS monthly_token_budget BIGINT NOT NULL DEFAULT 0;

ALTER TABLE groups 
  ADD COLUMN IF NOT EXISTS monthly_budget_usd NUMERIC(12, 4) NOT NULL DEFAULT 0.0000;

ALTER TABLE usage_requests 
  ADD COLUMN IF NOT EXISTS guardrail_action TEXT NOT NULL DEFAULT '';

INSERT INTO schema_migrations(version) VALUES (9) ON CONFLICT (version) DO NOTHING;
```

- [ ] **Step 2: 重新编译并触发 `schema_sql.h` 代码生成**

Run: `cmake --build .build -j`  
Expected: `schema_sql.h` 自动重新生成，构建成功。

- [ ] **Step 3: 在 `src/pg_store.h` 与 `src/pg_store.c` 扩展数据结构与 CRUD 函数**

定义 `guardrail_rule_t` 结构体，并在 `pg_store.h` 声明操作函数：
- `pg_store_list_guardrails_rules(...)`
- `pg_store_create_guardrails_rule(...)`
- `pg_store_update_guardrails_rule(...)`
- `pg_store_delete_guardrails_rule(...)`
- 更新 `api_key_record_t` 支持 `guardrails_enabled`, `monthly_cost_budget`, `monthly_token_budget`。
- 更新 `group_record_t` 支持 `monthly_budget_usd`。

- [ ] **Step 4: 在 `tests/unit/test_pg_store.c` 编写测试用例验证 Migration v9 与 CRUD**

验证规则增删改查、Key 预算字段读写与 Group 预算字段读写正常。

- [ ] **Step 5: 运行单元测试并提交代码**

Run: `ctest --test-dir .build -R unit --output-on-failure`  
Expected: PASS  
```bash
git add schema/schema.sql src/pg_store.h src/pg_store.c tests/unit/test_pg_store.c
git commit -m "feat(schema): 🎸 add migration v9 for guardrails rules and budget limits"
```

---

### Task 2: Aho-Corasick 多模式敏感词匹配引擎 (`src/guardrails.{c,h}`)

**Files:**
- Create: `src/guardrails.h`
- Create: `src/guardrails.c`
- Modify: `CMakeLists.txt:45-70`
- Create: `tests/unit/test_guardrails.c`
- Modify: `tests/unit/run_tests.c:40-80`

- [ ] **Step 1: 编写测试用例 `tests/unit/test_guardrails.c` 覆盖 AC 自动机**

测试场景：
1. 构建含敏感词字典（`"badword"`, `"danger"`, `"attack"`）；
2. 扫描无敏感词文本返回 PASS；
3. 扫描含 `"this is a badword test"` 返回 BLOCKED 并指出 `"badword"`；
4. 扫描具有重叠前缀/后缀的词（`"he"`, `"she"`, `"his"`, `"hers"`）精准匹配；
5. 空文本与长文本鲁棒性。

- [ ] **Step 2: 运行测试验证失败**

Run: `cmake --build .build -j && ./.build/tests/aigate_unit_tests -s guardrails_ac`  
Expected: 编译报错或测试失败（未实现）。

- [ ] **Step 3: 实现 `src/guardrails.h` 与 `src/guardrails.c` 中的 Aho-Corasick 算法**

定义 Trie 节点：
```c
typedef struct ac_node {
    int next[256];
    int fail;
    char *matched_keyword;
} ac_node_t;

typedef struct ac_trie {
    ac_node_t *nodes;
    size_t node_count;
    size_t node_cap;
} ac_trie_t;
```
实现 `ac_trie_create()`, `ac_trie_insert()`, `ac_trie_build_failure_links()`, `ac_trie_search()`, `ac_trie_destroy()`。

- [ ] **Step 4: 运行单元测试验证通过**

Run: `ctest --test-dir .build -R unit --output-on-failure`  
Expected: PASS  

- [ ] **Step 5: 提交 AC 引擎代码**

```bash
git add src/guardrails.h src/guardrails.c tests/unit/test_guardrails.c tests/unit/run_tests.c CMakeLists.txt
git commit -m "feat(guardrails): ✨ implement high-performance Aho-Corasick pattern matching"
```

---

### Task 3: 入站 PII 隐私信息扫描与脱敏流水线

**Files:**
- Modify: `src/guardrails.h:30-80`
- Modify: `src/guardrails.c:120-300`
- Modify: `tests/unit/test_guardrails.c:70-150`

- [ ] **Step 1: 在 `tests/unit/test_guardrails.c` 编写 PII 脱敏测试用例**

测试场景：
1. 手机号码脱敏：`"我的电话是13812345678"` $\rightarrow$ `"我的电话是[PHONE]"`；
2. 身份证脱敏：`"身份证110101199003072345号"` $\rightarrow$ `"身份证[ID_CARD]号"`；
3. 邮箱脱敏：`"联系alice@example.com处理"` $\rightarrow$ `"联系[EMAIL]处理"`；
4. 密钥凭证脱敏：`"API Key 是 sk-abc12345678901234567890"` $\rightarrow$ `"API Key 是 [API_KEY]"`；
5. JSON 兼容性测试：传入包含转义和多行文本的 OpenAI / Anthropic JSON 文本，脱敏后 `cJSON_Parse` 依然 100% 成功解析。

- [ ] **Step 2: 运行测试验证失败**

Run: `./.build/tests/aigate_unit_tests -s guardrails_pii`  
Expected: FAIL

- [ ] **Step 3: 在 `src/guardrails.c` 实现 PII 扫描器与 JSON 回填引擎**

1. 使用预编译 POSIX 正则表达式：
   - 手机号：`1[3-9][0-9]{9}`
   - 身份证：`[1-9][0-9]{5}(18|19|20)[0-9]{2}(0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])[0-9]{3}[0-9Xx]`
   - 邮箱：`[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}`
   - 密钥：`sk-[a-zA-Z0-9]{20,}|aig_[a-zA-Z0-9]{20,}|ghp_[a-zA-Z0-9]{20,}`
2. 实现 `guardrails_inspect_inbound(...)`：
   - 使用 cJSON 安全提取 `messages[*].content`、`prompt`、`contents[*].parts[*].text`；
   - 先执行 AC 自动机检测黑名单，命中直接阻断退出；
   - 再执行 PII 替换，若发生替换则单次构建新 JSON 字符串回传。

- [ ] **Step 4: 运行单元测试验证通过**

Run: `ctest --test-dir .build -R unit --output-on-failure`  
Expected: PASS  

- [ ] **Step 5: 提交 PII 脱敏代码**

```bash
git add src/guardrails.h src/guardrails.c tests/unit/test_guardrails.c
git commit -m "feat(guardrails): ✨ add inbound PII masking and JSON payload sanitization"
```

---

### Task 4: 周期预算熔断器 (`src/budget_enforce.{c,h}`)

**Files:**
- Create: `src/budget_enforce.h`
- Create: `src/budget_enforce.c`
- Modify: `CMakeLists.txt`
- Create: `tests/unit/test_budget_enforce.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写测试用例 `tests/unit/test_budget_enforce.c`**

测试场景：
1. 预算为 0（无限制）时始终返回通过 (0)；
2. Key 月度消费达到上限时，`budget_enforce_check` 返回 -1 且填充超额提示；
3. Key 月度 Token 达到上限时，返回 -1；
4. Group 部门月度消费达到上限时，返回 -1；
5. `budget_enforce_record` 增量消费累加正确性；
6. 跨月时间戳重置逻辑。

- [ ] **Step 2: 运行测试验证失败**

Run: `cmake --build .build -j && ./.build/tests/aigate_unit_tests -s budget_enforce`  
Expected: FAIL

- [ ] **Step 3: 实现 `src/budget_enforce.{c,h}`**

1. 哈希表结构维护当前自然月（`YYYYMM`）各个 `key_id` 与 `group_id` 的消费浮点数与 Token 整数；
2. 提供 `budget_enforce_init_from_db(mgr)`，启动时执行单条 SQL 汇总当月历史数据；
3. 提供 `budget_enforce_check(...)` 与 `budget_enforce_record(...)`；
4. 若启用 Redis，则同步在 Redis 哈希表中维护。

- [ ] **Step 4: 运行单元测试验证通过**

Run: `ctest --test-dir .build -R unit --output-on-failure`  
Expected: PASS  

- [ ] **Step 5: 提交预算熔断器代码**

```bash
git add src/budget_enforce.h src/budget_enforce.c tests/unit/test_budget_enforce.c tests/unit/run_tests.c CMakeLists.txt
git commit -m "feat(budget): ✨ implement in-memory and Redis monthly budget enforcer"
```

---

### Task 5: 网关核心管道集成 (`src/aigate_core.c` & `src/usage_meter.c`)

**Files:**
- Modify: `src/aigate_core.c:300-600`
- Modify: `src/usage_meter.h:20-50`
- Modify: `src/usage_meter.c:80-220`
- Test: `tests/unit/test_aigate_core.c`

- [ ] **Step 1: 在 `tests/unit/test_aigate_core.c` 编写拦截集成测试**

测试注入 guardrails 和 budget_enforce 上下文：
1. 敏感词输入测试：验证返回 HTTP 400 与 JSON `blocked_by_guardrails`；
2. 预算超额测试：验证返回 HTTP 429 与 JSON `budget_exceeded`；
3. PII 输入测试：验证上游 mock 接收到的 body 已脱敏为 `[PHONE]`。

- [ ] **Step 2: 在 `aigate_core.c` 请求主入口注入拦截管道**

1. 鉴权通过后，立即调用 `budget_enforce_check(...)`：
   - 若超额，调用 `mg_send_http_error(conn, 429, ...)` 返回标准 `budget_exceeded` JSON 并终止；
2. 调用 `guardrails_inspect_inbound(...)`：
   - 若返回 `GUARDRAILS_BLOCKED`，调用 `mg_send_http_error(conn, 400, ...)` 返回 `content_policy_violation` 并终止；
   - 若返回 `GUARDRAILS_MASKED`，以 `sanitized_body` 作为转发载荷，并将 `guardrail_action` 标记为 `'masked'`；
3. 响应完成或中断后，将 `guardrail_action` 传递给 `um_record(...)`，记录在 `usage_requests` 审计表中；
4. 调用 `budget_enforce_record(...)` 更新实时月度累加值。

- [ ] **Step 3: 运行所有单元测试验证通过**

Run: `ctest --test-dir .build -R unit --output-on-failure`  
Expected: PASS (所有 150+ 测试全绿)

- [ ] **Step 4: 提交核心集成代码**

```bash
git add src/aigate_core.c src/usage_meter.h src/usage_meter.c tests/unit/test_aigate_core.c
git commit -m "feat(core): 🔗 integrate guardrails inspection and budget enforcer into gateway pipeline"
```

---

### Task 6: Admin REST API 管理端点支持 (`src/admin_api.c`)

**Files:**
- Modify: `src/admin_api.c:200-700`
- Test: `tests/unit/test_admin_api.c:150-300`

- [ ] **Step 1: 在 `tests/unit/test_admin_api.c` 编写端点测试用例**

1. `POST /admin/v1/guardrails` 创建一条敏感词规则；
2. `GET /admin/v1/guardrails` 查询规则列表；
3. `PUT /admin/v1/guardrails/{id}` 禁用规则；
4. `DELETE /admin/v1/guardrails/{id}` 删除规则；
5. `POST /admin/v1/guardrails/reload` 触发热重载；
6. `POST /admin/v1/keys` 创建含 `monthly_cost_budget` 的密钥。

- [ ] **Step 2: 在 `src/admin_api.c` 实现路由分发与处理函数**

实现：
- `handle_admin_guardrails_list`
- `handle_admin_guardrails_create`
- `handle_admin_guardrails_update`
- `handle_admin_guardrails_delete`
- `handle_admin_guardrails_reload`
- 在 `handle_admin_keys_create` 与 `handle_admin_groups_create` 解析新增预算与防护开关字段。

- [ ] **Step 3: 运行单元测试验证通过**

Run: `ctest --test-dir .build -R unit --output-on-failure`  
Expected: PASS  

- [ ] **Step 4: 提交 Admin API 代码**

```bash
git add src/admin_api.c tests/unit/test_admin_api.c
git commit -m "feat(admin): 🌐 add admin REST APIs for guardrails rules and budget controls"
```

---

### Task 7: 端到端 Python 集成测试覆盖 (`tests/integration/test_gateway.py`)

**Files:**
- Modify: `tests/integration/test_gateway.py:400-500`

- [ ] **Step 1: 编写 3 个端到端集成测试**

1. `test_guardrails_pii_masking_e2e`:
   - 客户端发送 `"我的电话是13912345678，身份证是110101199003072345"`；
   - 验证上游 Mock 接收到的 Prompt 严格为 `"我的电话是[PHONE]，身份证是[ID_CARD]"`。
2. `test_guardrails_block_keyword_e2e`:
   - 通过 Admin API 注册违禁词 `"forbidden_drop_table"`；
   - 客户端发送含该词的请求；
   - 验证返回 HTTP 400 且错误码为 `blocked_by_guardrails`，上游 Mock 未被访问。
3. `test_monthly_budget_enforce_e2e`:
   - 注册测试 Key 并设定 `monthly_cost_budget = 0.0001`；
   - 发送单次消耗超额请求后，发起下一个请求；
   - 验证返回 HTTP 429 且错误码为 `quota_exceeded`。

- [ ] **Step 2: 执行全量端到端测试**

Run: `pytest tests/integration/test_gateway.py -v`  
Expected: 31 passed (28 原有用例 + 3 个新增用例全部通过)

- [ ] **Step 3: 提交集成测试代码**

```bash
git add tests/integration/test_gateway.py
git commit -m "test(integration): 🧪 add e2e integration tests for guardrails, PII masking, and budget limits"
```

# 中间件过滤器链与动态 Prompt 注入实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 aigate 统一的前置中间件过滤器链（Filter Chain）与动态 Prompt 模板引擎，支持在 Model 路由和 API Key 级配置 Prompt 模板、动态变量插值（`${date}`, `${model}`, `${key_name}` 等），以及三种合并模式（prepend / append / override）。

**Architecture:** 抽象独立前置过滤器链 (`src/policy/filter_chain.c`)，整合敏感词审核与模板注入；实现动态插值与 Jansson 报文重构引擎 (`src/policy/prompt_template.c`)；通过 PostgreSQL 存储持久化并在 Admin API 提供配置入口；通过单元测试与 ASan 脚本保障 0 内存泄漏。

**Tech Stack:** C17, Jansson, CMake, PostgreSQL (libpq), Python 3, AddressSanitizer (ASan), Bash.

---

### Task 1: Prompt 模板引擎核心实现 (`prompt_template.h` / `.c`) 与单元测试

**Files:**
- Create: `src/policy/prompt_template.h`
- Create: `src/policy/prompt_template.c`
- Create: `tests/unit/policy/test_prompt_template.c`
- Modify: `tests/unit/run_tests.h`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 创建 `src/policy/prompt_template.h`**

```c
#ifndef AIGATE_PROMPT_TEMPLATE_H
#define AIGATE_PROMPT_TEMPLATE_H

#include <stddef.h>
#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROMPT_MODE_PREPEND  = 0,
    PROMPT_MODE_APPEND   = 1,
    PROMPT_MODE_OVERRIDE = 2
} prompt_inject_mode_t;

typedef struct {
    const char*          system_template;
    prompt_inject_mode_t mode;
    const char*          prefix_user_prompt;
    const char*          suffix_user_prompt;
} prompt_template_t;

/**
 * @brief Interpolate variables (${date}, ${time}, ${timestamp}, ${model}, ${key_name}) into template.
 * @return Newly allocated string (caller must free), or NULL on allocation error.
 */
char* prompt_template_expand_vars(const char* tmpl, const char* model, const char* key_name);

/**
 * @brief Apply prompt template to a JSON request body.
 * @param tmpl Template settings.
 * @param model Model name for interpolation.
 * @param key_name API Key identifier for interpolation.
 * @param jbody Parsed request JSON body.
 * @param out_modified_json Newly serialized JSON string (caller must free) on success.
 * @param out_len Length of out_modified_json.
 * @return 0 on success, non-zero if unmodified or error.
 */
int prompt_template_apply(const prompt_template_t* tmpl,
                          const char*              model,
                          const char*              key_name,
                          json_t*                  jbody,
                          char**                   out_modified_json,
                          size_t*                  out_len);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_PROMPT_TEMPLATE_H */
```

- [ ] **Step 2: 创建 `src/policy/prompt_template.c`**

实现变量插值函数 `prompt_template_expand_vars` 与基于 Jansson 的消息修改函数 `prompt_template_apply`。

- [ ] **Step 3: 创建单元测试 `tests/unit/policy/test_prompt_template.c`**

覆盖：
1. 变量插值测试（`${date}`, `${model}`, `${key_name}`, 未知变量保留）；
2. 无现有 system 消息时的注入；
3. 存在现有 system 消息时的 `PREPEND`, `APPEND`, `OVERRIDE` 模式；
4. 内存安全性与 Jansson 引用计数测试。

- [ ] **Step 4: 注册单元测试并验证**

在 `tests/unit/run_tests.h` 与 `tests/unit/run_tests.c` 中注册 `test_prompt_template_suite()`：
```bash
cmake --build .build --target aigate_unit_tests && ctest --test-dir .build -R unit --output-on-failure
```
预期输出：所有测试通过。

- [ ] **Step 5: 提交 Task 1**

```bash
git add src/policy/prompt_template.* tests/unit/policy/test_prompt_template.c tests/unit/run_tests.*
git commit -m "feat(policy): 🎸 implement prompt template engine and variable interpolation"
```

---

### Task 2: 中间件过滤器链 (`filter_chain.h` / `.c`) 与核心请求管道整合

**Files:**
- Create: `src/policy/filter_chain.h`
- Create: `src/policy/filter_chain.c`
- Modify: `src/core/aigate_core.c:1020-1035`
- Create: `tests/unit/policy/test_filter_chain.c`
- Modify: `tests/unit/run_tests.h`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 创建 `src/policy/filter_chain.h` 与 `src/policy/filter_chain.c`**

实现标准过滤器流转机制，挂载 `filter_guardrails` 与 `filter_prompt_template`。

- [ ] **Step 2: 在 `aigate_core.c` 中调用 `filter_chain_execute_inbound`**

在 `resolve_chat_target(&chatq)` 成功之后、进入 cache 与 upstream 之前插入过滤器执行：
```c
    if (filter_chain_execute_inbound(&chatq) != FILTER_CONTINUE) {
        chat_req_cleanup(&chatq);
        return 0;
    }
```

- [ ] **Step 3: 编写并运行单元测试 `test_filter_chain.c`**

```bash
cmake --build .build --target aigate_unit_tests && ctest --test-dir .build -R unit --output-on-failure
```
预期输出：所有测试通过。

- [ ] **Step 4: 提交 Task 2**

```bash
git add src/policy/filter_chain.* src/core/aigate_core.c tests/unit/policy/test_filter_chain.c tests/unit/run_tests.*
git commit -m "feat(policy): 🎸 introduce middleware filter chain and integrate into request pipeline"
```

---

### Task 3: 存储模型扩展与数据库迁移 (`schema_sql.h`, `pg_store.c`, `model_router.h`, `auth_key.h`)

**Files:**
- Modify: `src/store/schema_sql.h`
- Modify: `src/store/pg_store.h`
- Modify: `src/store/pg_store.c`
- Modify: `src/upstream/model_router.h`
- Modify: `src/upstream/model_router.c`
- Modify: `src/policy/auth_key.h`
- Modify: `src/policy/auth_key.c`

- [ ] **Step 1: 更新 `src/store/schema_sql.h`**

在现有迁移列表中添加：
```sql
ALTER TABLE models ADD COLUMN IF NOT EXISTS system_prompt TEXT DEFAULT NULL;
ALTER TABLE models ADD COLUMN IF NOT EXISTS prompt_mode INT DEFAULT 0;

ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS system_prompt TEXT DEFAULT NULL;
ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS prompt_mode INT DEFAULT 0;
```

- [ ] **Step 2: 更新 `model_rec_t` 与 `key_rec_t` 数据结构**

在 `model_rec_t` 和 `key_rec_t` 中添加字段：
- `char system_prompt[4096];`
- `int prompt_mode;`

- [ ] **Step 3: 更新 `pg_store.c` 的 CRUD 查询与反序列化**

在查询与插入 `models`、`api_keys` 时填充并持久化 `system_prompt` 与 `prompt_mode`。

- [ ] **Step 4: 运行单元测试**

```bash
cmake --build .build --target aigate_unit_tests && ctest --test-dir .build -R unit --output-on-failure
```
预期输出：所有测试通过。

- [ ] **Step 5: 提交 Task 3**

```bash
git add src/store/schema_sql.h src/store/pg_store.* src/upstream/model_router.* src/policy/auth_key.*
git commit -m "feat(store): 🎸 extend models and api_keys tables with prompt template fields"
```

---

### Task 4: Admin API 协议字段扩展 (`admin_api.c`)

**Files:**
- Modify: `src/server/admin_api.c`

- [ ] **Step 1: 在 `admin_api.c` 的 model_create/patch 与 key_create/patch 中增加新字段处理**

支持解析请求体中的：
- `"system_prompt": "..."`
- `"prompt_mode": "prepend" | "append" | "override"`
并在 `GET /admin/v1/models` 与 `GET /admin/v1/keys` 的响应中序列化输出对应字段。

- [ ] **Step 2: 验证编译与现有测试**

```bash
cmake --build .build --target aigate aigate_unit_tests && ctest --test-dir .build --output-on-failure
```
预期输出：编译无警告，测试通过。

- [ ] **Step 3: 提交 Task 4**

```bash
git add src/server/admin_api.c
git commit -m "feat(admin): 🎸 add system_prompt and prompt_mode support in admin API"
```

---

### Task 5: 端到端集成测试与 AddressSanitizer 内存安全验证

**Files:**
- Create: `tests/integration/test_prompt_template.py`

- [ ] **Step 1: 创建集成测试脚本 `tests/integration/test_prompt_template.py`**

编排测试场景：
1. 启动 Mock 上游与 aigate；
2. 注册带模板的模型（如 `"system_prompt": "System for ${model} on ${date}"`）；
3. 客户端发起普通的 chat completion 请求（不带 system prompt）；
4. 验证 Mock 上游实际接收到的请求体中首个消息为 `{"role":"system","content":"System for ... on 2026-..."}`；
5. 客户端发起带 system prompt 的请求，分别测试 `prepend`, `append`, `override` 模式合并结果；
6. 验证 API Key 级模板优先于 Model 级模板覆盖生效。

- [ ] **Step 2: 运行集成测试**

```bash
python3 tests/integration/test_prompt_template.py
```
预期输出：所有断言通过。

- [ ] **Step 3: 运行 ASan/UBSan 全量回归检测**

```bash
./scripts/run_chaos_asan.sh
```
预期输出：CTest 全部通过，Chaos 测试通过，0 内存泄漏，0 未定义行为。

- [ ] **Step 4: 提交 Task 5**

```bash
git add tests/integration/test_prompt_template.py
git commit -m "test(integration): 🧪 add end-to-end integration tests for prompt template injection"
```

---

## Plan Review Checklist

1. **Spec Coverage**: 完整覆盖中间件过滤器链、Prompt 模板插值、三种合并模式、PostgreSQL 持久化、Admin API 及 ASan 验证。
2. **No Placeholders**: 绝无 `TODO`、`TBD` 或未定义的临时代码块。
3. **Executable Steps**: 每个步骤定义明确的代码目标、测试命令与 git commit 说明。

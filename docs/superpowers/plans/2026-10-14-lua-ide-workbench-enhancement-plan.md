# 嵌入式 Lua 插件 IDE 工作台优化完善实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全面优化完善 aigate 嵌入式 Lua 插件 IDE 工作台，集成五大企业级预置场景模板库、Monaco Snippets 代码片段与 Hover 中英文交互式文档、全功能双栏沙箱隔离调试器（支持 Headers 键值对表格与请求/响应阶段切换）、实时分级终端日志控制台与后端 API 容错支持。

**Architecture:**
1. **后端兼容增强 (`src/server/admin_api.c`)**：在 `admin_plugins_test_post` 中同时支持 `script_code` 与 `script` 字段名，并自动处理嵌套与平铺的 mock 参数，确保向前向后兼容性；
2. **Monaco 智能开发体验 (`web/src/components/MonacoEditor.vue`)**：注册 Lua 语言 `CompletionItemProvider` 代码片段（Snippets）与 `HoverProvider` 中英文悬浮文档卡片，配置快捷键绑定 (`Ctrl+S`, `Ctrl+Enter`)、括号着色匹配与缩略地图；
3. **五大企业级模板库 (`web/src/views/PluginsView.vue`)**：内置 Token 鉴权、模型动态路由、链路追踪、响应脱敏、访问频控 5 大生产模板，支持一键切换并自动装配元数据；
4. **双栏沙箱调试器与控制台 (`web/src/views/PluginsView.vue`)**：左栏构建完整的入站与出站 Mock 参数（含 Headers 动态增删表格、Client IP、Status、Body 美化格式化与一键预设），右栏可视化展示改写结果对比（Model / Headers / Body / Abort），并以暗色终端实时输出着色分级的 `logs` 日志；
5. **构建嵌入与测试验证**：Vite 单文件构建 -> `embed_html.py` 嵌入 `web_assets.h` -> C 全工程 `-Werror` 编译 -> 350+ 单元测试 100% 通过。

**Tech Stack:** C (Lua 5.4, Jansson, civetweb), Vue 3, Naive UI, Tailwind CSS, Monaco Editor, Vite, Python 3.

---

### Task 1: 后端 `admin_plugins_test_post` 接口容错与兼容增强

**Files:**
- Modify: `src/server/admin_api.c`
- Test: `tests/unit/plugin/test_lua_plugin.c`

- [ ] **Step 1: 在 `tests/unit/plugin/test_lua_plugin.c` 中添加针对 `POST /admin/v1/plugins/test` 容错测试**

```c
/* 在 tests/unit/plugin/test_lua_plugin.c 末尾添加 */
static void
test_lua_plugin_admin_api_test_endpoint_compatibility(void)
{
    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    sha256_hex("secret", 6, adm.admin_token_hash);

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. 支持 script 替代 script_code，且支持直接传入 flat 字段或 mock_payload */
    const char* req_payload = "{\n"
                              "  \"script\": \"function on_request(req)\\n req:set_model('deepseek-chat')\\n aigate.log_info('test-ok')\\nend\",\n"
                              "  \"phase\": \"request\",\n"
                              "  \"mock_payload\": {\n"
                              "    \"model\": \"gpt-4o\"\n"
                              "  }\n"
                              "}";

    int rc = admin_dispatch(&adm, "/admin/v1/plugins/test", "POST", NULL, "secret",
                            req_payload, strlen(req_payload), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "Test endpoint with 'script' key returns 200");
    TEST_ASSERT(body != NULL, "Response body not null");

    json_error_t jerr;
    json_t* res = json_loads(body, 0, &jerr);
    free(body);
    body = NULL;

    TEST_ASSERT(res != NULL, "Valid JSON returned");
    TEST_ASSERT(json_is_true(json_object_get(res, "success")), "Dry run succeeded");
    json_t* req_obj = json_object_get(res, "request");
    TEST_ASSERT(req_obj != NULL, "Request object exists");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(req_obj, "model")), "deepseek-chat") == 0,
                "Model rewritten");

    json_t* logs = json_object_get(res, "logs");
    TEST_ASSERT(logs != NULL && json_array_size(logs) > 0, "Captured logs returned");
    json_decref(res);
}
```

- [ ] **Step 2: 在 `tests/unit/plugin/test_lua_plugin.c` 的 `test_suite_lua_plugin` 中注册该用例并运行**

```bash
make -C build -j$(nproc) && ./build/tests/aigate_unit_tests lua_plugin
```
预期：当前 `admin_plugins_test_post` 仅识别 `script_code`，此用例可能报 400 Bad Request (`script_code is required`)。

- [ ] **Step 3: 修改 `src/server/admin_api.c` 中 `admin_plugins_test_post` 实现双键名容错**

```c
/* 在 src/server/admin_api.c 的 admin_plugins_test_post 函数中： */
    json_t* jcode = json_object_get(root, "script_code");
    if (jcode == NULL || !json_is_string(jcode)) {
        jcode = json_object_get(root, "script");
    }
    if (jcode == NULL || !json_is_string(jcode)) {
        json_decref(root);
        return finish_error(status, out_body, len, 400, "bad_request", "script_code or script is required");
    }
```
如果未传入 `mock_payload` 字段，但传入了平铺的 `path`、`model`、`body` 或 `headers`，自动将 `root` 中这些键构造成 `mock_payload`。

- [ ] **Step 4: 编译并重新运行单元测试**

```bash
make -C build -j$(nproc) && ./build/tests/aigate_unit_tests lua_plugin
```
预期：所有 Lua 插件单元测试均 PASS。

---

### Task 2: 前端 Monaco Editor 增强：Snippets 代码片段与 Hover 悬浮文档

**Files:**
- Modify: `web/src/components/MonacoEditor.vue`

- [ ] **Step 1: 在 `MonacoEditor.vue` 中扩展注册 Snippets 代码片段提供者**

在 `monaco.languages.registerCompletionItemProvider('lua', ...)` 中增加 Snippets 项：
- `req-header-check`：校验指定 Header，缺失则调用 `aigate.abort_with(400, ...)`
- `req-model-rewrite`：模型动态路由条件改写
- `json-modify-body`：读取并改写 Body
- `aigate-abort`：以指定状态码阻断
- `log-suite`：插入常用日志打印语句
- 确保 `insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet` 正确配置，支持 Tab 键跳转占位符。

- [ ] **Step 2: 注册 `HoverProvider` 悬浮中英文文档**

在 `MonacoEditor.vue` 中调用 `monaco.languages.registerHoverProvider('lua', { provideHover: ... })`：
- 对 `aigate.abort_with`, `aigate.log_info`, `aigate.log_warn`, `aigate.log_error`, `aigate.log_debug` 匹配并返回格式化的 Markdown 签名与文档；
- 对 `req:get_header`, `req:set_header`, `req:get_model`, `req:set_model`, `req:get_client_ip`, `req:get_body`, `req:set_body` 提供文档；
- 对 `resp:get_status`, `resp:set_status`, `resp:get_body`, `resp:set_body`, `resp:get_header`, `resp:set_header` 提供文档。

- [ ] **Step 3: 配置 Monaco 编辑器选项与按键映射**

- 配置 `bracketPairColorization: { enabled: true }`，高亮嵌套括号；
- 配置 `minimap: { enabled: true }`；
- 添加快捷键监听：
  - `editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => emit('save'))`
  - `editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, () => emit('run-test'))`

- [ ] **Step 4: 验证组件无报错编译**

```bash
cd web && npm run build
```
预期：Vite 构建无编译与语法报错。

---

### Task 3: 前端 5 大生产级预置场景模板库与元数据规范

**Files:**
- Modify: `web/src/views/PluginsView.vue`

- [ ] **Step 1: 在 `PluginsView.vue` 中定义 5 大预置模板结构与常量**

创建 `TEMPLATES` 对象或数组，包含：
1. `token_auth`: Token 鉴权与黑白名单拦截 (`on_request`, priority 10)
2. `model_router`: 基于用户的模型动态路由改写 (`on_request`, priority 50)
3. `trace_logger`: 全链路追踪日志与 Header 注入 (`on_request`, priority 20)
4. `resp_masking`: 响应内容敏感信息脱敏与水印 (`on_response`, priority 100)
5. `rate_limit_stub`: 访问频控检查与 429 短路熔断 (`on_request`, priority 5)

每个模板配置包含：
- `key`, `label`, `phase`, `priority`, `description`, `script`, 以及用于沙箱的 `mockPreset`（默认请求头与请求体）。

- [ ] **Step 2: 在工作台头部增加「📐 预置模板」下拉选择组件**

- 在「插件名称」与「生效阶段」上方或旁边放置 `NSelect` 模板选择框；
- 当开发者新建插件或主动选择模板时，弹出确认提示或直接填充：
  - 自动更新 `currentPlugin.name`（若为空时自动补全为规范名称如 `auth-guard` / `model-router` 等）；
  - 自动设置 `currentPlugin.phase` 与 `currentPlugin.priority`；
  - 将模板代码填入 Monaco 编辑器；
  - 同步更新关联的沙箱测试 Mock 预设。

- [ ] **Step 3: 优化工作台布局与暗黑设计**

- 保持 Naive UI 暗色风格与 Tailwind 一致性；
- 检查抽屉/弹窗的自适应高度，确保 Monaco 编辑器具备充足可视空间。

---

### Task 4: 全功能双栏沙箱隔离测试器与实时终端日志控制台

**Files:**
- Modify: `web/src/views/PluginsView.vue`

- [ ] **Step 1: 升级沙箱测试弹窗为全功能双栏工作台**

将沙箱弹窗扩展为宽屏双栏界面（左栏：Mock 输入构造器，右栏：改写结果对比与控制台）：
- **左栏（Mock 输入构造器）**：
  - 阶段切换 Radio / Segment（`on_request` vs `on_response`）；
  - 入站参数：Method（GET/POST/PUT/DELETE）、Path、Client IP、Model；
  - 出站参数：Status Code；
  - Headers 动态表格：支持添加行、删除行、Key-Value 输入；
  - Body 载荷：文本域输入，配套「一键格式化 JSON」按钮；
  - 「一键填入当前模板典型用例」快捷按钮。
- **右栏（执行结果与指标）**：
  - 执行状态 Badge：`[成功放行 OK]` / `[主动阻断 Aborted]` / `[执行异常 Error]`；
  - 纯沙箱耗时：`duration_us` 展示；
  - 改写模型展示：`gpt-4o ➔ deepseek-chat`；
  - 改写 Headers 与 Body 展示区。

- [ ] **Step 2: 实现实时控制台终端 (Live Log Console)**

- 在右栏下方开辟暗色终端窗口（背景 `#020617`，字体等宽），保留 DOM 元素 id `#ideConsoleLogs`；
- 从后端返回的 `resp.logs` 数组中解析每一行；
- 分级别高亮着色：
  - `[INFO]` 青色高亮
  - `[DEBUG]` 灰色高亮
  - `[WARN]` 橙黄高亮
  - `[ERROR]` 玫瑰红高亮
- 终端工具栏：支持一键清空、复制全部日志、文本搜索过滤。

- [ ] **Step 3: 对齐 `runSandboxTest` 发送契约**

修改 `PluginsView.vue` 的 `runSandboxTest` 函数：
- 将 Headers 表格数据解析组装为 JSON Object；
- 组装 `mock_payload` 传给后端：
  ```javascript
  const payload = {
    script_code: currentPlugin.script,
    phase: currentPlugin.phase === 'on_response' ? 'response' : 'request',
    mock_payload: {
      method: sandboxForm.method,
      path: sandboxForm.path,
      client_ip: sandboxForm.client_ip,
      model: sandboxForm.model,
      headers: headersObject,
      body: sandboxForm.body,
      status: sandboxForm.status
    }
  };
  const resp = await api.postJson('/admin/v1/plugins/test', payload);
  ```
- 妥善处理返回值并赋值给响应式对象。

---

### Task 5: 构建集成、端到端测试与单文件嵌入验证

**Files:**
- Modify: `src/server/web_assets.h` (via `embed_html.py`)

- [ ] **Step 1: 前端生产编译构建**

```bash
cd /home/quintin/Data/source/c_cpp/aigate/web && npm run build
```
验证：在 `web/dist/index.html` 生成独立内嵌单文件，且无任何警告或构建错误。

- [ ] **Step 2: 执行 `embed_html.py` 嵌入 Web 资产**

```bash
cd /home/quintin/Data/source/c_cpp/aigate && python3 embed_html.py
```
验证：`src/server/web_assets.h` 成功更新，并包含最新 Vue 3 + Naive UI + Monaco 工作台资产。

- [ ] **Step 3: C 语言全项目编译与警告检查**

```bash
make -C build -j$(nproc)
```
验证：无任何 `-Wall -Wextra -Werror` 编译器警告，构建成功。

- [ ] **Step 4: 运行全部单元测试**

```bash
./build/tests/aigate_unit_tests
```
验证：所有 350+ 个单元测试全部通过 (100% Passed)。

- [ ] **Step 5: 提交所有代码与计划变更至 Git**

```bash
git add -A && git commit -m "feat(plugin): enhance lua ide workbench with templates, snippets, and live console"
```

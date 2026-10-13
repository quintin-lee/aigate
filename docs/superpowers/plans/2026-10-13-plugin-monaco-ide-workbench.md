# Plugin Monaco IDE Workbench & Sandbox Diff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建集成 Monaco Editor 的沉浸式双栏插件 IDE 工作台，具备 Lua 5.4 / `aigate` 全局 API 智能提示补全、500ms 防抖实时语法检测与红线定位、原生双屏并列 Diff 差异对比器以及离线断网优雅降级。

**Architecture:** 
1. 后端在 `src/server/admin_api.c` 提供轻量微秒级语法预检接口 `POST /admin/v1/plugins/validate`，解析错误行号并结构化返回；
2. 前端在 `web/admin.html` 引入双栏分屏工作台（左侧 60% Monaco Editor，右侧 40% Mock 配置 + Monaco Diff 对比器 + Console 控制台）；
3. 注册 `aigate` 原生函数与 `req`/`resp` 对象智能补全，配置防抖请求打标 Error Markers，并实现 3 秒超时离线回退机制。

**Tech Stack:** C (Lua 5.4, Jansson, civetweb), HTML5/JavaScript (Monaco Editor 0.45.0, CSS Grid/Flexbox), CMake, AddressSanitizer/UBSan.

---

### Task 1: 后端轻量语法校验端点 `POST /admin/v1/plugins/validate`

**Files:**
- Modify: `src/server/admin_api.c`
- Test: `tests/unit/plugin/test_lua_plugin.c`

- [ ] **Step 1: 在 `tests/unit/plugin/test_lua_plugin.c` 中添加对 `POST /admin/v1/plugins/validate` 的单元测试**

```c
/* 在 test_lua_plugin.c 的 test_lua_plugin_admin_api_crud 末尾或新增测试用例 */
static void
test_lua_plugin_validate_api(void)
{
    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    sha256_hex("secret", 6, adm.admin_token_hash);

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. Valid syntax */
    const char* valid_code = "{\"script_code\":\"function on_request(req)\\n  req:set_header('x-t', '1')\\nend\"}";
    int rc = admin_dispatch(&adm, "/admin/v1/plugins/validate", "POST", NULL, "secret", valid_code, strlen(valid_code), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "Validate valid syntax returns 200");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL && json_is_true(json_object_get(res, "valid")), "Valid is true");
    json_decref(res);

    /* 2. Syntax error on line 2 */
    const char* bad_code = "{\"script_code\":\"function on_request(req)\\n  broken syntax here !!!\\nend\"}";
    rc = admin_dispatch(&adm, "/admin/v1/plugins/validate", "POST", NULL, "secret", bad_code, strlen(bad_code), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "Validate bad syntax returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL && json_is_false(json_object_get(res, "valid")), "Valid is false");
    TEST_ASSERT(json_integer_value(json_object_get(res, "line")) == 2, "Line is 2");
    TEST_ASSERT(json_string_value(json_object_get(res, "message")) != NULL, "Message present");
    json_decref(res);

    /* 3. Missing script_code -> 400 bad_request */
    const char* empty_code = "{}";
    rc = admin_dispatch(&adm, "/admin/v1/plugins/validate", "POST", NULL, "secret", empty_code, strlen(empty_code), &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 400, "Missing script_code returns 400");
    free(body);
}
```

- [ ] **Step 2: 运行测试验证失败**

```bash
make -C build -j8 && ./build/tests/aigate_unit_tests lua_plugin
```
预期：FAIL (no such admin endpoint 404).

- [ ] **Step 3: 在 `src/server/admin_api.c` 中实现 `admin_plugins_validate_post` 及路由分发**

```c
static int
admin_plugins_validate_post(admin_ctx_t* adm,
                            const void*  req_body,
                            size_t       req_len,
                            int*         status,
                            char**       out_body,
                            size_t*      len)
{
    (void)adm;
    json_error_t jerr;
    json_t*      root = json_loadb((const char*)req_body, req_len, 0, &jerr);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return finish_error(status, out_body, len, 400, "bad_request", "invalid json body");
    }

    json_t* jcode = json_object_get(root, "script_code");
    if (jcode == NULL || !json_is_string(jcode)) {
        json_decref(root);
        return finish_error(status, out_body, len, 400, "bad_request", "script_code is required");
    }

    const char* script_code = json_string_value(jcode);
    char        syn_err[256] = {0};
    int         valid_rc = lua_plugin_validate_syntax(script_code, syn_err, sizeof(syn_err));
    json_decref(root);

    json_t* resp = json_object();
    if (valid_rc == 0) {
        json_object_set_new(resp, "valid", json_true());
    } else {
        json_object_set_new(resp, "valid", json_false());
        json_object_set_new(resp, "raw_error", json_string(syn_err));

        /* Extract line number: [string "..."]:line: message */
        int line = 1;
        const char* p = strchr(syn_err, ':');
        if (p != NULL) {
            int parsed_line = atoi(p + 1);
            if (parsed_line > 0) {
                line = parsed_line;
            }
            const char* msg_p = strchr(p + 1, ':');
            if (msg_p != NULL) {
                while (*(msg_p + 1) == ' ') msg_p++;
                json_object_set_new(resp, "message", json_string(msg_p + 1));
            } else {
                json_object_set_new(resp, "message", json_string(syn_err));
            }
        } else {
            json_object_set_new(resp, "message", json_string(syn_err));
        }
        json_object_set_new(resp, "line", json_integer(line));
        json_object_set_new(resp, "column", json_integer(1));
    }

    return finish_json(status, out_body, len, 200, resp);
}
```
并在 `admin_dispatch()` 的 `strncmp(rest, "plugins", 7) == 0` 块中加入：
```c
        if (strcmp(rest, "plugins/validate") == 0 && strcmp(method, "POST") == 0) {
            return admin_plugins_validate_post(adm, body, body_len, out_status, out_body, out_len);
        }
```

- [ ] **Step 4: 编译并运行单元测试验证通过**

```bash
make -C build -j8 && ./build/tests/aigate_unit_tests lua_plugin
```
预期：PASS: 7/7 test(s), 0 failure(s).

- [ ] **Step 5: 提交代码**

```bash
git add src/server/admin_api.c tests/unit/plugin/test_lua_plugin.c tests/unit/run_tests.c
git commit -m "feat(plugin): 🔍 add lightweight syntax validation endpoint POST /admin/v1/plugins/validate"
```

---

### Task 2: Web Admin 沉浸式双栏 IDE 布局与响应式结构重构

**Files:**
- Modify: `web/admin.html`

- [ ] **Step 1: 在 `web/admin.html` 中重构插件抽屉为全宽沉浸式双栏 IDE 工作台**
将现有单一列的 `#plugin-drawer` 重构为具有专业 IDE 体验的 `#plugin-ide-modal`：
1. 顶部操作栏：插件标题输入、阶段选择下拉、优先级、模型匹配规则、启用开关、格式化按钮 `[🧹 格式化]`、测试按钮 `[⚡ 沙箱测试]`、保存按钮 `[💾 保存并生效]`、关闭按钮；
2. 左右双栏容器：
   - 左栏（`flex: 1.2; min-width: 500px;`）：Monaco 编辑器挂载容器 `#monaco-editor-container` 与离线降级备用文本框 `#plugin-script-fallback`；底部附带语法状态指示条 `#ide-status-bar`（行号/列号/语法检测指示灯）；
   - 右栏（`flex: 0.8; min-width: 380px;`）：Mock 请求配置面版（支持折叠）+ Monaco Diff 对比容器 `#monaco-diff-container`（带并排/内联切换开关）+ 控制台执行日志 `#ide-console-output`。

- [ ] **Step 2: 验证 HTML 模板编译无语法错误**

```bash
make -C build generate_admin_ui_html
```
预期：成功生成 `build/generated/admin_ui_html.h`，无字符转义异常。

- [ ] **Step 3: 提交布局代码**

```bash
git add web/admin.html
git commit -m "feat(admin-ui): 🖥️ build responsive split-pane plugin IDE workbench layout"
```

---

### Task 3: 集成 Monaco Editor 核心、智能补全与优雅降级

**Files:**
- Modify: `web/admin.html`

- [ ] **Step 1: 实现 Monaco 异步 Loader 与 3 秒离线回退机制**
在 `web/admin.html` 的 JavaScript 块中添加 `initMonacoEditor()`：
```javascript
let gMonacoEditor = null;
let gMonacoDiffEditor = null;
let gMonacoLoaded = false;

function loadMonacoAsync() {
  if (window.monaco) {
    gMonacoLoaded = true;
    return Promise.resolve(window.monaco);
  }
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      console.warn("Monaco loader timeout (offline network detected, fallback enabled)");
      reject(new Error("Monaco CDN timeout"));
    }, 3000);

    const script = document.createElement("script");
    script.src = "https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.45.0/min/vs/loader.min.js";
    script.onload = () => {
      require.config({ paths: { vs: "https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.45.0/min/vs" } });
      require(["vs/editor/editor.main"], () => {
        clearTimeout(timer);
        gMonacoLoaded = true;
        registerLuaIntelliSense();
        resolve(window.monaco);
      });
    };
    script.onerror = () => {
      clearTimeout(timer);
      reject(new Error("Failed to load Monaco script"));
    };
    document.head.appendChild(script);
  });
}
```

- [ ] **Step 2: 注册 `aigate` 原生函数与 `req`/`resp` 对象智能补全**
实现 `registerLuaIntelliSense()`：
```javascript
function registerLuaIntelliSense() {
  if (!window.monaco) return;
  monaco.languages.registerCompletionItemProvider('lua', {
    provideCompletionItems: (model, position) => {
      const suggestions = [
        // aigate module
        { label: 'aigate.log_info', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_info("${1:message}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '打印 INFO 级别运行日志' },
        { label: 'aigate.log_warn', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_warn("${1:message}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '打印 WARN 级别运行日志' },
        { label: 'aigate.log_error', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.log_error("${1:message}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '打印 ERROR 级别运行日志' },
        { label: 'aigate.sha256', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.sha256(${1:str})', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '计算字符串的 SHA-256 十六进制哈希' },
        { label: 'aigate.json_decode', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.json_decode(${1:json_str})', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '解析 JSON 字符串为 Lua Table' },
        { label: 'aigate.json_encode', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.json_encode(${1:table})', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '序列化 Lua Table 为 JSON 字符串' },
        { label: 'aigate.abort', kind: monaco.languages.CompletionItemKind.Function, insertText: 'aigate.abort(${1:403}, "${2:forbidden}", "${3:Access denied}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '短路熔断并返回自定义 HTTP 错误' },
        // req methods
        { label: 'req:get_header', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:get_header("${1:header_name}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '获取请求头字段内容' },
        { label: 'req:set_header', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:set_header("${1:header_name}", "${2:value}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '添加或覆盖请求头' },
        { label: 'req:get_model', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:get_model()', documentation: '获取目标模型标识' },
        { label: 'req:set_model', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:set_model("${1:new_model}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '动态改写路由大模型标识' },
        { label: 'req:get_body', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:get_body()', documentation: '获取原始请求体内容' },
        { label: 'req:set_body', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:set_body(${1:new_body})', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '替换修改请求体内容' },
        { label: 'req:get_json', kind: monaco.languages.CompletionItemKind.Method, insertText: 'req:get_json()', documentation: '解析请求体 JSON 为 Lua Table' },
        // resp methods
        { label: 'resp:get_header', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_header("${1:header_name}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '获取响应头字段内容' },
        { label: 'resp:set_header', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:set_header("${1:header_name}", "${2:value}")', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '添加或覆盖响应头' },
        { label: 'resp:get_status', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_status()', documentation: '获取当前 HTTP 响应状态码' },
        { label: 'resp:set_status', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:set_status(${1:200})', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '改写 HTTP 响应状态码' },
        { label: 'resp:get_body', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_body()', documentation: '获取原始响应体内容' },
        { label: 'resp:set_body', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:set_body(${1:new_body})', insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, documentation: '替换响应体内容 (如脱敏替换)' },
        { label: 'resp:get_json', kind: monaco.languages.CompletionItemKind.Method, insertText: 'resp:get_json()', documentation: '解析响应体 JSON 为 Lua Table' }
      ];
      return { suggestions };
    }
  });
}
```

- [ ] **Step 3: 编译并验证**

```bash
make -C build -j8
```
预期：编译成功。

- [ ] **Step 4: 提交代码**

```bash
git add web/admin.html
git commit -m "feat(admin-ui): 🧠 integrate monaco editor with lua 5.4 intellisense and offline fallback"
```

---

### Task 4: 实现 500ms 防抖实时语法检测与错误波浪线标记

**Files:**
- Modify: `web/admin.html`

- [ ] **Step 1: 在编辑输入事件中挂载 500ms 防抖并调用验证 API**
```javascript
let gSyntaxDebounceTimer = null;

function setupLiveSyntaxLinting(editor) {
  editor.onDidChangeModelContent(() => {
    clearTimeout(gSyntaxDebounceTimer);
    gSyntaxDebounceTimer = setTimeout(async () => {
      const code = editor.getValue();
      await lintPluginCode(code);
    }, 500);
  });
}

async function lintPluginCode(code) {
  if (!code || !code.trim()) {
    clearEditorMarkers();
    updateLintStatus(true, "语法检测：空代码");
    return;
  }
  try {
    const res = await adminFetch("/admin/v1/plugins/validate", {
      method: "POST",
      body: JSON.stringify({ script_code: code })
    });
    if (res.valid) {
      clearEditorMarkers();
      updateLintStatus(true, "语法检测：正常 🟢");
    } else {
      setEditorErrorMarker(res.line || 1, res.message || "语法错误");
      updateLintStatus(false, `第 ${res.line || 1} 行语法错误: ${res.message || ''} ❌`);
    }
  } catch (err) {
    console.error("Syntax linting error:", err);
  }
}

function setEditorErrorMarker(line, message) {
  if (!gMonacoEditor || !window.monaco) return;
  const model = gMonacoEditor.getModel();
  monaco.editor.setModelMarkers(model, 'lua-linter', [{
    startLineNumber: line,
    startColumn: 1,
    endLineNumber: line,
    endColumn: 1000,
    message: message,
    severity: monaco.MarkerSeverity.Error
  }]);
}

function clearEditorMarkers() {
  if (!gMonacoEditor || !window.monaco) return;
  const model = gMonacoEditor.getModel();
  monaco.editor.setModelMarkers(model, 'lua-linter', []);
}
```

- [ ] **Step 2: 编译测试并提交**

```bash
make -C build -j8
git add web/admin.html
git commit -m "feat(admin-ui): ⚡ implement 500ms debounced live syntax linting with error markers"
```

---

### Task 5: 集成 Monaco Diff Editor 双屏并列对比与控制台追踪

**Files:**
- Modify: `web/admin.html`

- [ ] **Step 1: 初始化 Monaco 原生 Diff 对比器并支持并排/内联一键切换**
```javascript
function initDiffEditor(container) {
  if (!window.monaco) return null;
  const diffEditor = monaco.editor.createDiffEditor(container, {
    theme: 'vs-dark',
    readOnly: true,
    renderSideBySide: true,
    automaticLayout: true,
    minimap: { enabled: false }
  });
  return diffEditor;
}

function toggleDiffSideBySide(enabled) {
  if (!gMonacoDiffEditor) return;
  gMonacoDiffEditor.updateOptions({ renderSideBySide: enabled });
}

function displayDryRunDiff(originalObj, modifiedObj) {
  if (!gMonacoDiffEditor || !window.monaco) {
    // 降级使用普通对比展示
    renderFallbackDiff(originalObj, modifiedObj);
    return;
  }
  const originalStr = JSON.stringify(originalObj, null, 2);
  const modifiedStr = JSON.stringify(modifiedObj, null, 2);

  const originalModel = monaco.editor.createModel(originalStr, 'json');
  const modifiedModel = monaco.editor.createModel(modifiedStr, 'json');

  gMonacoDiffEditor.setModel({
    original: originalModel,
    modified: modifiedModel
  });
}
```

- [ ] **Step 2: 处理 Abort 熔断与控制台日志输出**
Dry-run 结果返回后：
1. 若 `res.aborted == true`，展示醒目的 `[ABORT 拦截]` 警示横幅与 HTTP 状态码；
2. 控制台 `#ide-console-output` 格式化打印捕获的 `res.logs` 数组，展示日志时间、级别与耗时标签（`⚡ 执行耗时: XX μs`）。

- [ ] **Step 3: 编译并通过全量单元测试**

```bash
make -C build -j8 && ./build/tests/aigate_unit_tests
```
预期：PASS: 350+/350+ test(s), 0 failure(s).

- [ ] **Step 4: 提交代码**

```bash
git add web/admin.html
git commit -m "feat(admin-ui): 📊 integrate monaco side-by-side diff editor and console execution trace"
```

---

### Task 6: 内存安全验证与全量回归测试

**Files:**
- Test: All tests under ASan/UBSan

- [ ] **Step 1: 运行全量 ASan/UBSan 构建与测试套件**

```bash
cmake -B build_asan -S . -DAIGATE_SANITIZERS=ON && make -C build_asan -j8 && ./build_asan/tests/aigate_unit_tests
```
预期：PASS 100%，无 AddressSanitizer 内存泄漏与未定义行为报告。

- [ ] **Step 2: 验证编译 0 告警**

```bash
make -C build -j8
```
预期：`-Wall -Wextra -Werror` 0 warnings, 0 errors。

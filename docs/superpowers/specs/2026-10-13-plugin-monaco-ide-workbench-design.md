# 插件管理与调试工作台优化设计规范 (Plugin Monaco IDE & Sandbox Diff Spec)

## 1. 概述与设计背景
`aigate` 已完成嵌入式 Lua 5.4 脚本插件引擎核心（包含内存沙箱、微秒级执行、指令步数熔断、数据库持久化与原子热重载）。为了极大提升网关管理员与开发者的脚本编写、调试与发布体验，本项目旨在将现有的基础抽屉式文本输入框重构优化为**沉浸式双栏 Monaco IDE 插件工作台**。

核心能力包括：
1. **沉浸式双栏分屏工作台**：左侧为全功能代码编辑区，右侧为交互式沙箱测试与 Diff 对比区；
2. **Monaco Editor Lua 5.4 智能补全 (IntelliSense)**：内置 `aigate.*`、`req:*`、`resp:*` 完整 API 说明、方法签名与代码片段（Snippets）；
3. **轻量实时语法检测与红线定位 (Live Linting)**：后端新增 `POST /admin/v1/plugins/validate` 路由，前端 500ms 输入防抖检测，在出错行绘制红色波浪线与错误气泡；
4. **Monaco 原生双屏并列 Diff (Side-by-Side Diff)**：直观对比 Mock 请求/响应在插件处理前后的差异（字符级绿色增/红色删高亮），并支持一键切换单列内联模式；
5. **离线与断网优雅降级 (Resilient Fallback)**：在完全隔离的内网无外网环境下，Monaco 加载超时自动降级为内置基础编辑器，保证系统 100% 稳定可用，绝不白屏。

---

## 2. 交互布局与视觉架构

```
+--------------------------------------------------------------------------------------------------+
| 顶部导航栏 / 插件元数据区: [插件名称: tagger] [执行阶段: 请求前 ▼] [优先级: 5] [匹配模型: *]    |
| [状态: 已启用 🟢]   |   [格式化代码 🧹]  [一键沙箱测试 ⚡]  [保存并热加载 💾]  [退出工作台 ✖]        |
+-----------------------------------------------------------------+--------------------------------+
| 左栏 (60% 宽度): Monaco Lua 代码编辑器                          | 右栏 (40% 宽度): 沙箱测试 & 差异|
| - 行号、代码折叠、Lua 语法高亮                                   | +----------------------------+ |
| - aigate / req / resp 智能提示 IntelliSense                     | | Mock 输入参数 (可折叠)      | |
| - 错误行红色波浪线标记 (500ms 防抖自动检测)                     | | - Method, Path, Headers    | |
| - 优雅离线降级支持                                              | | - Mock Request/Resp Body   | |
|                                                                 | +----------------------------+ |
|                                                                 | | 沙箱 Diff 视图 (Monaco)    | |
|                                                                 | | [并排 ⇋ 内联] [原始 vs 修改]| |
|                                                                 | +----------------------------+ |
|                                                                 | | 终端控制台 (Console Logs)  | |
|                                                                 | | [INFO] Processed mock req..| |
+-----------------------------------------------------------------+--------------------------------+
```

### 响应式适配设计
- **宽屏设备（屏幕宽度 ≥ 1200px）**：左右双栏展开（60% 编辑器 / 40% 沙箱）。
- **中窄屏设备（屏幕宽度 < 1200px）**：自动切换为选项卡折叠模式（`[代码编辑]` ⇋ `[沙箱与Diff]`），确保在笔记本或平板上编辑区域足够宽阔。

---

## 3. 后端轻量语法校验 API 规范

### 端点定义
- **路径**：`POST /admin/v1/plugins/validate`
- **认证**：与所有 Admin API 一致（Bearer Token / SHA-256 校验）
- **性能预期**：微秒级（≤ 50 μs），仅调用 Lua 编译器的词法/语法树分析，不初始化沙箱环境，不执行代码，零系统副作用。

### 请求体 (Request Payload)
```json
{
  "script_code": "function on_request(req)\n  req:set_header('x-test', 'ok')\nend"
}
```

### 响应体 (Response Body)
1. **语法正确时 (200 OK)**：
```json
{
  "valid": true
}
```

2. **语法错误时 (200 OK)**：
```json
{
  "valid": false,
  "line": 4,
  "column": 1,
  "message": "unexpected symbol near 'end'",
  "raw_error": "[string \"code\"]:4: unexpected symbol near 'end'"
}
```

### 行号提取逻辑
针对 Lua 编译报错格式 `[string "..."]:<line>: <message>`，在 C 语言中解析冒号后的整数行号，并剥离前缀返回结构化行号与可读错误说明。

---

## 4. 前端 Monaco 编辑器与智能补全规范

### 4.1 异步加载与优雅降级
1. 通过 `vs/loader.js` (默认 CDN: cdnjs / unpkg) 动态注入 Monaco 核心。
2. 设置 3 秒加载超时定时器：
   - 成功加载：初始化 Monaco Editor 实例；
   - 超时或网络异常：切换至内置等宽代码文本框，并在工作台顶部显示状态标牌 *「当前运行于离线基础模式」*，所有保存与测试流程完全正常可用。

### 4.2 IntelliSense 智能补全注册
注册 `monaco.languages.registerCompletionItemProvider('lua', ...)`，内置如下补全元数据：

| 符号 | 补全类型 | 描述与文档说明 |
|---|---|---|
| `aigate.log_info(msg)` | Method | 输出 INFO 级别运行时日志 |
| `aigate.log_warn(msg)` | Method | 输出 WARN 级别运行时日志 |
| `aigate.log_error(msg)` | Method | 输出 ERROR 级别运行时日志 |
| `aigate.sha256(text)` | Method | 计算输入文本的 SHA-256 十六进制哈希 |
| `aigate.json_decode(str)` | Method | 解析 JSON 字符串为 Lua Table 字典 |
| `aigate.json_encode(tbl)` | Method | 将 Lua Table 序列化为紧凑 JSON 字符串 |
| `aigate.abort(status, type, msg)` | Method | 立即中断请求流水线并向客户端返回错误响应 (如 401/403) |
| `req:get_header(key)` | Method | 获取指定请求头内容 (忽略大小写) |
| `req:set_header(key, val)` | Method | 设置或改写指定请求头 |
| `req:get_model()` | Method | 获取当前请求的目标大模型标识 (如 "gpt-4o") |
| `req:set_model(name)` | Method | 动态重写路由大模型标识 |
| `req:get_body()` | Method | 获取原始请求 Body 字符串 |
| `req:set_body(str)` | Method | 重写请求 Body 内容 |
| `req:get_json()` | Method | 获取已解析为 Lua Table 的请求 Body |
| `resp:get_header(key)` | Method | 获取指定响应头内容 |
| `resp:set_header(key, val)` | Method | 设置或改写响应头 |
| `resp:get_status()` | Method | 获取 HTTP 响应状态码 (如 200) |
| `resp:set_status(code)` | Method | 改写 HTTP 响应状态码 |
| `resp:get_body()` | Method | 获取原始响应 Body 字符串 |
| `resp:set_body(str)` | Method | 重写响应 Body 内容 (如敏感数据脱敏替换) |
| `resp:get_json()` | Method | 获取已解析为 Lua Table 的响应 Body |

### 4.3 语法检测与错误波浪线标记
1. 监听 `editor.onDidChangeModelContent`，开启 500ms 防抖；
2. 防抖触发后，异步调用 `/admin/v1/plugins/validate`；
3. 根据返回结果调用 `monaco.editor.setModelMarkers(model, 'lua-linter', markers)`：
   - 失败时在对应行生成错误波浪线，并在编辑器行号槽标记错误图标；
   - 顶部状态栏显示错误摘要，点击直达出错代码行。

---

## 5. 沙箱 Diff 对比与控制台输出规范

### 5.1 Monaco Diff Editor 实例
- 使用 `monaco.editor.createDiffEditor(container, { ... })` 渲染对比结果；
- 维护两个只读只写 Model：`originalModel` 与 `modifiedModel`；
- 支持工具栏开关切换 `renderSideBySide: true / false`（左右并排 ⇋ 单列内联）；
- 请求阶段对比维度：`{ "model": ..., "headers": ..., "body": ... }`；
- 响应阶段对比维度：`{ "status": ..., "headers": ..., "body": ... }`。

### 5.2 Abort 拦截响应态
- 当检测到返回的 JSON 包含 `aborted: true` 时，Diff 区域切换为告警横幅模式，展示状态码（如 `[401 UNAUTHORIZED]`）、拦截消息及堆栈。

### 5.3 实时日志与性能度量
- 下方控制台打印本次 Dry-run 捕获的执行日志列表；
- 标注执行耗时标签：`⚡ 执行耗时: X μs`。

---

## 6. 测试与质量门禁

1. **单元测试矩阵**：
   - `test_admin_api.c`: 新增 `test_admin_plugins_validate` 测试合法语法、非法语法、多行行号提取及空字符串边界；
   - `test_admin_ui.c`: 验证生成的 `admin_ui_html.h` 中包含 Monaco Loader 逻辑与 Diff 容器结构。
2. **构建与内存安全**：
   - `-Wall -Wextra -Werror` 0 告警；
   - 全量 350+ 测试 100% 通过；
   - ASan/UBSan 下 0 内存泄漏与 0 越界访问。

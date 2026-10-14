# 嵌入式 Lua 插件 IDE 工作台优化完善设计规范 (Lua Plugin IDE Workbench Enhancement Design Spec)

**文档状态**: 已设计评审 (Design Approved)  
**设计日期**: 2026-10-14  
**目标版本**: aigate v1.0+  
**影响模块**: 前端 Web 控制台 (`web/src/views/PluginsView.vue`, `web/src/components/MonacoEditor.vue`), 后端 Admin API (`src/server/admin_api.c`)

---

## 1. 概述与设计目标 (Executive Summary & Goals)

### 1.1 背景
aigate 具备基于 Lua 5.4 的高性能嵌入式插件扩展系统，能够在请求入站 (`on_request`) 与上游响应出站 (`on_response`) 阶段以微秒级开销完成鉴权、模型动态路由重写、请求/响应体篡改、日志审计与主动熔断阻断。网关内核已实现沙箱指令熔断保护与 `lua_plugin_test_dry_run` 仿真执行引擎。

然而，现存的 Web 插件工作台存在以下体验与功能短板：
1. **预置模板缺失**：仅提供单调的空白脚本模板，缺少企业级生产高频场景参考；
2. **编辑器补全简陋**：Monaco 编辑器仅提供基础关键词提示，缺少 Tab 展开的 Snippets 代码片段与鼠标悬浮中英文 API 交互式文档；
3. **沙箱调试能力未释放**：仅支持最简单的 Path/Model/Body 输入，未打通底层已支持的 Headers 键值对增删、Client IP 构造、出站状态码、以及 `aigate.log_*` 日志捕获回显；
4. **API 键名容错性**：后台测试接口对 `script_code` 与 `script` 存在命名不一致潜在风险。

### 1.2 目标指标 (Key Objectives)
- **开箱即用的模板库**：提供 5 大企业级生产实战场景模板，覆盖 90%+ 生产插件需求；
- **沉浸式 Monaco 开发体验**：提供 6+ 组高频 Tab 自动展开 Snippets，以及覆盖所有 `aigate.*`, `req:*`, `resp:*` 的丰富 Hover 悬浮文档与语法红波浪线指示；
- **全功能双栏沙箱调试台**：支持入站 (`on_request`) 与出站 (`on_response`) 的完整 Mock 参数构造（含 Headers 表格、Client IP、Status、Body），支持一键根据模板预填 Mock 数据；
- **实时控制台终端 (Live Log Console)**：微秒级输出执行耗时、改写输出对比（Model / Headers / Body / Abort），并以暗色终端风格展示着色分级控制台日志；
- **高标准构建与无损集成**：继续保持现有 350 个 C 单元测试 100% 通过率；Vite 打包无损单文件内嵌进 `src/server/web_assets.h`，无警告编译通过。

---

## 2. 预置企业级 Lua 插件模板库规范 (Enterprise Template Library)

前端在新建/编辑工作台中新增「📐 预置模板」下拉选择器。选择模板后自动填充名称、生效阶段、默认优先级，并将带有详细中文注释的代码加载至 Monaco 编辑器：

### 2.1 模板列表与元数据定义
| ID | 模板名称 | 阶段 (`phase`) | 默认优先级 | 核心 API | 典型应用场景 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `token_auth` | **Token 鉴权与黑白名单拦截** | `on_request` | 10 | `req:get_header`, `aigate.abort_with`, `aigate.log_warn` | 生产环境入口安全网关，基于请求头 Bearer Token 验证鉴权与非法请求阻断 |
| `model_router` | **基于用户的模型动态路由改写** | `on_request` | 50 | `req:get_header`, `req:set_model`, `aigate.log_info` | VIP / 企业租户智能改写模型请求，如 `gpt-4o` 无感升级或重路由至 `deepseek-chat` |
| `trace_logger` | **全链路追踪日志与 Header 注入** | `on_request` | 20 | `req:get_header`, `req:set_header`, `aigate.log_info` | 生成/透传分布式链路 Trace ID，注入出入站网关追踪标 |
| `resp_masking` | **响应内容敏感信息脱敏与水印** | `on_response` | 100 | `resp:get_body`, `resp:set_body`, `resp:set_header` | 大模型回答出站时基于正则表达式脱敏手机号、身份证号或嵌入企业溯源水印 |
| `rate_limit_stub` | **访问频控检查与 429 短路熔断** | `on_request` | 5 | `req:get_client_ip`, `aigate.abort_with`, `aigate.log_warn` | 识别异常 Client IP 并在微秒内直接短路返回 429 频控响应，保护下游模型集群 |

### 2.2 生产级模板源码定义

```lua
-- 1. token_auth: Token 鉴权与黑白名单拦截
local aigate = require("aigate")

function on_request(req)
    local auth_header = req:get_header("authorization")
    if not auth_header or not string.match(auth_header, "^Bearer%s+") then
        aigate.log_warn("拦截未授权请求: 缺少有效 Authorization 头")
        aigate.abort_with(401, '{"error":{"code":"unauthorized","message":"Missing or invalid Bearer token"}}')
        return
    end

    local token = string.gsub(auth_header, "^Bearer%s+", "")
    -- 模拟黑名单或受限 token 阻断
    if token == "banned-test-token" then
        aigate.log_warn("拦截黑名单 Token: " .. token)
        aigate.abort_with(403, '{"error":{"code":"forbidden","message":"Your token has been revoked"}}')
        return
    end

    aigate.log_info("Token 鉴权通过")
end

-- 2. model_router: 基于用户的模型动态路由改写
local aigate = require("aigate")

function on_request(req)
    local current_model = req:get_model()
    local user_tier = req:get_header("x-user-tier")

    aigate.log_info("收到请求，原始模型: " .. (current_model or "nil") .. ", 用户等级: " .. (user_tier or "free"))

    -- 若为 vip 客户且请求为 gpt-4o，无缝路由至定制高性能集群 deepseek-chat
    if user_tier == "vip" and current_model == "gpt-4o" then
        req:set_model("deepseek-chat")
        req:set_header("x-gateway-routed", "deepseek-cluster")
        aigate.log_info("VIP 用户模型动态路由重写: gpt-4o -> deepseek-chat")
    end
end

-- 3. trace_logger: 全链路追踪日志与 Header 注入
local aigate = require("aigate")

function on_request(req)
    local trace_id = req:get_header("x-request-id")
    if not trace_id or trace_id == "" then
        trace_id = "trace-" .. tostring(os.time()) .. "-" .. tostring(math.random(1000, 9999))
        req:set_header("x-request-id", trace_id)
    end

    local client_ip = req:get_client_ip() or "unknown"
    local model = req:get_model() or "default"
    aigate.log_info(string.format("[%s] 收到请求: IP=%s, Model=%s", trace_id, client_ip, model))
end

-- 4. resp_masking: 响应内容敏感信息脱敏与水印
local aigate = require("aigate")

function on_response(resp)
    local body = resp:get_body()
    if not body or body == "" then return end

    -- 正则脱敏 11 位中国大陆手机号码 (13x/15x/18x/19x 等) 为 138****1234
    local masked_body, count = string.gsub(body, "(1[3-9]%d)(%d%d%d%d)(%d%d%d%d)", "%1****%3")
    if count > 0 then
        aigate.log_info("响应体脱敏完成，共掩码敏感手机号: " .. tostring(count) .. " 处")
        resp:set_body(masked_body)
        resp:set_header("x-privacy-masked", "true")
    end

    -- 注入出站安全水印标
    resp:set_header("x-gateway-watermark", "verified-by-aigate")
end

-- 5. rate_limit_stub: 访问频控检查与 429 短路熔断
local aigate = require("aigate")

function on_request(req)
    local client_ip = req:get_client_ip()
    local banned_subnets = { "10.99.", "192.0.2." }

    for _, prefix in ipairs(banned_subnets) do
        if client_ip and string.sub(client_ip, 1, #prefix) == prefix then
            aigate.log_warn("触发频控短路熔断: 来源 IP 受限 " .. client_ip)
            aigate.abort_with(429, '{"error":{"code":"rate_limited","message":"Too many requests from your IP, please retry later"}}')
            return
        end
    end
end
```

---

## 3. Monaco Editor 开发体验增强规范 (Editor UX Polish)

### 3.1 Snippets 代码片段规范 (Tab Expansion)
在 `MonacoEditor.vue` 中注册 `monaco.languages.registerCompletionItemProvider('lua', ...)`，新增以下生产级 Snippets：

```javascript
[
  {
    label: 'req-header-check',
    kind: monaco.languages.CompletionItemKind.Snippet,
    insertText: [
      'local ${1:val} = req:get_header("${2:x-custom-header}")',
      'if not ${1:val} then',
      '    aigate.abort_with(400, \'{"error":"Missing required header"}\')',
      '    return',
      'end'
    ].join('\n'),
    insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
    documentation: '获取并校验指定 Header，若缺失则熔断阻断'
  },
  {
    label: 'req-model-rewrite',
    kind: monaco.languages.CompletionItemKind.Snippet,
    insertText: [
      'local model = req:get_model()',
      'if model == "${1:gpt-4o}" then',
      '    req:set_model("${2:deepseek-chat}")',
      '    aigate.log_info("Model rewritten to ${2:deepseek-chat}")',
      'end'
    ].join('\n'),
    insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
    documentation: '条件判断并改写请求大模型名称'
  },
  {
    label: 'json-modify-body',
    kind: monaco.languages.CompletionItemKind.Snippet,
    insertText: [
      'local body = req:get_body()',
      '-- 对请求/响应 body 进行文本替换或处理',
      'local new_body = string.gsub(body, "${1:search}", "${2:replace}")',
      'req:set_body(new_body)'
    ].join('\n'),
    insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
    documentation: '读取并改写 HTTP Body 载荷'
  },
  {
    label: 'aigate-abort',
    kind: monaco.languages.CompletionItemKind.Snippet,
    insertText: 'aigate.abort_with(${1:403}, \'{"error":{"code":"forbidden","message":"${2:Access Denied}"}}\')',
    insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
    documentation: '立即以指定 HTTP 状态码与 JSON 载荷阻断请求'
  },
  {
    label: 'log-suite',
    kind: monaco.languages.CompletionItemKind.Snippet,
    insertText: [
      'aigate.log_info("${1:info message}")',
      'aigate.log_warn("${2:warn message}")'
    ].join('\n'),
    insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
    documentation: '插入常用网关日志打印语句'
  }
]
```

### 3.2 Hover 交互式中英文文档规范 (Hover Provider)
注册 `monaco.languages.registerHoverProvider('lua', ...)`，当开发者光标悬浮在符号上时呈现 Markdown 格式卡片：

- **`aigate.abort_with(status, body)`**：
  - **签名**：`aigate.abort_with(status: integer, body: string) -> nil`
  - **说明**：立即终止当前处理流程并向客户端返回指定 HTTP 状态码及响应体。后续所有插件及上游调用均被短路跳过。
- **`aigate.log_info / log_warn / log_error / log_debug(msg)`**：
  - **签名**：`aigate.log_*(msg: string) -> nil`
  - **说明**：将日志打印至网关事件流与沙箱调试控制台。微秒级开销，无阻塞。
- **`req:get_header(name)` / `req:set_header(name, value)`**：
  - **签名**：`req:get_header(name: string) -> string | nil`, `req:set_header(name: string, value: string) -> nil`
  - **说明**：读取或动态注入/修改入站 HTTP 请求头。
- **`req:get_model()` / `req:set_model(name)`**：
  - **签名**：`req:get_model() -> string | nil`, `req:set_model(name: string) -> nil`
  - **说明**：获取或重写请求大模型标识（如将 `gpt-4o` 改写为 `deepseek-chat`）。
- **`resp:get_status()` / `resp:set_status(code)`**：
  - **签名**：`resp:get_status() -> integer`, `resp:set_status(code: integer) -> nil`
  - **说明**：获取或改写出站 HTTP 状态码。
- **`resp:get_body()` / `resp:set_body(new_body)`**：
  - **签名**：`resp:get_body() -> string | nil`, `resp:set_body(new_body: string) -> nil`
  - **说明**：出站阶段获取或修改模型上游返回的原始文本（如用于敏感词过滤、脱敏、水印）。

### 3.3 编辑器按键与辅助支持
- 快捷键 `Ctrl+S` / `Cmd+S`：触发保存当前插件草稿并执行语法校验；
- 快捷键 `Ctrl+Enter` / `Cmd+Enter`：直接唤起/执行沙箱隔离测试；
- 括号着色与匹配：启用 `bracketPairColorization: { enabled: true }`；
- 迷你地图：启用 `minimap: { enabled: true }`。

---

## 4. 增强型沙箱隔离测试器与运行日志控制台规范 (Sandbox Inspector)

### 4.1 Mock 参数构造面板
根据插件生效阶段展示不同参数：
1. **`on_request` 模式**：
   - `Method`：GET / POST / PUT / DELETE（默认 POST）
   - `Path`：输入框，默认 `/v1/chat/completions`
   - `Client IP`：输入框，默认 `127.0.0.1`
   - `Model`：输入框，默认 `gpt-4o`
   - `Headers`：动态表格，支持添加、修改、删除 Key-Value 行，提供常用头快速预填（`Authorization`, `Content-Type`, `X-User-Tier` 等）
   - `Body`：JSON 文本域，支持「一键美化排版 (Format JSON)」
2. **`on_response` 模式**：
   - `Status Code`：数字输入，默认 `200`
   - `Headers`：响应头表格，默认包含 `Content-Type: application/json`
   - `Body`：响应体 JSON，模拟大模型回复内容

### 4.2 智能一键预设 (One-Click Preset Fill)
工作台提供快捷按钮「填入当前模板典型用例」：
- 切换到 **Token 鉴权** 模板时：自动在 Headers 表格填入 `Authorization: Bearer sk-dev-test`；
- 切换到 **模型动态路由** 模板时：自动填入 `model: gpt-4o`，并在 Headers 填入 `X-User-Tier: vip`；
- 切换到 **全链路追踪** 模板时：自动注入空的或既有 `X-Request-ID`；
- 切换到 **响应脱敏** 模板时：自动将阶段切换为 `on_response`，并在 Body 中填充含敏感手机号的 Mock 回复；
- 切换到 **频控熔断** 模板时：自动将 Client IP 填入测试黑名单网段 `10.99.1.5`。

### 4.3 实时控制台终端 (Live Log Console)
1. **DOM 探针与挂载**：在模板中暴露 `#ideConsoleLogs` 与终端容器，供调试与测试自动化直接查询。
2. **运行指标与状态摘要**：
   - 状态 Tag：`[OK 执行通过]` (绿色) / `[Aborted 熔断阻断: 403]` (橙黄色) / `[Error 语法或运行时异常]` (红色)
   - 耗时指标：`沙箱纯执行耗时: X μs`
3. **改写差异比对 (Output Inspector)**：
   - 模型重写标记：如 `gpt-4o ➔ deepseek-chat`
   - 注入 Headers 标记：新增或被修改的 Header
   - 改写后的 Body 输出
   - 熔断详情：HTTP 状态码 + 阻断返回内容
4. **终端日志输出**：
   - 提取后台回传的 `logs` 数组；
   - 分级颜色着色：
     - `[INFO]` -> `#38bdf8` (青色)
     - `[DEBUG]` -> `#94a3b8` (灰色)
     - `[WARN]` -> `#fbbf24` (黄色)
     - `[ERROR]` -> `#f43f5e` (红色)
   - 终端工具栏：提供「清空控制台」、「复制全部日志」、「关键词过滤」。

---

## 5. 后台 API 契约与向后兼容设计 (Backend API Contract)

### 5.1 `POST /admin/v1/plugins/test` 接口契约
在 `src/server/admin_api.c` 中，保持向后兼容，同时支持 `script_code` 与 `script` 字段：
```c
/* 读取 script_code，若为空则降级尝试 script */
json_t* jcode = json_object_get(root, "script_code");
if (jcode == NULL || !json_is_string(jcode)) {
    jcode = json_object_get(root, "script");
}
```
同时，如果客户端发送了平铺的 `headers`/`body`/`path`/`model` 且未包装在 `mock_payload` 中，后台亦可自动封装，确保鲁棒性。

### 5.2 响应协议定义
```json
{
  "status": "ok",
  "success": true,
  "aborted": false,
  "duration_us": 128,
  "logs": [
    "[INFO] 收到请求，原始模型: gpt-4o, 用户等级: vip",
    "[INFO] VIP 用户模型动态路由重写: gpt-4o -> deepseek-chat"
  ],
  "request": {
    "model": "deepseek-chat",
    "headers": {
      "Authorization": "Bearer sk-dev-test",
      "X-User-Tier": "vip",
      "X-Gateway-Routed": "deepseek-cluster"
    },
    "body": "{\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}"
  }
}
```

---

## 6. 测试与验证计划 (Verification Plan)

1. **C 语言单元测试基线 (100% Pass Rate)**：
   - 运行测试套件：确保 `tests/unit/plugin/test_lua_plugin.c` 及所有 350 个现有测试全部通过。
2. **前端打包与构建单文件内嵌**：
   - `cd web && npm run build`：确保 Vue 3 + Naive UI + Tailwind + Monaco 打包输出单文件 `web/dist/index.html`。
   - `python3 embed_html.py`：将 HTML 文件编译并写入 `src/server/web_assets.h`。
   - `make -j$(nproc)`：全项目编译，`-Wall -Wextra -Werror` 零警告通过。
3. **功能性走查**：
   - 验证五大模板载入无语法错误；
   - 验证 Monaco 补全菜单与 Tab 代码片段正常展开；
   - 验证悬浮 Hover 文档正常弹出；
   - 验证沙箱在请求与响应两阶段下 Headers、Client IP、Body、Status 均能被正确传入、改写并回传；
   - 验证控制台日志高亮与清空、复制功能正常。

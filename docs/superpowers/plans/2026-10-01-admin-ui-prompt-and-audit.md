# Admin 控制台 Prompt 模板可视化编辑与实时请求审计流实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 aigate 嵌入式 Web 控制台 (`web/admin.html`) 新增两个一级独立工作台：
1. **📝 Prompt 模板 (`tab-prompts`)**：集中式模板预设库管理、动态变量提取、实时插值沙盒与 Token 估算、以及与模型/密钥的双向同步。
2. **📋 实时请求审计流 (`tab-audit`)**：全屏实时监控工作台，具备实时流式自动刷新（2s/5s/10s/暂停）、多维度过滤、核心 KPI 看板卡片、侧边滑出请求详情抽屉（Token 消耗分布与元数据）与 CSV/JSON 导出。

**Architecture:** 保持 aigate 单二进制嵌入式前端架构（Vanilla JS + Tailwind CSS + 原生 CSS/SVG 徽章），由 `scripts/embed_html.py` 将 `web/admin.html` 转换为 C 头文件 `build/generated/admin_ui_html.h` 供 civetweb HTTP 服务直接交付。全量复用 `apiCall()` 统一请求通信与异常处理机制，无需任何 npm / Node.js 外部构建链。

**Tech Stack:** HTML5, Tailwind CSS Utility, Vanilla ES6+ JavaScript, CMake, Python 3, Pytest, C17.

---

## 文件影响范围映射

- **前端单页源文件：**
  - `web/admin.html`:
    - 侧边栏与移动端导航：新增 `prompts` 与 `audit` 导航按钮。
    - 主视图容器：新增 `<div id="tab-prompts">` 与 `<div id="tab-audit">` 面板结构及侧边抽屉。
    - 核心 JavaScript：新增 Prompt 预设库、变量动态提取/插值沙盒、Model/Key API 同步逻辑；新增审计日志流式轮询控制器、KPI 计算、多维过滤、抽屉展示与 CSV/JSON 导出。
- **构建生成的 C 头文件：**
  - `build/generated/admin_ui_html.h`（由 CMake 目标 `generate_admin_ui_html` / `scripts/embed_html.py` 自动重新生成）。
- **自动化测试文件：**
  - `tests/integration/test_gateway.py`: 增加 Admin UI Prompt 模板与实时请求审计流的元素与接口集成测试用例。

---

## 任务拆分列表

### Task 1: 侧边栏导航与基础路由生命周期扩展

**Files:**
- Modify: `web/admin.html:230-295` (桌面侧边栏导航)
- Modify: `web/admin.html:325-340` (主导航占位)
- Modify: `web/admin.html:360-375` (移动端横向滑动导航)
- Modify: `web/admin.html:2050-2400` (状态定义、`switchTab` 路由与生命周期)

- [x] **Step 1: 在桌面侧边导航与移动端导航中增加「📝 Prompt 模板」与「📋 审计日志」**

在 `web/admin.html` 中：
1. 侧边栏「核心管理」分组内，在「🤖 模型」与「🔑 API 密钥」之间插入：
```html
<button data-tab="prompts" onclick="switchTab('prompts')" class="side-link" id="nav-prompts-side">
  <span class="text-base">📝</span>
  <span>Prompt 模板</span>
</button>
```
2. 侧边栏「组织与成本」分组内，在「📈 用量统计」后插入：
```html
<button data-tab="audit" onclick="switchTab('audit')" class="side-link" id="nav-audit-side">
  <span class="text-base">📋</span>
  <span>审计日志</span>
</button>
```
3. 在 `#mainNav` 中同步添加 `<button id="nav-prompts"...>` 与 `<button id="nav-audit"...>`。
4. 在 `#mobileNav` 中同步添加移动端导航按钮。

- [x] **Step 2: 更新 `state`、`paginationState`、`switchTab` 与 `tabTitles`**

在 `web/admin.html` 的 `<script>` 中：
1. `state` 扩充：
```javascript
promptPresets: [],
activePromptId: null,
promptVariables: {},
auditAutoRefresh: 5000,
auditTimerId: null,
auditRecords: [],
selectedAuditRecord: null,
```
2. `paginationState` 扩充：
```javascript
audit: { page: 1, limit: 20, total: 0 },
```
3. `switchTab(tabId)` 中：
   - `tabTitles` 增加：
     ```javascript
     prompts: "Prompt 模板管理",
     audit: "请求审计日志流"
     ```
   - 生命周期管理：
     - 若离开 `audit`（`tabId !== 'audit'` 且 `state.auditTimerId` 存在），调用 `stopAuditPolling()` 暂停轮询，防止后台无谓消耗带宽与 CPU。
     - 若进入 `audit`，调用 `startAuditPolling()`。
     - 若进入 `prompts`，调用 `initPromptWorkspace()` 初始化模型/密钥下拉及预设库。

- [x] **Step 3: 运行 CMake 目标生成 C 头文件并验证编译**

Run: `ninja generate_admin_ui_html aigate` (或 `cmake --build .build -j$(nproc)`)  
Expected: 成功生成 `admin_ui_html.h` 并成功构建可执行程序，无 warning。

- [x] **Step 4: Commit Task 1**

```bash
git add web/admin.html
git commit -m "feat(ui): 🧭 add prompt templates and audit log tabs to navigation and router"
```

---

### Task 2: 📝 Prompt 模板管理面板骨架与样式 (HTML 结构)

**Files:**
- Modify: `web/admin.html:640-700` (在 `tab-models` 与 `tab-keys` 之间加入 `tab-prompts`)

- [x] **Step 1: 编写 `tab-prompts` 主容器及双列响应式布局**

在 `web/admin.html` 中插入 `<div id="tab-prompts" class="tab-content hidden space-y-6">`：
1. **面板顶栏**：
   - 标题：“📝 Prompt 模板管理与变量沙盒”。
   - 副标题：“集中维护业务 Prompt 模板库，提取并调试动态变量 `{{var}}`，一键同步绑定至模型或 API 密钥”。
   - 操作按钮：“+ 新建模板” (`openCreatePromptModal()`)、“↺ 重置官方预设” (`resetPromptPresets()`)、“📥 导出配置” / “📤 导入配置”。
2. **左列：模板库与绑定同步 (40% 宽)**：
   - 搜索与分类过滤框 (`#promptSearchInput`, `#promptCategoryFilter`)。
   - 模板预设卡片列表容器 (`#promptPresetsList`)。
   - **Model / Key 快速绑定卡片**：
     - 目标类型选择（单选/切换：模型 Model / 密钥 Key）。
     - 目标下拉选择器 (`#promptTargetSelect`)。
     - 操作按钮：“🚀 一键同步到该配置” (`syncPromptToTarget()`) 与 “⬇️ 从目标反向载入” (`loadPromptFromTarget()`).
3. **右列：模板编辑器与变量沙盒 (60% 宽)**：
   - 模板名称与说明编辑框 (`#promptEditName`, `#promptEditDesc`)。
   - 注入模式选择器 (`#promptEditMode`: `prepend` 前置 / `append` 追加 / `override` 覆盖)。
   - 提示词多行文本编辑器 (`#promptEditTextarea`)，高对比 monospace 字体。
   - 变量快捷插入按钮工具条（`{{user_id}}`, `{{model}}`, `{{date}}`, `{{team}}`, `{{client_ip}}`）。
   - **动态变量沙盒输入区 (`#promptVariableInputsContainer`)**：
     - 根据正文自动提取变量渲染的键值对输入列表。
     - 支持手动“+ 添加自定义变量”。
   - **实时插值渲染预览与 Token 统计**：
     - 预览文本区 (`#promptRenderedPreview`)。
     - 统计指示：字符数 (`#promptCharCount`)、预估 Tokens (`#promptTokenCount`)。
     - 操作区：“📋 复制渲染结果” (`copyPromptPreview()`)、“⚡ 在调试场中测试” (`testPromptInPlayground()`).

- [x] **Step 2: 验证 HTML 嵌套与样式完整性**

Run: `python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h`  
Expected: 转换成功，无字符集或标签闭合异常。

- [x] **Step 3: Commit Task 2**

```bash
git add web/admin.html
git commit -m "feat(ui): 📐 add prompt templates workspace HTML skeleton and layout"
```

---

### Task 3: 📝 Prompt 模板管理交互逻辑与 API 同步 (JavaScript)

**Files:**
- Modify: `web/admin.html:2400-3000` (Prompt 模块 JS 逻辑实现)

- [x] **Step 1: 编写内置官方预设与 `localStorage` 持久化逻辑**

在 `web/admin.html` 的 JS 中实现：
1. 预置 4 个经典官方模板数据：
   - `corporate_safety`: 企业安全与合规守则 (`prepend`, 包含 `{{company_name}}`, `{{safety_level}}`)
   - `strict_json`: 严格 JSON 结构化输出规范 (`append`, 包含 `{{schema_definition}}`)
   - `code_reviewer`: 资深软件工程师代码审查助手 (`override`, 包含 `{{language}}`, `{{framework}}`)
   - `support_tone`: 品牌客服标准声调与服务规范 (`prepend`, 包含 `{{team}}`, `{{date}}`)
2. `loadPromptPresets()`: 从 `localStorage` 读取 `aigate_prompt_presets`，若无则初始化官方预设。
3. `savePromptPresets()`: 保存预设列表至 `localStorage`。
4. `renderPromptPresetsList()`: 渲染左侧卡片，支持激活高亮与快速切换。
5. `selectPromptPreset(id)`: 将选中模板填充至右侧编辑器。
6. `createNewPromptPreset()`, `deletePromptPreset(id)`, `resetPromptPresets()`.

- [x] **Step 2: 编写动态变量扫描、实时插值与 Token 估算引擎**

1. 正则扫描：`scanPromptVariables(text)`:
   - 使用正则 `/\{\{\s*([a-zA-Z0-9_\.\-]+)\s*\}\}/g` 提取全部唯一变量名。
2. 变量输入表单自动渲染：`renderPromptVariableInputs()`:
   - 为每个提取出的变量渲染一行输入框。
   - 智能赋予默认值：`date` -> 当前 YYYY-MM-DD，`user_id` -> `usr_demo_01`，`client_ip` -> `192.168.1.100` 等。
3. 实时插值计算与展示：`updatePromptPreview()`:
   - 随编辑器输入或变量输入框修改，即时替换 `{{var}}` 占位符。
   - 更新字符数 `length`。
   - 粗略估算 Tokens（按中英混合 ~3.5-4 字符/Token 估算，并在卡片标明 `~N tokens`）。
4. 辅助动作：
   - `insertPromptVariable(name)`: 在当前光标处插入 `{{name}}`。
   - `copyPromptPreview()`: 复制插值结果到剪贴板并提示 Toast。
   - `testPromptInPlayground()`: 切换至 `playground` Tab，并将预设模型和当前 System Prompt 带入调试场输入框。

- [x] **Step 3: 编写与后端 Model / Key 的双向绑定同步**

1. 载入可用目标：`populatePromptTargets()`:
   - 从 `state.models` 与 `state.keys` 填充 `#promptTargetSelect`。
2. `syncPromptToTarget()`:
   - 获取当前选中的目标（Model 名称或 Key ID）、Prompt 内容及 Mode。
   - 若目标为 Model：发送 `PUT /admin/v1/models/{name}`，payload: `{"system_prompt": prompt, "prompt_mode": mode}`。
   - 若目标为 Key：发送 `PUT /admin/v1/keys/{id}`，payload: `{"system_prompt": prompt, "prompt_mode": mode}`。
   - 成功后提示 Toast：“已成功同步并持久化到该模型/密钥”。
3. `loadPromptFromTarget()`:
   - 发送 `GET /admin/v1/models` 或 `GET /admin/v1/keys` 获取最新数据。
   - 将该目标的 `system_prompt` 与 `prompt_mode` 载入到当前编辑器中，自动触发变量沙盒更新。

- [x] **Step 4: 编译验证与单元逻辑自检**

Run: `ninja generate_admin_ui_html aigate`  
Expected: 编译通过，无报错。

- [x] **Step 5: Commit Task 3**

```bash
git add web/admin.html
git commit -m "feat(ui): ⚡ implement prompt template library, variable sandbox and API sync"
```

---

### Task 4: 📋 实时请求审计流面板骨架与侧边抽屉 (HTML 结构)

**Files:**
- Modify: `web/admin.html:980-1100` (在 `tab-usage` 之后插入 `tab-audit`)
- Modify: `web/admin.html:1500-1600` (插入 `auditDetailDrawer` 侧边抽屉与遮罩)

- [x] **Step 1: 编写 `tab-audit` 主面板骨架与 KPI 卡片**

在 `web/admin.html` 中插入 `<div id="tab-audit" class="tab-content hidden space-y-6">`：
1. **顶栏标题与流式控制器**：
   - 标题：“📋 实时请求审计流 (Live Request Audit Stream)”。
   - 自动刷新控制器：
     - 下拉框 (`#auditRefreshInterval`: `0` 暂停, `2000` 2秒, `5000` 5秒, `10000` 10秒)。
     - 呼吸指示灯 (`#auditStreamIndicator`: 绿灯脉冲表示流式活动中，黄灯表示已暂停)。
     - 手动刷新按钮 (`#auditManualRefreshBtn`).
   - 导出按钮：“📥 导出 CSV” (`exportAuditAsCsv()`)、“📥 导出 JSON” (`exportAuditAsJson()`).
2. **4 个核心 KPI 指标卡**：
   - 窗口请求总量 (`#auditKpiTotalRequests`)
   - 平均响应延迟 (`#auditKpiAvgLatency`)
   - 异常错误率 (`#auditKpiErrorRate`，4xx/5xx 占比)
   - 风控干预次数 (`#auditKpiGuardrailActions`，blocked/masked 汇总)
3. **多维筛选过滤栏**：
   - API Key 过滤下拉 (`#auditFilterKey`)
   - 模型过滤下拉 (`#auditFilterModel`)
   - 状态码过滤 (`#auditFilterStatus`: 全部, 2xx 成功, 4xx 客户端错误, 5xx 服务端错误)
   - 风控动作过滤 (`#auditFilterGuardrail`: 全部, 无干预 none, 敏感词脱敏 masked, 阻断拦截 blocked)
   - 关键词搜索框 (`#auditFilterKeyword`, 实时模糊匹配模型、Key 或状态码)
4. **实时审计流数据表格 (`#auditTable`)**：
   - 列定义：时间戳、API Key、模型、供应商、HTTP 状态、Tokens (Prompt / Comp / Total)、耗时、风控处理、操作。
   - 表体容器 (`#auditTableBody`) 与 空状态提示 (`#auditEmptyState`)。
   - 分页栏容器 (`#auditPaginationBar`，复用分页器控件组件).

- [x] **Step 2: 编写侧边滑出抽屉 (`auditDetailDrawer`)**

在 `web/admin.html` 的抽屉/模态框区域插入：
1. 背景遮罩 (`#auditDetailBackdrop`, 点击自动关闭)。
2. 侧边抽屉容器 (`#auditDetailDrawer`, `fixed top-0 right-0 w-full max-w-xl h-full bg-slate-900 shadow-2xl`，支持平滑滑入滑出 `translate-x-full transition-transform`):
   - 顶栏：请求详情标题、关闭按钮 (✕)。
   - **核心概览卡**：HTTP 状态徽章、时间戳 (本地时区/UTC)、耗时 (ms)、API Key ID、模型与供应商。
   - **Token 消耗分布图与指标**：
     - 4 格指标卡：Prompt Tokens、Completion Tokens、Cached Prompt Tokens、Reasoning Tokens。
     - 堆叠水平彩色进度条直观呈现比例。
   - **安全与风控动作卡**：展示 Guardrail Action（彩色徽章及文字说明）。
   - **原始 JSON 元数据查看器**：
     - 高亮代码块，一键“📋 复制 JSON”按钮。

- [x] **Step 3: 验证 HTML 嵌套与样式编译**

Run: `ninja generate_admin_ui_html aigate`  
Expected: 成功重新生成 C 头文件并编译通过。

- [x] **Step 4: Commit Task 4**

```bash
git add web/admin.html
git commit -m "feat(ui): 📐 add live request audit stream HTML skeleton and detail slide-over drawer"
```

---

### Task 5: 📋 实时请求审计流交互逻辑与数据处理 (JavaScript)

**Files:**
- Modify: `web/admin.html:3000-3500` (审计日志 JS 模块)

- [x] **Step 1: 编写流式轮询控制器与生命周期管理**

在 `web/admin.html` 的 JS 中实现：
1. `auditState`:
   ```javascript
   const auditState = {
     interval: 5000,
     timer: null,
     records: [],
     filtered: [],
     selectedRecord: null,
     isFetching: false
   };
   ```
2. `startAuditPolling()`:
   - 清除已有 timer。
   - 立刻触发一次 `fetchAuditRequests(true)`。
   - 若 `auditState.interval > 0`，启动 `setInterval(...)` 并在 UI 状态灯展示绿灯脉冲。
3. `stopAuditPolling()`:
   - 清除 timer，更新 UI 状态灯为黄色暂停。
4. `setAuditRefreshInterval(val)`:
   - 动态更新间隔并重置轮询。

- [x] **Step 2: 编写审计日志拉取、KPI 计算与多维过滤**

1. `fetchAuditRequests(silent = false)`:
   - 调用 `apiCall('/admin/v1/usage/requests?limit=100')`。
   - 将返回的 `requests` 存入 `auditState.records`。
   - 计算并更新 KPI 指标卡：
     - 总请求数：`records.length`
     - 平均延迟：`sum(latency_ms) / count`
     - 错误率：`count(status >= 400) / count * 100%`
     - 风控干预数：`count(guardrail_action != "none" && guardrail_action != "")`
   - 触发 `filterAuditRequests(false)`.
2. `filterAuditRequests(resetPage = false)`:
   - 结合 Key、Model、Status（2xx/4xx/5xx）、Guardrail 以及关键词输入框进行复合过滤。
   - 分页切片并调用 `renderAuditTable()`.
3. `renderAuditTable()`:
   - 渲染行：时间格式化、色彩徽章（200 OK 绿, 4xx 橙, 5xx 红）、耗时（<500ms 绿, <2s 黄, >=2s 红）、风控动作（blocked 红, masked 黄, none 灰）。
   - 每行末尾附带“查看详情”按钮，绑定点击事件。

- [x] **Step 3: 编写侧边滑出抽屉与导出功能**

1. `openAuditDetail(record)`:
   - 将选中的单条请求数据注入 `#auditDetailDrawer`。
   - 填充 Token 指标与百分比条。
   - 填充 JSON 预览框。
   - 移除 `translate-x-full` 并移除遮罩 `hidden`，展现平滑滑入动画。
2. `closeAuditDetail()`:
   - 添加 `translate-x-full`，关闭遮罩。
3. `copyAuditDetailJson()`: 复制当前请求的原始 JSON 结构。
4. `exportAuditAsCsv()` 与 `exportAuditAsJson()`:
   - 基于当前过滤后的 `auditState.filtered` 列表生成 Blob 数据并触发浏览器原生下载。

- [x] **Step 4: 编译验证**

Run: `ninja generate_admin_ui_html aigate`  
Expected: 成功构建无警告。

- [x] **Step 5: Commit Task 5**

```bash
git add web/admin.html
git commit -m "feat(ui): ⚡ implement live request audit stream polling, KPIs, drawer and export"
```

---

### Task 6: 端到端集成测试与完整功能验证

**Files:**
- Modify: `tests/integration/test_gateway.py` (添加针对 Prompt 模板与审计日志的前端元素与接口端到端测试)

- [x] **Step 1: 在 `test_gateway.py` 中编写 `test_admin_ui_prompt_and_audit_features`**

测试内容包含：
1. 访问 `/admin` 获取 HTML 交付内容，断言包含：
   - 导航：`#nav-prompts`, `#nav-audit`, `#nav-prompts-side`, `#nav-audit-side`.
   - 主面板：`#tab-prompts`, `#tab-audit`.
   - Prompt 关键控件：`#promptEditTextarea`, `#promptEditMode`, `#promptVariableInputsContainer`, `#promptRenderedPreview`, `#promptTargetSelect`.
   - 审计日志关键控件：`#auditRefreshInterval`, `#auditKpiTotalRequests`, `#auditTableBody`, `#auditDetailDrawer`.
2. 测试 Prompt 模板后端持久化联动：
   - 创建测试 Model 与 Key。
   - 发送带有 `system_prompt` 与 `prompt_mode` 的更新请求，验证后端存储与检索。
3. 测试审计请求日志 API (`GET /admin/v1/usage/requests`) 兼容性与结构完整性。

- [x] **Step 2: 运行集成测试验证**

Run: `pytest tests/integration/test_gateway.py -k "test_admin_ui_prompt_and_audit_features" -v`  
Expected: 1 passed, 0 failed.

- [x] **Step 3: 运行全量 Admin UI 与 Gateway 测试**

Run: `pytest tests/integration/test_gateway.py -k "admin_ui" -v`  
Expected: All admin UI tests pass.

- [x] **Step 4: 最终构建与代码规范检查**

Run: `git status` 与 `ninja aigate`  
Expected: 构建完全 clean，无未跟踪多余文件。

- [x] **Step 5: Commit Task 6**

```bash
git add tests/integration/test_gateway.py
git commit -m "test(admin_ui): 🧪 add integration tests for prompt template and audit log features"
```

---

## 验证与验收标准

1. **构建集成验证：** `ninja generate_admin_ui_html aigate` 编译 0 warning，生成的 `admin_ui_html.h` 正确包含全部新增面板与 JS 逻辑。
2. **界面交互验证：**
   - 访问 `/admin`，侧边栏与移动端均能无缝切换至 `📝 Prompt 模板` 与 `📋 审计日志`。
   - Prompt 模板能够动态提取变量 `{{var}}`，输入变量时实时在下方渲染插值结果并计算 Token 估算；能够一键同步到指定 Model 或 Key。
   - 审计日志流能够以设置的间隔（2s/5s/10s）自动轮询 `/admin/v1/usage/requests`，支持暂停，点击单行能滑出侧边抽屉查看 Token 比例和 JSON 详情，并支持一键导出 CSV/JSON。
3. **自动化测试验证：** `pytest tests/integration/test_gateway.py -k "admin_ui"` 100% 通过。

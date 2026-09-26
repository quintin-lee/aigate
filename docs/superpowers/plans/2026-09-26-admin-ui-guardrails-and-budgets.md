# Admin 控制台安全风控管理与月度预算集成实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 aigate 嵌入式 Web 控制台 (`web/admin.html`) 增加「安全风控管理 (Guardrails)」专区、实时规则测试沙箱、一键预置 PII 规则，并在 API 密钥、部门分组与请求审计中深度集成月度预算与风控过滤。

**Architecture:** 保持 aigate 单二进制嵌入式前端架构（Vanilla JS + Tailwind CSS + 原生 CSS 变量），由 `scripts/embed_html.py` 将 `web/admin.html` 转换为 C 头文件 `src/admin_ui_html.h` 供 civetweb HTTP 服务直接交付。全量复用 `apiCall()` 统一请求通信与异常处理机制。

**Tech Stack:** HTML5, Tailwind CSS Utility, Vanilla ES6+ JavaScript, CMake, Python 3 (嵌入转换与 Pytest 集成测试), C17.

---

## 文件影响范围映射

- **修改前端单页源文件：**
  - `web/admin.html`: 增加风控 Tab、测试沙箱、规则表格、抽屉表单；增加 keys/groups 月度预算列与表单项；增加 requests 表格风控徽章与过滤。
- **构建生成的 C 头文件：**
  - `src/admin_ui_html.h`（由 CMake 目标 `generate_admin_ui_html` 自动基于 `web/admin.html` 重新生成）。
- **自动化测试文件：**
  - `tests/integration/test_gateway.py`: 增加 Admin UI 风控与预算元素验证。

---

## 任务拆分列表

### Task 1: 侧边栏导航与基础骨架扩充

**Files:**
- Modify: `web/admin.html:225-275` (桌面与移动端导航), `web/admin.html:1570-1650` (`switchTab` & `tabTitles`)

- [x] **Step 1: 在侧边导航栏与移动端抽屉中增加「🛡️ 安全风控」入口**

在 `web/admin.html` 侧边导航的「组织与成本」之前增加「安全与合规」分组标题与导航按钮：
```html
<div class="side-section-title pt-3">安全与合规</div>
<button data-tab="guardrails" onclick="switchTab('guardrails')" class="side-link" id="nav-guardrails">
  <span class="text-base">🛡️</span>
  <span>安全风控</span>
</button>
```
在移动端水平抽屉导航中同步加入该按钮。

- [x] **Step 2: 更新 `switchTab`、`tabTitles` 与 `refreshCurrentTab` 映射**

在 `web/admin.html` 的 JS 部分：
1. `tabTitles` 字典中添加 `guardrails: "安全风控"`。
2. `refreshCurrentTab()` 中增加分支：
```javascript
} else if (state.activeTab === "guardrails") {
  fetchGuardrails();
}
```

- [x] **Step 3: 运行 CMake 重新生成头文件并验证编译**

Run: `cmake --build .build -j$(nproc)`
Expected: Build succeeds with 0 warnings.

- [x] **Step 4: Commit Task 1**

```bash
git add web/admin.html
git commit -m "feat(ui): 🧭 add guardrails navigation items and tab switching router"
```

---

### Task 2: 🛡️ 安全风控管理面板结构与弹窗模态框 (HTML 模板)

**Files:**
- Modify: `web/admin.html:800-1100` (主内容区增加 `tab-guardrails` 面板与抽屉 Modal)

- [x] **Step 1: 编写 `tab-guardrails` 面板骨架与统计指标卡**

在 `web/admin.html` 的 main 容器内添加：
```html
<section id="tab-guardrails" class="tab-content hidden space-y-6">
  <!-- 顶栏标题与操作 -->
  <div class="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-4">
    <div>
      <h2 class="text-xl font-bold tracking-tight text-white flex items-center gap-2">
        <span>🛡️</span> 安全风控规则管理
      </h2>
      <p class="text-xs text-slate-400 mt-1">基于 Aho-Corasick 敏感词过滤与正则脱敏引擎，提供毫秒级入站防护与隐私脱敏。</p>
    </div>
    <div class="flex items-center gap-2">
      <button onclick="reloadGuardrailsEngine()" class="px-3.5 py-1.5 rounded-lg text-xs font-medium bg-slate-800 hover:bg-slate-700 text-slate-200 border border-slate-700 transition flex items-center gap-1.5 shadow-sm">
        <span>🔄</span> <span>热重载引擎</span>
      </button>
      <button onclick="openGuardrailModal()" class="px-3.5 py-1.5 rounded-lg text-xs font-semibold bg-brand-600 hover:bg-brand-500 text-white transition flex items-center gap-1.5 shadow-sm">
        <span>+</span> <span>新建规则</span>
      </button>
    </div>
  </div>

  <!-- 4 个统计指标卡 -->
  <div class="grid grid-cols-2 lg:grid-cols-4 gap-4">
    <div class="card p-4">
      <div class="text-[11px] text-slate-400 font-medium">有效规则总数</div>
      <div id="grStatTotal" class="text-2xl font-bold text-sky-400 mt-1">0</div>
    </div>
    <div class="card p-4">
      <div class="text-[11px] text-slate-400 font-medium">敏感词阻断 (Block)</div>
      <div id="grStatKeyword" class="text-2xl font-bold text-rose-400 mt-1">0</div>
    </div>
    <div class="card p-4">
      <div class="text-[11px] text-slate-400 font-medium">隐私脱敏 (Mask)</div>
      <div id="grStatPii" class="text-2xl font-bold text-amber-400 mt-1">0</div>
    </div>
    <div class="card p-4">
      <div class="text-[11px] text-slate-400 font-medium">引擎同步状态</div>
      <div id="grStatStatus" class="text-sm font-bold text-emerald-400 mt-2 flex items-center gap-1">
        <span class="w-2 h-2 rounded-full bg-emerald-400"></span> <span>已就绪</span>
      </div>
    </div>
  </div>
```

- [x] **Step 2: 编写在线实时规则测试沙箱 (Rule Sandbox)**

在统计卡下方增加：
```html
  <div class="card p-4 border border-slate-800 bg-slate-900/60">
    <div class="flex items-center justify-between mb-2">
      <div class="text-xs font-bold text-slate-200 flex items-center gap-1.5">
        <span>🧪</span> <span>实时规则测试沙箱 (Rule Sandbox)</span>
      </div>
      <span class="text-[10px] text-slate-500">本地内存仿真，不消耗调用配额</span>
    </div>
    <div class="flex flex-col sm:flex-row gap-2">
      <input type="text" id="grSandboxInput" placeholder="输入测试语句，例如：联系电话 13812345678，请勿 DROP TABLE。" class="form-input flex-1 text-xs" />
      <button onclick="testGuardrailsSandbox()" class="px-4 py-1.5 rounded-lg text-xs font-medium bg-slate-800 hover:bg-slate-700 text-white border border-slate-700 transition">
        执行匹配测试
      </button>
    </div>
    <div id="grSandboxResult" class="hidden mt-2 p-2.5 rounded-lg text-xs font-mono"></div>
  </div>
```

- [x] **Step 3: 编写规则数据表格与新建/编辑抽屉 (Modal)**

在沙箱下方增加：
- 快速预置按钮（`⚡ 一键预置常用 PII 规则`）与类型过滤选择框。
- 规则表格容器（ID、类型徽章、Pattern 代码块、处置动作、分类、启用开关、操作按钮）。
- 在页面底部新增 `#guardrailModal`（包含 `grFormType`、`grFormPattern`、`grFormAction`、`grFormCategory`、`grFormEnabled` 控件）。

- [x] **Step 4: 运行 CMake 重新编译验证无语法错误**

Run: `cmake --build .build -j$(nproc)`
Expected: Build succeeds.

- [x] **Step 5: Commit Task 2**

```bash
git add web/admin.html
git commit -m "feat(ui): 🎨 scaffold guardrails management panel and rule modal templates"
```

---

### Task 3: 🛡️ 安全风控前端 JS 交互逻辑

**Files:**
- Modify: `web/admin.html:2800-3100` (脚本区加入风控管理完整业务逻辑)

- [x] **Step 1: 规则数据获取与指标卡渲染 (`fetchGuardrails`, `renderGuardrailsTable`)**

实现 `fetchGuardrails()`：
- 调用 `apiCall("/admin/v1/guardrails?limit=1000")` 获取规则列表。
- 计算统计数据并填充 `grStatTotal`, `grStatKeyword`, `grStatPii`。
- 渲染表格行，展示每条规则的 ID、类型（`keyword` 蓝标 / `pii` 紫标 / `regex` 青标）、模式（`<code>`）、处置动作（`🛑 拦截` / `🛡️ 脱敏`）、分类标签、启用 Toggle Switch、编辑与删除按钮。

- [x] **Step 2: 规则新增、修改与删除交互 (`submitGuardrailForm`, `toggleGuardrailRule`, `deleteGuardrailRule`)**

- `openGuardrailModal(rule)`: 打开抽屉并填充表单（支持新建或编辑模式）。
- `submitGuardrailForm()`: 校验表单字段，根据是否具备 ID 发起 `POST /admin/v1/guardrails` 或 `PATCH /admin/v1/guardrails/:id`，成功后提示 Toast 并重新拉取列表。
- `toggleGuardrailRule(id, newEnabled)`: 即时发起 PATCH 请求更新 `enabled`，局部刷新开关状态。
- `deleteGuardrailRule(id)`: 弹出确认后发起 `DELETE /admin/v1/guardrails/:id`，成功后刷新列表。
- `reloadGuardrailsEngine()`: 发起 `POST /admin/v1/guardrails/reload`，更新引擎状态。

- [x] **Step 3: 一键预置常用 PII 规则与沙箱测试实现 (`seedDefaultPiiRules`, `testGuardrailsSandbox`)**

- `seedDefaultPiiRules()`: 批量顺序注册 4 条常用 PII 规则（手机号 `[PHONE]`、身份证 `[ID_CARD]`、邮箱 `[EMAIL]`、API Key `[API_KEY]`），遇到已存在的跳过，完成后刷新列表。
- `testGuardrailsSandbox()`:
  - 读取输入文本。
  - 在前端直接针对现有规则列表执行模式测试：
    1. 敏感词规则：大小写不敏感匹配；
    2. PII 规则：正则测试（手机号、邮箱等）；
  - 渲染高亮告警框，明确指出命中阻断（红色）、脱敏结果（黄色）或通过（绿色）。

- [x] **Step 4: 编译验证**

Run: `cmake --build .build -j$(nproc)`
Expected: Build succeeds.

- [x] **Step 5: Commit Task 3**

```bash
git add web/admin.html
git commit -m "feat(ui): ⚡ implement guardrails CRUD, sandbox tester, and PII seed logic"
```

---

### Task 4: 🔑 API 密钥与部门分组界面增强 (月度预算与风控)

**Files:**
- Modify: `web/admin.html:2420-2650` (Keys 表格与表单), `web/admin.html:2650-2800` (Groups 表格与表单)

- [x] **Step 1: API 密钥表格与模态框扩展**

1. 在 `keys` 表格表头增加 `月度费用预算`、`月度 Token 预算`、`安全风控` 列。
2. 在 `renderKeysTable()` 中增加相应单元格内容：
   - 费用预算：`$20.00 / 月` 或 `不限`。
   - Token 预算：`500,000 / 月` 或 `不限`。
   - 风控状态：`🛡️ 启用` (绿标) 或 `✕ 禁用` (灰标)。
3. 在 `#keyModal` 表单中增加：
   - `kFormMonthlyCostBudget`（数值，默认 0.0）
   - `kFormMonthlyTokenBudget`（整数，默认 0）
   - `kFormGuardrailsEnabled`（Checkbox，默认 checked）
4. 在 `submitKeyForm()` 中将这三个字段打包进请求体。

- [x] **Step 2: 部门分组表格与模态框扩展**

1. 在 `groups` 表格表头增加 `月度费用限额` 列。
2. 在 `renderGroupsTable()` 中展示 `$100.00 / 月` 或 `不限`。
3. 在 `#groupModal` 表单中增加 `gFormMonthlyBudget`（数值，默认 0.0）。
4. 在 `submitGroupForm()` 中将 `monthly_budget_usd` 字段提交。

- [x] **Step 3: 编译验证**

Run: `cmake --build .build -j$(nproc)`
Expected: Build succeeds.

- [x] **Step 4: Commit Task 4**

```bash
git add web/admin.html
git commit -m "feat(ui): 💰 add monthly budgets and guardrail toggles to keys and groups panels"
```

---

### Task 5: 📈 请求明细与审计日志风控增强

**Files:**
- Modify: `web/admin.html:2800-2950` (用量审计明细与表格渲染)

- [x] **Step 1: 请求明细表格增加「风控处置」列**

在用量审计表格的表头与行模板中加入风控状态展示：
- 若 `r.guardrail_action === 'blocked'`：展示 `<span class="px-1.5 py-0.5 rounded text-[10px] font-bold bg-rose-500/20 text-rose-400 border border-rose-500/30">🛑 拦截</span>`。
- 若 `r.guardrail_action === 'masked'`：展示 `<span class="px-1.5 py-0.5 rounded text-[10px] font-bold bg-amber-500/20 text-amber-400 border border-amber-500/30">🛡️ 脱敏</span>`。
- 否则展示 `<span class="text-slate-500 text-[10px]">正常</span>`。

- [x] **Step 2: 顶部过滤工具条增加风控动作下拉筛选**

在审计表格顶部增加下拉菜单：
```html
<select id="reqGuardrailFilter" onchange="filterUsageRequestsTable()" class="form-input text-xs py-1">
  <option value="all">风控处置: 全部</option>
  <option value="blocked">🛑 仅拦截 (blocked)</option>
  <option value="masked">🛡️ 仅脱敏 (masked)</option>
  <option value="pass">正常透传 (pass)</option>
</select>
```
在 `filterUsageRequestsTable()` 中增加对 `guardrail_action` 的匹配过滤。

- [x] **Step 3: 编译验证**

Run: `cmake --build .build -j$(nproc)`
Expected: Build succeeds.

- [x] **Step 4: Commit Task 5**

```bash
git add web/admin.html
git commit -m "feat(ui): 🔍 add guardrail action badges and filtering in request audit logs"
```

---

### Task 6: 自动化测试与全量回归验证

**Files:**
- Modify: `tests/integration/test_gateway.py`

- [x] **Step 1: 在 `test_gateway.py` 中更新 `test_admin_ui_endpoints` 并新增 `test_admin_ui_guardrails_features`**

验证：
1. `GET /admin` 返回的 HTML 包含 `tab-guardrails`、`nav-guardrails`、`grSandboxInput`、`kFormMonthlyCostBudget`、`reqGuardrailFilter`。
2. 运行集成测试验证端到端交付。

- [x] **Step 2: 执行全量单元测试与集成测试套件**

1. C 单元测试：`ctest --test-dir .build -R unit --output-on-failure`（预期 167/167 通过）。
2. Python E2E 测试：`pytest tests/integration/test_gateway.py -v`（预期 31+ 项全量通过）。

- [x] **Step 3: Commit Task 6**

```bash
git add tests/integration/test_gateway.py
git commit -m "test(ui): 🧪 add e2e integration tests for admin ui guardrails and budget controls"
```

---

## 7. 执行选项交接 (Execution Handoff)

请确认是否按照该计划开始执行。完成后您可以选择：
1. **Subagent-Driven (推荐)**：每个任务分配独立子代理执行并进行逐项审查。
2. **Inline Execution**：在当前会话中分步执行。

# Design Specification: Admin Web UI Prompt Template Management & Live Request Audit Stream

**Date:** 2026-10-01  
**Status:** In Review  
**Target:** `web/admin.html`, `scripts/embed_html.py`  
**Related Endpoints:** `/admin/v1/models`, `/admin/v1/keys`, `/admin/v1/usage/requests`, `/v1/chat/completions`

---

## 1. Overview & Objectives

In the previous iteration, AIGate successfully implemented the core C middleware filter chain for dynamic Prompt template injection (`system_prompt` and `prompt_mode`: `prepend`, `append`, `override`) supporting variables like `{{user_id}}`, `{{model}}`, `{{date}}`, and database migration v10.

This specification designs the **frontend management interface** in `web/admin.html`, adding two dedicated top-level tabs to the Admin Web UI:
1. **📝 Prompt 模板 (Prompt Templates)**: A centralized prompt management workspace featuring a preset template library, dynamic variable auto-detection, a live interpolation sandbox with token estimation, and bidirectional synchronization with Models and API Keys.
2. **📋 审计日志 (Request Audit Log Stream)**: A real-time observability workstation featuring live polling (with paused/interval controls), multi-dimensional filtering, KPI summary metric cards, interactive slide-over request drawers, and data export (CSV/JSON).

---

## 2. Navigation & Layout Architecture

### 2.1 Navigation Hierarchy
Two new first-class tabs are added to both the desktop sidebar and mobile navigation:
- **`prompts`** (`#nav-prompts` / `#nav-prompts-side`): "📝 Prompt 模板" (Placed between "🤖 模型" and "🔑 API 密钥")
- **`audit`** (`#nav-audit` / `#nav-audit-side`): "📋 审计日志" (Placed between "📈 用量" and "🛰️ 实时大屏")

```mermaid
graph TD
    A[Admin Web UI Sidebar & Header] --> B[Overview]
    A --> C[Providers]
    A --> D[Models]
    A --> E[📝 Prompt 模板 (New)]
    A --> F[Keys]
    A --> G[Guardrails]
    A --> H[Groups]
    A --> I[Cost]
    A --> J[Usage]
    A --> K[📋 审计日志 (New)]
    A --> L[Live Dashboard]
    A --> M[Cache]
    A --> N[Playground]
    A --> O[Metrics]
```

### 2.2 Tab Switching & Lifecycle
- Updated `switchTab(tabId)`:
  - When switching to `audit`: auto-starts the polling timer if active, immediately refreshes requests.
  - When switching away from `audit`: pauses background polling timers to conserve network bandwidth and client CPU.
  - When switching to `prompts`: loads models and keys list for dropdown binding, restores current active template.

---

## 3. Component Design: Prompt Templates Workspace (`tab-prompts`)

### 3.1 Screen Layout (Two-Column Desktop / Stacked Mobile)
- **Left Column (40% width on desktop): 模板库与绑定控制区 (Template Library & Model/Key Sync)**
  - **Header & Actions**: "新建模板", "重置预设", "导入/导出 JSON".
  - **Template List**: Card/List view displaying templates with tags (e.g. `System`, `Safety`, `Custom`), active indicator, and injection mode badges (`prepend`, `append`, `override`).
  - **Sync Panel (Model / Key 快速绑定)**:
    - Target Type Selector: `[绑定到模型 (Model)]` vs `[绑定到密钥 (Key)]`.
    - Dropdown: Choose specific registered Model or Key (fetched via `/admin/v1/models` and `/admin/v1/keys`).
    - Sync Button: "一键同步/更新到该配置" (sends `PUT /admin/v1/models/{id}` or `PUT /admin/v1/keys/{id}` with `system_prompt` and `prompt_mode`).
    - Reverse Action: "从选中目标反向读取 Prompt".

- **Right Column (60% width on desktop): 编辑器与变量调试沙盒 (Editor & Variable Sandbox)**
  - **Template Details Editor**:
    - Template Name & Description.
    - Injection Mode Selector: `prepend` (优先前置), `append` (尾部追加), `override` (完全覆盖).
    - System Prompt Textarea: High-contrast monospace editor with keyword variable highlighting helper (`{{var}}`).
    - Variable Quick-Insert Toolbar: Chips for `{{user_id}}`, `{{model}}`, `{{date}}`, `{{team}}`, `{{client_ip}}`.
  - **Dynamic Variable Detection & Sandbox**:
    - Scans template text with regex `/\{\{\s*([a-zA-Z0-9_\.\-]+)\s*\}\}/g` to dynamically detect all referenced variables.
    - Generates corresponding input fields for each detected variable with sensible defaults (e.g. `date` = current date YYYY-MM-DD, `user_id` = `usr_demo_01`).
    - Users can add custom key-value pairs.
  - **Live Preview & Token Metrics**:
    - Displays the final interpolated text in real-time.
    - Metric counters: Character count and estimated Tokens (~4 chars/token).
    - Action buttons: "复制渲染结果", "在调试场中运行测试 (Open in Playground)".

### 3.2 Built-in Presets
Built-in preset templates stored in client memory / `localStorage`:
1. **企业安全与道德规范 (Corporate Safety Policy)**:
   - Mode: `prepend`
   - Content: Standard compliance instructions ensuring harmless, honest, and enterprise-aligned outputs.
2. **严格 JSON 输出格式化 (Strict JSON Output Enforcer)**:
   - Mode: `append`
   - Content: Instructing the LLM to output valid JSON conforming to specified schemas without markdown wrappers.
3. **技术专家与代码审查 (Senior Software Engineer & Reviewer)**:
   - Mode: `override`
   - Content: Persona instructing clean code, security best practices, and edge case handling.
4. **客服应答与品牌声调 (Customer Support & Tone)**:
   - Mode: `prepend`
   - Content: Empathetic, polite, and brand-consistent support guidelines referencing `{{team}}` and `{{date}}`.

---

## 4. Component Design: Live Request Audit Stream (`tab-audit`)

### 4.1 Screen Layout
- **Top Row: Summary KPI Cards**
  - **窗口请求总数 (Recent Requests)**: Total number of requests in query window.
  - **平均响应耗时 (Avg Latency)**: Average round-trip latency in milliseconds.
  - **异常错误率 (Error Rate)**: Percentage of requests where HTTP status >= 400.
  - **风控干预拦截 (Guardrail Interventions)**: Total requests with `guardrail_action != "none"` (masked or blocked).

- **Toolbar: Stream Controls & Multi-Dimensional Filters**
  - **Auto-Refresh Controls**:
    - Dropdown: `暂停 (Paused)`, `2 秒`, `5 秒`, `10 秒`.
    - Status Indicator: Animated pulsing green dot when running, amber dot when paused.
    - Manual Refresh Button: Instant query.
  - **Filters**:
    - **API Key**: All Keys vs specific Key dropdown.
    - **Status Code**: All / 2xx 成功 (Success) / 4xx 客户端错误 (Client Err) / 5xx 服务端错误 (Server Err).
    - **Model**: All Models vs specific Model dropdown.
    - **Guardrail Action**: All / 无干预 (`none`) / 敏感词脱敏 (`masked`) / 阻断拦截 (`blocked`).
  - **Export Action**:
    - Download as CSV.
    - Download as JSON.

- **Data Table: Audit Logs Stream**
  - Columns:
    1. **时间 (Timestamp)**: ISO string formatted to local time `YYYY-MM-DD HH:mm:ss`.
    2. **API 密钥 (Key ID / Name)**: Badge displaying Key ID or alias.
    3. **模型 (Model)**: Model name tag.
    4. **供应商 (Provider)**: Upstream provider tag.
    5. **状态 (HTTP Status)**:
       - 200: Green badge
       - 4xx: Yellow badge
       - 5xx: Red badge
    6. **Token 消耗 (Tokens)**: Compact breakdown `P: {prompt} / C: {comp} (Σ {total})`.
    7. **耗时 (Latency)**: Latency in ms (color-coded: <500ms green, <2000ms yellow, >=2000ms red).
    8. **风控动作 (Guardrail)**:
       - `none`: Subtle grey
       - `masked`: Warning amber
       - `blocked`: Danger red
    9. **操作**: "详情 (Detail)" button.

- **Slide-over Drawer: Request Inspector (侧边滑出抽屉)**
  - Clicking any row or "详情" opens a full slide-over panel:
    - **Header**: Request timestamp, HTTP status badge, Key ID, Model.
    - **Token Distribution Visualizer**:
      - Stacked visual bar + stats cards: Prompt Tokens, Completion Tokens, Cached Prompt Tokens, Reasoning Tokens.
    - **Performance & Guardrail Breakdown**:
      - Latency (ms), Guardrail Action.
    - **Quick Navigations**:
      - "跳转至该 API Key" / "跳转至该 Model".
    - **Raw Data Viewer**:
      - Formatted JSON inspector with one-click copy.

---

## 5. Technical Implementation Details

### 5.1 Architecture & Single-File Guarantee
- All code resides directly inside `web/admin.html`.
- No Node.js / Webpack / Vite build tools required.
- Uses standard DOM APIs and existing helpers:
  - `apiCall(path, method, body)` for asynchronous API operations.
  - `showNotification(msg, type)` for toast feedback.
  - `formatNumber`, `escapeHtml` for safe rendering.
- `scripts/embed_html.py` compiles `web/admin.html` into `build/generated/admin_ui_html.h` without breakage.

### 5.2 State Management
```javascript
// Prompt templates state
const promptState = {
  presets: [],          // array of template objects
  activePresetId: null, // current selected template ID
  variables: {},        // detected variables { key: value }
  models: [],           // fetched from /admin/v1/models
  keys: []              // fetched from /admin/v1/keys
};

// Audit stream state
const auditState = {
  autoRefreshInterval: 5000, // ms, 0 = paused
  timerId: null,
  filters: {
    key_id: '',
    status: '',
    model: '',
    guardrail: ''
  },
  page: 1,
  limit: 50,
  records: [],
  selectedRecord: null
};
```

### 5.3 Error Handling & Resilience
- Network failure during polling will show subtle offline indicator without breaking page state.
- Graceful degradation when `localStorage` is disabled or full.
- Clean timer cancellation on tab switch prevents memory leaks and spurious background queries.

---

## 6. Verification & Test Plan

1. **Compilation & Embed Test**:
   - Run `python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h`.
   - Run `ninja generate_admin_ui_html aigate` to ensure C header compilation succeeds cleanly.
2. **Visual & Responsive Verification**:
   - Check desktop resolution (1440px): Sidebar items, two-column layout for prompts, wide table for audit logs.
   - Check mobile resolution (<768px): Header drawer, tabs horizontal scroller, stacked layouts.
3. **Prompt Sandbox Functional Verification**:
   - Edit template with `{{user_id}}` and custom `{{dept}}`.
   - Verify variables dynamically appear in the inputs section.
   - Verify typing in variable inputs immediately updates the preview.
   - Verify "同步至模型" and "同步至密钥" successfully calls backend PUT APIs and persists data.
4. **Audit Log Functional Verification**:
   - Verify `/admin/v1/usage/requests` results render in table.
   - Test filters: status code, guardrail action, key id.
   - Test auto-refresh interval switching and pausing.
   - Test slide-over drawer opening and JSON copying.
   - Test CSV and JSON export downloads.

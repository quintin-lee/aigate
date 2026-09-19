# 管理后台视觉重设计 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 对 `web/admin.html` 落定 4 节视觉重设计（全深色侧边导航 / 图标统计卡 / 宽松表格 / 聊天气泡），只动样式层。

**Architecture:** 单文件原地改样式：`<style>` 兜底区加侧边栏 fallback + CSS 变量，HTML 骨架改导航与卡片，Chart.js 配置加渐变，三表行渲染与调试场输出渲染换肤。JS 数据逻辑、接口、ID 全部保留。

**Tech Stack:** Tailwind CDN（保留）、Chart.js（保留）、原生 JS、无构建步骤。

---

## File Structure

只动一个文件，5 个编辑区：

- `web/admin.html:28-46` — `<style>` 兜底区：加 CSS 变量 + 侧边栏/气泡/表格 fallback
- `web/admin.html:50-115` — header 顶栏 + `#mainNav` + `#mobileNav` 改侧边栏
- `web/admin.html:124-255` — 总览统计卡 + 图表卡 + 快捷操作
- `web/admin.html:275-414` + JS `renderModelsTable/renderKeysTable/renderUsageTable` — 三表
- `web/admin.html:479-504` + JS `runPlaygroundRequest` + `web/admin.html:535-699` — 调试场 + 5 弹窗

`switchTab` 依赖的 ID（`nav-overview…nav-metrics`、`tab-*`）、`connectionStatusBadge` 三态、`confirmModal` Promise 链一律保留。

---

### Task 1: 色板 token + 侧边栏骨架 + 瘦顶栏

**Files:**
- Modify: `web/admin.html:28-46`（style 兜底区）
- Modify: `web/admin.html:48-115`（body 布局 + header + 双导航）

- [ ] **Step 1: style 区追加 token 与侧边栏 fallback**

在 `web/admin.html:45`（`}` 闭合 `@media` 后、`</style>` 前）插入：

```css
:root {
  --bg-side: #0f172a; --bg-main: #1e293b; --bg-card: #0f172a;
  --line: #334155; --brand: #6366f1; --ok: #22c55e; --amber: #f59e0b;
  --blue: #3b82f6; --danger: #ef4444; --txt: #f1f5f9; --txt-dim: #94a3b8;
}
#sideNav { display: none; }
@media (min-width: 768px) {
  #sideNav { display: flex !important; }
  #mobileNav { display: none !important; }
}
```

- [ ] **Step 2: body 改横向布局**

`web/admin.html:48` 的 `flex flex-col` 改为 `flex`（侧边栏 + 右列）：

```html
<body class="bg-slate-950 text-slate-100 min-h-screen flex font-sans selection:bg-brand-500 selection:text-white">
```

- [ ] **Step 3: header 改为侧边栏 + 瘦顶栏**

将 `web/admin.html:51-105` 的 `<header>` 替换为：左边 `aside#sideNav`（保留 `id="mainNav"` 的 nav 与 6 个 `nav-overview…nav-metrics` 按钮、文案、emoji、onclick 不变），右边瘦顶栏（保留 `connectionStatusBadge`、`tokenBtn`、`refreshCurrentTab` 按钮不变）。移动端 `web/admin.html:108` 的 `#mobileNav` 整块保留不动。

- [ ] **Step 4: 校验 ID 无丢失**

Run: `grep -o 'id="\(mainNav\|mobileNav\|nav-overview\|nav-models\|nav-keys\|nav-usage\|nav-playground\|nav-metrics\|connectionStatusBadge\|tokenBtn\)"' web/admin.html | sort | uniq -c`
Expected: 每个 ID 恰好出现 1 次（`mobileNav` 在 CSS 出现 2 次是允许的，用 `id="mobileNav"` 精确匹配应为 1 次）

- [ ] **Step 5: Commit**

```bash
git add web/admin.html
git commit -m "feat(ui): ✨ add sidebar navigation with dark palette tokens"
```

---

### Task 2: 总览统计卡图标化 + 图表靛蓝渐变

**Files:**
- Modify: `web/admin.html:126-174`（4 张统计卡）
- Modify: `web/admin.html:179-196`（图表卡与图例）
- Modify: `web/admin.html:921-939`（`renderOverviewChart` 数据集与 scales）

- [ ] **Step 1: 统计卡改图标卡结构**

4 张卡统一改为（示例第 1 卡，其余换图标/底色/ID，ID `statActiveModels` 等保留）：

```html
<div class="glass-panel p-5 rounded-2xl relative overflow-hidden group hover:border-slate-700 transition flex items-center space-x-4">
  <div class="w-10 h-10 rounded-xl flex items-center justify-center text-xl shrink-0" style="background:rgba(99,102,241,.15)">🤖</div>
  <div class="min-w-0">
    <div class="text-xs font-semibold text-slate-400 uppercase tracking-wider">活跃模型</div>
    <div class="mt-1 flex items-baseline space-x-2">
      <div id="statActiveModels" class="text-3xl font-extrabold text-white">0</div>
      <div id="statTotalModelsBadge" class="text-xs px-2 py-0.5 rounded-md bg-indigo-500/10 text-indigo-400 border border-indigo-500/20">共 0 个</div>
    </div>
  </div>
</div>
```

4 卡图标/底色：🤖 靛 `rgba(99,102,241,.15)` / 🔑 绿 `rgba(34,197,94,.15)` / 📊 琥珀 `rgba(245,158,11,.15)` / 🪙 蓝 `rgba(59,130,246,.15)`。原第三行小字描述删除。

- [ ] **Step 2: 图表改渐变面积**

`web/admin.html:921` 的 `new Chart(ctx, {` 改为先建渐变再传参：

```js
const g1 = ctx.getContext("2d").createLinearGradient(0, 0, 0, 256);
g1.addColorStop(0, "rgba(99,102,241,0.55)");
g1.addColorStop(1, "rgba(99,102,241,0.02)");
const g2 = ctx.getContext("2d").createLinearGradient(0, 0, 0, 256);
g2.addColorStop(0, "rgba(34,211,238,0.45)");
g2.addColorStop(1, "rgba(34,211,238,0.02)");
state.overviewChart = new Chart(ctx, {
  type: "line",
  data: {
    labels: labels,
    datasets: [
      { label: "提示 Token", data: ptokData, borderColor: "#6366f1", backgroundColor: g1, fill: true, tension: 0.35, pointRadius: 0, borderWidth: 2 },
      { label: "补全 Token", data: ctokData, borderColor: "#22d3ee", backgroundColor: g2, fill: true, tension: 0.35, pointRadius: 0, borderWidth: 2 }
    ]
  },
  options: { /* 保持原 options 不变 */ }
```

注意：原 `type: "bar"` 改 `type: "line"`，删掉两处 `borderRadius: 6`（线图无此属性）。

- [ ] **Step 3: JS 语法检查**

Run: `python3 -c "import re;h=open('web/admin.html').read();s=re.findall(r'<script>(.*?)</script>',h,re.S);open('/tmp/cfg.js','w').write(s[0]);open('/tmp/core.js','w').write(s[1])" && node --check /tmp/cfg.js && node --check /tmp/core.js`
Expected: 无输出（两脚本均通过）

- [ ] **Step 4: Commit**

```bash
git add web/admin.html
git commit -m "feat(ui): ✨ restyle overview stat cards and gradient chart"
```

---

### Task 3: 三表统一（宽松行高 + 描边徽标 + 胶囊筛选）

**Files:**
- Modify: `web/admin.html:279-286`、`320-327`、`395-404`（三表 thead）
- Modify: `web/admin.html:956-995`（`renderModelsTable`）、`1151-1191`（`renderKeysTable`）、`1366-1393`（`renderUsageTable`）
- Modify: `web/admin.html:266,307`（搜索框）、`351-388`（用量筛选栏）

- [ ] **Step 1: thead 加 sticky**

三处 `<tr class="border-b border-slate-800 bg-slate-900/60 …">` 改为 `bg-slate-950 … sticky top-0 z-10`（其余类保留），并把三处 `th`/`td` 的 `py-3` 改为 `py-4`（52px 宽松行高）。

- [ ] **Step 2: 行渲染 hover 与徽标描边化**

三处 `tr.className = "hover:bg-slate-900/40 transition"` 改为 `"hover:bg-slate-800/60 transition"`。
状态徽标统一为描边胶囊（示例模型表启用态，其余同理只换色与文案）：

```js
`<button onclick="toggleModelEnabled('${m.name}', false)" class="px-2.5 py-0.5 rounded-full text-[10px] font-semibold bg-transparent text-emerald-400 border border-emerald-500/50 hover:bg-emerald-500/10 transition">● 已启用</button>`
```

`renderKeysTable` 的活跃/已吊销、`renderModelsTable` 的停用态与渠道徽标（Anthropic 紫 / OpenAI 绿）同步去掉 `bg-*/10` 实底改为 `bg-transparent` + `/50` 描边。

- [ ] **Step 3: 筛选控件胶囊化**

`#modelSearchInput`、`#keySearchInput`、`#metricFilterInput`、用量筛选栏内 `select/input/button` 的 `rounded-xl` 改为 `rounded-full`（`px-3` 改为 `px-4` 保持呼吸感）。只改 class，不碰 `oninput/onchange/onclick`。

- [ ] **Step 4: 空状态加引导按钮**

`#modelsEmptyState`、`#keysEmptyState` 内追加一行次按钮（示例模型）：

```html
<div class="mt-3"><button onclick="openModelModal()" class="px-4 py-1.5 rounded-full text-xs font-semibold bg-indigo-600 hover:bg-indigo-500 text-white transition">+ 注册模型</button></div>
```

密钥表同理（`openKeyModal()`，emerald）。用量空状态保持纯文字（无创建动作可引导）。

- [ ] **Step 5: 检查 + Commit**

Run: `node --check /tmp/core.js`（沿用 Task 2 提取的文件；若已改动则重跑 Task 2 Step 3 的提取命令）
Expected: 无输出

```bash
git add web/admin.html
git commit -m "feat(ui): ✨ unify tables with roomy rows and outline badges"
```

---

### Task 4: 调试场气泡 + 5 弹窗统一

**Files:**
- Modify: `web/admin.html:492-494`（`#playOutputArea` 容器）
- Modify: `web/admin.html:1428-1560`（`runPlaygroundRequest` 输出写入）
- Modify: `web/admin.html:535-699`（5 个弹窗容器圆角）

- [ ] **Step 1: 输出区容器去 mono 底**

`#playOutputArea` 的 class 删除 `font-mono bg-slate-950/70`，改为 `space-y-3`（气泡纵向排列），保留 `flex-1 overflow-y-auto custom-scroll p-3 rounded-xl border border-slate-800/80 text-xs leading-relaxed`。

- [ ] **Step 2: 用户气泡在发送时插入**

在 `runPlaygroundRequest` 内 `outArea.innerText = ""` 之后（约 1452 行）插入：

```js
outArea.innerHTML = `<div class="flex justify-end"><div class="max-w-[85%] bg-indigo-600 text-white rounded-2xl rounded-br-md px-3.5 py-2.5 whitespace-pre-wrap">${usr.replace(/</g, "&lt;")}</div></div><div class="flex justify-start"><div id="playAssistantBubble" class="max-w-[85%] bg-slate-900 border border-slate-700 text-slate-200 rounded-2xl rounded-bl-md px-3.5 py-2.5 whitespace-pre-wrap min-h-[2.5em]"></div></div>`;
const assistantBubble = document.getElementById("playAssistantBubble");
```

并将函数内后续所有 `outArea.innerText = …` / `outArea.innerText += …` / `outArea.innerText = fullText` 改为对 `assistantBubble` 的同等操作（共 5 处：错误分支、非流分支、流式循环、`AbortError` 分支、catch 分支）。`clearPlaygroundResponse` 内 `playOutputArea` 的 `innerText = ""` 改为 `innerHTML = ""`。

- [ ] **Step 3: 弹窗统一 16px 圆角**

5 个弹窗内层容器 `rounded-2xl` 保持不变（已是 16px），只统一遮罩：确认已是 `bg-slate-950/80 backdrop-blur-sm`；主按钮已是靛蓝实心、次按钮描边——本步仅把 `#confirmModal` 的确认红按钮保留（危险操作语义），其余 4 个弹窗的取消按钮统一为 `px-4 py-2 rounded-xl text-xs text-slate-400 hover:text-white border border-slate-700 hover:border-slate-500 transition`（描边化）。改动 4 处 class，不碰 onclick。

- [ ] **Step 4: 检查 + Commit**

Run: Task 2 Step 3 的提取 + `node --check` 命令
Expected: 无输出

```bash
git add web/admin.html
git commit -m "feat(ui): ✨ restyle playground bubbles and modal buttons"
```

---

### Task 5: 回归验证与落地

**Files:** 无改动（验证 + 收尾）

- [ ] **Step 1: 无 CDN 回归**

Run: `grep -c 'id="sideNav"' web/admin.html && grep -c '#sideNav' web/admin.html && grep -o 'id="nav-[a-z]*"' web/admin.html | wc -l`
Expected: `1`、`2`（CSS 两处：基础 + media）、`6`

- [ ] **Step 2: 旧逻辑零残留**

Run: `grep -n 'outArea\.innerText' web/admin.html; grep -n 'type: "bar"' web/admin.html; grep -n 'hover:bg-slate-900/40' web/admin.html`
Expected: 三条命令均无输出

- [ ] **Step 3: 浏览器实地验证**

用 playwright 打开 `web/admin.html`（或本地静态服务）：1440px 与 375px 各截一屏，逐项对照 spec 第 4 节验收标准（Tab 切换、弹窗开合、ESC、无数据/加载三态）。发现错位就地修、重截。

- [ ] **Step 4: 最终 Commit（如有修复）**

```bash
git add web/admin.html
git commit -m "fix(ui): 🐛 align redesign details with spec acceptance criteria"
```

（如 Step 3 一次通过，则跳过本步，无空提交。）

---

## Self-Review

1. **Spec 覆盖**：第 1 节→Task 1（侧边栏/瘦顶栏/色板/移动端复用 mobileNav）；第 2 节→Task 2（图标卡/渐变图/快捷操作随卡片换肤——快捷操作按钮为 `bg-slate-900/80` 已与新卡片同色，无需单列）；第 3 节→Task 3（三表 sticky/hover/描边/胶囊/空状态；加载占位行仅文字无需换肤）；第 4 节→Task 4（气泡/表单胶囊随筛选栏统一/弹窗按钮层级/ESC 不动）。验收标准→Task 5。无遗漏。
2. **占位符扫描**：无 TBD/TODO/“类似 Task N”；每步含完整代码或精确命令。
3. **类型一致**：`assistantBubble` 在 Task 4 Step 2 定义、Step 2 内复用；`g1/g2` 在 Task 2 Step 2 内定义并使用；ID 白名单在 Task 1/5 两头呼应。一致。

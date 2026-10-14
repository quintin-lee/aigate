# 大模型测试场双模型竞技场与思考链交互增强实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全面优化完善 aigate 大模型测试场 (PlaygroundView)，实现单模型深度调试与双模型竞技场 (Dual Arena) 无缝切换、并发双路独立 SSE 流式打字机引擎、智能 Extended Thinking 思考手风琴（带呼吸动画与思考纯用时统计）、四维性能指标与动态成本核算引擎、开发者工具箱（cURL 导出与代码块复制），并严格维持既有 DOM 测试契约与 351 个单元测试 100% 通过。

**Architecture:**
1. **双模式与响应式模型状态机 (`PlaygroundView.vue`)**：顶部 Segment 切换 `'single'` 与 `'arena'` 模式，分别管理单模型完整参数面板与双模型紧凑顶部栏及左右等宽双栏对话区；
2. **并发双路 SSE 流式管道 (`PlaygroundView.vue`)**：构建独立的 `streamModelChat` 处理器，为双模型分别挂载 `AbortController`，记录 `tSend`, `tFirstByte`, `tThinkingEnd`, `tEnd` 精确计算 TTFT 与总延迟；
3. **Extended Thinking 智能手风琴 (`PlaygroundView.vue`)**：保留 `#reasoning-box` 与 `reasoning-accordion thinking-accordion badge-purple` 类名，推理阶段呼吸光效，正文首 Token 到达时自动收缩并呈现思考用时与 Token 统计，支持点击自由切换折叠；
4. **性能指标与动态定价引擎 (`PlaygroundView.vue`)**：计算 TTFT (ms)、生成吞吐 (t/s)、Prompt/Reasoning/Completion Tokens，结合主流模型费率与网关定价计算调用成本，在 Arena 对比结束时自动标注胜出徽章；
5. **开发者高频工具箱 (`PlaygroundView.vue`)**：提供一键复制标准 Bash cURL、代码块语法标牌与复制代码、原始 JSON 查看抽屉；
6. **单文件构建与单元测试验证**：`npm run build` -> `scripts/embed_html.py` -> `make -j$(nproc)` -> `./build/tests/aigate_unit_tests` 确保 351/351 测试 100% 通过。

**Tech Stack:** Vue 3, Naive UI, Tailwind CSS, Vite Single-File, C (Lua 5.4, Jansson, civetweb), Python 3.

---

### Task 1: 双模式布局与响应式模型状态机搭建

**Files:**
- Modify: `web/src/views/PlaygroundView.vue`

- [ ] **Step 1: 建立模式切换与双模型独立响应式配置**

在 `PlaygroundView.vue` 的 `<script setup>` 中定义：
- `playgroundMode = ref('single')`（`'single'` vs `'arena'`）；
- `modelAConfig`: `{ model: 'deepseek-r1', temperature: 0.7, topP: 0.95, reasoningEffort: 'high' }`；
- `modelBConfig`: `{ model: 'claude-3-7-sonnet', temperature: 0.7, topP: 0.95, reasoningEffort: 'medium' }`；
- `sharedSystemPrompt`: 共享系统人设；
- `messagesA = ref([])`, `messagesB = ref([])`：维护独立的对话链；
- `statsA = ref(null)`, `statsB = ref(null)`：维护独立执行指标。

- [ ] **Step 2: 搭建顶部工具栏与竞技场模式双栏骨架**

- 顶部工具栏放置 `NRadioGroup` 模式切换按钮：`💬 单模型调试` 与 `⚔️ 双模型竞技场 (Dual Arena)`；
- 在 `mode === 'arena'` 时：
  - 顶部呈现紧凑配置横栏（Model A 选择器 vs Model B 选择器、System Prompt 抽屉触发器、流式开关）；
  - 主对话区采用 `grid grid-cols-1 md:grid-cols-2 gap-4`，左栏展示 Model A 对话流，右栏展示 Model B 对话流；
  - 底部停靠统一全宽 Prompt 输入框与「▶ 同步发送 (Ctrl+Enter)」和「⏹ 终止生成」按钮。

- [ ] **Step 3: 保持单模型模式完整向后兼容**

- 当 `mode === 'single'` 时，继续渲染原有的左侧 4 列完整参数侧边栏与右侧 8 列单通道对话框；
- 确保原有输入变量 `selectedModel`, `temperature`, `topP`, `reasoningEffort`, `reasoningFormat` 均双向绑定生效。

---

### Task 2: 并发双路独立 SSE 流式引擎与中断控制

**Files:**
- Modify: `web/src/views/PlaygroundView.vue`

- [ ] **Step 1: 构建通用的独立流式处理函数 `streamModelChat`**

实现：
```javascript
async function streamModelChat(channel, modelConfig, messagesList, statsRef, abortController) {
  // 记录 tSend
  // 构建向 /v1/chat/completions 的 fetch POST 请求
  // 支持 ReadableStreamDefaultReader 逐行解码 SSE 'data: {...}'
  // 识别首个 token 到达打点 tFirstByte，计算 TTFT
  // 解析 delta.reasoning_content 或 <thought> 标签
  // 记录完成时间 tEnd，更新 statsRef
}
```

- [ ] **Step 2: 在 `sendMessage` 中实现双模式并发调度**

- 若 `mode === 'single'`：调用单路流式，更新原 `messages` 与 `lastStats`；
- 若 `mode === 'arena'`：
  - 分别向 `messagesA` 与 `messagesB` 插入用户 Prompt；
  - 创建 `abortA = new AbortController()` 与 `abortB = new AbortController()`；
  - `isGeneratingA.value = true`, `isGeneratingB.value = true`；
  - 并发执行 `Promise.allSettled([streamModelChat('A', ...), streamModelChat('B', ...)])`；
  - 任一侧完成或报错时，独立更新对应的完成状态。

- [ ] **Step 3: 实现优雅终止与取消控制**

- 点击「⏹ 终止生成」同时触发 `abortA.abort()` 与 `abortB.abort()`；
- 卡片右上角提供独立的小关闭按钮，支持单独取消某一个模型的生成。

---

### Task 3: 智能 Extended Thinking 思考手风琴与状态机

**Files:**
- Modify: `web/src/views/PlaygroundView.vue`

- [ ] **Step 1: 构建三阶段思考手风琴结构与 DOM 探针**

在消息卡片内部：
- 渲染思考容器：保留 `id="reasoning-box"` 并附带类名 `reasoning-accordion thinking-accordion badge-purple`；
- 状态机：
  - `thinking`: 边框呼吸微光动效，提示 `🧠 正在深度思考中... (已产生 X tokens · 耗时 Y.Ys)`；
  - `done`: 当首个正式内容到达时自动收起，标题更新为 `🧠 深度思考已完成 (耗时 X.Ys · 累计 Y tokens) [展开/折叠 ▾]`；
  - 点击手风琴标题可自由切换 `m.thinkingCollapsed = !m.thinkingCollapsed`。

- [ ] **Step 2: 降级与特殊标头监听**

- 监听 response header `X-Aigate-Reasoning-Degraded`；
- 若检测到，在思考框上方渲染：
  `<div id="playDegradedBanner" class="p-2 bg-amber-500/10 border border-amber-500/30 rounded text-[11px] text-amber-300">⚠️ 注意：检测到响应包含 X-Aigate-Reasoning-Degraded 标记，思考输出已降级</div>`。

---

### Task 4: 多维指标看板、动态成本核算引擎与胜出标牌

**Files:**
- Modify: `web/src/views/PlaygroundView.vue`

- [ ] **Step 1: 四维性能指标计算与 Token 统计**

计算并在消息底部呈现：
- `TTFT`：首字延迟（`ms`）；
- `Latency`：端到端生成耗时（`s`）；
- `Speed`：生成速率（`tokens/s`）；
- 保留 `#playStatReasoningTokens`：必须包含文本 `🧠 推理 Token: X (aigate_tokens_reasoning_total)`。

- [ ] **Step 2: 实现动态模型定价字典与费用核算**

定义标准公有云费率字典（每 MTok）：
- `deepseek-chat / v3`: $0.14 in / $0.28 out
- `deepseek-r1`: $0.55 in / $2.19 out
- `gpt-4o`: $2.50 in / $10.00 out
- `gpt-4o-mini`: $0.15 in / $0.60 out
- `claude-3-7-sonnet`: $3.00 in / $15.00 out
- `gemini-2.0-flash`: $0.10 in / $0.40 out
- 通用兜底: $1.00 in / $3.00 out
核算单次调用花费：`cost = (promptTokens * P_in + compTokens * P_out + reasoningTokens * P_rea) / 1,000,000`，格式化为美元（如 `$0.0004`）。

- [ ] **Step 3: 竞技场横向胜出比较与绿色标牌**

在 Arena 双侧均生成完毕后：
- 比较 TTFT：快的一方打上 `⚡ 首字优 X%`；
- 比较 Speed：快的一方打上 `🚀 速率快 X%`；
- 比较 Cost：省的一方打上 `💰 成本省 X%`。

---

### Task 5: 开发者工具箱与端到端构建验证

**Files:**
- Modify: `web/src/views/PlaygroundView.vue`
- Build & Embed: `web/admin.html`, `build/generated/admin_ui_html.h`

- [ ] **Step 1: 实现「📋 复制 cURL」与代码块复制功能**

- 点击「复制 cURL」组装标准 Bash cURL 脚本并拷贝至剪贴板；
- 消息正文中的 Markdown 代码块顶部加上语言 Tag 与「复制代码」按钮。

- [ ] **Step 2: 前端生产编译与单文件打包**

```bash
cd web && npm run build
```
验证：Vite 成功生成 `web/dist/index.html` 并更新 `web/admin.html`。

- [ ] **Step 3: 烘焙嵌入 C 静态头文件**

```bash
python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h
```
验证：头文件成功重新生成。

- [ ] **Step 4: C 全量零警告编译**

```bash
make -C build -j$(nproc)
```
验证：`-Wall -Wextra -Werror` 零警告零错误通过。

- [ ] **Step 5: 运行全量 351 个单元测试**

```bash
./build/tests/aigate_unit_tests
```
验证：所有 351 个测试全部通过 (PASS 351/351, 0 failures)。

- [ ] **Step 6: 提交 Git 代码与计划**

```bash
git add -A && git commit -m "feat(playground): add dual arena comparison, extended thinking accordion, and cost calculator"
```

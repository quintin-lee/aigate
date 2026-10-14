# 大模型测试场双模型竞技场与思考链交互增强设计规范 (Playground Dual Arena & Thinking Design Spec)

**文档状态**: 已设计评审 (Design Approved)  
**设计日期**: 2026-10-14  
**目标版本**: aigate v1.0+  
**影响模块**: 前端 Web 控制台 (`web/src/views/PlaygroundView.vue`, `web/src/api/client.js`)

---

## 1. 概述与设计目标 (Executive Summary & Goals)

### 1.1 背景
aigate 作为面向企业级高并发与全生态模型路由的 AI 网关，已经完备集成了 DeepSeek-R1、OpenAI o1/o3、Claude 3.7 Sonnet、Gemini 2.0 Flash 等顶尖大模型及其 Extended Thinking 深度推理思考链扩展，支持原生与标签模拟双向转换。

现有的 Web 管理控制台中的 Playground 测试场仅提供了单模型、单对话流的基础输入/输出面板：
1. **对比评测繁琐**：评估两个模型（例如 `deepseek-r1` 与 `claude-3-7-sonnet`）在同一 Prompt 下的生成表现时，必须反复切换模型、清空重跑，无法同屏直观横向比对；
2. **思考链交互单一**：深度思考过程作为静态文本块平铺展开，长思考（数千 Token）严重挤占屏幕垂直高度；且缺乏思考阶段的呼吸感提示与精准思考纯耗时；
3. **关键性能与成本黑盒**：缺少首字时延 (TTFT)、生成吞吐速率 (tokens/s)、细分 Token 消耗明细与单次调用成本换算，开发者难以评估性价比；
4. **代码复现与调试不便**：缺少代码块一键复制、cURL 命令行生成和原始 JSON 查看能力。

### 1.2 核心目标
- **双模式无缝切换**：顶部一键在「💬 单模型深度调试」与「⚔️ 双模型竞技场 (Dual Arena)」之间切换；
- **并发双路独立 SSE 流式引擎**：同步向双模型派发请求，双通道并行打字机流式输出，独立取消与异常隔离；
- **智能 Extended Thinking 手风琴**：流式推理时光效呼吸，推理完毕进入正文时自动收起并标明思考精确用时（如 `🧠 深度思考已完成 (耗时 2.1s · 1,450 tokens)`），支持自由展开/折叠；
- **全维度性能指标与动态成本看板**：实时测量 TTFT (ms)、总耗时 (s)、速率 (t/s)、Prompt/Reasoning/Completion Tokens，动态联动网关定价核算单次成本；
- **开发者调试利器**：Markdown 代码块一键复制、一键生成标准 Bash cURL 脚本、单次原始 HTTP JSON 查看抽屉；
- **DOM 探针 100% 契约兼容**：无损保留 `#reasoning-box`, `reasoning-accordion thinking-accordion badge-purple`, `#playReasoningFormat`, `#playDegradedBanner`, `#playStatReasoningTokens` 等关键属性，确保既有 351 个单元测试持续 100% 通过。

---

## 2. 界面布局与双模式架构设计 (UI & Layout Architecture)

### 2.1 模式控制与全局工具栏
工作台顶部由 `NRadioGroup` / 紧凑按钮组提供模式选择：
```
┌────────────────────────────────────────────────────────────────────────┐
│ 🎮 大模型测试场 (Playground Workbench)                                  │
│ 模式: [● 💬 单模型深度调试 ]  [   ⚔️ 双模型竞技场 (Dual Arena) ]          │
│ 快捷工具: [📋 导出 cURL]  [⚙️ 折叠参数栏]  [🗑️ 清空对话]                 │
└────────────────────────────────────────────────────────────────────────┘
```
1. **单模型调试视图 (Single Mode)**：
   - 左侧（4 列）：模型选择、System Prompt、Temperature 滑块、Top-P 滑块、Reasoning Effort 下拉框、思考标签格式、降级警告横幅、流式开关；
   - 右侧（8 列）：单通道对话时间线、实时性能统计条、统一输入栏。
2. **双模型竞技场视图 (Dual Arena Mode)**：
   - 顶部折叠栏：紧凑放置 Model A 选择器 (带独立 Temperature / Effort) 与 Model B 选择器 (带独立 Temperature / Effort)，以及共享的 System Prompt 与流式开关；
   - 中间主体：`grid grid-cols-1 md:grid-cols-2 gap-4` 双栏并排，左栏为 Model A 对话区，右栏为 Model B 对话区；
   - 底部停靠：全宽 Prompt 输入框与「▶ 同步发送请求 (Ctrl+Enter)」和「⏹ 终止生成」按钮。

---

## 3. 并发双路 SSE 流式管道与 Extended Thinking 交互规范 (Streaming & Thinking)

### 3.1 双路并发流式引擎设计
在竞技场模式下，点击发送将触发独立且平行的两个异步 SSE 读取循环：
```javascript
const abortControllerA = new AbortController();
const abortControllerB = new AbortController();

// 并行派发请求
await Promise.allSettled([
  streamChat('A', modelAConfig, abortControllerA.signal),
  streamChat('B', modelBConfig, abortControllerB.signal)
]);
```
- **管道隔离**：每个通道独立维护 `tSend`, `tFirstByte`, `tThinkingEnd`, `tEnd` 时间戳；
- **首字时延 (TTFT)**：当通道接收到第一个非空 SSE chunk 时打点计算 `TTFT = tFirstByte - tSend`；
- **优雅取消**：支持点击全局「终止生成」通过两个 Controller 同时 abort，亦支持单侧点击中断单个模型。

### 3.2 思考推理链状态机与视觉标准
每个 Assistant 回复卡片内部的思考手风琴划分为四阶段：
1. **等待推理 (Pending)**：展示脉冲骨架光效；
2. **深度推理中 (Thinking in Progress)**：
   - 容器边框为动态青紫色微光，背景渐变 `bg-cyan-950/20`；
   - 标题徽章：`🧠 正在深度思考中... (已推理 X tokens · 耗时 Y.Ys)`；
   - 内容区：实时打字机呈现思考文本；
3. **推理完成自动折叠 (Thinking Completed)**：
   - 当接收到首个正式回答内容 Token 时，思考计时停止，手风琴自动平滑收起；
   - 标题徽章更新为：`🧠 深度思考已完成 (耗时 X.Ys · 累计 Y tokens) [展开/折叠 ▾]`；
   - 点击标题栏可在展开与折叠间自由切换；
4. **正文生成中 (Content Streaming)**：
   - 下方卡片独立流式输出最终文本。
- **DOM 契约**：必须包含 `id="reasoning-box"` 以及类名 `reasoning-accordion thinking-accordion badge-purple`。

### 3.3 降级标头感知
检查 response header `X-Aigate-Reasoning-Degraded`，若为 `"true"`，在对应模型区域渲染警告条：
`<div id="playDegradedBanner">⚠️ 注意：检测到响应包含 X-Aigate-Reasoning-Degraded 标记，思考输出已降级</div>`。

---

## 4. 多维性能指标与动态成本核算引擎规范 (Metrics & Cost Engine)

### 4.1 四维性能指标表
每个模型对话回合末尾浮现卡片级指标条：
- `TTFT`: 首字耗时，毫秒（如 `245 ms`）；
- `Latency`: 总生成耗时，秒（如 `3.42 s`）；
- `Speed`: 产出速率（`tokens/s`，如 `72.8 t/s`）；
- `Reasoning Tokens`: 推理 Token 数，保留 `#playStatReasoningTokens`（如 `🧠 推理 Token: 1250 (aigate_tokens_reasoning_total)`）；
- `Prompt & Completion Tokens`: 细分统计。

### 4.2 动态成本核算模型 (Cost Model)
```
Cost = (PromptTokens × P_in + CompletionTokens × P_out + ReasoningTokens × P_reasoning) / 1,000,000
```
- **定价数据源**：
  1. 优先读取已配置的模型单价 (`/admin/v1/models` 返回的 `pricing` 对象)；
  2. 若模型未配置，则回退至主流公有云标准单价表：
     - `deepseek-chat / deepseek-v3`: Input $0.14/M, Output $0.28/M, Reasoning $0.28/M
     - `deepseek-r1`: Input $0.55/M, Output $2.19/M, Reasoning $2.19/M
     - `gpt-4o`: Input $2.50/M, Output $10.00/M
     - `gpt-4o-mini`: Input $0.15/M, Output $0.60/M
     - `claude-3-7-sonnet`: Input $3.00/M, Output $15.00/M, Reasoning $15.00/M
     - `gemini-2.0-flash`: Input $0.10/M, Output $0.40/M
     - 默认兜底: Input $1.00/M, Output $3.00/M
- **货币展示**：格式化为美元 `$0.00042`，并在竞技场胜出方打上省钱百分比标记。

### 4.3 竞技场双栏胜出标牌 (Arena Winners)
在 Arena 对比结束后，自动对比两栏数据并渲染高亮 Tag：
- 胜出方标绿：`⚡ 首字优 2.1x`、`🚀 生成快 38%`、`💰 成本省 85%`。

---

## 5. 开发者调试工具箱规范 (Developer Tooling)

### 5.1 cURL 命令行生成导出
点击「📋 导出 cURL」按钮，自动根据当前选定模型、系统人设、参数及最近提问组装出完整的可执行 Bash cURL 命令并写入系统剪贴板。

### 5.2 Markdown 代码块高亮与复制代码
大模型回答中包含的 ````lang ... ```` 代码块自动加上深色标题栏，显示语言标识并配备「复制代码」按钮，点击反馈「已复制到剪贴板」。

### 5.3 原始 HTTP JSON 查看
消息气泡底部增加「<{}> 查看原始数据」，点击展示包含完整 Request Body 与 Response Chunks 的 JSON 抽屉。

---

## 6. 构建与全量测试验证计划 (Verification Plan)

1. **前端工程打包构建**：
   - 执行 `cd web && npm run build`；
   - 验证 Vite Single-File 打包正常输出 `dist/index.html`，构建耗时在 15s 以内，无任何编译警告或类型错误。
2. **烘焙嵌入 C 资产**：
   - 执行 `python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h`；
   - 确认嵌入后的静态 C 头文件生成成功。
3. **C 编译器构建**：
   - 执行 `make -C build -j$(nproc)`；
   - 验证在 `-Wall -Wextra -Werror` 极严标准下零警告编译成功。
4. **全量单元测试覆盖**：
   - 执行 `./build/tests/aigate_unit_tests`；
   - 验证包含 `test_admin_ui_watermark_and_chain_elements`、`test_reasoning_*` 等在内的全部 351 个单元测试 100% 通过（PASS 351/351）。

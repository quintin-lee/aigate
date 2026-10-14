# 上游服务商与智能模型路由中枢 (Providers & Models Router) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `ProvidersView.vue` 与 `ModelsView.vue` 升级为全能双驱的上游健康巡检与多目标智能路由中枢，支持健康态势雷达、并发探针巡检、多目标路由池 (`targets[]`)、主备梯队与权重分流、SLA 容灾降级兜底、深度推理 (Reasoning) 视窗与内置路由仿真沙箱抽屉。

**Architecture:** 前端利用 Naive UI + TailwindCSS 在 `ProvidersView.vue` 增加 4 栅格健康态势卡片与探针全量测速 (`/admin/v1/providers/health` 与 `/admin/v1/providers/probe`)；在 `ModelsView.vue` 中构建可视化多目标路由池与流量分配预览条，对接底层 `targets[]` 数组、`lb_policy`（`priority` / `weighted` / `p2c`）与 `fallback_model`，并提供无外部依赖的单请求路由命中仿真推演抽屉。

**Tech Stack:** Vue 3, Naive UI, Tailwind CSS, Vite (Single-file inlined bundle), C99 Core Data Plane (`model_router.c`, `circuit_breaker.c`, `health_prober.c`).

---

### Task 1: 服务商健康态势与全量探活大盘升级

**Files:**
- Modify: `web/src/views/ProvidersView.vue`

- [ ] **Step 1: 在 `ProvidersView.vue` 顶部引入 4 栅格健康态势雷达卡片**

在顶部控制栏下方放置响应式 4 栅格健康大盘：
1. 🟢 **健康在线渠道**：统计当前处于 `CLOSED` / `healthy` 的服务商数量与在线率（如 4/5 80%）；
2. 🟡 **熔断与降级预警**：实时统计处于 `DEGRADED` 或 `OPEN` 的通道数；
3. ⚡ **全局平均探活 RTT**：计算各服务商探针测速的平均网络延迟（如 148 ms）；
4. 📦 **承载模型总数**：统计当前所有供应商承载的公开模型总数。

- [ ] **Step 2: 增强供应商表格列与全量并发探活巡检**

- 增加状态呼吸灯（🟢 在线、🟡 SLA 降级、🔴 熔断摘除、⚪ 未知）；
- Base URL 列清晰标识密钥形态（`env:VAR` 环境变量引用、`pg:BLOB` 加密凭证）；
- 承载模型列增加模型计数胶囊；
- 顶部增加“⚡ 全量探针测速巡检”按钮，点击调用 `POST /admin/v1/providers/probe`，并自动拉取 `/admin/v1/providers/health` 刷新最新 RTT。

---

### Task 2: 多目标弹性路由池 (`targets[]`) 与算法调度器实现

**Files:**
- Modify: `web/src/views/ModelsView.vue`

- [ ] **Step 1: 在模型配置弹窗中增加多目标路由池 (Upstream Targets Pool)**

支持动态添加/移除 1~8 个物理后端目标：
- 目标渠道：下拉选择供应商（openai, azure, anthropic, deepseek 等）；
- 目标端点：可选自定义覆盖 Base URL；
- 凭证引用：可选自定义覆盖密钥引用 (`env:NAME` 或 `pg:BLOB`)；
- 主备梯队 (`priority`)：0 为主力 Primary，1 为热备 Fallback，2 为逃生；
- 分流权重 (`weight`)：支持调整数值权重（如 7:3）；
- 单节点最大并发 (`max_concurrency`)：保护后端通道防超限。

- [ ] **Step 2: 增加实时流量分配百分比预览条 (Traffic Ratio Preview) 与算法选择器**

- 动态响应式计算同一梯队内的权重占比（如 70% OpenAI 官方 + 30% Azure OpenAI）；
- 渲染渐变彩色分流条，直观预览流量分发预期；
- 增加算法策略选择器 (`lb_policy`)：
  - `priority`：严格主备顺序容灾；
  - `weighted` / `weighted_round_robin`：按权重比例分流；
  - `p2c` / `adaptive_latency`：两两抽样最小延迟调度。

---

### Task 3: SLA 容灾降级模型、熔断门限与深度思考 (Reasoning) 视窗

**Files:**
- Modify: `web/src/views/ModelsView.vue`

- [ ] **Step 1: 实现 SLA 容灾降级兜底模型与熔断阈值配置**

- 在模型表单中增加“SLA 容灾降级 (Fallback Model)”配置区：
  - `fallback_model` 下拉选择框（过滤除自身以外的所有可用模型，如切换至 `deepseek-v3` 或 `gpt-4o-mini`）；
  - 呈现熔断错误率阈值 (40%) 与 SLA 延迟劣化门限 (3000ms) 提示。

- [ ] **Step 2: 完善深度思考推理视窗 (Reasoning) 并确保已有测试 DOM 契约完全保留**

- 严格保留现有测试 DOM ID：
  - `id="mFormSupportsReasoning"`：支持深度推理开关；
  - `id="mFormThinkingBudget"`：默认思考 Token 预算输入框；
  - `id="mFormTagEmulationDefault"`：`<thought>` 思考标签模拟开关；
  - `id="mFormPricingReasoning"`：推理 Token 独立费率输入框。
- 增加推理卡片视觉容器，针对 DeepSeek-R1 / o1 / o3 / Claude 3.7 Sonnet 思考流提供友好的参数解释与提示。

---

### Task 4: 智能路由仿真沙箱与端到端推演抽屉实现

**Files:**
- Modify: `web/src/views/ModelsView.vue`

- [ ] **Step 1: 在模型表格操作列增加“🔬 仿真测试”按钮**

在每个模型的“操作”列中加入“🔬 仿真测试”按钮，点击唤出右侧抽屉 `simulationDrawerVisible`。

- [ ] **Step 2: 构建智能路由仿真沙箱抽屉 (Routing Simulator Drawer)**

- 仿真报告卡片：
  - 实时评估当前模型的 targets 数组与 `lb_policy`；
  - 显示模拟命中的具体 Target 节点（名称、URL、探活延迟、熔断状态）；
  - 解释选路决策逻辑（如“主力 Priority 0 正常，基于 Weighted 7:3 抽样命中 Target #1”）；
- 容灾逃生推演：
  - 动态展示若 Target #1 突发故障时，流量将自动旁路至哪个备份 Target 或 `fallback_model`；
- 数据面参数校验：
  - 展示实际注入的思考预算与标签包装规则；
- 一键重新仿真按钮，方便运维人员连续验证随机权重分布。

---

### Task 5: 单文件构建集成、端到端测试验证与代码提交

**Files:**
- Modify: `web/admin.html`, `build/generated/admin_ui_html.h`

- [ ] **Step 1: 前端生产编译打包**

```bash
cd web && npm run build
```
验证：Vite 成功生成 `web/dist/index.html` 并内嵌至 `web/admin.html`。

- [ ] **Step 2: 烘焙嵌入 C 头文件**

```bash
python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h
```

- [ ] **Step 3: 全局编译 C 工程**

```bash
make -C build -j$(nproc)
```
验证：`-Wall -Wextra -Werror` 零警告零错误通过。

- [ ] **Step 4: 运行全量单元测试**

```bash
./build/tests/aigate_unit_tests
```
验证：所有单元测试 100% 全部通过 (PASS 352/352, 0 failures)。

- [ ] **Step 5: 提交所有代码与计划至 Git**

# 全链路实时流量观测与调用链全景大盘优化完善实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全面优化完善 aigate 实时流量运维观测大屏 (`LiveView.vue`)，集成顶层实时流量与性能态势雷达大盘、上游提供商健康与 SLA 熔断状态芯片条、高精实时请求流水瀑布与多维过滤、熔断/探活/告警分轨流、单请求全链路诊断抽屉，以及 SSE 长连接与无锁轮询智能双驱自愈机制，保持 100% 单元测试通过率。

**Architecture:**
1. **顶层实时流量与性能态势雷达 (`web/src/views/LiveView.vue`)**：4 栅格卡片基于滑动时间窗口秒级计算 QPS、P90/P99 延迟分位、异常阻断率与 Token 吞吐速率；
2. **上游提供商实时健康与熔断指示条 (`web/src/views/LiveView.vue`)**：横向呈现各主要渠道（OpenAI, Anthropic, DeepSeek, Cache 等）的连通性、探活时延与熔断状态机（CLOSED/DEGRADED/OPEN），支持点击联动过滤；
3. **实时请求流水瀑布与多维快筛 (`web/src/views/LiveView.vue`)**：左侧 8 栏展示高精流水行，支持状态码（200/403/500/Cache）与模型即时快筛、延迟分级变色与自动滚屏锁定；
4. **熔断自愈/探活/告警分轨流与诊断抽屉 (`web/src/views/LiveView.vue`)**：右侧 4 栏分轨呈现底层事件总线发布的熔断流转、心跳探活与安全告警，点击流水可滑出抽屉深度诊断单次请求全链路耗时与成本；
5. **SSE + 轮询双驱自愈与全流程验证**：主用 SSE 长连接，断网时智能平滑切换至 `/admin/v1/audit/events` 保底轮询；全量打包烘焙并保障 352+ 单元测试 100% 全部通过。

**Tech Stack:** Vue 3, Naive UI, Tailwind CSS, Vite Single-File, Python 3, C (Civetweb SSE, EventBus).

---

### Task 1: 顶层实时流量与性能态势雷达大盘组件实现

**Files:**
- Modify: `web/src/views/LiveView.vue`

- [ ] **Step 1: 在 `LiveView.vue` 顶部增加 4 栅格实时性能雷达大盘卡片**

在顶部标题与控制栏下方放置 4 栅格响应式卡片组：
1. ⚡ **实时网关吞吐 (QPS)**：展示 `currentQps.toFixed(1) + ' req/s'`，附带窗口内总请求计数；
2. ⏱️ **P90 / P99 响应延迟**：展示 `p90Latency + 'ms / ' + p99Latency + 'ms'`，并显示平均时延 `avgLatency + 'ms'`；
3. 🚨 **异常与阻断率**：展示 `errorRate.toFixed(1) + '%' (4xx/5xx)`，并展示风控拦截与上游故障细分计数；
4. 🪙 **Token 吞吐速率**：展示 `tokenRate + ' tok/s'`，体现大模型推理算力瞬时流动速率。

- [ ] **Step 2: 实现滑动窗口动态性能指标统计逻辑**

编写响应式计算属性或定时统计方法，每 1~2 秒基于最近 10 秒捕获的请求数组动态计算分位数、错误率与瞬时 QPS。

---

### Task 2: 上游提供商实时健康与 SLA 熔断状态芯片条实现

**Files:**
- Modify: `web/src/views/LiveView.vue`

- [ ] **Step 1: 在雷达卡片下方增加提供商健康与熔断指示条**

横向滑动呈现主流渠道提供商卡片（OpenAI, Anthropic, DeepSeek, Local Cache 等）：
- 供应商名称与路由标识；
- 熔断状态机标签：`CLOSED` (绿灯) / `DEGRADED` (橙灯，提示 SLA 触发比率) / `OPEN` (红灯，提示渠道摘除) / `CACHE` (蓝灯，提示缓存命中)；
- 探活时延与累计请求数。

- [ ] **Step 2: 绑定点击联动过滤事件**

点击任意提供商芯片，将当前选中的提供商过滤器 `selectedProvider` 设为对应渠道，左侧流水即刻联动过滤。

---

### Task 3: 实时请求瀑布流、多维过滤与自动滚屏锁定实现

**Files:**
- Modify: `web/src/views/LiveView.vue`

- [ ] **Step 1: 升级左侧 8 栏请求流水头部工具栏**

增加控制栏：
- 状态快筛胶囊：`全部` / `2xx 正常` / `4xx/5xx 异常` / `⚡ 仅看缓存`；
- 自动滚屏开关：`🔒 锁定最新流动` / `🔓 解锁停留排查`；
- 保留上限控制（保持最近 200 条请求记录，先进先出队列）。

- [ ] **Step 2: 重构流水条目行样式 (Waterfall Row)**

丰富每个请求条目的视觉元素：
- 状态码徽章（200 绿、403 红带阻断规则标签、429 橙、5xx 红、200 CACHED 蓝）；
- 模型与上游路由（如 `gpt-4o ➔ openai`）；
- 客户端来源 IP 及 Key ID；
- 耗时 `latency_ms`（< 300ms 绿色、300~1000ms 橙色、> 1000ms 红色）；
- Token 数与调用成本核算（如 `68 tok · $0.00018`）；
- 精确时间戳 `HH:mm:ss`；
- 绑定 `@click="openRequestTrace(req)"` 点击交互。

---

### Task 4: 熔断自愈/探活/告警分轨流、单请求链路抽屉与双驱自愈实现

**Files:**
- Modify: `web/src/views/LiveView.vue`

- [ ] **Step 1: 升级右侧 4 栏告警流为多事件总线分轨**

解析并展示四类事件：
1. `circuit_breaker`：熔断器流转事件（如 `CLOSED ➔ DEGRADED`，提示降级原因）；
2. `安全策略阻断`：403 阻断事件（命中规则、来源 IP 与危险标签）；
3. `health_probe`：周期性探活心跳日志；
4. `budget_alert`：客户端凭证配额接近预警。

- [ ] **Step 2: 实现单请求全链路诊断抽屉 (Trace Drawer)**

点击流水条目时滑出右侧抽屉：
- 请求元数据：Trace ID、时间戳、状态码、模型、提供商、客户端 IP、Key ID；
- 性能与缓存分析：总延迟分解、是否命中缓存；
- Token 与费用：Prompt Tokens、Completion Tokens、精确预估费用；
- 风控策略明细：Guardrail 判定结果；
- 操作：一键复制请求 JSON 诊断摘要。

- [ ] **Step 3: 实现 SSE 长连接与优雅自愈降级机制**

- 默认调用 `EventSource` 监听 `/admin/v1/events?token=...`；
- 若 SSE 断开超过 3 秒，自动启动保底轮询定时器（每 2.5 秒调用 `GET /admin/v1/audit/events`），将新事件增量合入；
- 当 SSE 重新握手成功后，自动关闭保底轮询；
- 保留已有控制契约：`togglePause` 与 `clearEvents`。

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

```bash
git add -A && git commit -m "feat(live): add live radar kpis, provider health strip, and request trace drawer"
```

# 网关实时事件流监控室与上游健康主动巡检实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 aigate 网关的实时事件总线 (SSE Event Bus)、上游健康主动巡检守护引擎 (Health Prober) 以及 Web 控制台动态运维监控大屏 (`#tab-live`)，实现实时指标流、请求瀑布流、供应商健康矩阵与告警时间线。

**Architecture:** 
1. **事件总线 (`src/event_bus.{c,h}`)**：有界多路复用发布-订阅环形缓冲池（最大 8 个并发 Admin 订阅者），线程安全，自动保活 Ping，慢消费者无阻丢弃保护。
2. **主动巡检引擎 (`src/health_prober.{c,h}`)**：轻量后台周期巡检线程（默认 60s），异步探活所有上游 Provider，状态机迁移（HEALTHY, DEGRADED, DOWN, PAUSED），并暴露 `GET /admin/v1/providers/health` 与 `POST /admin/v1/providers/probe` 端点。
3. **SSE 传输层 (`src/transport_civetweb.c`, `src/admin_api.{c,h}`)**：`GET /admin/v1/events` 原生 HTTP/1.1 `text/event-stream` 长连接，支持 Bearer / Query Token 鉴权与断开检测。
4. **数据面集成 (`src/aigate_core.c`, `src/circuit_breaker.c`, `src/budget_enforce.c`)**：在请求完成、熔断变迁、预算告警处发布事件。
5. **嵌入式大屏 (`web/admin.html`)**：单页嵌入式原生 HTML/JS/CSS，包含实时指标卡（QPS/延迟/吞吐）、健康雷达卡片（带一键巡检）、动态瀑布流（支持暂停检索）与告警时间线。

**Tech Stack:** C17, Civetweb 1.16, libcurl, Jansson, POSIX Threads, Vanilla JS (Fetch Streams / EventSource), Tailwind CSS, Pytest.

---

## 文件影响范围映射

- **新增核心源文件：**
  - `src/event_bus.h`, `src/event_bus.c`: 实时事件总线与订阅池。
  - `src/health_prober.h`, `src/health_prober.c`: 上游健康主动巡检引擎。
  - `tests/unit/test_event_bus.c`: 事件总线单元测试。
  - `tests/unit/test_health_prober.c`: 探活引擎单元测试。
- **修改现有核心源文件：**
  - `src/admin_api.h`, `src/admin_api.c`: 增加健康查询与巡检触发接口；管理事件流。
  - `src/transport_civetweb.c`: 注册 `GET /admin/v1/events` SSE 端点。
  - `src/aigate_core.h`, `src/aigate_core.c`: 接入 `event_bus` 与 `health_prober`，发布请求完成事件。
  - `src/circuit_breaker.h`, `src/circuit_breaker.c`: 熔断状态变迁发布事件。
  - `src/budget_enforce.h`, `src/budget_enforce.c`: 预算告警发布事件。
  - `src/main.c`: 网关启动时初始化并启动 prober，关闭时释放。
  - `tests/unit/run_tests.c`: 注册新单元测试。
- **修改前端单页源文件：**
  - `web/admin.html`: 增加 `#tab-live` 结构、大屏 JS 逻辑与 SSE 连接管理。
- **自动化测试文件：**
  - `tests/integration/test_gateway.py`: 增加 SSE 实时流端到端验证与健康探测验证。

---

## 任务拆分列表

### Task 1: 实现实时事件总线 (`src/event_bus.{c,h}`) 与单元测试

**Files:**
- Create: `src/event_bus.h`, `src/event_bus.c`
- Create: `tests/unit/test_event_bus.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写 `src/event_bus.h` 接口定义**
  - 定义 `event_type_t` (`EVENT_REQUEST`, `EVENT_CIRCUIT_BREAKER`, `EVENT_HEALTH_PROBE`, `EVENT_BUDGET_ALERT`, `EVENT_PING`)。
  - 定义 `event_item_t`（类型、时间戳、JSON 载荷字符串）。
  - 定义 `event_bus_t` 与订阅者句柄 `event_sub_t`。
  - 声明生命周期函数：`event_bus_new()`, `event_bus_free()`, `event_bus_subscribe()`, `event_bus_unsubscribe()`, `event_bus_pop()`, `event_bus_publish()` 及其便捷辅助函数。

- [ ] **Step 2: 编写 `src/event_bus.c` 实现**
  - 实现基于 `pthread_mutex_t` 与 `pthread_cond_t` 的线程安全分发。
  - 支持固定最大 8 个并发订阅者，每个订阅者拥有 64 容量的环形队列。
  - 慢消费者保护：当队列满时，丢弃最旧的普通事件并记录丢弃计数。
  - 实现 `event_bus_pop` 带毫秒级超时的等待机制。

- [ ] **Step 3: 编写 `tests/unit/test_event_bus.c` 单元测试**
  - 测试创建与销毁。
  - 测试订阅与注销。
  - 测试事件广播与跨线程消费。
  - 测试队列满时的环形覆盖与丢弃保护。
  - 测试超时返回。

- [ ] **Step 4: 在 `tests/unit/run_tests.c` 注册并运行测试**
  - 执行 `cmake --build .build -j$(nproc)`
  - 执行 `ctest --test-dir .build -R unit --output-on-failure` 验证测试通过。

- [ ] **Step 5: Commit Task 1**
  ```bash
  git add src/event_bus.h src/event_bus.c tests/unit/test_event_bus.c tests/unit/run_tests.c
  git commit -m "feat(events): ⚡ implement in-memory pub-sub event bus for real-time telemetry"
  ```

---

### Task 2: 实现上游健康主动巡检引擎 (`src/health_prober.{c,h}`) 与单元测试

**Files:**
- Create: `src/health_prober.h`, `src/health_prober.c`
- Create: `tests/unit/test_health_prober.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写 `src/health_prober.h` 接口定义**
  - 定义 `health_status_t` (`HEALTHY`, `DEGRADED`, `DOWN`, `PAUSED`, `UNKNOWN`)。
  - 定义 `provider_health_t` 结构体（名称、endpoint、状态、RTT、连续失败数、最后探测时间、报错）。
  - 声明 `health_prober_t` 及操作接口：`health_prober_new()`, `health_prober_free()`, `health_prober_start()`, `health_prober_stop()`, `health_prober_probe_all()`, `health_prober_probe_one()`, `health_prober_get_status_json()`。

- [ ] **Step 2: 编写 `src/health_prober.c` 实现**
  - 实现内存状态表与锁保护。
  - 实现后台探活工作线程（周期休眠 `interval_sec`，默认 60s，可通过环境变量 `AIGATE_HEALTH_PROBE_INTERVAL_SEC` 配置）。
  - 调用 `upstream_probe()` 获取真实状态码与 RTT。
  - 状态判定机：200 且 RTT < 2000ms 为 HEALTHY；200 且 RTT $\ge$ 2000ms 为 DEGRADED；401/403 或连续失败 $\ge$ 2 次为 DOWN。
  - 状态发生跃迁时通过关联的 `event_bus` 触发 `health_probe` 事件。

- [ ] **Step 3: 编写 `tests/unit/test_health_prober.c` 单元测试**
  - 测试探活状态表初始化与更新。
  - 测试状态机判定（HEALTHY / DEGRADED / DOWN）。
  - 测试 JSON 序列化输出。

- [ ] **Step 4: 在 `tests/unit/run_tests.c` 注册并运行单元测试**
  - 编译并执行 `ctest --test-dir .build -R unit --output-on-failure`。

- [ ] **Step 5: Commit Task 2**
  ```bash
  git add src/health_prober.h src/health_prober.c tests/unit/test_health_prober.c tests/unit/run_tests.c
  git commit -m "feat(prober): 🩺 implement active upstream health prober engine"
  ```

---

### Task 3: Admin REST API 扩充与 SSE 端点支持

**Files:**
- Modify: `src/admin_api.h`, `src/admin_api.c`
- Modify: `src/transport_civetweb.c`

- [ ] **Step 1: 在 `admin_api.c` 中添加健康查询与即时巡检端点**
  - `GET /admin/v1/providers/health`：调用 `health_prober_get_status_json()` 返回全量健康状态。
  - `POST /admin/v1/providers/probe`：调用 `health_prober_probe_all()` 即时并发探活并返回最新健康状态。
  - 更新现有的 `POST /admin/v1/providers/:id/test`，在单测完成后同步更新 `health_prober` 中的健康记录。

- [ ] **Step 2: 在 `transport_civetweb.c` 中实现 SSE 端点 `GET /admin/v1/events`**
  - 解析 `Authorization: Bearer <token>` 或 URL Query `?token=<token>`。
  - 校验 Admin Token 有效性，若失败返回 401。
  - 发送 SSE 响应头：
    ```http
    HTTP/1.1 200 OK\r\n
    Content-Type: text/event-stream\r\n
    Cache-Control: no-cache, no-transform\r\n
    Connection: keep-alive\r\n
    Access-Control-Allow-Origin: *\r\n\r\n
    ```
  - 注册 `event_sub_t`，进入消费循环：
    - 等待 `event_bus_pop`（超时 15s）。
    - 超时发送 `: ping\n\n`。
    - 有事件时发送 `event: <type>\ndata: <json>\n\n`。
    - 若 `mg_write` 失败（客户端关闭页面或网络中断），跳出循环并注销订阅者。

- [ ] **Step 3: 运行 CMake 编译验证无语法与告警错误**
  - 执行 `cmake --build .build -j$(nproc)`。

- [ ] **Step 4: Commit Task 3**
  ```bash
  git add src/admin_api.h src/admin_api.c src/transport_civetweb.c
  git commit -m "feat(admin): 🌐 expose health probe endpoints and /admin/v1/events SSE stream"
  ```

---

### Task 4: 数据面与网关核心事件发布集成

**Files:**
- Modify: `src/aigate_core.h`, `src/aigate_core.c`
- Modify: `src/circuit_breaker.h`, `src/circuit_breaker.c`
- Modify: `src/budget_enforce.h`, `src/budget_enforce.c`
- Modify: `src/main.c`

- [ ] **Step 1: 在 `aigate_core_t` 中持有 `event_bus_t*` 和 `health_prober_t*`**
  - 在 `aigate_core_init` 中注入或创建 `event_bus` 与 `health_prober`。
  - 关联至 `admin_api`。

- [ ] **Step 2: 在 `aigate_core.c` 的请求收尾阶段触发 `publish_request`**
  - 在流式（streaming）与非流式（non-streaming）请求完成处（包含成功与失败），提取 `model`, `provider`, `status`, `lat_ns`, `tokens`, `cost`, `guardrail_action`，发布至事件总线。

- [ ] **Step 3: 在 `circuit_breaker.c` 中触发 `publish_cb`**
  - 当状态在 `CLOSED`, `OPEN`, `HALF_OPEN` 间跃迁时发布事件。

- [ ] **Step 4: 在 `budget_enforce.c` 中触发 `publish_budget`**
  - 当当月用量达到预算 80% 警戒线或 100% 阻断线时发布告警事件。

- [ ] **Step 5: 在 `main.c` 中管理 prober 生命周期**
  - 启动网关时调用 `health_prober_start()`。
  - 网关优雅退出（SIGINT/SIGTERM）时调用 `health_prober_stop()` 与资源释放。

- [ ] **Step 6: 编译并执行全量单元测试**
  - `cmake --build .build -j$(nproc)`
  - `ctest --test-dir .build -R unit --output-on-failure` 验证全绿。

- [ ] **Step 7: Commit Task 4**
  ```bash
  git add src/aigate_core.h src/aigate_core.c src/circuit_breaker.h src/circuit_breaker.c src/budget_enforce.h src/budget_enforce.c src/main.c
  git commit -m "feat(core): 🔗 hook event bus and health prober into gateway data-plane"
  ```

---

### Task 5: Web 控制台实时大屏界面结构 (`web/admin.html`)

**Files:**
- Modify: `web/admin.html:220-360` (侧边栏与移动端导航), `web/admin.html:1500-1800` (`#tab-live` HTML)

- [ ] **Step 1: 侧边导航与移动端导航增加「📡 实时大屏」入口与实时指示灯**
  - 侧边栏与移动导航顶端增加：
    ```html
    <button data-tab="live" onclick="switchTab('live')" class="side-link" id="nav-live-side">
      <span class="text-base">📡</span>
      <span class="flex-1">实时大屏</span>
      <span id="liveStatusDot" class="inline-block w-2 h-2 rounded-full bg-slate-500"></span>
    </button>
    ```

- [ ] **Step 2: 搭建 `#tab-live` 面板 HTML 结构**
  - **顶部实时状态栏**：
    - 连接状态徽章（`#liveConnBadge`，显示 🟢 Live / 🔴 离线 / 🟡 重连中）。
    - 4 项动态指标卡：实时 QPS (`#liveStatQps`)、滑动 P99 延迟 (`#liveStatP99`)、Token/s 吞吐 (`#liveStatToks`)、今日实时风控数 (`#liveStatGuardrails`)。
  - **模块一：上游供应商健康雷达 (Provider Health Matrix)**：
    - 容器 `#liveHealthRadar`，卡片网格布局。
    - 顶部工具栏带「⚡ 一键全量巡检 (Probe All)」按钮与最后更新时间戳。
  - **模块二：实时请求瀑布流 (Live Request Waterfall)**：
    - 表格 `#liveWaterfallTable`：时间、Key、Model、Provider、状态码 Badge、耗时、Token、费用、风控处置。
    - 操作栏：⏸️ 暂停流 / ▶️ 继续流开关、关键词搜索过滤框、一键清屏按钮。
  - **模块三：系统事件与告警时间线 (Live Event Ticker)**：
    - 侧栏 `#liveAlertTicker` 列表，展示熔断跳闸、节点异常、预算告警。
    - 页面右上角全局浮动 Toast 容器 `#liveToastContainer`。

- [ ] **Step 3: 运行 CMake 重新生成头文件并验证编译**
  - `cmake --build .build -j$(nproc)`

- [ ] **Step 4: Commit Task 5**
  ```bash
  git add web/admin.html
  git commit -m "feat(ui): 🎨 scaffold live operations room dashboard in admin console"
  ```

---

### Task 6: Web 控制台 SSE 客户端与实时大屏交互逻辑

**Files:**
- Modify: `web/admin.html:3100-3600` (JS 脚本区)

- [ ] **Step 1: 实现 SSE 长连接管理与自动重连机制**
  - 实现 `connectLiveStream()`：
    - 构造 URL `/admin/v1/events?token=` + 当前 Token。
    - 使用原生 `new EventSource()`（或带有 Reader 的 `fetch`）。
    - 监听 `open`：更新 `#liveConnBadge` 为绿色呼吸动效 `🟢 实时连接中 (Live)`。
    - 监听 `error`：更新为黄色 `🟡 正在重连...`，并在断线后使用指数退避重连。

- [ ] **Step 2: 实现实时事件消费与大屏动态渲染**
  - 监听 `request` 事件：
    - 累加最近 5 秒滑动窗口数据，更新实时 QPS 与 Token/s。
    - 计算滑动 P99/平均延迟并更新指标卡。
    - 向 `#liveWaterfallTable` 首部 prepend 新行（最多保留 50 行，支持暂停流开关）。
    - 点击行可查看原始 JSON 详情弹窗。
  - 监听 `circuit_breaker` 事件：
    - 触发告警 Toast（红框提示：“⚠️ 节点熔断预警”）。
    - 追加到 `#liveAlertTicker`。
  - 监听 `health_probe` 事件：
    - 局部更新对应的 Provider 卡片红绿灯、RTT 与报错信息。
  - 监听 `budget_alert` 事件：
    - 弹出高优先级 Toast 警报，记录到告警时间线。

- [ ] **Step 3: 实现健康雷达初始化与「⚡ 一键全量巡检」交互**
  - `fetchProviderHealth()`：调用 `GET /admin/v1/providers/health` 渲染各供应商卡片。
  - `triggerProbeAll()`：点击后按钮展示旋转 loading 动画，调用 `POST /admin/v1/providers/probe`，完成后就地重绘各节点卡片并提示 Toast。
  - 单节点「🔍 立即测试」按钮联动。

- [ ] **Step 4: 编译并验证**
  - `cmake --build .build -j$(nproc)`

- [ ] **Step 5: Commit Task 6**
  ```bash
  git add web/admin.html
  git commit -m "feat(ui): ⚡ implement SSE streaming client and real-time dashboard interactions"
  ```

---

### Task 7: 端到端自动化集成测试与验证交付

**Files:**
- Modify: `tests/integration/test_gateway.py`

- [ ] **Step 1: 在 `test_gateway.py` 中编写 SSE 实时流端到端测试**
  - `test_admin_events_sse_stream`:
    - 启动后台线程或异步流连接 `GET /admin/v1/events?token=...`。
    - 发送数据面 Chat 请求。
    - 断言在 SSE 流中实时收到 `event: request`，解析 JSON 包含 model、status 200 与 token 数据。

- [ ] **Step 2: 编写上游健康巡检与批量探测接口测试**
  - `test_provider_health_probe_endpoints`:
    - 调用 `GET /admin/v1/providers/health`，验证返回 providers 列表及其状态结构。
    - 调用 `POST /admin/v1/providers/probe`，验证批量探活执行与最新延迟指标。

- [ ] **Step 3: 执行全量测试套件回归**
  - C 单元测试：`ctest --test-dir .build -R unit --output-on-failure`（预期 100% 通过）。
  - Python E2E 测试：`pytest tests/integration/test_gateway.py -v`（预期 34+ 项全量通过）。

- [ ] **Step 4: Commit Task 7**
  ```bash
  git add tests/integration/test_gateway.py
  git commit -m "test(live): 🧪 add e2e integration tests for SSE events and health prober"
  ```

---

## 阶段验收与质量准则

1. **测试覆盖**：
   - C 单元测试全面覆盖 `event_bus`（环形队列溢出、多订阅者、慢客户端丢弃）与 `health_prober`（状态判定、周期唤醒）。
   - Python E2E 测试覆盖真正的 HTTP SSE 客户端消费与供应商健康探活接口。
2. **零编译告警**：
   - 严格遵循 C17 `-Wall -Wextra -Werror` 标准，无未初始化变量与资源泄漏。
3. **单二进制交付**：
   - `web/admin.html` 编译内嵌进 C 二进制，启动即用，零外部资源依赖。

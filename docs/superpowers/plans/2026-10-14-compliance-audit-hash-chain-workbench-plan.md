# 合规审计与全链路不可篡改存证工作台优化完善实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全面优化完善 aigate 合规审计与全链路防篡改存证工作台 (`AuditView.vue`)，集成顶层审计态势雷达大盘、密码学哈希链 (Hash Chain Ribbon) 拓扑与断链排障胶囊流、现代化多维审计事件检索与实时无锁环形流，以及深度取证抽屉 (`#auditForensicDrawer`) 与取证报告生成，严格保持 100% 自动化测试契约与单元测试通过率。

**Architecture:**
1. **顶层审计态势雷达大盘 (`web/src/views/AuditView.vue`)**：4 栅格实时聚合已签名区块高度、哈希链验真状态 (PASS/TAMPER)、严重级别分布 (CRITICAL/WARN/INFO) 与实时内存环形流状态；
2. **密码学哈希链拓扑胶囊带 (`web/src/views/AuditView.vue`)**：水平平滑流动的区块胶囊展示链式指针、当前哈希与前驱哈希绑定，遇断链错误即刻红标预警，并支持点击穿透定位；
3. **多维审计检索与双模态流控 (`web/src/views/AuditView.vue`)**：支持全文关键词检索、严重度筛选、模型筛选、🚨「仅看违规」一键切片，以及实时流 (▶/⏸) 与 NDJSON 导出；
4. **深度取证抽屉 (`#auditForensicDrawer`) 与报告生成 (`web/src/views/AuditView.vue`)**：呈现完整元数据、规则拦截留存、HMAC 存证哈希与前驱哈希比对，支持一键复制凭据与导出 Markdown 取证报告；
5. **全流程构建与质量验证**：Vite 单文件构建 -> `embed_html.py` 嵌入 -> C 全工程 `-Werror` 编译 -> 352+ 单元测试 100% 全部通过。

**Tech Stack:** Vue 3, Naive UI, Tailwind CSS, Vite Single-File, Python 3, C (OpenSSL HMAC-SHA256, Jansson).

---

### Task 1: 前端顶层审计态势雷达大盘组件实现

**Files:**
- Modify: `web/src/views/AuditView.vue`

- [ ] **Step 1: 在 `AuditView.vue` 顶部增加 4 栅格态势雷达大盘卡片**

在顶部标题和操作栏之间放置 4 栅格响应式卡片组：
1. 🔐 **已签名存证区块**：展示 `chainStatus ? chainStatus.total_records : auditEvents.length`，附带 `HMAC-SHA256` 徽章；
2. 🛡️ **哈希链防篡改状态**：展示 `chainStatus?.valid ? '100% 完整' : '待验真 / 正常'`（若篡改则展示 `❌ 检测到断链`）；
3. 🚨 **安全阻断与违规事件**：展示 `violationCount`，玫瑰红 `text-rose-400` 突出显示；
4. ⚡ **实时审计流状态**：展示 `livePolling ? '实时捕获中' : '静态就绪'`，带有状态指示灯。

- [ ] **Step 2: 完善顶部快捷操作栏**

保留并强化以下操作按钮：
- `🔐 校验哈希链完整性`（`:loading="verifyingChain"`，调用 `verifyHashChain`）；
- `▶️ 实时审计流 / ⏸️ 暂停审计流`（保留函数名 `toggleAuditLiveStream`，满足测试契约）；
- `📥 导出 NDJSON`（保留函数名 `exportAuditNdjson`，满足测试契约）；
- `🔄 刷新`（调用 `loadEvents`）。

---

### Task 2: 密码学哈希链 (Hash Chain Ribbon) 拓扑与断链排障组件实现

**Files:**
- Modify: `web/src/views/AuditView.vue`

- [ ] **Step 1: 实现水平平滑滑动的 Hash Chain Ribbon 区块胶囊流**

在中部区域渲染区块流：
- 动态从 `auditEvents` 计算并反向生成连续的已签名区块胶囊节点：
  - 区块高度：`Block #seq`（最新区块标有 `Tip`）；
  - 当前哈希前缀：`Hash: xxxxxxxx...`；
  - 前驱哈希前缀：`Prev: yyyyyyyy...`；
  - 业务摘要：时间、模型名称与处置动作（`PASS` / `BLOCK`）；
  - 状态指示圆点（翠绿色代表有效，最新区块带有青蓝光晕强调）；
- 区块之间以单向箭头 `➔` 连接。

- [ ] **Step 2: 实现哈希链断裂篡改预警 Banner 与排障诊断**

在验真接口返回 `valid === false` 时：
- 展示猩红警告卡片：
  - `❌ 警告：检测到审计日志篡改或链断裂！`；
  - `断裂起始区块：Block #broken_seq`；
  - `底层原因：broken_reason`；
- 区块拓扑带中将断裂区块节点渲染为红框闪烁。

- [ ] **Step 3: 点击区块胶囊穿透联动取证抽屉**

为每个区块胶囊绑定 `@click="viewDetails(event)"`，点击自动拉起 `#auditForensicDrawer` 并聚焦该区块密码学凭证。

---

### Task 3: 现代化多维审计事件检索表、双模态过滤与实时流实现

**Files:**
- Modify: `web/src/views/AuditView.vue`

- [ ] **Step 1: 实现多维过滤工具栏**

在数据表上方添加复合控制栏：
- 全文模糊搜索框（绑定 `searchKeyword`，对 IP、Trace ID、规则、Prompt 文本即时过滤）；
- 严重度下拉选择（全部 / `CRITICAL` / `WARN` / `INFO`）；
- 模型分类下拉选择（动态提取当前出现的模型名称列表）；
- 🚨 **「仅看违规拦截」一键切换按钮**（动态切换 `onlyViolations`，显示违规数量统计徽章）；
- 实时流状态指示（实时轮询开启时显示动态脉冲绿点）。

- [ ] **Step 2: 优化 `NDataTable` 列配置与渲染**

重构数据表列：
- **时间**：`created_at` 或 `timestamp_ms`，格式化为标准时间并附带相对时间 Tooltip；
- **区块序号**：`#seq_id`，以青色等宽字体呈现；
- **严重度**：`severity` 渲染为 Naive UI 标签（CRITICAL 红色、WARN 橙色、INFO 灰色）；
- **调用模型 / 上游**：显示请求模型与路由模型（如 `gpt-4o · openai`，降级回退时红字标出）；
- **客户端 IP**：等宽字体渲染 IPv4/IPv6；
- **耗时与消耗**：显示总延迟、TTFT 首字延迟与 Token 计数（例如 `120ms (TTFT 35ms) · 57 tok`）；
- **触发规则与状态**：阻断（`⛔ 阻断 [dan_prompt] (403)`）、告警（`⚠️ 告警 [pii_leak]`）或放行（`✅ 正常放行 (200)`）；
- **操作**：`查看取证` 按钮唤起抽屉。

- [ ] **Step 3: 保证数据拉取兼容性**

在 `loadEvents` 中协同对接 `GET /admin/v1/audit/events`：
- 若返回 `{ events: [...] }` 则取 `data.events`；
- 若直接返回数组则取 `data`；
- 更新事件总数与违规统计。

---

### Task 4: 深度取证抽屉 (`#auditForensicDrawer`) 升级与合规报告生成

**Files:**
- Modify: `web/src/views/AuditView.vue`

- [ ] **Step 1: 升级 `#auditForensicDrawer` 抽屉布局与分层**

保持抽屉容器 `id="auditForensicDrawer"`：
1. **基础元数据卡片**：事件序列号、全局 Trace ID、租户、客户端 IP、状态码、延迟拆解、Token 消耗；
2. **风控拦截与违规留存**：命中规则标签、规则描述、截获的风险提示词文本 (`prompt_snippet` 或 `payload`)、降级原因 (`fallback_reason`)；
3. **密码学存证凭证 (HMAC-SHA256 Proof)**：
   - 当前区块哈希 `Block Hash (64位十六进制)`；
   - 前驱哈希 `Previous Hash`；
   - 验证徽章 `● HMAC-SHA256 存证签名有效吻合`。

- [ ] **Step 2: 实现取证凭据一键复制与报告导出**

- `📋 复制存证凭据`：将包含 Trace ID、时间、哈希链指纹的 JSON 存证摘要复制至剪贴板；
- `📄 导出取证报告 (Markdown)`：生成结构化事件调查报告供安全审计人员归档与司法取证。

- [ ] **Step 3: 严格校验保留测试 DOM 契约**

确认以下四大探针 100% 存在且可交互：
- `id="tab-audit"`
- `id="auditForensicDrawer"`
- `toggleAuditLiveStream`
- `exportAuditNdjson`

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
git add -A && git commit -m "feat(audit): add hash chain ribbon, audit radar kpis, and enhanced forensic drawer"
```

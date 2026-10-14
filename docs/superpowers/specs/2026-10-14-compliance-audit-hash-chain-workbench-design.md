# 合规审计与全链路防篡改存证工作台 (Compliance Audit & Hash Chain Studio) 设计规范

- **状态**: Approved
- **日期**: 2026-10-14
- **模块**: `web/src/views/AuditView.vue`, `src/observe/audit_hash_chain.c`, `src/server/admin_api.c`
- **目标**: 将 aigate 的安全审计控制台全面升级为**全能综合型合规审计与密码学防篡改存证工作台**，包含顶层审计态势雷达、密码学区块链条拓扑可视化胶囊流 (Hash Chain Ribbon)、多维复合事件检索与实时无锁环形流、深度取证抽屉 (`#auditForensicDrawer`) 与取证报告生成，严格保持 100% 自动化测试契约与单元测试通过率。

---

## 1. 业务目标与架构背景

### 1.1 业务诉求
在金融、政企、医疗与安全红队对抗等强监管场景中，大模型网关必须具备数学级的**不可篡改**与**不可抵赖**存证审计能力。
aigate 底层虽然已具备纯 C 实现的 HMAC-SHA256 密码学哈希链 (`audit_hash_chain_sign` & `audit_hash_chain_verify_file`) 与无锁环形缓冲区 (`audit_logger_query_recent`)，但现存的前端 `AuditView.vue` 界面较为简单：
1. 缺乏顶层全局审计态势的大盘洞察（已签名高度、防篡改校验状态、各级别违规分布、实时流吞吐）；
2. 密码学哈希链仅有文本提示，缺乏区块前驱哈希绑定、断链红色告警与穿透排障的可视化拓扑（Hash Chain Ribbon）；
3. 审计事件检索缺少多维过滤（严重度、模型、租户、客户端 IP、一键仅看违规）；
4. 取证抽屉排版较为原始，缺少密码学规范化待签名串的校验比对与结构化 Markdown/JSON 取证报告导出功能。

### 1.2 架构原则
1. **纯 C 引擎与零拷贝性能**：无缝对接现有 `GET /admin/v1/audit/events`、`GET /admin/v1/audit/violations` 与 `POST /admin/v1/audit/chain/verify`，保证毫秒级拉取与亚毫秒级断链排查；
2. **密码学完整性透明可视化**：以区块胶囊拓扑带直观表达 $H_i = \text{HMAC-SHA256}(\text{Secret}, H_{i-1} \parallel \text{Seq}_i \parallel \text{Payload})$，发生单点篡改即刻高亮标红断裂起始序号 `broken_seq`；
3. **复合双模态协同**：以实时内存环形流为主，配合「🚨 仅看违规」快速切片与 NDJSON 离线归档；
4. **严格 DOM 测试契约保持**：严格保留现有自动化测试探针与函数（`#tab-audit`, `#auditForensicDrawer`, `toggleAuditLiveStream`, `exportAuditNdjson`），保证现有 352+ C 单元测试 100% 通过。

---

## 2. 界面与交互设计规范

### 2.1 顶层审计与存证态势雷达大盘 (Audit Radar Cards)
位于页面顶部标题下方，设计为 4 栅格响应式卡片组：

| 栅格卡片 | 指标字段与来源 | 样式与视觉强调 | 说明与业务价值 |
| :--- | :--- | :--- | :--- |
| **🔐 已签名存证区块** | `chainStatus?.total_records \|\| auditEvents.length` | 靛蓝光晕，等宽字体 `font-mono` | 展示密码学链上累计不可抵赖区块高度 |
| **🛡️ 哈希链完整性状态** | `chainStatus?.valid` (PASS / TAMPER) | 正常翡翠绿 `100% 完整`；异常猩红 `❌ 检测到断链` | 反映整个审计日志文件的密码学完整性校验结果 |
| **🚨 违规与拦截事件** | 严重度为 `CRITICAL` / `WARN` 的事件总数 | 玫瑰红 `text-rose-400` 突出展示 | 实时聚焦越狱注入、敏感词触发、SLA 降级等安全事件 |
| **⚡ 实时审计流状态** | `livePolling` (Active / Paused) | 天空蓝 `text-sky-400`，带脉冲呼吸指示 | 展示内存环形缓冲区 (Ring-Buffer) 实时捕获状态 |

**快捷操作栏**：
- `🔐 校验哈希链完整性`：发起 `POST /admin/v1/audit/chain/verify` 并展示 loading 动画；
- `▶️ 开启实时流 / ⏸️ 暂停审计流`：方法名 `toggleAuditLiveStream`，3 秒定时拉取最新事件；
- `📥 导出 NDJSON`：方法名 `exportAuditNdjson`，将当前事件集合导出为 `.ndjson` 存证文件；
- `🔄 刷新`：拉取最新事件并更新大盘。

---

### 2.2 密码学哈希链拓扑胶囊带 (Hash Chain Ribbon)

#### 2.2.1 区块流展示
在中部以水平滚动的区块胶囊流展示最新已签名区块：
- 每个区块节点包含：
  - 区块高度标签：`Block #seq`（最新区块带有 `Tip` 标识）；
  - 当前哈希简写：`hash.slice(0, 8)...`；
  - 前驱哈希引用：`Prev: prev_hash.slice(0, 8)...`；
  - 关联业务简述：时间（如 `20:16:15`）、模型（如 `gpt-4o`）、处置动作（`PASS` / `BLOCK`）；
  - 状态指示灯：绿色圆点（有效）/ 蓝色光晕（最新区块）。
- 区块之间以箭头 `➔` 链接，形象展示密码学单向哈希链指针。

#### 2.2.2 篡改与断链预警 (Tamper Alert)
当验真接口返回 `valid: false` 时：
- 顶部展示醒目预警卡片：
  - `❌ 警告：检测到审计日志篡改或链断裂！`
  - `断裂起始区块：Block #broken_seq`
  - `底层原因：broken_reason`（例如：`prev_hash mismatch` 或 `payload hash mismatch`）；
- 区块拓扑带将断裂点节点渲染为猩红边框并带有红色脉冲光，支持直接点击穿透到该区块进行深入取证。

#### 2.2.3 点击区块与取证抽屉联动
点击任意区块胶囊，自动将该区块对应的审计记录载入右侧 `#auditForensicDrawer`，并高亮显示其哈希凭据。

---

### 2.3 现代化多维审计事件检索表 (Audit Events Table)

#### 2.3.1 复合过滤工具栏
1. **全文检索输入框**：支持对 `client_ip`、`trace_id`、`prompt_snippet`、`rule_tag` 进行前端即时过滤；
2. **严重等级下拉选择**：全部 / `CRITICAL` / `WARN` / `INFO`；
3. **模型下拉选择**：动态根据事件列表提取不重复模型选项；
4. **🚨「仅看违规拦截」快速切片按钮**：
   - 切换状态后仅展示严重等级非 `INFO`，或状态码 $\ge 400$，或包含 `violation_type` 的事件；
   - 按钮显示当前违规事件条数计数。

#### 2.3.2 表格列定义 (NDataTable)
- **时间 (Time)**：格式化时间字符串，等宽字体；
- **区块序号 (Block Seq)**：`#seq_id`，以青色标签展示；
- **严重度 (Severity)**：
  - `CRITICAL`：红色标签 `error`；
  - `WARN`：橙色标签 `warning`；
  - `INFO`：浅灰标签 `default`；
- **调用模型 / 上游路由**：例如 `gpt-4o · openai`，发生 SLA 回退时标注 `(降级)`；
- **客户端 IP**：等宽字体渲染客户端 IPv4/IPv6；
- **耗时与消耗**：例如 `120ms (TTFT 35ms) · 57 tok`；
- **触发规则与状态**：
  - 阻断：`⛔ 阻断 [dan_prompt] (403)`；
  - 告警：`⚠️ 告警 [pii_leak]`；
  - 放行：`✅ 正常放行 (200)`；
- **操作 (Actions)**：`查看取证` 按钮，点击唤起抽屉。

---

### 2.4 深度取证抽屉 (`#auditForensicDrawer`)

#### 2.4.1 DOM 契约保证
容器必须显式声明 `id="auditForensicDrawer"`，保证已有自动化测试套件无缝识别。

#### 2.4.2 抽屉展示分区
1. **基础元数据卡片**：
   - 事件编号、全局 Trace ID、发生时间、客户端 IP 与租户、HTTP 状态码、总耗时、首字延迟 TTFT、Prompt/Completion Token 计数。
2. **安全风险与处置留存**：
   - 命中规则标签与触发原因；
   - 截获的敏感/攻击提示词片段 (`prompt_snippet` 或 `payload`)；
   - SLA 降级与路由回退原因 (`fallback_reason`)。
3. **密码学存证凭证 (HMAC-SHA256 Proof)**：
   - 当前区块哈希 `block_hash`；
   - 前驱区块哈希 `prev_hash`；
   - 密码学签名有效性徽章（`● HMAC-SHA256 存证签名有效吻合`）。
4. **合规取证报告导出**：
   - `📋 复制存证凭据`：一键复制 JSON 存证摘要；
   - `📄 导出取证报告 (Markdown)`：生成结构化取证报告文档（包含网关签名、事件时间戳、违规内容样本与哈希值）。

---

## 3. 接口数据协议规范

### 3.1 验真接口：`POST /admin/v1/audit/chain/verify`
- **请求体**：空或 `{}`
- **响应体**：
```json
{
  "status": "ok",
  "valid": true,
  "total_records": 128,
  "head_seq": 128,
  "head_hash": "4f89d3c50db21a78e901ff2a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e",
  "broken_seq": 0,
  "broken_reason": null
}
```
*异常场景（被篡改）*：
```json
{
  "status": "ok",
  "valid": false,
  "total_records": 105,
  "head_seq": 128,
  "head_hash": "4f89d3c50db2...",
  "broken_seq": 106,
  "broken_reason": "prev_hash mismatch at record 106"
}
```
*前端兼容层*：同时兼容读取 `resp.valid || resp.verified`、`resp.total_records || resp.blocks_checked`、`resp.head_hash || resp.latest_hash`。

### 3.2 实时流接口：`GET /admin/v1/audit/events`
- **查询参数**：`limit=50&after_seq=0`
- **响应体**：
```json
{
  "status": "ok",
  "events": [
    {
      "seq_id": 108,
      "trace_id": "tr-9a1b2c3d",
      "tenant_id": "tenant_prod_01",
      "client_ip": "192.168.1.12",
      "model": "gpt-4o",
      "routed_model": "gpt-4o",
      "provider": "openai",
      "http_status": 200,
      "prompt_tokens": 12,
      "completion_tokens": 45,
      "ttft_ms": 35,
      "total_latency_ms": 120,
      "severity": "INFO",
      "violation_type": "",
      "rule_detail": "",
      "rule_tag": "NONE",
      "fallback_reason": "",
      "prompt_snippet": "What is quicksort?",
      "timestamp_ms": 1791980000000
    }
  ],
  "latest_seq": 108,
  "missed_count": 0
}
```

---

## 4. 自动化测试与质量契约

### 4.1 DOM 测试契约
必须严格保留并导出以下 DOM 元素与标识：
- `id="tab-audit"`（根容器）
- `id="auditForensicDrawer"`（取证抽屉组件）
- `toggleAuditLiveStream`（用于单元测试与控制台交互的流控制函数）
- `exportAuditNdjson`（用于单元测试与导出 NDJSON 的函数）

### 4.2 编译与回归指标
- 前端打包：`npm run build` 生成 `web/dist/index.html` 并内嵌；
- C 语言烘焙：`python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h`；
- C 编译要求：`make -C build` 必须在 `-Wall -Wextra -Werror` 下 0 警告 0 报错；
- 单元测试：`./build/tests/aigate_unit_tests` 必须 100% 全部通过 (352/352, 0 failures)。

# 上游服务商与智能模型路由中枢 (Providers & Models Router) 设计规范

- **状态**: Approved
- **日期**: 2026-10-15
- **模块**: `web/src/views/ProvidersView.vue`, `web/src/views/ModelsView.vue`, `src/upstream/model_router.c`, `src/server/admin_api.c`, `src/store/pg_store.h`
- **目标**: 将 aigate 的上游服务商管理 (`ProvidersView.vue`) 与模型路由配置 (`ModelsView.vue`) 深度融合，打造成全能双驱的**智能模型路由与多渠道弹性容灾中枢**。涵盖服务商动态健康探活大盘、多目标权重与主备路由池 (`targets[]`)、SLA 故障自动降级、深度推理 (Reasoning / Thinking) 视窗，以及免外部调用的内置智能路由仿真沙箱。

---

## 1. 业务目标与架构背景

### 1.1 业务痛点与技术诉求
在企业级大模型网关中，单一上游渠道存在极高单点故障风险（如 OpenAI 偶发 500、Claude 速率限制 429、网络专线抖动），且推理算力存在跨云多供应商多副本的部署需求。
aigate 底层纯 C 数据面（`model_router.c`, `circuit_breaker.c`, `health_prober.c`, `pg_store.h`）已内建强大的企业级特性：
1. **多目标路由数组 (`targets[]`)**：支持为单个公开模型挂载最多 8 个物理后端目标，每个目标拥有独立优先级（`priority`）、权重（`weight`）、凭证与端点；
2. **多模负载均衡算法 (`lb_policy`)**：支持严格主备顺序 (`priority`)、按权重平滑轮询 (`weighted` / `weighted_round_robin`)，以及基于两两抽样最小延迟的自适应调度 (`p2c` / `adaptive_latency`)；
3. **SLA 熔断与平滑降级链 (`circuit_breaker.c`)**：支持在主模型故障或超时达到门限时，自动无感重写为备选模型 (`fallback_model`)；
4. **深度推理引擎适配 (`supports_reasoning`, `thinking_budget`, `tag_emulation_default`)**：支持对 DeepSeek-R1、o1/o3-mini、Claude 3.7 Sonnet 进行思考 Token 预算注入与 `<thought>` 标签协议适配；
5. **主动探活心跳检测 (`health_prober.c`)**：后台周期性探测所有供应商网络健康并暴露 `/admin/v1/providers/health`。

然而，现有的前端控制台界面（`ProvidersView.vue` 和 `ModelsView.vue`）停留在简单的单行单目标表格，没有将上述核心能力暴露给运维与管理人员。

### 1.2 核心设计原则
1. **零学习成本与渐进增强**：单目标配置与多目标路由池平滑兼容，不破坏现有数据库与接口兼容性；
2. **状态透明与态势感知**：实时感知上游网络探针 RTT 与熔断状态机流转，变被动排障为主动预防；
3. **仿真驱动配置 (Simulation First)**：内置路由沙箱抽屉，配置变更后可在控制台一键推演选路结果与容灾逃生链路，杜绝线上失误；
4. **工程与测试质量闭环**：保持 Vite 单文件嵌入规范，维持 352+ 项 C 单元测试 100% 通过与 `-Wall -Wextra -Werror` 零警告标准。

---

## 2. 界面与交互设计规范

### 2.1 供应商健康态势与实时巡检大盘 (`ProvidersView.vue`)

#### 2.1.1 顶部 4 栅格健康态势雷达卡片
| 栅格卡片 | 数据来源 / 计算方式 | 视觉样式 | 核心价值 |
| :--- | :--- | :--- | :--- |
| 🟢 **健康在线渠道** | 状态为 `healthy` / `CLOSED` 的服务商占比 | 翠绿色 `text-emerald-400` | 宏观洞察各主流渠道的整体可用性 |
| 🟡 **熔断与降级预警** | 状态为 `DEGRADED` 或 `OPEN` 的服务商数量 | 琥珀黄 `text-amber-400` | 实时警示 SLA 超标或已被摘除的通道 |
| ⚡ **全局平均探活 RTT** | 各渠道探针响应时延加权均值 | 亮天蓝 `text-sky-400` | 评估数据面通往上游大模型 API 的网络连通质量 |
| 📦 **承载模型总数** | 已挂载并生效的公开模型总数 | 紫罗兰 `text-indigo-400` | 掌握网关当前对外暴露的推理模型资产规模 |

#### 2.1.2 供应商列表增强
- **状态指示呼吸灯**：每行供应商 ID 左侧带有状态圆点（🟢 在线、🟡 SLA 降级、🔴 熔断摘除、⚪ 探测中）；
- **密钥多形态展示**：区分环境变量引用（`env:NAME`）、加密凭证（`pg:BLOB`）与直接密钥，防止密钥泄露；
- **全量并发探活巡检**：顶部保留“⚡ 全量探针测速巡检”按钮，点击调用 `POST /admin/v1/providers/probe`，实时更新各供应商 RTT 时延；
- **承载模型快速联动**：展示“N 个模型”胶囊，点击即可联动跳转筛选对应模型。

---

### 2.2 模型多目标弹性路由池 (`ModelsView.vue` Targets 构建)

#### 2.2.1 可视化多目标上游池 (Upstream Target Pool)
在模型编辑/新增弹窗中，全面支持配置 1~8 个目标节点 (`targets[]`)：
- **服务商与端点**：选择服务商通道（OpenAI / Azure / Anthropic / DeepSeek / 本地 Ollama 等）与端点 URL；
- **主备梯队分级 (`priority`)**：
  - `Priority 0`：主力集群（同梯队按权重并行承接流量）；
  - `Priority 1 / 2`：热备与逃生集群（仅在 Priority 0 主力全部熔断时激活）；
- **流量权重 (`weight`) 与分流预览条**：
  - 支持数字权重设置（如 7:3）；
  - 动态渲染“流量分配预期条 (Traffic Ratio Preview)”，直观体现各主力通道承载的流量百分比；
- **单目标最大并发 (`max_concurrency`)**：防止单一上游通道由于超额并发而被供应商封禁。

#### 2.2.2 负载均衡调度算法 (`lb_policy`) 切换
- `priority`：严格主备梯队降级；
- `weighted` / `weighted_round_robin`：按配置权重比例平滑分流；
- `p2c (Power of Two Choices) / adaptive_latency`：两两随机抽样选择实时网络 RTT 最小的健康目标，实现超低延迟智能路由。

---

### 2.3 SLA 容灾降级模型与深度思考 (Reasoning) 参数视窗

#### 2.3.1 SLA 容灾降级模型 (`fallback_model`)
- **透明重写选路**：当模型的所有上游 targets 均处于熔断或超时不可用时，系统自动无缝重写请求为备选模型（如 `gpt-4o` 故障时透明切至 `deepseek-v3`）；
- **动态阈值展示**：显示熔断错误率门限（如 40%）与延迟劣化门限（如 3000ms）。

#### 2.3.2 深度思考推理 (Reasoning) 参数视窗
- **支持深度思考推理 (`supports_reasoning`)**：一键开启推理模型特性；
- **默认思考预算 (`thinking_budget`)**：设置默认思考 Token 额度（如 8,192 tok），在客户端未传递时自动注入；
- **思考标签模拟 (`tag_emulation_default`)**：自动将上游不同的推理字段格式统一包装为标准 `<thought>...</thought>` 标签，确保通用客户端兼容；
- **推理独立计费倍率 (`pricing_reasoning`)**：支持为思维链 Token 设置专属费率。

---

### 2.4 智能路由仿真沙箱与端到端诊断 (Routing Simulator Drawer)

在 `ModelsView.vue` 的每行模型操作列增加“🔬 路由仿真”按钮，点击滑出右侧仿真抽屉：
1. **算法选路命中报告**：根据当前配置的 `lb_policy`、`priority` 与实时健康度，实时推演请求将命中的具体 Target 节点；
2. **容灾逃生推演**：直观展示若当前 Target 发生熔断故障时，流量将如何自动流转至第二主力、热备梯队或备用模型；
3. **参数重写与注入校验**：校验 `thinking_budget` 与 `<thought>` 标签模拟规则；
4. **一键重复模拟**：支持运维人员快速点击重测，直观感知权重随机抽样分布。

---

## 3. 接口契约与数据流规范

### 3.1 供应商相关接口 (`src/server/admin_api.c`)
- `GET /admin/v1/providers`: 查询供应商列表及基本信息；
- `POST /admin/v1/providers`: 新增供应商；
- `PUT /admin/v1/providers/:id`: 更新供应商配置；
- `DELETE /admin/v1/providers/:id`: 删除供应商；
- `GET /admin/v1/providers/health`: 获取所有上游供应商的探活健康快照与 RTT 时延；
- `POST /admin/v1/providers/probe`: 触发全局即时并发探针测速；
- `POST /admin/v1/providers/:id/probe`: 触发指定供应商单点测速。

### 3.2 模型路由相关接口 (`src/server/admin_api.c`)
- `GET /admin/v1/models`: 查询所有模型，包含 `targets` 数组、`lb_policy`、`fallback_model` 及 reasoning 配置；
- `POST /admin/v1/models`: 新增模型，支持提交 `targets` 数组与调度策略；
- `PUT /admin/v1/models/:name`: 更新模型，完整支持多目标权重、熔断降级与思考参数；
- `DELETE /admin/v1/models/:name`: 下线删除模型。

---

## 4. 实施与质量保证

1. **测试驱动开发与契约保证**：
   - 保持已有 DOM 标识及核心方法兼容；
   - 保证 C 语言单元测试中对多目标路由、P2C 算法及 reasoning 参数测试用例全量保持绿灯（`PASS 352/352`）。
2. **构建与集成闭环**：
   - 前端 Vite 生成单文件 bundle 并嵌入 `admin_ui_html.h`；
   - 保证编译无任何 Warning（`-Wall -Wextra -Werror`）。

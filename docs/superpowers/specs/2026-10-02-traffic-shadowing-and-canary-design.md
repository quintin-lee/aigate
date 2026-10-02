# 流量镜像与金丝雀灰度分流评估引擎 (Traffic Shadowing & Canary A/B Testing Engine) 设计规范

- **状态**: 草案 (Draft / Pending Approval)
- **创建日期**: 2026-10-02
- **责任模块**: `src/policy/shadow.h`, `src/policy/shadow.c`, `src/core/model_router.c`, `src/core/pipeline_chat.c`, `src/server/admin_api.c`, `web/admin.html`
- **目标**: 构建纯 C17 高性能流量镜像与金丝雀灰度分流评估引擎，支持毫秒级异步零风险流量复制、真实生产流量比例灰度切流、多维模型回答 Side-by-Side 对比与时延/成本效益评估看板。

---

## 1. 背景与目标 (Context & Objectives)

在企业大模型落地过程中，从昂贵的高端专有模型（如 GPT-4o、Claude 3.5 Sonnet）向高性价比模型（如 DeepSeek-V3/R1、开源模型微调版本）迁移，或者在同系列模型的升级迭代（如 3.5 到 4.0）过程中，技术团队面临核心挑战：
1. **模型回答质量评估风险**：大模型属于非确定性生成系统，静态 Benchmark 难以反映复杂真实业务上下文下的问答一致性与合规性；
2. **生产流量直接切流风险**：若直接切流，新候选模型可能面临偶发 5xx 错误、未知并发限流或响应耗时严重劣化；
3. **性能与降本效益缺乏量化**：缺乏端到端对比同一批真实业务请求在不同模型下的首字时延 (TTFT)、总响应时间与 Token 成本节约账单。

本引擎旨在提供工业级双模流量调度与评测解决方案：
1. **纯异步零风险流量镜像 (Asynchronous Traffic Shadowing)**：
   - 在网关层拦截命中规则的生产流量，以 `< 10μs` 的极低开销将请求克隆并投递到后台异步队列；
   - 生产客户端完全无感知，主请求响应时间与稳定性 100% 隔离不受任何干扰；
   - 后台专有 Worker 执行影子模型调用并记录详细耗时、TTFT、Token 计数与文本摘要。
2. **渐进式金丝雀灰度分流 (Progressive Canary Routing)**：
   - 支持按比例（如 5%、20%、50%）将生产客户端的真实流量切流至新候选模型；
   - 响应头自动注入 `X-Aigate-Canary: true`，出现持续 5xx 异常时自动触发熔断回滚，切回主模型保护业务。
3. **Side-by-Side 文本与效益双路对比看板 (Interactive Evaluation Console)**：
   - 内存环形队列维护最近 200 条双路快照，支持在 Web 控制台进行 Prompt、主回答 vs 影子回答的双栏 Diff 检视；
   - 自动生成时延优化比例（如 `-52%`）、Token 费用节约预估（如 `-85%`）与模型可用率对比报告。

---

## 2. 总体架构与数据流 (Architecture & Data Flow)

```
[ 客户端请求 POST /v1/chat/completions ]
             │
             ▼
   [ 1. 鉴权与前置安全风控 (Auth & Inbound Guardrails) ]
             │
             ▼
   [ 2. 路由与分流决策 (Router & Shadow/Canary Hook) ]
             │
      ┌──────┴────────────────────────────────────────────────┐
      │ 命中 CANARY 金丝雀模式 (例如 15% 流量)                  │ 命中 SHADOW 镜像模式 (例如 20% 流量)
      ▼                                                       ▼
[ 替换目标为候选模型 ]                                     [ 零延迟极速克隆请求上下文 (< 10μs) ]
• 正常进入主流水线                                           • 投递到无锁有界队列 shadow_queue_t (256容量)
• 客户端响应注入 X-Aigate-Canary 头                         • 若满载立即丢弃并记数，绝不阻塞！
• 记录金丝雀指标 (遇 5xx 自动回滚)                           │
      │                                                       ▼ (主请求毫发无损继续执行)
      │                                             [ 3. 后台独立影子 Worker 线程池 ]
      │                                                       │
      │                                                       ├─► 协议转换与重定向至 target_model
      │                                                       ├─► 向上游发送影子 HTTP 调用 (带专用超时)
      │                                                       ├─► 计算影子 TTFT、总时延、Token 与费用
      │                                                       └─► 截取前 500 字符文本回答
      │                                                               │
      ▼                                                               ▼
   [ 4. 主请求正常响应客户端 (完成) ] ──────────────► [ 5. 双路对齐与归并引擎 (Pair Merger) ]
                                                                      │
                                                                      ├─► 生成 shadow_eval_item_t
                                                                      ├─► 写入内存极速环形池 (最近 200 条)
                                                                      └─► 更新原子聚合统计看板 (时延比/省钱率)
```

---

## 3. 详细设计 (Detailed Design)

### 3.1 核心数据结构 (`src/policy/shadow.h`)

#### 3.1.1 分流模式与规则定义
```c
/**
 * @brief 流量分流评测模式枚举
 */
typedef enum {
    TRAFFIC_MODE_SHADOW = 0, /**< 纯异步影子镜像：主请求正常返回，后台静默复制调用影子模型 */
    TRAFFIC_MODE_CANARY = 1  /**< 金丝雀灰度分流：按比例将真实客户端流量切换至候选模型 */
} traffic_mode_t;

/**
 * @brief 流量镜像与金丝雀分流规则
 */
typedef struct {
    long           id;                  /**< 规则主键 ID */
    char           source_model[64];    /**< 源模型标识 (例如 "gpt-4o") */
    char           target_model[64];    /**< 目标候选/影子模型 (例如 "deepseek-chat") */
    char           target_provider[32]; /**< 指定目标供应商 (可选，例如 "deepseek") */
    traffic_mode_t mode;                /**< 分流模式: SHADOW 或 CANARY */
    double         sample_rate;         /**< 采样率或灰度权重 (0.0 ~ 1.0) */
    char           header_match[64];    /**< 可选 Header 过滤 (例如 "x-env: test") */
    bool           enabled;             /**< 是否启用 */
    uint32_t       timeout_ms;          /**< 影子请求专属超时 (默认 10000ms，防挂死) */
} shadow_rule_t;
```

#### 3.1.2 评测对比快照 (`shadow_eval_item_t`)
```c
/**
 * @brief 单次请求双路对比评估记录
 */
typedef struct {
    char     eval_id[33];             /**< 唯一评估 ID (16 字节十六进制) */
    char     trace_id[33];            /**< 关联的 OpenTelemetry W3C Trace ID */
    char     source_model[64];        /**< 主请求模型 */
    char     target_model[64];        /**< 影子/金丝雀模型 */
    traffic_mode_t mode;              /**< SHADOW 还是 CANARY */
    
    /* 性能指标对比 */
    double   primary_latency_ms;      /**< 主响应总时延 (ms) */
    double   shadow_latency_ms;       /**< 影子响应总时延 (ms) */
    double   primary_ttft_ms;         /**< 主响应首字延迟 TTFT (ms) */
    double   shadow_ttft_ms;          /**< 影子响应首字延迟 TTFT (ms) */
    
    /* 状态与消耗对比 */
    int      primary_http_status;     /**< 主响应 HTTP 状态码 */
    int      shadow_http_status;      /**< 影子响应 HTTP 状态码 */
    long     primary_tokens;          /**< 主响应 Prompt + Completion Tokens */
    long     shadow_tokens;           /**< 影子响应 Prompt + Completion Tokens */
    double   primary_cost_usd;        /**< 主模型费用估算 ($) */
    double   shadow_cost_usd;         /**< 影子模型费用估算 ($) */
    
    /* 响应内容截取 (用于控制台文本 Diff 检查) */
    char     prompt_preview[256];     /**< 提示词前 255 字符摘要 */
    char     primary_resp_snippet[512];/**< 主模型回答摘要 */
    char     shadow_resp_snippet[512]; /**< 影子模型回答摘要 */
    
    uint64_t timestamp_us;            /**< 请求发生的时间戳 (微秒) */
} shadow_eval_item_t;
```

#### 3.1.3 影子引擎状态与缓冲区
```c
#define SHADOW_QUEUE_CAPACITY 256
#define SHADOW_RECENT_EVAL_CAPACITY 200

/**
 * @brief 异步影子任务数据
 */
typedef struct {
    char            eval_id[33];
    char            trace_id[33];
    shadow_rule_t   rule;
    char*           body_copy;
    size_t          body_len;
    char            prompt_preview[256];
    double          primary_latency_ms;
    double          primary_ttft_ms;
    int             primary_http_status;
    long            primary_tokens;
    double          primary_cost_usd;
    char            primary_resp_snippet[512];
    uint64_t        timestamp_us;
} shadow_task_t;

/**
 * @brief 影子引擎全局管理器
 */
typedef struct shadow_engine shadow_engine_t;
```

---

### 3.2 核心执行流水线调度

1. **路由与分流钩子 (`src/core/model_router.c`)**：
   - 规则匹配优先评估 `mode == TRAFFIC_MODE_CANARY`；
   - 若命中金丝雀采样，路由重定向至 `target_model`，在下游响应头中输出 `x-aigate-canary: true`；
   - 金丝雀链路继承现有断路器，3 次 5xx 自动熔断回滚至源模型。
2. **流水线镜像分流钩子 (`src/core/pipeline_chat.c`)**：
   - 若命中 `mode == TRAFFIC_MODE_SHADOW`：
   - 提取 Prompt 前 255 字符摘要；
   - 异步入队：将请求入参打包为 `shadow_task_t`，非阻塞推入有界环形队列；
   - 队列满载立即丢弃并原子递增 `dropped_shadow_requests` 计数器，保证主流程时延开销 `< 10μs`。
3. **后台 Worker 线程执行 (`src/policy/shadow.c`)**：
   - 线程弹出任务，利用 `provider_adapter` 改写 `model` 字段并发起独立 HTTP 客户端调用；
   - 严格应用规则的 `timeout_ms` 限制，防止影子端点挂起耗尽资源；
   - 捕获状态码、耗时、TTFT 首字延迟、Token 统计与回答前 512 字节摘要；
   - 归并生成 `shadow_eval_item_t` 并存入 200 容量的全局循环评测池中；
   - 原子更新累计 KPI：`shadow_total_count`、`cost_saved_usd`、`avg_latency_reduction_pct`、`shadow_success_rate`。

---

### 3.3 数据库模型与存储设计

1. **`shadow_rules` (分流规则表)**：
   ```sql
   CREATE TABLE IF NOT EXISTS shadow_rules (
       id SERIAL PRIMARY KEY,
       source_model VARCHAR(64) NOT NULL,
       target_model VARCHAR(64) NOT NULL,
       target_provider VARCHAR(32) DEFAULT '',
       mode VARCHAR(16) NOT NULL DEFAULT 'shadow',
       sample_rate DOUBLE PRECISION NOT NULL DEFAULT 1.0,
       header_match VARCHAR(64) DEFAULT '',
       enabled BOOLEAN NOT NULL DEFAULT TRUE,
       timeout_ms INT NOT NULL DEFAULT 10000,
       created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
   );
   ```
2. **`shadow_eval_metrics` (聚合对比统计表)**：
   ```sql
   CREATE TABLE IF NOT EXISTS shadow_eval_metrics (
       id SERIAL PRIMARY KEY,
       date_bucket VARCHAR(16) NOT NULL,
       source_model VARCHAR(64) NOT NULL,
       target_model VARCHAR(64) NOT NULL,
       eval_count INT DEFAULT 0,
       primary_cost_usd DOUBLE PRECISION DEFAULT 0.0,
       shadow_cost_usd DOUBLE PRECISION DEFAULT 0.0,
       primary_avg_lat_ms DOUBLE PRECISION DEFAULT 0.0,
       shadow_avg_lat_ms DOUBLE PRECISION DEFAULT 0.0,
       shadow_err_count INT DEFAULT 0
   );
   ```

---

### 3.4 管理端点规范 (Admin REST API)

| 方法 | 路径 | 描述 |
|---|---|---|
| `GET` | `/admin/v1/shadow/rules` | 获取所有配置的流量镜像与金丝雀分流规则列表 |
| `POST` | `/admin/v1/shadow/rules` | 新增流量镜像或金丝雀规则 |
| `PUT` | `/admin/v1/shadow/rules/:id` | 修改规则（启用状态、采样率、过滤头或超时时间） |
| `DELETE` | `/admin/v1/shadow/rules/:id` | 删除指定的规则 |
| `GET` | `/admin/v1/shadow/evaluations` | 查询内存循环池中最近的 Side-by-Side 详细对比快照 |
| `GET` | `/admin/v1/shadow/stats` | 获取全局聚合效益指标（累计评测数、时延降幅比、节约费用、成功率） |

---

### 3.5 Web 控制台交互界面 (`web/admin.html`)

1. **左侧新增专用导航标签**：`🧪 流量镜像与金丝雀 (Traffic Shadowing & Canary)`；
2. **4 大核心效益 KPI 统计卡片**：
   - `累计评测请求数` (Total Evaluated Requests)
   - `时延降幅对比` (Latency Reduction %, 例如 `240ms -> 118ms ⚡ -50.8%`)
   - `成本节约预估` (Estimated Cost Savings, 例如 `原 $18.40 vs 现 $2.10 💰 -88.5%`)
   - `影子可用率` (Shadow Health / Success Rate, 例如 `99.8%`)
3. **规则配置与管理卡片**：
   - 规则列表表格，带状态开关、源模型 -> 目标模型流向指示、采样比例滑动调节器；
   - 「添加评测规则」弹窗模态框 (`#shadowRuleModal`)；
4. **实时 Side-by-Side 双路对比数据流表格**：
   - 列表展示：时间、Prompt 摘要、主模型 vs 影子模型响应摘要、时延对比标签、费用节约徽章；
   - 行操作：
     - **`🔍 双路对比`**：点击弹出左右双栏文本对比抽屉 (`#shadowDiffModal`)，直观对照主模型与候选模型回答差异；
     - **`⚡ 追踪瀑布图`**：一键联动跳转至该请求的 OpenTelemetry 耗时瀑布图进行深度剖析。

---

## 4. 验证与验收标准 (Acceptance Criteria)

- [ ] 规则管理与解析：
  - 支持 `SHADOW`（纯镜像）与 `CANARY`（真实分流）模式配置与动态热生效；
  - 采样率与过滤头（`sample_rate`, `header_match`）判定准确。
- [ ] 异步流量镜像执行：
  - 生产主请求耗时不受后台镜像影响，入队耗时 `< 10μs`；
  - 队列满载时丢弃且不阻塞主请求，丢弃指标正常累计；
  - 影子请求具备独立的超时保护与独立连接管理。
- [ ] 金丝雀灰度分流与自愈：
  - 命中金丝雀请求正确注入 `X-Aigate-Canary: true` 响应头；
  - 上游候选模型连续报错时自动熔断回滚至主稳定模型。
- [ ] 评测对齐与看板：
  - 内存环形队列维护最近 200 条双路对比条目；
  - Admin REST API 正确返回对比快照与聚合统计；
  - Web 控制台提供直观的 KPI 卡片、规则配置面板与左右双栏对比模态框。
- [ ] 构建与质量门禁：
  - CTest 全量通过率 100%；
  - Doxygen 保持严格 0 Warning；
  - 纯 C17 标准实现。

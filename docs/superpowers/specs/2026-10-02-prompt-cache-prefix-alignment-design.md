# 提示词前缀对齐与 KV/Prompt Cache 命中优化引擎设计规范

- **状态**: 已实现并通过验证 (Implemented & Verified)
- **创建日期**: 2026-10-02
- **责任模块**: `src/policy/cache_optimizer.h`, `src/policy/cache_optimizer.c`, `src/core/pipeline_chat.c`, `src/core/aigate_core.c`, `src/server/admin_api.c`, `src/store/pg_store.c`, `web/admin.html`
- **目标**: 构建纯 C17 高性能提示词前缀对齐与 KV/Prompt Cache 命中优化引擎，在微秒级执行开销（$< 300\mu\text{s}$）内实现 Tools 字典序稳定排序、易变动态时戳与 UUID 智能下沉、Anthropic Claude 智能分块加权打点（至多 4 处 `cache_control: {"type": "ephemeral"}`）以及多协议响应 Usage 归一化解析，为公有云与私有化部署（vLLM / SGLang RadixAttention）带来 50%~90% 的上游计算成本缩减与高达 80% 的首字时延优化。

---

## 1. 背景与业务痛点 (Context & Objectives)

现代主流大语言模型（如 Anthropic Claude 3.5 Sonnet / Haiku、OpenAI GPT-4o / GPT-4o-mini）以及开源私有化推理框架（如 vLLM PagedAttention、SGLang RadixAttention、DeepSeek）均已广泛支持**提示词缓存 (Prompt / KV Caching)**。
- 命中缓存的输入 Token 可享有 **50%（OpenAI）至 90%（Anthropic）的费用折扣**；
- 模型无需重新前向计算庞大的静态前缀，显著减少 GPU 显存注意力计算，**首字时延 (TTFT) 降低高达 70%~80%**。

但在实际生产网关中，客户端发来的请求往往无法稳定命中上游缓存，核心瓶颈在于：
1. **工具列表（Tools / Functions）序列无序抖动**：客户端 SDK 或 Agent 框架生成工具列表时依赖哈希表或动态加载，造成相同功能的 Tools 数组在不同请求中顺序不一致，从第 1 个 Tool 开始就破坏了前缀 Token 序列一致性；
2. **易变动态内容置顶（Dynamic Content Header Pollution）**：业务框架习惯在 System Prompt 开头硬编码动态信息（例如：`"Today is 2026-10-02 17:10:00. You are a helpful assistant..."` 或当前会话 Session/Request ID），导致后续数千至数万 Token 的静态规则与 Few-Shot 示例全部 Cache Miss；
3. **缺少针对 Claude 的显式断点打点**：Anthropic 协议强制要求客户端在消息块中显式指定 `"cache_control": {"type": "ephemeral"}`（且限制单次请求最多 4 处、门槛 $\ge 1024$ tokens），普通客户端 SDK 并无此能力；
4. **指标与效益黑盒**：各模型提供方返回的缓存用量字段差异大（OpenAI 的 `prompt_tokens_details.cached_tokens` vs Anthropic 的 `cache_read_input_tokens`），运维团队无法度量缓存节约的实际 ROI。

本引擎在纯 C17 标准下，以极低开销（$< 300\mu\text{s}$）为网关提供全自动的端到端透明规整与打点方案。

---

## 2. 总体架构与流水线流向 (Architecture & Flow)

```
[ 客户端请求 POST /v1/chat/completions ]
                 │
                 ▼
       [ 1. 鉴权与前置安全风控 (Auth & Inbound Guardrails) ]
                 │
                 ▼
       [ 2. 网关本地响应缓存查表 (Local Response Cache) ]
                 ├─► [ 缓存命中 Cache HIT ] ─────────────► [ 直接快速返回，零上游费用 ]
                 │
                 ▼ (缓存未命中 Cache MISS)
       [ 3. 提示词压缩引擎 (Prompt Compressor) ] (若启用，先执行历史折叠/空白去噪)
                 │
                 ▼
       [ 4. 提示词缓存与前缀对齐优化引擎 (Prompt Cache Optimizer) ] ⭐ (核心新增)
                 │
                 ├─► 检查客户端 Header: `X-Aigate-Prompt-Cache` (auto | on | off)
                 ├─► 匹配规则表: model_pattern, sort_tools, sink_dynamic_system, inject_anthropic_breakpoints
                 │
                 ├─► 阶段一 (通用前缀规范化 - 字节流完美对齐)：
                 │     • Tools/Functions 数组按 `name` 严格字典序就地稳定重排；
                 │     • System Prompt 动态时戳 / UUID 智能检测与下沉至尾部；
                 │     • 冗余空白压缩与规范化；
                 │
                 ├─► 阶段二 (上游特定协议打点 - Claude Ephemeral Breakpoints)：
                 │     • 判定上游若为 Anthropic/Claude：
                 │     • 评估 System, Tools, 历史对白 Token 门槛 (>= 1024 tokens)；
                 │     • 加权注入 `cache_control: {"type": "ephemeral"}` (至多 4 处)；
                 │
                 └─► 记录优化快照至内存环形缓冲区 `cache_optimizer_snapshots` (200 槽位)
                 │
                 ▼
       [ 5. 模型路由与上游分发 (Model Router & Upstream Dispatch) ] ──► (发送前缀完美对齐的请求)
                 │
                 ▼
       [ 6. 响应用量解析与交付 (Settlement & Observability) ]
                 ├─► 解析上游 Response Usage:
                 │     • OpenAI/vLLM: `usage.prompt_tokens_details.cached_tokens`
                 │     • Anthropic: `usage.cache_read_input_tokens`, `usage.cache_creation_input_tokens`
                 ├─► 记录全局聚合指标（累计节省费用、平均命中率、TTFT 优化率）
                 ├─► 注入出向响应头:
                 │     • `X-Aigate-Prompt-Cache-Hit: true/false`
                 │     • `X-Aigate-Prompt-Cache-Tokens: <count>`
                 │     • `X-Aigate-Prompt-Cache-Savings: <$0.0000>`
                 └─► 记录 OpenTelemetry 追踪属性
```

---

## 3. 核心功能与关键算法设计

### 3.1 核心数据结构 (`src/policy/cache_optimizer.h`)

```c
#define CACHE_OPTIMIZER_MAX_SNAPSHOTS 200
#define ANTHROPIC_CACHE_MIN_TOKENS    1024
#define ANTHROPIC_MAX_BREAKPOINTS     4

/**
 * 缓存优化规则结构体
 */
typedef struct {
    uint32_t id;
    char     model_pattern[64];              /**< 模型通配符，例如 "claude-*" 或 "*" */
    bool     enabled;                        /**< 是否启用本规则 */
    bool     sort_tools;                     /**< 是否启用 Tools 字典序重排 */
    bool     sink_dynamic_system;            /**< 是否启用 System 易变动态时戳下沉 */
    bool     inject_anthropic_breakpoints;   /**< 是否启用 Anthropic 自动打点 */
    uint32_t min_tokens_threshold;           /**< 触发打点与优化的最小估算 Token 数 (默认 1024) */
    time_t   created_at;
    time_t   updated_at;
} cache_optimizer_rule_t;

/**
 * 单次执行优化结果
 */
typedef struct {
    bool     optimized;                      /**< 是否实际修改了 Payload */
    bool     tools_sorted;                   /**< 是否重排了 Tools 列表 */
    bool     dynamic_sunk;                   /**< 是否执行了动态内容下沉 */
    int      breakpoints_injected;           /**< 注入的断点数量 (0~4) */
    char*    optimized_payload;              /**< 优化后的新 JSON 字符串 (调用方释放) */
    size_t   optimized_len;                  /**< 优化后 Payload 长度 */
    uint32_t latency_us;                     /**< 执行开销 (微秒) */
} cache_optimizer_result_t;

/**
 * 内存环形快照条目 (用于 Web 控制台审查)
 */
typedef struct {
    char     req_id[64];                     /**< 请求 ID */
    char     model[64];                      /**< 模型名称 */
    time_t   timestamp;                      /**< 发生时间戳 */
    bool     upstream_cache_hit;             /**< 上游是否命中缓存 */
    uint32_t prompt_tokens;                  /**< 原始输入 Token 数 */
    uint32_t cached_tokens;                  /**< 命中缓存 Token 数 */
    double   cost_savings_usd;               /**< 节约美金 */
    uint32_t latency_us;                     /**< 执行耗时 */
    int      breakpoints_count;              /**< 注入断点数 */
    bool     dynamic_sunk;                   /**< 是否执行了时戳下沉 */
    bool     tools_sorted;                   /**< 是否重排了 Tools */
} cache_optimizer_snapshot_t;

/**
 * 全局效益聚合统计指标
 */
typedef struct {
    uint64_t total_optimized_requests;       /**< 经过优化的请求总数 */
    uint64_t upstream_cache_hit_requests;    /**< 上游命中缓存的请求总数 */
    uint64_t total_prompt_tokens;            /**< 总提示词 Token 数 */
    uint64_t total_cached_tokens;            /**< 命中的缓存 Token 总数 */
    double   total_savings_usd;              /**< 累计节省美金 */
    uint64_t avg_latency_us;                 /**< 平均优化执行耗时 */
} cache_optimizer_stats_t;
```

---

### 3.2 Tools 字典序稳定重排 (`cache_optimizer_sort_tools`)

1. 提取请求 JSON 中的 `tools` 或 `functions` 数组；
2. 若子项数量 $\ge 2$：
   - 提取每个 tool 对象的 `function.name`（若为 OpenAI 1.0+ 格式）或 `name`；
   - 采用稳定快速排序算法，按 ASCII 字典序递增排列；
   - 就地重建 cJSON 链表节点的 `next` / `prev` 指针；
3. 确保所有客户端生成的工具定义数组在字节流上绝对一致。

---

### 3.3 易变动态内容智能识别与下沉 (`cache_optimizer_sink_dynamic_system`)

1. 扫描 System 消息的文本开头部分；
2. 正则状态机识别典型的易变时空上下文：
   - ISO/常见日期时戳：`\d{4}[-/]\d{2}[-/]\d{2}`、`\d{2}:\d{2}:\d{2}`、`Today is ...`、`Current date:...`
   - 会话 UUID/ID：`[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-...`、`session_id:...`
3. 若首句或开头段落匹配到上述易变模式：
   - 剥离该动态短句；
   - 将剥离后的纯静态核心指令置于头部；
   - 将提取的动态元数据追加至 System 消息末尾（例如 `\n\n[Runtime Context: ...]\n`）；
4. 保证静态主体在物理前缀上保持 100% 字节流复用。

---

### 3.4 Anthropic 智能断点加权分配 (`cache_optimizer_inject_anthropic_breakpoints`)

针对请求模型包含 `claude` 或协议适配场景：
1. 评估各个组成部分的 Token 长度；
2. 按照以下优先级在有效位置注入 `"cache_control": {"type": "ephemeral"}`，且总数不超过 4 处：
   - **优先级 1 (Tools)**：若 Tools 数组总 Token $\ge 1024$，在最后一个 tool 中注入；
   - **优先级 2 (System)**：若下沉后的 System 静态规则总 Token $\ge 1024$，在 System 内容末尾注入；
   - **优先级 3 (历史对白)**：若存在多轮历史，在倒数第 2 轮 Assistant 回复末尾注入；
   - **优先级 4 (超长 RAG 知识)**：若存在独立的大文档块，在文档末尾注入。

---

### 3.5 上游缓存用量多协议归一化解析 (`cache_optimizer_parse_upstream_usage`)

* **OpenAI / vLLM / SGLang**：
  提取 `usage.prompt_tokens_details.cached_tokens`；
* **Anthropic**：
  提取 `usage.cache_read_input_tokens` 与 `usage.cache_creation_input_tokens`；
* **节约换算公式**：
  - Claude 模型：$1 \times 10^{-6} \times \text{cached\_tokens} \times (\text{Price}_{\text{input}} \times 0.90)$
  - OpenAI 模型：$1 \times 10^{-6} \times \text{cached\_tokens} \times (\text{Price}_{\text{input}} \times 0.50)$

---

## 4. 数据库持久化与管理接口 (Storage & Admin REST)

### 4.1 PostgreSQL Schema 迁移 (v15)

```sql
CREATE TABLE IF NOT EXISTS cache_optimizer_rules (
    id SERIAL PRIMARY KEY,
    model_pattern VARCHAR(64) NOT NULL UNIQUE,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    sort_tools BOOLEAN NOT NULL DEFAULT TRUE,
    sink_dynamic_system BOOLEAN NOT NULL DEFAULT TRUE,
    inject_anthropic_breakpoints BOOLEAN NOT NULL DEFAULT TRUE,
    min_tokens_threshold INT NOT NULL DEFAULT 1024,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_cache_optimizer_rules_model ON cache_optimizer_rules(model_pattern);
```

### 4.2 Admin REST 端点

- `GET /admin/v1/cache-optimizer/rules`
- `POST /admin/v1/cache-optimizer/rules`
- `PUT /admin/v1/cache-optimizer/rules/{id}`
- `DELETE /admin/v1/cache-optimizer/rules/{id}`
- `GET /admin/v1/cache-optimizer/snapshots`
- `GET /admin/v1/cache-optimizer/stats`

---

## 5. Web 控制台交互界面 (`web/admin.html`)

1. **左侧侧边栏导航项**：`🎯 提示词缓存 (Prompt Cache)`；
2. **4 大核心效益 KPI 卡片**：
   - `上游缓存命中率`（例如 `68.5% 🎯`）
   - `累计命中 Token`（例如 `4,850,200 🪙`）
   - `预估费用节约`（例如 `-$45.20 💰 (90% Claude / 50% OpenAI)`）
   - `平均优化耗时`（例如 `135 μs ⚡`）
3. **规则配置面板**：
   - 规则列表表格，带状态开关、Tools 排序、时戳下沉、Anthropic 打点开关与阈值；
   - 新建规则模态弹窗 (`#cacheOptimizerRuleModal`)；
4. **实时请求流与断点审查器 (`#cacheOptimizerDetailModal`)**：
   - 实时呈现近 200 条请求流水；
   - 包含上游命中徽章（`🟢 CACHE HIT` / `⚪ MISS`）、命中 Token 占比条、节省金额；
   - 点击查看详细断点注入位置与下沉的动态信息元数据。

---

## 6. 验证与验收标准 (Acceptance Criteria)

- [x] 规则管理与解析：
  - 支持 Tools 排序、时戳下沉、Claude 打点三项独立开关及 `min_tokens_threshold` 阈值配置；
  - 客户端入向控制头 `X-Aigate-Prompt-Cache: auto|on|off` 解析准确并生效。
- [x] 算法与沙箱执行：
  - 纯 C17 原生实现，执行开销稳定 $< 300\mu\text{s}$；
  - Tools 数组按名称 100% 稳定字典序排序；
  - System 消息中的动态日期/UUID 准确下沉至末尾，头部核心指令 100% 保持前缀对齐；
  - Anthropic 格式下加权注入至多 4 处 `cache_control: {"type": "ephemeral"}`，且单项 Token 必须 $\ge 1024$；
  - 优化结果若校验失败自动无损回退原 Payload。
- [x] 响应解析与度量追踪：
  - 正确解析 OpenAI `prompt_tokens_details.cached_tokens` 与 Claude `cache_read_input_tokens`；
  - 自动向出向响应注入 `X-Aigate-Prompt-Cache-*` 头；
  - OpenTelemetry 属性中记录 `gen_ai.usage.prompt_tokens_cached`。
- [x] 控制台看板与审查：
  - 内存环形队列维护最近 200 条优化快照；
  - Admin REST API 准确返回快照与聚合效益统计；
  - Web 控制台提供 4 张 KPI 卡片、规则配置面板与断点审查模态框。
- [x] 构建与质量门禁：
  - 单元测试与 CTest 全量通过率 100%；
  - Doxygen 保持严格 0 Warning；
  - 纯 C17 标准实现，无任何外部 C++ 依赖。

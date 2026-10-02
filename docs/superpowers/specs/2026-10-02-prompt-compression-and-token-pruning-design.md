# 提示词自适应压缩与 Token 瘦身引擎 (Prompt Compression & Token Pruning Engine) 设计规范

- **状态**: 草案 (Draft / Pending Implementation)
- **创建日期**: 2026-10-02
- **责任模块**: `src/policy/prompt_compressor.h`, `src/policy/prompt_compressor.c`, `src/core/pipeline_chat.c`, `src/core/aigate_core.c`, `src/server/admin_api.c`, `src/store/pg_store.c`, `web/admin.html`
- **目标**: 构建纯 C17 高性能提示词自适应压缩与 Token 瘦身引擎，支持微秒级执行开销、多轮历史自适应滑动折叠、结构化格式去噪与句子密度重要度剪枝，在 100% 确保代码块、Tool Calls 与最新用户提问安全的前提下，实现 30%~60% 的 Token 成本节约与首字时延优化。

---

## 1. 背景与业务目标 (Context & Objectives)

在大语言模型网关日常业务与 Agent 对话、RAG 检索增强生成等长上下文场景中，客户端请求常伴随大量历史多轮会话、冗余空行、排版标记与低信息密度客套语。这种无序膨胀直接引发两大痛点：
1. **费用昂贵**：上游专有模型（如 GPT-4o、Claude 3.5 Sonnet）计费按输入 Token 线性递增，过长上下文导致账单指数级上升；
2. **时延劣化**：上游模型注意力计算与前向推理开销与上下文长度紧密相关，长 Prompt 会显著拖慢首字时延 (TTFT)。

本引擎在纯 C17 标准下提供全自研、轻量化（< 500μs 极低开销）、高容错的请求流水线端到端无损瘦身方案：
1. **多级混合剪枝引擎 (Multi-Tier Hybrid Pruning)**：
   - **历史轮次自适应滑动保留**：自动折叠超限早期历史，完整保留 System Prompt、最新一轮 User 提问及关键 Tool 调用链；
   - **结构化去噪与空白规范化**：自动规整连续冗余空行与制表符；
   - **句子级重要度密度剪枝**：针对冗余客套话与低信息熵长句执行重要度加权剔除，达到设定目标压缩率。
2. **状态机全方位安全沙箱 (Protection Sandbox)**：
   - 严格保护 Markdown 代码块（```...```）缩进、换行与语法完整性；
   - 严格保护 PII 脱敏占位符（`{{PII_*}}`），保证响应阶段出向逆向还原不损坏；
   - 严格保护 Function Calling / Tool Schema 参数格式。
3. **闭环可观测性与 Web 控制台看板**：
   - 出向响应头注入 `X-Aigate-Prompt-Original-Tokens`、`X-Aigate-Prompt-Compressed-Tokens`、`X-Aigate-Compression-Ratio` 与 `X-Aigate-Compression-Saved-Tokens`；
   - OpenTelemetry Span `prompt_compression` 精细记录耗时与节约指标；
   - Web 控制台提供 4 大效益 KPI 卡片、规则热配置面板，以及原始 vs 压缩后 Prompt 的 **Side-by-Side 左右双栏对比抽屉**。

---

## 2. 总体架构与流水线流向 (Architecture & Flow)

```
[ 客户端请求 POST /v1/chat/completions ]
              │
              ▼
    [ 1. 鉴权与前置安全风控 (Auth & Inbound Guardrails) ]
              │ (执行 PII 脱敏，生成 {{PII_*}} 占位符)
              ▼
    [ 2. 响应缓存查表 (Response Cache Lookup) ]
              ├─► [ 缓存命中 Cache HIT ] ──────────────► [ 直接快速返回，无上游费用 ]
              │
              ▼ (缓存未命中 Cache MISS)
    [ 3. 提示词自适应压缩引擎 (Prompt Compressor) ]
              │
              ├─► 检查客户端 Header: X-Aigate-Compress (auto | off | moderate | aggressive)
              ├─► 匹配规则表中的 model_pattern 与 min_tokens 触发阈值
              ├─► 开启 OTel Span: "prompt_compression"
              ├─► 状态机保护解析 (识别代码块、Tool Calls、PII、最新 User 消息)
              ├─► 层次化三阶剪枝与新 JSON Payload 构建
              ├─► 校验有效性 (若解析异常，自动回退原 Payload，100% 安全)
              └─► 记录快照至环形缓冲区 compressor_cache_t (容量 200 条)
              │
              ▼
    [ 4. 路由分发 (Model Router & Upstream Dispatch) ]  ──► (向上游发送瘦身后的 Payload)
              │
              ▼
    [ 5. 客户端响应交付 (Response Settlement) ]
              └─► 自动注入 X-Aigate-Compression-* 响应头
```

---

## 3. 详细技术规范与接口设计 (Technical Specifications)

### 3.1 核心数据结构与枚举 (`src/policy/prompt_compressor.h`)

```c
/** 提示词压缩激进程度 */
typedef enum {
    COMPRESS_LEVEL_OFF        = 0, /**< 关闭压缩 */
    COMPRESS_LEVEL_MODERATE   = 1, /**< 温和模式：格式去噪 + 历史滑动窗口 */
    COMPRESS_LEVEL_AGGRESSIVE = 2  /**< 激进模式：温和模式 + 句子级重要度密度剪枝 */
} compressor_level_t;

/** 提示词压缩规则 */
typedef struct {
    char                id[37];               /**< 规则唯一 UUID (36 字符 + \0) */
    char                model_pattern[64];    /**< 目标模型匹配通配符，如 "gpt-4o*", "*" */
    bool                enabled;              /**< 是否启用本规则 */
    compressor_level_t  level;                /**< 压缩等级 */
    uint32_t            min_tokens;           /**< 触发阈值：Prompt Tokens 达到该值时启动压缩 (默认 2048) */
    uint32_t            max_history_turns;    /**< 保留的最大历史会话轮次 (默认 6 轮) */
    double              target_ratio;         /**< 目标保留比例 0.10 ~ 0.90 (默认 0.60 即削减 40%) */
    bool                preserve_system;      /**< 严格保护 System Prompt 核心约束 (默认 true) */
    bool                preserve_code;        /**< 严格保护代码块 (```) 缩进与换行 (默认 true) */
    bool                preserve_tools;       /**< 严格保护 Tool Calls 与 Function Schema (默认 true) */
    int64_t             created_at;           /**< 创建时间戳 (秒) */
    int64_t             updated_at;           /**< 更新时间戳 (秒) */
} compressor_rule_t;

/** 单次压缩执行结果明细 */
typedef struct {
    bool        compressed;            /**< 是否实际发生了压缩剪枝 */
    uint32_t    original_tokens;       /**< 原始预估 Prompt Token 数 */
    uint32_t    compressed_tokens;     /**< 压缩后 Prompt Token 数 */
    uint32_t    saved_tokens;          /**< 节省的 Token 数量 (original - compressed) */
    double      compression_ratio;     /**< 实际保留比例 (compressed / original) */
    uint64_t    elapsed_us;            /**< 压缩操作耗时 (微秒，要求 < 500μs) */
    char*       compressed_payload;    /**< 压缩后的完整 JSON 字符串 (调用方负责 free) */
    size_t      compressed_len;        /**< 压缩后 Payload 字节数 */
} compressor_result_t;

/** 内存环形快照条目 (用于控制台 Diff 抽屉与调试，容量 200 条) */
typedef struct {
    char        req_id[37];            /**< 请求/Trace ID */
    char        model[64];             /**< 请求的模型名称 */
    int64_t     timestamp;             /**< 发生时间戳 (秒) */
    uint32_t    original_tokens;       /**< 原始 Token 数 */
    uint32_t    compressed_tokens;     /**< 压缩后 Token 数 */
    uint32_t    saved_tokens;          /**< 节省 Token 数 */
    double      compression_ratio;     /**< 压缩率 */
    uint32_t    elapsed_us;            /**< 压缩耗时 (微秒) */
    char        prompt_preview[128];   /**< 原始 Prompt 摘要 */
    char        orig_preview[512];     /**< 压缩前 Messages 文本快照 */
    char        comp_preview[512];     /**< 压缩后 Messages 文本快照 */
} compressor_snapshot_t;

/** 全局聚合效益统计 */
typedef struct {
    uint64_t    total_evaluated;       /**< 经评估的总请求数 */
    uint64_t    total_compressed;      /**< 实际发生压缩的请求数 */
    uint64_t    total_orig_tokens;     /**< 累计原始 Token 数 */
    uint64_t    total_comp_tokens;     /**< 累计压缩后 Token 数 */
    uint64_t    total_saved_tokens;    /**< 累计节约 Token 总数 */
    uint64_t    total_duration_us;     /**< 累计压缩耗时 (微秒) */
    double      estimated_cost_saved;  /**< 预估节约美元总额 */
} compressor_stats_t;
```

---

### 3.2 剪枝算法与安全沙箱执行机制 (`src/policy/prompt_compressor.c`)

1. **Token 快速预估算法**：
   - 纯 C 原生快速字符/单词扫描，预估 Token 量：$\text{Tokens} \approx \text{ASCII Words} \times 1.3 + \text{UTF-8 Multi-byte Bytes} / 2$；
   - 耗时 $< 5\mu\text{s}$，若低于 `min_tokens` 阈值直接原样透传。
2. **Stage 1: 历史轮次自适应滑动保留**：
   - 解析 `messages` 数组；
   - 定位最后一个 `role="user"` 消息：赋予最高保护优先级，禁止整体丢弃；
   - 提取开头的 `role="system"` 或 `role="developer"`：若 `preserve_system=true`，完整保留；
   - 识别关联的 `tool_calls` 与 `role="tool"` 响应对：若 `preserve_tools=true`，关联成对保留；
   - 超出 `max_history_turns` 的陈旧历史轮次，优先折叠为 `"[Earlier conversation omitted]"` 单条提示或直接安全移除。
3. **Stage 2: 格式结构去噪与空白规范化**：
   - 状态机单遍扫描文本，判定当前字符是否处于 Markdown 代码块（行首以 ` ``` ` 起止）；
   - 代码块内部：严格跳过，保持原始换行与缩进；
   - 代码块外部：连续换行 `\n\n\n+` 规范化为 `\n\n`；行首行尾多余空格与制表符修整；连续空标点与分割线精简。
4. **Stage 3: 句子级重要度密度剪枝 (激进模式)**：
   - 针对长 Assistant 历史消息，切分句子集合；
   - 保护包含 `{{PII_*}}` 占位符、约束词（"must", "json", "schema", "禁止", "必须"）以及首尾关键句；
   - 计算中间句子信息熵密度，剔除客套性固定模板句，直至达到 `target_ratio`。
5. **安全兜底断言**：
   - 新建的 JSON 必须通过完整合法性校验；若遇内存异常或校验失败，立即回退原 Payload，对外响应成功率 100% 免疫。

---

### 3.3 请求流水线与请求头注入 (`src/core/pipeline_chat.c`, `src/core/aigate_core.c`)

1. **入向控制头支持**：
   - `X-Aigate-Compress: auto`（默认自适应）
   - `X-Aigate-Compress: off` 或 `none`（强制透传）
   - `X-Aigate-Compress: moderate`（强制温和）
   - `X-Aigate-Compress: aggressive`（强制激进）
2. **出向响应头注入**（仅在实际发生压缩时）：
   - `X-Aigate-Prompt-Original-Tokens: <orig>`
   - `X-Aigate-Prompt-Compressed-Tokens: <comp>`
   - `X-Aigate-Compression-Ratio: <ratio>%`
   - `X-Aigate-Compression-Saved-Tokens: <saved>`
3. **生命周期管理**：
   - `chat_req_t` 结构体持有 `compressor_result_t comp_result`；
   - 请求终结时由 `chat_req_cleanup()` 统一释放 `comp_result.compressed_payload`，彻底杜绝内存泄漏。

---

### 3.4 OpenTelemetry 链路集成 (`src/observe/tracer.c`)

在 `trace_ctx` 中开启专有 Span：
- **Span 名称**：`prompt_compression`
- **Span Kind**：`SPAN_KIND_INTERNAL`
- **Span 属性**：
  - `aigate.compression.applied`: `true`
  - `aigate.compression.level`: `"moderate"` / `"aggressive"`
  - `aigate.compression.original_tokens`: 整数
  - `aigate.compression.compressed_tokens`: 整数
  - `aigate.compression.saved_tokens`: 整数
  - `aigate.compression.ratio`: 浮点数
  - `aigate.compression.duration_us`: 耗时微秒

---

### 3.5 数据库持久化规范 (`schema/schema.sql` & `src/store/pg_store.c`)

```sql
CREATE TABLE IF NOT EXISTS compressor_rules (
    id                  VARCHAR(36) PRIMARY KEY,
    model_pattern       VARCHAR(64) NOT NULL,
    enabled             BOOLEAN NOT NULL DEFAULT TRUE,
    level               INTEGER NOT NULL DEFAULT 1,          -- 1: Moderate, 2: Aggressive
    min_tokens          INTEGER NOT NULL DEFAULT 2048,
    max_history_turns   INTEGER NOT NULL DEFAULT 6,
    target_ratio        DOUBLE PRECISION NOT NULL DEFAULT 0.60,
    preserve_system     BOOLEAN NOT NULL DEFAULT TRUE,
    preserve_code       BOOLEAN NOT NULL DEFAULT TRUE,
    preserve_tools      BOOLEAN NOT NULL DEFAULT TRUE,
    created_at          BIGINT NOT NULL,
    updated_at          BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_compressor_rules_model ON compressor_rules(model_pattern);
```

提供 C API：
- `bool pg_store_compressor_rules_load(pg_store_t* store, compressor_rule_t* out_rules, size_t max_rules, size_t* out_count);`
- `bool pg_store_compressor_rule_upsert(pg_store_t* store, const compressor_rule_t* rule);`
- `bool pg_store_compressor_rule_delete(pg_store_t* store, const char* rule_id);`

---

### 3.6 管理后台 REST API 规范 (`src/server/admin_api.c`)

| 方法 | 路径 | 描述 |
|---|---|---|
| `GET` | `/admin/v1/compressor/rules` | 获取所有提示词压缩规则列表 |
| `POST` | `/admin/v1/compressor/rules` | 新增压缩规则 |
| `PUT` | `/admin/v1/compressor/rules/:id` | 修改指定规则配置（启用/阈值/轮次/保护） |
| `DELETE` | `/admin/v1/compressor/rules/:id` | 删除指定的压缩规则 |
| `GET` | `/admin/v1/compressor/snapshots` | 获取内存环形队列中最近 200 条压缩请求快照 |
| `GET` | `/admin/v1/compressor/stats` | 获取全局聚合统计指标（累计节省 Token、平均压缩比、节约美元、平均耗时） |

---

### 3.7 Web 控制台交互界面 (`web/admin.html`)

1. **左侧新增专用导航标签**：`🗜️ 提示词压缩 (Prompt Compressor)`；
2. **4 大核心效益 KPI 统计卡片**：
   - `累计节省 Token`（例如 `1,420,800 🪙 -52.4%`）
   - `预估费用节约`（例如 `-$21.45 💰`）
   - `平均保留压缩率`（例如 `47.6% ⚡`）
   - `平均剪枝耗时`（例如 `195 μs 🚀`）
3. **规则配置与管理卡片**：
   - 规则列表表格，带状态开关、模型通配符、阈值滑动调节器与保护开关；
   - 「添加压缩规则」模态弹窗 (`#compressorRuleModal`)；
4. **实时压缩数据流与 Side-by-Side 左右双栏对比抽屉 (`#compressorDiffModal`)**：
   - 实时列表呈现近期被压缩修剪的请求（时间戳、模型、原始/压缩 Token、节约量、耗时）；
   - **`🔍 文本双栏对比`**：点击某行弹出 Side-by-Side 对比模态框：
     - **左栏（原始提示词）**：展示包含未修剪的冗余多轮历史与空白；
     - **右栏（优化后提示词）**：高亮展示保留的 System 指令、完整代码块、Tool Calls 与最新 User 提问；
     - **顶部统计徽章**：清晰标注节省 Token 数与压缩率。

---

## 4. 验证与验收标准 (Acceptance Criteria)

- [ ] 规则管理与解析：
  - 支持 `COMPRESS_LEVEL_MODERATE`（温和）与 `COMPRESS_LEVEL_AGGRESSIVE`（激进）模式配置与动态热生效；
  - 触发阈值 `min_tokens` 与入向控制头 `X-Aigate-Compress` 解析准确。
- [ ] 剪枝算法与安全沙箱执行：
  - 纯 C17 原生实现，执行开销稳定 `< 500μs`；
  - Markdown 代码块（```...```）缩进、换行 100% 保护无损；
  - PII 掩码占位符 `{{PII_*}}` 100% 完整保留；
  - Tool Calls 与最新一轮 User 提问绝对完整保留；
  - 压缩结果语法校验失败时自动回退原 Payload，服务成功率 100% 稳定。
- [ ] 响应头与链路追踪：
  - 命中压缩的请求自动注入 `X-Aigate-Compression-*` 响应头；
  - OpenTelemetry Span `prompt_compression` 正确记录耗时与 Token 指标。
- [ ] 控制台看板与对比：
  - 内存环形队列维护最近 200 条压缩条目；
  - Admin REST API 正确返回快照与聚合统计；
  - Web 控制台提供直观的 4 张 KPI 卡片、规则配置面板与左右双栏对比模态框。
- [ ] 构建与质量门禁：
  - 单元测试与 CTest 全量通过率 100%；
  - Doxygen 保持严格 0 Warning；
  - 纯 C17 标准实现，无任何外部 C++ 依赖。

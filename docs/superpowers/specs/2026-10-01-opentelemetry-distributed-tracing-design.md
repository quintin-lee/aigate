# OpenTelemetry 分布式追踪与全链路耗时瀑布图 (Distributed Tracing & Waterfall Profiling) 设计规范

- **状态**: 草案 (Draft / Pending Approval)
- **创建日期**: 2026-10-01
- **责任模块**: `src/observe/tracer.h`, `src/observe/tracer.c`, `src/core/pipeline_chat.c`, `src/server/admin_api.c`, `web/admin.html`
- **目标**: 构建符合 W3C TraceContext 与 OpenTelemetry GenAI Semantic Conventions 标准的全链路分布式追踪系统，支持毫秒级低开销异步批处理导出与控制台内置交互式耗时瀑布图。

---

## 1. 背景与目标 (Context & Objectives)

在企业接入公网与私有化多模型供应商的过程中，网关不仅是流量入口与风控闸口，更是全链路性能排障与调用诊断的核心中枢。由于大模型交互具有**长耗时、高网络敏感度、流式跨包传输以及复杂安全审查**的特性，传统的单次请求时间戳记录无法清晰定位系统瓶颈究竟发生在：
1. **客户端至网关的网络排队与鉴权流控**；
2. **多模态与安全风控检测（敏感词过滤、PII 正则与校验和运算）**；
3. **响应缓存命中与指纹计算**；
4. **上游大模型握手与首字延迟（Time to First Token, TTFT）**；
5. **长文本流式 Chunk 跨包聚合与传输**；
6. **出站滑动窗口去标识化逆向还原**。

本项目旨在为 `aigate` 构建原生的企业级分布式追踪体系：
1. **W3C TraceContext 全链路透传**：支持标准 `traceparent` 头接入与跨系统传递，打通前端应用 -> 网关 -> 上游大模型的多跳链路；
2. **OpenTelemetry GenAI 语义规范对齐**：全面记录标准属性（如 `gen_ai.system`、`gen_ai.request.model`、`gen_ai.usage.*`）；
3. **尾部自适应采样算法 (Adaptive Tail-Sampling)**：兼顾极高 QPS 下的极低开销与异常/慢请求的 100% 捕获；
4. **零主线程阻塞的异步批处理导出器**：内存环形缓冲削峰填谷，后台 Worker 异步按批次将数据推送到企业 OTLP 收集器（Jaeger、Tempo 等）；
5. **控制台交互式耗时瀑布图 (Interactive Waterfall Timeline)**：无需任何外部依赖，在 `web/admin.html` 中直观展开多轨道甘特图，一键定位毫秒级性能毛刺。

---

## 2. 总体架构与数据流 (Architecture & Data Flow)

```
[ 客户端 Client ]
       │  携带 W3C traceparent (或由网关自生成)
       ▼
[ aigate 核心请求处理线程 (Worker Thread) ]
       │  • 预分配 trace_context_t (零堆内存分配)
       │  • 各阶段打点 (clock_gettime MONOTONIC，开销 < 2μs)
       │  • Span 树: root ➔ auth ➔ guardrails ➔ cache ➔ router ➔ ttft ➔ stream ➔ outbound
       │  • 请求完成: 执行智能尾部采样评估 (异常/慢请求必采)
       ▼ (命中采样)
[ 内存环形缓冲队列 (Trace Ring Buffer, 容量 1024) ]
       │ (非阻塞入队，满时丢弃最旧数据，绝不阻塞主线程)
       ▼
[ 后台批处理线程 (OTLP Exporter Worker) ]
       ├─────────────────────────────────┐
       ▼                                 ▼
[ 本地近期索引池 (最近 500 条) ]    [ OTLP/HTTP 批处理上报 ]
       │                                 │ (满 50 条或达 2 秒)
       ▼                                 ▼
[ Admin API: /admin/v1/traces/:id ] [ 外部 Collector (Jaeger / Tempo) ]
       │
       ▼
[ web/admin.html 交互式耗时瀑布图 ]
```

---

## 3. 详细设计 (Detailed Specifications)

### 3.1 W3C TraceContext 规范
遵循 [W3C TraceContext Level 1](https://www.w3.org/TR/trace-context/) 协议标准：
1. **入站解析 (`traceparent`)**：
   - 格式：`version-trace_id-parent_id-trace_flags`（如 `00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01`）；
   - 若客户端未提供或格式非法，网关使用系统加密级随机数自动生成合规的 16 字节 `trace_id` 与 8 字节 `root_span_id`。
2. **出站透传**：
   - 当网关向外部上游模型（OpenAI、Anthropic、Gemini、DeepSeek）发送 HTTP/HTTPS 请求时，注入由网关 `trace_id` 和当前 `upstream_span_id` 组装的标准 `traceparent` 头，实现跨厂商分布式链路关联。

### 3.2 内存数据结构 (C17 零堆碎片)
在 `src/observe/tracer.h` 中定义结构体，预嵌于 `chat_req_t`：

```c
#define TRACE_MAX_SPANS 16
#define TRACE_MAX_ATTRS 8

typedef enum {
    SPAN_KIND_INTERNAL = 0,
    SPAN_KIND_SERVER   = 1,
    SPAN_KIND_CLIENT   = 2
} span_kind_t;

typedef enum {
    SPAN_STATUS_UNSET = 0,
    SPAN_STATUS_OK    = 1,
    SPAN_STATUS_ERROR = 2
} span_status_t;

typedef struct {
    char key[32];
    char value[64];
} span_attr_t;

typedef struct {
    char          span_id[17];         /* 16 字符十六进制 + \0 */
    char          parent_span_id[17];  /* 16 字符十六进制 + \0 */
    char          name[32];            /* Span 阶段名称 */
    span_kind_t   kind;
    uint64_t      start_time_ns;       /* CLOCK_MONOTONIC 纳秒戳 */
    uint64_t      end_time_ns;
    span_status_t status;
    char          status_desc[64];
    span_attr_t   attributes[TRACE_MAX_ATTRS];
    int           attr_count;
} trace_span_t;

typedef struct {
    char          trace_id[33];        /* 32 字符十六进制 + \0 */
    char          root_span_id[17];
    char          inbound_parent_id[17];
    bool          is_sampled;
    uint8_t       trace_flags;
    trace_span_t  spans[TRACE_MAX_SPANS];
    int           span_count;
    uint64_t      req_start_realtime_us; /* CLOCK_REALTIME 微秒戳对齐 */
} trace_context_t;
```

### 3.3 智能尾部自适应采样算法 (Tail-Sampling)
为了在大流量生产环境下以最低资源消耗精准捕获有价值的链路，网关采用两阶段判定：
1. **入口判定 (Head Sampling)**：
   - 若客户端携带 `trace_flags & 0x01`，无条件保留采样；
   - 否则根据管理员设置的全局比例（如 `10%`）按伪随机数初筛。
2. **尾部兜底 (Tail/Retroactive Sampling)**：
   - 在请求生命周期末尾（`aigate_core.c` 结束阶段），若满足以下条件之一，即使入口未被采样，也**强制提升为已采样**：
     - **状态码异常**：HTTP 状态码 `>= 400`；
     - **安全阻断**：敏感词阻断、PII 违规阻断、Prompt 注入拦截；
     - **慢请求突发**：端到端总耗时超过配置的慢请求阈值（默认 `2000ms`）或首包延迟（TTFT）`> 1500ms`。

### 3.4 阶段打点与 OpenTelemetry GenAI 语义对齐
标准内置 8 大核心 Spans 与属性：

| Span 名称 | Kind | 对应阶段与含义 | 关键 GenAI 与网关元数据属性 |
|---|---|---|---|
| `root` | Server | 请求自进入网关至完全返回终端客户端 | `gen_ai.system`, `gen_ai.request.model`, `gen_ai.response.model`, `gen_ai.usage.*`, `aigate.client.key_id` |
| `auth_and_limits` | Internal | API Key 验证、部门预算与速率限制检查 | `aigate.group.name`, `aigate.rate_limit.allowed: true` |
| `guardrails_inbound` | Internal | Aho-Corasick 关键词阻断、PII 正则脱敏与注入审查 | `aigate.guardrails.pii_masked: <int>`, `aigate.guardrails.blocked: false` |
| `cache_lookup` | Internal | SHA256 提示词归一化指纹检索 | `aigate.cache.hit: true/false`, `aigate.cache.key: <hash>` |
| `router_and_hedge` | Internal | EWMA 延迟路由选择、备用通道对冲决策 | `aigate.router.selected_upstream: <name>`, `aigate.router.hedged: false` |
| `upstream_ttft` | Client | 上游握手至接收到首个数据块（首字时间 TTFT） | `aigate.latency.ttft_ms: <float>`, `http.status_code: 200` |
| `upstream_streaming` | Client | 后续流式 Chunk 的聚合传输与重组过程 | `aigate.stream.chunk_count: <int>`, `aigate.stream.bytes_received: <int>` |
| `guardrails_outbound` | Internal | 出站滑动窗口去标识化逆向还原 | `aigate.guardrails.restored_count: <int>` |

### 3.5 异步批处理导出器 (OTLP/HTTP Exporter)
1. **工作机制**：
   - 维护容量为 1024 的内存环形队列，由互斥锁与条件变量保护；
   - 独立低优先级线程 `trace_exporter_worker` 每隔 2 秒或每累积 50 条 Trace 执行一次批处理上报；
   - 序列化为遵循 OpenTelemetry 规范的 OTLP/HTTP JSON，发往 `POST /v1/traces`。
2. **零配置友好**：
   - 若未配置 `otlp_endpoint`，Worker 不发起网络请求，仅维护本地内存视图，零多余网络开销。
3. **故障隔离与防雪崩**：
   - 远端收集器不可达时，Worker 实行指数退避，日志限制每 60 秒最多打印 1 条告警；
   - 队列满载时自动淘汰最旧链路并原子自增 `dropped_traces_total` 指标。

### 3.6 管理端点规范 (Admin REST API)
1. **`GET /admin/v1/traces/:trace_id`**：
   - 返回指定链路的完整 Span 列表、时间戳、耗时与键值对属性 JSON。
2. **`GET /admin/v1/traces/config`**：
   - 获取当前追踪子系统配置：`enabled`、`sample_rate`、`otlp_endpoint`、`slow_threshold_ms`、`buffer_usage`。
3. **`PUT /admin/v1/traces/config`**：
   - 动态更新配置（支持热修改采样率与上报端点）。

### 3.7 Web 控制台可视化瀑布图 (`web/admin.html`)
1. **审计日志列表联动**：
   - 在「📋 审计日志」各行中显示紫色 `Trace ID` 徽章并提供「🔍 耗时瀑布图」操作按钮；
2. **交互式多轨道甘特瀑布图 (Waterfall Timeline)**：
   - 顶部统计卡片：总耗时、TTFT 首字延迟、模型、Token、Trace ID；
   - 顶部标尺时间轴（0ms 至总耗时）；
   - 各阶段横向色块条按真实相对起始时间与耗时比例渲染；
   - 点击色块条实时查看属性详情（Attributes 表格）。

---

## 4. 验证与验收标准 (Acceptance Criteria)

- [ ] W3C 上下文透传与生成：
  - 合法 `traceparent` 正确继承 `trace_id` 与父 `span_id`；
  - 缺失或非法 `traceparent` 时自动补全生成合规随机 ID；
  - 向上游发送请求时成功注入更新后的 `traceparent`。
- [ ] 智能尾部采样：
  - 常规请求受配置比例约束；
  - HTTP 4xx/5xx、风控拦截、慢请求（>2000ms）100% 捕获。
- [ ] OTLP/HTTP 批处理导出：
  - 生成格式符合 OpenTelemetry 标准 OTLP/HTTP JSON schema；
  - 远端失败时安全退避，环形队列满时丢弃不阻塞。
- [ ] Web 控制台与管理端点：
  - `/admin/v1/traces/:trace_id` 正确输出各阶段 Span 与耗时；
  - `web/admin.html` 审计日志中点击可弹出耗时瀑布图抽屉。
- [ ] 构建与质量门禁：
  - CTest 全量通过率 100%；
  - Doxygen 保持严格 0 warnings。

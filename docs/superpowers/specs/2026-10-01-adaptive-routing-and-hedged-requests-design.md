# 自适应动态延迟路由与对冲请求（Hedged Requests）架构设计规约

- **日期**：2026-10-01
- **状态**：Approved (已评审通过)
- **目标组件**：`latency_tracker`, `model_router`, `upstream_hedged`, `metrics`, `admin_api`, `web/admin.html`
- **对标规范**：Google Tail at Scale (Jeff Dean), Envoy AI Gateway Hedged Requests, Cloudflare AI Gateway Smart Routing

---

## 1. 目标与背景

在大语言模型（LLM）API 网关的生产流量中，长尾延迟（P95 / P99 Tail Latency）常常受制于上游提供商的偶发慢响应、队列排队或网络链路抖动。在传统串行重试（Failover on 5xx/Timeout）模式下，客户端必须承受完整的超时时间（例如 5~10 秒）才能切换到备选通道，导致长尾体验恶化。

本项目为 AIGate 构建**自适应智能路由体系**与**对冲请求（Hedged Requests）并发竞速引擎**，主要达成：
1. **P95 / EWMA 动态延迟感知**：无锁极速环形滑动窗口追踪各模型端点实时延迟，提供毫秒级 P95 分位计算与指数平滑（EWMA）。
2. **自适应自愈路由调度**：支持 `latency_p95`（最优延迟置顶）与 `dynamic_weighted`（延迟倒数动态加权），使流量自动向高速节点倾斜。
3. **协作型长尾对冲并发竞速**：首发请求超过延迟窗口（P95 或显式指定 `hedged_delay_ms`）未返回时，投机派生对冲请求；非流式与流式（TTFT 裁决）先到先得（Winner Takes All），败者立即优雅断开连接。
4. **配额安全防护（Hedge Budget）**：内置单请求最多 1 次对冲限制与全局/模型级对冲配额保护（默认 ≤15% 请求允许对冲），防止雪崩放大。
5. **管理控制台联动**：Admin API 与嵌入式 Web 控制台提供可视化配置与实时生效。

---

## 2. 系统架构与交互流程

```
                                    ┌────────────────────────┐
                                    │    Client Inbound      │
                                    │ (Sync / SSE Streaming) │
                                    └───────────┬────────────┘
                                                │
                                    ┌───────────▼────────────┐
                                    │   model_router_select  │
                                    │ (Filter CB / Sort P95) │
                                    └───────────┬────────────┘
                                                │
                          ┌─────────────────────┴─────────────────────┐
                          │                                           │
                Hedge Disabled / Candidates < 2              Hedge Enabled & Admitted
                          │                                           │
                ┌─────────▼──────────────┐                ┌───────────▼───────────┐
                │ Serial Upstream Call   │                │ upstream_call_hedged  │
                │ (with 5xx/429 failover)│                └───────────┬───────────┘
                └────────────────────────┘                            │
                                                      ┌───────────────┴───────────────┐
                                                      │                               │
                                            Primary Worker (T0)              Wait Window D (P95)
                                                      │                               │
                                              Finish < D ms? ────────(No)─────────────┤
                                                      │                               │
                                                    (Yes)                     Trigger Hedge Worker (T1)
                                                      │                               │
                                          ┌───────────▼────────────┐      ┌───────────▼───────────┐
                                          │ 0 Redundant Requests   │      │ Concurrent Race:      │
                                          │ Direct Output          │      │ First HTTP 200 / TTFT │
                                          └────────────────────────┘      └───────────┬───────────┘
                                                                                      │
                                                                          ┌───────────▼───────────┐
                                                                          │ Winner: Stream/Return │
                                                                          │ Loser: Cancel & Abort │
                                                                          └───────────────────────┘
```

---

## 3. 详细设计与核心组件

### 3.1 数据库与存储扩展 (Migration v12)

在 `src/store/schema_sql.h` 中增加 Migration v12：
```sql
-- Migration v12: adaptive latency routing & hedged requests support
ALTER TABLE models
  ADD COLUMN IF NOT EXISTS hedged_delay_ms INT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS hedge_budget_pct INT NOT NULL DEFAULT 15,
  ADD COLUMN IF NOT EXISTS hedged_enabled BOOLEAN NOT NULL DEFAULT FALSE;

INSERT INTO schema_migrations(version) VALUES (12) ON CONFLICT (version) DO NOTHING;
```

`src/store/pg_store.h` 中 `model_rec_t` 结构体扩充：
```c
typedef struct model_rec {
    // ... 原有字段 ...
    int     hedged_delay_ms;    /**< 0 for auto-P95, >0 for static ms delay */
    int     hedge_budget_pct;   /**< Max % of requests that can trigger hedge (default: 15) */
    bool    hedged_enabled;     /**< True if hedged speculative execution is active */
} model_rec_t;
```

### 3.2 P95 / EWMA 动态延迟追踪器 (`src/policy/latency_tracker.h/c`)

#### 结构体定义与常量
- 单端点容量：`LATENCY_TRACKER_WINDOW_SZ = 64`
- 端点条目最大数量：`LATENCY_TRACKER_MAX_ENTRIES = 512`
- EWMA 衰减因子：$\alpha = 0.2$

```c
typedef struct latency_entry {
    char     model[64];
    char     endpoint[256];
    uint32_t samples_ms[64];
    uint32_t head;
    uint32_t count;
    uint32_t cached_p95_ms;
    double   ewma_ms;
    uint64_t total_requests;
    uint64_t hedged_requests;
    pthread_rwlock_t rwlock;
} latency_entry_t;

typedef struct latency_tracker {
    latency_entry_t  entries[512];
    size_t           count;
    pthread_rwlock_t table_lock;
} latency_tracker_t;
```

#### 核心算法逻辑
1. **样本更新 (`latency_tracker_record`)**：
   - 记录新延迟 $L_{\text{ms}} = \max(1, \text{latency\_ns} / 1000000)$。
   - 环形缓冲区写入：`samples_ms[head % 64] = L_ms; head++; if (count < 64) count++;`
   - EWMA 更新：若初次记录 `ewma_ms = L_ms`，否则 `ewma_ms = 0.2 * L_ms + 0.8 * ewma_ms`。
   - 重新计算或缓存 P95：复制有效样本到栈上数组，执行 QuickSelect 或排序，索引为 $\lfloor count \times 0.95 \rfloor$。
2. **对冲配额准入判断 (`latency_tracker_hedge_admitted`)**：
   - 检查 `(hedged_requests * 100) / (total_requests + 1) <= budget_pct`。
   - 若超出配额，返回 `false`，降级为普通串行单发。

### 3.3 自适应动态路由调度 (`src/upstream/model_router.c`)

在 `model_router_select_candidates` 中加入对 `latency_p95` 与 `dynamic_weighted` 的支持：
1. **熔断器筛选**：通过 `cb_allow_request` 剔除处于 `CB_OPEN` 状态的端点。
2. **`latency_p95` 策略**：
   - 从 `latency_tracker` 中读取各健康端点的 `cached_p95_ms`。
   - 升序排序：$P95(T_0) \le P95(T_1) \le \dots$。
   - 最优者成为主发起目标，次优者成为对冲/故障转移备选。
3. **`dynamic_weighted` 策略**：
   - 各健康端点动态权重：$W_i = \max(1, \lfloor 1000 / (EWMA_i + 10) \rfloor)$。
   - 依据动态权重执行加权轮询选取首发与备选目标。

### 3.4 协作型对冲执行引擎 (`src/upstream/upstream_hedged.h/c`)

```c
typedef struct hedged_state {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    atomic_int      winner;           /**< 0: undetermined, 1: primary, 2: secondary */
    atomic_bool     primary_cancel;   /**< True to abort primary curl */
    atomic_bool     secondary_cancel; /**< True to abort secondary curl */
    atomic_bool     first_token_seen; /**< For streaming TTFT race */
} hedged_state_t;
```

#### 执行流程（同步/非流式）
1. 计算等待窗口 $D$：
   - 若 `model->hedged_delay_ms > 0`，则 $D = \text{hedged\_delay\_ms}$；
   - 否则 $D = \text{latency\_tracker\_get\_p95\_ms}$（受制于 $[100\text{ms}, 5000\text{ms}]$ 边界约束）。
2. 主工作线程发起对 Primary Target 的 HTTP 请求。
3. 主线程调用 `pthread_cond_timedwait` 等待 $D$ 毫秒：
   - **情况 A (快路径)**：Primary 响应完成（HTTP 200）。主线程被唤醒，`winner = 1`，直接提取响应数据返回。无需派生次级线程，网络开销为 0。
   - **情况 B (长尾慢请求对冲)**：超时未完成。主线程检查 Hedge Budget，若通过则派生 Secondary Worker 请求 Secondary Target。
4. 两路并发竞争：
   - 率先返回有效 HTTP 200 的线程，使用 CAS 原子操作设置 `winner`，并将对方的 `cancel` 标志置为 `true`。
   - 败者线程在 libcurl 回调（`write_cb` 或 `xferinfo_cb`）检测到 `cancel == true` 时立即返回非 0，libcurl 迅速断开连接并释放内存。

#### 执行流程（流式 SSE）
1. 胜负裁决标准由“完整响应交付”切换为“**Time-To-First-Token (TTFT)**”。
2. 任一路工作线程接收到上游返回的 HTTP 200 头并解析出首个有效 SSE chunk（如 `data: {"choices":...`）时：
   - 原子置位 `winner = my_id` 与 `first_token_seen = true`。
   - 另一路被立即终止。
   - 胜者通道直接挂接客户端流式输出逻辑，持续推送后续 Token，确保流式输出平滑无缝。

---

## 4. 容错、降级与边界处理

1. **单通道兜底**：若模型仅配置了 1 个 Target，对冲逻辑自动跳过，走常规单发。
2. **全失败回退**：若 Primary 与 Secondary 均报错（如均返回 500/503），返回常规 502/Bad Gateway 错误，两路失败分别计入 `circuit_breaker` 计数器。
3. **冷启动安全**：在系统刚启动无历史延迟样本时，P95 返回安全默认值（1000ms），避免在未建立基准时误触发高频对冲。
4. **内存与资源回收**：
   - 每一个对冲任务拥有一对相互隔离的缓冲区。
   - 败者退出时由各 Worker 线程自清理所持有的局部 curl handle 与 merged 报文内存，严防内存泄漏与野指针。

---

## 5. Web 控制台与运维管理

- **Web 控制台 (`web/admin.html`)**：
  - Model 管理模态框新增配置项：
    - `lb_policy` 下拉框新增 `⚡ latency_p95 (自适应 P95 优先)` 与 `⚖️ dynamic_weighted (动态延迟加权)`。
    - `hedged_enabled` 开关。
    - `hedged_delay_ms` 输入框（提示：0 为智能自适应）。
    - `hedge_budget_pct` 输入框（默认 15%）。
  - Model 表格增加徽章：`Hedged (P95)`、`Dyn-Weighted`。
- **Prometheus 监控指标 (`src/observe/metrics.c`)**：
  - `aigate_hedged_requests_total{model="..."}`
  - `aigate_hedged_won_total{model="...", winner="secondary"}`

---

## 6. 测试与验证策略

1. **单元测试 (`tests/unit/`)**：
   - `test_latency_tracker.c`：验证环形窗口、EWMA 衰减、P95 计算准确度、并发写入与对冲预算比例计算。
   - `test_model_router_adaptive.c`：模拟不同端点延迟数据，验证 `model_router_select_candidates` 能正确输出由 P95 决定的候选次序。
2. **端到端集成测试 (`tests/integration/test_adaptive_hedging.py`)**：
   - 搭建 Mock Upstream 服务，提供快速节点（50ms）与慢速长尾节点（1500ms）。
   - **Case 1**：普通快请求不触发对冲，验证 0 额外网络开销。
   - **Case 2**：慢请求触发对冲，首个响应在 ~200ms 返回，验证长尾延迟削减。
   - **Case 3**：流式 SSE TTFT 竞速测试，验证首包到达后败者被快速断开。
   - **Case 4**：Hedge Budget 限制验证，高频并发慢请求下对冲比例严格限制在 ≤15%。

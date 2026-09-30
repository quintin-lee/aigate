# 性能基准与压测套件 (Performance Benchmark Suite) 设计规范

- **状态**: Approved
- **日期**: 2026-09-30
- **目标**: 为 aigate 构建标准化、全自动化的端到端性能基准与高并发压测套件，支持 k6 与 wrk，覆盖非流式、流式 SSE、本地响应缓存命中与多候选故障转移场景。

---

## 1. 背景与目标

在完成核心网关管道解耦（`aigate_core.c` 拆解为 `pipeline_chat.c`, `pipeline_embeddings.c`, `pipeline_native.c`, `pipeline_responses.c`）后，为了量化网关本身的代理开销、转发吞吐能力以及在极端压力下的稳定性，需要构建标准化的性能基准套件。

### 1.1 核心目标
1. **纯代理开销隔离**：通过本地受控极速 Mock 上游，消除外网上游网络波动与大模型推理耗时干扰，准确测定 aigate 在鉴权、限流、路由匹配、流式协议解析、缓存读写上的微秒/毫秒级纯代理耗时。
2. **多工具双引擎支持**：
   - **k6**：支持流式 SSE 分块解析、首字延迟（TTFT, Time-To-First-Token）精确度量、虚拟用户梯度爬坡（Ramping VUs）与结构化指标导出。
   - **wrk**：支持高并发多线程发包，用于压榨极限吞吐（QPS）与精确时延分位数（p50/p90/p99/p99.9）。
3. **关键场景全覆盖**：
   - 场景 1：非流式极速代理 (`bench-sync`)
   - 场景 2：流式 SSE 逐字推送 (`bench-stream`，度量 TTFT)
   - 场景 3：响应缓存命中 (`bench-cache`，度量本地内存缓存下的吞吐放大)
   - 场景 4：多候选故障转移 (`bench-failover`，度量主节点 503 触发熔断与备节点重试开销)
4. **一键全自动执行**：提供自动化驱动器（`bench.py` 与 `run.sh`），自动完成环境自检、Mock 上游启动、网关沙箱启动、场景执行、结果聚合与 Markdown 报告生成，并具备完善的进程退出清理机制。

---

## 2. 总体架构与目录结构

所有压测与基准代码存放在独立目录 [`benchmarks/`](file:///home/quintin/Data/source/c_cpp/aigate/benchmarks)，与现有功能代码和单元测试解耦。

```text
benchmarks/
├── README.md                   # 压测套件使用指南与依赖工具安装说明
├── run.sh                      # 简易 Shell 启动入口
├── bench.py                    # 核心 Python 自动化编排器
├── mock_upstream.py            # 高性能受控 Mock 上游服务
├── config/
│   ├── aigate_bench.conf       # 专用于压测的 aigate 配置文件模版
│   └── fixtures/               # 标准请求体
│       ├── chat_sync.json      # 非流式普通对话请求
│       ├── chat_stream.json    # 流式 SSE 对话请求 (stream: true)
│       └── chat_cache.json     # 静态固化 prompt 的缓存命中请求
├── k6/
│   ├── scenarios.js            # k6 统一多场景发压脚本
│   └── lib/
│       └── sse_parser.js       # k6 SSE 流式分块解析器
├── wrk/
│   ├── sync.lua                # wrk 非流式请求脚本
│   ├── stream.lua              # wrk 流式连接请求脚本
│   └── cache.lua               # wrk 缓存命中测试脚本
└── reports/                    # 压测报告输出目录
    └── .gitkeep
```

---

## 3. 受控 Mock 上游设计 (`mock_upstream.py`)

Mock 上游采用 Python 原生异步 HTTP 服务（基于 `http.server` 或轻量异步套接字，零第三方强依赖），监听本地端口（默认 `127.0.0.1:19090`）。

### 3.1 路由行为规范

1. **`POST /upstream/sync`**：
   - 行为：零等待立即返回 HTTP 200。
   - 响应格式：标准 OpenAI chat.completion JSON 结构体，携带固定的 token usage。
2. **`POST /upstream/stream`**：
   - 行为：返回 `Content-Type: text/event-stream; charset=utf-8`。
   - 发送 5 个连续的 SSE 块：
     - Chunk 1: `data: {"id":"chatcmpl-bench","choices":[{"delta":{"role":"assistant"}}]}`
     - Chunk 2~4: `data: {"choices":[{"delta":{"content":" token"}}]}` (每个 chunk 之间停顿 5ms)
     - Chunk 5: `data: [DONE]`
3. **`POST /upstream/fail`**：
   - 行为：固定立即返回 HTTP 503 Service Unavailable，模拟上游节点熔断与不可用。
4. **`POST /upstream/backup`**：
   - 行为：备用节点，返回 HTTP 200，承接故障转移。
5. **`GET /health`**：
   - 行为：返回 HTTP 200 `{"status":"ok"}`，供自动化驱动器进行存活探测。

---

## 4. 四大压测场景设计

### 4.1 场景 1：非流式代理极限性能 (`bench-sync`)
- **路由设置**：`bench-sync` -> `http://127.0.0.1:19090/upstream/sync`
- **请求体**：`benchmarks/config/fixtures/chat_sync.json`
- **目标指标**：
  - 饱和极限吞吐（QPS, Requests/sec）
  - 纯代理中位延迟（p50）与长尾延迟（p90, p99）
  - CPU 与内存利用率

### 4.2 场景 2：流式 SSE 与首字时延 (`bench-stream`)
- **路由设置**：`bench-stream` -> `http://127.0.0.1:19090/upstream/stream`
- **请求体**：`benchmarks/config/fixtures/chat_stream.json`
- **目标指标**：
  - 首字延迟（TTFT, Time-To-First-Token）：发出请求到客户端完整读取首个 SSE 数据行的时间差。
  - 长连接保持开销与流式并发连接池稳定性。

### 4.3 场景 3：响应缓存命中对比 (`bench-cache`)
- **路由设置**：`bench-cache` -> `http://127.0.0.1:19090/upstream/sync`（启用本地 Response Cache）
- **请求体**：`benchmarks/config/fixtures/chat_cache.json`
- **预热**：执行前先发送 1 次请求填充本地缓存（MISS -> 写入缓存）。
- **目标指标**：
  - 纯命中状态（`X-Cache: HIT`）下的微秒级时延。
  - 缓存命中对网关 QPS 的放大倍数。

### 4.4 场景 4：多候选容灾故障转移 (`bench-failover`)
- **路由设置**：
  - 候选 1 (Primary)：`http://127.0.0.1:19090/upstream/fail` (返回 503)
  - 候选 2 (Secondary)：`http://127.0.0.1:19090/upstream/backup` (返回 200)
- **目标指标**：
  - 故障转移成功率（应达到 100%）。
  - 熔断重试对请求总体耗时的增量影响。

---

## 5. 压测脚本实现规范

### 5.1 k6 场景脚本 (`benchmarks/k6/scenarios.js`)
- **加压阶段**：
  - Ramping VUs：5s 预热爬坡至目标并发，15s 维持满载，5s 平滑下降。
- **自定义 Metrics**：
  - `Trend('ttft_ms')`：首字延迟毫秒数。
  - `Rate('cache_hit_rate')`：通过 `res.headers['X-Cache'] === 'HIT'` 计算。
  - `Rate('failover_success_rate')`：通过 `res.status === 200 && res.headers['X-Upstream-Provider'] === 'secondary'` 计算。
- **SSE 解析**：在响应流接收时按行匹配 `data: `，首个包含内容的包记录 `ttft_ms.add(Date.now() - startTime)`。

### 5.2 wrk Lua 脚本 (`benchmarks/wrk/*.lua`)
- **`sync.lua`**：载入静态 payload，设置 Authorization 与 Content-Type 请求头，驱动非流式饱和发包。
- **`cache.lua`**：高频发送固定 Prompt，专测内存缓存峰值 QPS。
- **`stream.lua`**：支持流式长连接压测。

---

## 6. 自动化编排器设计 (`bench.py` & `run.sh`)

### 6.1 生命周期时序

```text
[bench.py 启动]
       │
       ▼
1. 环境预检 (Check aigate binary, Check k6 / wrk tools)
       │
       ▼
2. 启动 Mock 上游服务 (127.0.0.1:19090) 并轮询 /health
       │
       ▼
3. 动态生成压测配置文件 (设置端口 18080, 密钥, 4 条基准路由)
       │
       ▼
4. 启动 aigate 进程并等待 /health 200 OK
       │
       ▼
5. 依序或按需调度压测工具 (k6 / wrk) 执行各场景并收集结果
       │
       ▼
6. 优雅清理进程 (SIGTERM -> 等待 -> SIGKILL fallback)
       │
       ▼
7. 聚合指标数据，输出终端汇总与 Markdown 格式报告文件
```

### 6.2 异常与中断安全保证
- 注册 `SIGINT` 与 `SIGTERM` 信号处理器。
- 主逻辑包裹在 `try ... finally` 块中，无论发压成功、失败或人为中途中断，确保 aigate 与 Mock 上游子进程必定被正确清理，绝不残留端口占用。

---

## 7. 报告格式规范

生成的压测报告存放在 `benchmarks/reports/benchmark_<YYYYMMDD_HHMMSS>.md`，包含系统运行环境、Git Commit 版本号以及规范的对比表格：

```markdown
# aigate 性能基准测试报告
- **时间**: 2026-09-30 18:00:00
- **Git Commit**: 81a9301
- **操作系统**: Linux x86_64
- **压测工具**: k6 / wrk

| 场景 | 工具 | 并发连接/VUs | 吞吐量 (QPS) | TTFT (首字延迟) | p50 延迟 | p90 延迟 | p99 延迟 | 成功率 |
|---|---|---|---|---|---|---|---|---|
| **Sync Chat** (非流式转发) | k6 | 50 | 12,450 req/s | - | 1.8 ms | 3.2 ms | 5.1 ms | 100% |
| **Stream SSE** (流式逐字) | k6 | 50 | 4,200 req/s | 3.5 ms | 28.1 ms | 31.0 ms | 35.4 ms | 100% |
| **Cache HIT** (响应缓存命中) | wrk | 64 | 48,100 req/s | - | 0.2 ms | 0.4 ms | 0.8 ms | 100% |
| **Failover** (故障节点转移) | k6 | 20 | 8,900 req/s | - | 2.9 ms | 4.8 ms | 7.6 ms | 100% |
```

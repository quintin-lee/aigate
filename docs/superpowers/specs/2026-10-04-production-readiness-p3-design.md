# aigate 生产可用性 (Production-Readiness) P3 终极就绪设计规范

- **状态**: 实施中 (In Progress)
- **创建日期**: 2026-10-04
- **责任模块**:
  - `src/observe/metrics.h`, `src/observe/metrics.c` (TTFT 首字延迟 Prometheus 监控直方图)
  - `src/core/pipeline_chat.c` (TTFT 指标埋点上报)
  - `src/store/pg_store.h` & `src/upstream/model_router.h`/`.c` (上游在途请求并发限制信号量 `max_concurrent`)
  - `src/core/config.h`, `src/core/config.c`, `src/server/transport_civetweb.c` (原生 TLS/HTTPS 监听 `AIGATE_SSL_CERT`/`KEY`)
  - `deploy/grafana/dashboard.json` (增加 TTFT 首字延迟与在途并发泳道)
  - `deploy/helm/aigate/` (生产级 Helm Chart 打包，涵盖 ConfigMap, Secret, Deployment, Service, Ingress, HPA, PDB)
  - `deploy/bench/` (流式 SSE 并发压测脚本与容量规格 SIZING 指南)
- **目标**: 补齐生产上线的最后一公里：大模型核心 SLA 监控（TTFT）、上游防打崩保护（并发信号量）、单机与零信任网关原生 HTTPS、企业级 Helm Chart 与生产容量规划基准。

---

## 1. 架构与设计细节

### 1.1 TTFT (Time To First Token) Prometheus 观测直方图
- **问题**: 用户对大模型生成应用（如智能对话、代码生成）的核心体验取决于**首字吐出速度**。原网关仅记录总延时 (`aigate_upstream_latency_ns_ns`)，缺少针对首字吐出的分布直方图。
- **设计**:
  - 在 `src/observe/metrics.c` 维护 `aigate_upstream_ttft_ns_ns` 直方图（按 provider 标签打点）。
  - 直方图桶分布适配大模型首字延迟特征（从 50ms 到 30s）：`0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0, +Inf`。
  - 在 `src/core/pipeline_chat.c` 首字 chunk 抵达分支中调用 `metrics_record_upstream_ttft(provider, ttft_ns)`。
  - 在 Grafana 大盘中展示各上游提供商的 TTFT P50 / P90 / P99 趋势。

### 1.2 上游模型级在途并发信号量控制 (`max_concurrent`)
- **问题**: 本地 GPU 推理服务（如 vLLM/Ollama）或有限配额的商业 Provider 在突发高并发下极易显存超限或返回 429。
- **设计**:
  - 在 `upstream_target_t` 与 `model_rec_t` 结构体中增加 `int max_concurrent;`（默认 0，表示不限制）。
  - 在 `model_router.c` 中为每个目标维护原子在途计数器 `atomic_int in_flight;`。
  - 路由选择时：若候选目标的 `max_concurrent > 0` 且 `atomic_load(&target->in_flight) >= target->max_concurrent`，则跳过该目标并尝试故障转移/降级至下一个可用目标。
  - 若所有目标均满载：优雅返回 HTTP 429（带有 `Retry-After: 1` 与错误信息 `"upstream concurrency limit exceeded"`）。
  - 请求/流式响应结束时（无论成功还是中断）原子减 1。

### 1.3 原生 TLS/HTTPS 监听 (`AIGATE_SSL_CERT` / `AIGATE_SSL_KEY`)
- **设计**:
  - 在 `aigate_config` 中增加 `ssl_cert[512]` 与 `ssl_key[512]`。
  - 从环境变量 `AIGATE_SSL_CERT` 与 `AIGATE_SSL_KEY` 读取。
  - 在 `transport_civetweb_start` 中：
    - 若配置了 `ssl_cert`，CivetWeb 监听配置自动增加 `ssl_certificate` 与 `ssl_certificate_key`（或合并 PEM）。
    - 端口若为标准数字且包含 HTTPS 配置，则启动时在 CivetWeb 选项中绑定为 SSL 端口（例如 `8443s`）。

### 1.4 企业级官方 Helm Chart (`deploy/helm/aigate`)
- **设计**:
  - 遵循标准 Helm v3 规范，包含：
    - `Chart.yaml`: name `aigate`, version `1.0.0`, appVersion `1.0.0`
    - `values.yaml`: 全量配置默认值，涵盖副本数、镜像、资源配额、探针、Ingress、环境变量（DB/Redis/Log等）
    - `templates/`:
      - `deployment.yaml`
      - `service.yaml`
      - `ingress.yaml`
      - `configmap.yaml`
      - `secret.yaml`
      - `hpa.yaml`
      - `pdb.yaml`
      - `_helpers.tpl`

### 1.5 压测套件与容量规划指南 (`deploy/bench/`)
- 交付针对大模型 SSE 长连接流式输出的测试脚本 `deploy/bench/k6_stream_test.js`。
- 交付硬件资源规划与内核调优指南 `deploy/bench/SIZING.md`（包括并发数、FD 限制、线程池计算公式、内存预估）。

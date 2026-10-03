# aigate 生产可用性 (Production-Readiness) P1 改造设计规范

- **状态**: 规划完成 (Planned & Ready for Execution)
- **创建日期**: 2026-10-03
- **责任模块**:
  - `src/core/config.h`, `src/core/config.c` (配置加载: CORS 与结构化日志环境变量)
  - `src/core/aigate_log.h`, `src/core/aigate_log.c` (日志级别过滤与 JSON 格式化)
  - `src/server/transport_civetweb.h`, `src/server/transport_civetweb.c` (CORS OPTIONS 预检与安全响应头)
  - `deploy/kubernetes/` (Kubernetes 生产编排清单)
  - `deploy/prometheus/` (Prometheus 生产告警规则)
- **目标**: 在完成 P0 核心吞吐与高可用基础设施之上，打通 Web 前端跨域调用（CORS）、对接现代云原生观测栈（结构化 JSON 日志与等级降噪），并交付标准 Kubernetes 部署编排与 Prometheus 告警规范。

---

## 1. 业务背景与问题分析 (Problem Statement)

在 P0 阶段，网关成功解决了工作线程写死、探针与优雅停机、真实 IP 提取、数据库批量刷写连接隔离与 Redis 故障降级。但在实际面向生产交付与前端业务对接时，存在以下 3 个关键瓶颈：

### 1.1 前端直连受阻：缺乏 CORS 跨域 OPTIONS 预检支持
- **现状**: 现代前端 Web 应用（React/Next.js/内网 AI 助手前端）直连网关 `/v1/chat/completions` 时，浏览器必定首先发出 `OPTIONS` Preflight 请求。网关目前未拦截 OPTIONS，而是将其送入推理管线，由于 Preflight 请求不携带 `Authorization` 头，网关直接返回 `401 Unauthorized`，导致前端跨域调用 100% 失败。
- **目标**: 拦截 `/v1/` 与 `/admin/` 的 `OPTIONS` 请求，快速响应 `204 No Content`，注入规范的 CORS 响应头，并在普通响应中附加全局安全头（`X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: strict-origin-when-cross-origin`）。

### 1.2 云原生日志盲区：纯文本日志与全局日志锁争用
- **现状**: `aigate_log.c` 固定输出纯文本格式，并且在全局互斥锁下无条件执行 `fflush(stderr)`，无日志等级过滤。
- **危害**: 在几千 QPS 的压测或生产高峰下，频繁输出普通 INFO 日志会导致所有工作线程争用全局日志锁；在 Kubernetes 结合 ELK / Loki / Datadog 收集日志时，纯文本格式无法被高效检索与多维分析。
- **目标**: 增加 `AIGATE_LOG_FORMAT=text|json` 与 `AIGATE_LOG_LEVEL=debug|info|warn|error`，低于目标级别的日志在进锁前直接 fast-return，支持输出标准 JSON 格式。

### 1.3 云原生交付物缺失：缺少生产级 K8s 清单与监控告警
- **现状**: 仓库仅提供本地单机测试用的 `docker-compose.yml`，缺少企业容器编排与告警交付件。
- **目标**: 提供标准生产级 Kubernetes Manifests（Deployment 带探针与优雅停机、Service、ConfigMap、HPA、PDB）与 Prometheus Alerting Rules。

---

## 2. 总体架构设计 (Architecture Design)

```
                            [ Web 浏览器 / SPA / Next.js 前端 ]
                                             │
                       OPTIONS 预检 (Preflight) / POST 数据请求
                                             │
                                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ aigate transport_civetweb                                                   │
│                                                                             │
│  【OPTIONS 预检拦截】                                                        │
│    ├── 匹配 method == "OPTIONS"                                             │
│    └── 快速返回 204 No Content (不查数据库、不走鉴权、不打上游)                    │
│        ├── Access-Control-Allow-Origin: <AIGATE_CORS_ALLOW_ORIGIN | *>      │
│        ├── Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS... │
│        ├── Access-Control-Allow-Headers: Authorization, Content-Type...     │
│        └── Access-Control-Max-Age: 86400                                    │
│                                                                             │
│  【正常数据响应安全头注入】                                                    │
│    ├── Access-Control-Allow-Origin                                          │
│    ├── X-Content-Type-Options: nosniff                                      │
│    ├── X-Frame-Options: DENY                                                │
│    └── Referrer-Policy: strict-origin-when-cross-origin                     │
└─────────────────────────────────────────────────────────────────────────────┘
                                             │
                                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ aigate_log 统一日志子系统                                                     │
│                                                                             │
│   aigate_log(level, file, line, fmt, ...)                                   │
│     ├── 1. 级别过滤 (Level Gate): log_level < configured_level -> 立即返回    │
│     │   (零互斥锁争用、零内存分配)                                              │
│     ├── 2. 获取 g_log_mtx 互斥锁                                             │
│     └── 3. 格式化输出:                                                      │
│         ├── FORMAT_TEXT: "2026-10-03T19:00:00.000 INFO msg (file:line)"    │
│         └── FORMAT_JSON: {"ts":"...","level":"INFO","file":"...","msg":"."} │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 详细设计与实现细节

### 3.1 CORS 配置与传输层拦截 (`config`, `transport_civetweb`)
1. **新增环境变量**:
   - `AIGATE_CORS_ALLOW_ORIGIN`: 默认 `"*"`，支持精确域名（如 `"https://chat.example.com"`）。
2. **CivetWeb 预检拦截**:
   - 在 `handle_v1` 与 `handle_admin` 入口：
     ```c
     if (strcmp(ri->request_method, "OPTIONS") == 0) {
         mg_printf(conn,
             "HTTP/1.1 204 No Content\r\n"
             "Access-Control-Allow-Origin: %s\r\n"
             "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS, HEAD\r\n"
             "Access-Control-Allow-Headers: Authorization, Content-Type, Cache-Control, "
             "X-Aigate-Target-Provider, X-Aigate-Compress, X-Aigate-Prompt-Cache, "
             "traceparent, X-Requested-With, Accept\r\n"
             "Access-Control-Max-Age: 86400\r\n"
             "Content-Length: 0\r\n"
             "Connection: keep-alive\r\n\r\n",
             cw->cors_allow_origin);
         return 1;
     }
     ```
3. **安全响应头统一添加**:
   - 在 `cw_write` 响应头发送、`send_http_error_json` 以及 `handle_admin` 输出中注入：
     - `Access-Control-Allow-Origin: %s`
     - `X-Content-Type-Options: nosniff`
     - `X-Frame-Options: DENY`

### 3.2 结构化 JSON 日志与级别过滤 (`aigate_log`)
1. **新增配置项**:
   - `AIGATE_LOG_FORMAT`: `"text"` (默认) 或 `"json"`.
   - `AIGATE_LOG_LEVEL`: `"debug"`, `"info"` (默认), `"warn"`, `"error"`.
2. **级别过滤枚举**:
   ```c
   typedef enum {
       AIGATE_LOG_LVL_DEBUG = 0,
       AIGATE_LOG_LVL_INFO  = 1,
       AIGATE_LOG_LVL_WARN  = 2,
       AIGATE_LOG_LVL_ERROR = 3
   } aigate_log_level_t;
   ```
3. **无锁快筛 (Lock-free Fast Exit)**:
   - 进入 `aigate_log()` 时，先比较 `level_enum < g_configured_level`，若低于则直接 `return`，完全避免进入 `pthread_mutex_lock(&g_log_mtx)`，高并发下将日志锁争用降为零。
4. **JSON 格式化**:
   - 提取格式化文本，进行标准 JSON 转义（转义 `"`、`\`、换行），组装为：
     `{"ts":"2026-10-03T19:00:00.123Z","level":"INFO","caller":"src/main.c:120","msg":"aigate ready on :8080"}\n`

### 3.3 Kubernetes 生产编排与 Prometheus 告警规则
1. **`deploy/kubernetes/`**:
   - `deployment.yaml`: 双副本高可用、CPU/内存 requests/limits、readinessProbe/livenessProbe/startupProbe（调用 `/ready` 与 `/live`）、`terminationGracePeriodSeconds: 30`。
   - `service.yaml`: ClusterIP 暴露 8080 与 8081。
   - `configmap.yaml`: 标准环境变量注入。
   - `hpa.yaml`: 基于 CPU 70% 自动伸缩至 2~10 副本。
   - `pdb.yaml`: `minAvailable: 1` 确保滚动发布零断流。
2. **`deploy/prometheus/alerts.yaml`**:
   - `AigateDown`: 网关实例离线告警。
   - `AigateHigh5xxRate`: 5xx 占比超过 1% 告警。
   - `AigateP99LatencyHigh`: P99 延迟超过 3 秒告警。
   - `AigateRedisFailOpenActive`: Redis 故障触发降级告警。

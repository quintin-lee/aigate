# aigate 生产可用性 (Production-Readiness) P1 实施计划

> **规范文件**: `docs/superpowers/specs/2026-10-03-production-readiness-p1-design.md`

## 任务目标
实施并验证三大 P1 关键生产能力：
1. **Task 1: CORS 跨域预检与全局安全响应头** (支持 Web 浏览器/SPA 前端直连)
2. **Task 2: 云原生结构化 JSON 日志与无锁动态日志等级过滤** (支持 K8s/ELK/Loki，高并发零锁竞争)
3. **Task 3: 生产级 Kubernetes 编排清单与 Prometheus 告警规则** (交付 Enterprise 级 Helm/Manifests 与 alerts.yaml)
4. **Task 4: 文档与配置同步** (更新 CONFIGURATION.md 与 .env.example)

---

## 任务执行清单

- [x] **Task 1: CORS 跨域支持与全局安全响应头**
  - [x] `src/core/config.h` & `src/core/config.c`: 增加 `cors_allow_origin` (环境变量 `AIGATE_CORS_ALLOW_ORIGIN`，默认 `"*"`).
  - [x] `src/server/transport_civetweb.h` & `src/server/transport_civetweb.c`:
    - 在 `transport_civetweb_t` 中持有 `cors_allow_origin` 配置并传递.
    - 在 `handle_v1` 与 `handle_admin` 拦截 `OPTIONS` 请求，快速返回 204 No Content，带标准 CORS 头.
    - 在所有响应路径（`cw_write`, `send_http_error_json`, `handle_admin`）附加 `Access-Control-Allow-Origin` 以及 `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: strict-origin-when-cross-origin`.
  - [x] `tests/unit/server/test_transport_civetweb.c`: 增加 CORS 预检与安全响应头测试用例.

- [x] **Task 2: 结构化 JSON 日志与无锁日志等级过滤**
  - [x] `src/core/config.h` & `src/core/config.c`: 增加 `log_format` (`AIGATE_LOG_FORMAT=text|json`) 和 `log_level` (`AIGATE_LOG_LEVEL=debug|info|warn|error`).
  - [x] `src/core/aigate_log.h` & `src/core/aigate_log.c`:
    - 实现 `aigate_log_init(const char* format, const char* level)` 与等级枚举.
    - 实现入锁前 fast-return 过滤.
    - 实现结构化 JSON 格式输出 `{"ts":"...","level":"...","file":"...","line":...,"msg":"..."}\n`.
  - [x] `src/main.c`: 读取配置后调用 `aigate_log_init(cfg.log_format, cfg.log_level)`.
  - [x] `tests/unit/core/test_config.c`: 增加日志配置读取与合法性测试.
  - [x] `tests/unit/core/test_log.c`: 增加日志过滤与 JSON 输出测试.

- [x] **Task 3: Kubernetes 生产编排清单与 Prometheus 告警规则**
  - [x] 创建 `deploy/kubernetes/`:
    - `deployment.yaml` (探针、资源约束、优雅停机、非 root 运行)
    - `service.yaml` (端口暴露)
    - `configmap.yaml` & `secret.yaml`
    - `hpa.yaml` (水平自动扩缩容)
    - `pdb.yaml` (滚动更新零断流)
  - [x] 创建 `deploy/prometheus/alerts.yaml` (网关下线、5xx 错误率、P99 延迟、降级告警规则).

- [x] **Task 4: 文档与配置同步**
  - [x] 更新 `docs/CONFIGURATION.md`: 补充 `AIGATE_CORS_ALLOW_ORIGIN`, `AIGATE_LOG_FORMAT`, `AIGATE_LOG_LEVEL`.
  - [x] 更新 `.env.example`: 补充对应环境变量及注释.

- [x] **Task 5: 全量验证与提交**
  - [x] 运行 `ctest` 确保 100% 测试通过.
  - [x] 运行 `docker compose build` 验证容器镜像编译无警告.
  - [ ] Git commit 符合 Gitmoji 规范.

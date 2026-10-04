# aigate 生产可用性 (Production-Readiness) P2 进阶设计规范

- **状态**: 实施中 (In Progress)
- **创建日期**: 2026-10-04
- **责任模块**:
  - `src/main.c` (SIGHUP 信号捕获、无锁平滑热重载、主密钥轮换 CLI 子命令)
  - `src/server/transport_civetweb.h`, `src/server/transport_civetweb.c` (动态配置热重载线程安全注入)
  - `deploy/kubernetes/ingress.yaml` (生产级 Ingress 路由、长超时、关闭 buffering、TLS 证书自动化)
  - `deploy/grafana/dashboard.json` (企业级全景监控大盘)
  - `.github/workflows/ci.yml` (云端多编译器自动化质量门禁与镜像构建)
- **目标**: 在完成 P0/P1 的网关内核高吞吐、容灾降级、CORS 跨域与 K8s/Prometheus 交付基础上，补齐真实生产流量接入（防断流 Ingress）、零停机配置热重载（SIGHUP）、开箱即用可视化运维（Grafana 大盘）、全自动 CI/CD 门禁与主密钥轮换合规工具。

---

## 1. 业务背景与问题分析 (Problem Statement)

### 1.1 Ingress 长流式防断断流 (LLM Streaming Ingress)
- **问题**: 在生产 Kubernetes 集群部署中，外部客户端（如 Web 页面、移动端、第三方系统）经由 Ingress 访问网关。对于大模型推理（特别是思考链 Reasoning 模型或长文档生成），单次流式响应时间常在 60s ~ 300s 以上。若使用未调优的 Nginx Ingress，默认 60s 的 `proxy-read-timeout` 将强行中断连接，且默认开启的 `proxy-buffering` 会阻滞首字流式吐出。
- **目标**: 提供标准生产级 `deploy/kubernetes/ingress.yaml`，预配置 `proxy-read-timeout: "3600"`、`proxy-send-timeout: "3600"`、`proxy-buffering: "off"`、`proxy-body-size: "10m"` 以及基于 `cert-manager` 的 Let's Encrypt 自动 TLS 签发。

### 1.2 进程级平滑热重载 (Zero-Downtime Hot Reload)
- **问题**: 生产环境下由于排障或策略调整（例如线上临时将 `AIGATE_LOG_LEVEL` 调为 `debug`、更新 `AIGATE_CORS_ALLOW_ORIGIN` 允许新前端域名、调整 `AIGATE_TRUSTED_PROXIES` 反代列表），若通过重启 Pod 刷新配置，会导致正在进行的数百条长流式会话全部断流。且未捕获 `SIGHUP` 信号时，任何触发 HUP 信号的操作（如 Kubernetes ConfigMap 自动重载控制器）会直接杀死网关进程。
- **目标**: 在 `src/main.c` 捕获 `SIGHUP` 信号：重新从环境变量/系统配置刷新轻量级配置，在 `transport_civetweb` 中提供线程安全的原子/互斥更新，实现零断流、零重启平滑热更。

### 1.3 可视化监控大盘 (Grafana Dashboard)
- **问题**: 虽然网关已在 `/metrics` 暴露了丰富的 Prometheus 观测指标，且配置了 `alerts.yaml` 告警规则，但缺少统一的 Grafana 交互看板。运维和架构师难以直观查看不同模型的 QPS、延迟分位数 P99、Prompt 缓存节省比例与熔断状态。
- **目标**: 交付开箱即用的 `deploy/grafana/dashboard.json`，提供核心流量、模型延迟、Token/成本经济学、熔断与故障转移全景图。

### 1.4 CI/CD 自动化流水线 (CI/CD Quality Gate)
- **问题**: 项目依赖开发者本地 `ctest`，缺少云端 GitHub Actions 质量把控，无法保障 PR 提交时的 0 编译警告、全量单元测试与内存泄漏卡点。
- **目标**: 交付 `.github/workflows/ci.yml`，包含多编译器严格编译、CTest 自动化验证、ASan/UBSan 检测与 Docker 镜像构建验证。

### 1.5 主密钥轮换实用工具 (Master Key Rotation)
- **问题**: PostgreSQL 中上游 Provider 的 API Key 采用 AES-256-GCM 主密钥加密存储为 `pg:<hex>` 格式。当企业触发安全合规要求需要轮换 `AIGATE_MASTER_KEY` 时，缺乏安全的存量数据重加密手段。
- **目标**: 提供 CLI 模式 `./aigate --rotate-master-key <old_hex_64> <new_hex_64>`，安全连接数据库并原子轮换所有已加密的 `models` 表 API 密钥。

---

## 2. 总体设计细节

### 2.1 SIGHUP 平滑热重载架构
1. `src/main.c`:
   - 定义 `static volatile sig_atomic_t g_reload = 0;`。
   - `sig_handler(int sig)`:
     ```c
     if (sig == SIGHUP) {
         g_reload = 1;
         return;
     }
     g_stop = 1;
     ```
   - 在主轮询循环 `while (!g_stop)` 中：
     ```c
     if (g_reload) {
         g_reload = 0;
         /* 重新加载轻量级环境变量配置 */
         aigate_config new_cfg;
         if (aigate_config_load(&new_cfg) == 0) {
             aigate_log_init(new_cfg.log_format, new_cfg.log_level);
             transport_civetweb_update_cors(cw, new_cfg.cors_allow_origin);
             transport_civetweb_update_trusted_proxies(cw, new_cfg.trusted_proxies);
             AIGATE_LOG_INFO("main: configuration reloaded via SIGHUP (level=%s, cors=%s, proxies=%s)",
                             new_cfg.log_level, new_cfg.cors_allow_origin, new_cfg.trusted_proxies);
         }
     }
     ```
2. `src/server/transport_civetweb.h` & `c`:
   - 在 `transport_civetweb_t` 中保护 `cors_allow_origin` 与 `trusted_proxies` 的并发读写，增加更新函数：
     `void transport_civetweb_update_cors(transport_civetweb_t* cw, const char* origin);`
     `void transport_civetweb_update_trusted_proxies(transport_civetweb_t* cw, const char* proxies);`

### 2.2 主密钥轮换工具设计
在 `src/main.c` 检查 `argc >= 4 && strcmp(argv[1], "--rotate-master-key") == 0`:
- 解析旧密钥 64-hex 与新密钥 64-hex。
- 连接 PostgreSQL，查询所有含有 `api_key LIKE 'pg:%'` 的模型记录。
- 使用 `secret_decrypt(old_key, ...)` 解密，再用 `secret_encrypt(new_key, ...)` 重加密。
- 事务更新数据库，打印统计摘要（如 `Successfully rotated 8 upstream model keys`）。

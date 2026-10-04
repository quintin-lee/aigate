# aigate 生产可用性 (Production-Readiness) P2 实施计划

> **规范文件**: `docs/superpowers/specs/2026-10-04-production-readiness-p2-design.md`

## 任务目标
实施并验证三大进阶生产就绪能力：
1. **Task 1: SIGHUP 信号捕获与动态配置平滑热重载** (零断流调整日志级别/CORS/受信代理)
2. **Task 2: Kubernetes Ingress 生产级路由清单** (长流式防切断配置、SSE 零缓冲直推、TLS 证书自动化)
3. **Task 3: 开箱即用企业级 Grafana 监控大盘** (`deploy/grafana/dashboard.json` 覆盖全生命周期指标)
4. **Task 4: GitHub Actions 生产 CI/CD 流水线** (`.github/workflows/ci.yml` 多架构编译、0警告、全单测卡点)
5. **Task 5: 主密钥离线轮换 CLI 工具** (`--rotate-master-key` 支持生产密钥无损原子轮换)
6. **Task 6: 全量验证与提交** (全量测试 100% 通过、Docker 构建通过、Git 规范提交)

---

## 任务执行清单

- [x] **Task 1: SIGHUP 信号捕获与动态配置平滑热重载**
  - [x] `src/server/transport_civetweb.h` & `src/server/transport_civetweb.c`:
    - 增加互斥锁保护动态配置项读写.
    - 实现 `transport_civetweb_update_cors(cw, origin)`.
    - 实现 `transport_civetweb_update_trusted_proxies(cw, proxies)`.
  - [x] `src/main.c`:
    - 捕获 `SIGHUP` 信号，置位 `g_reload = 1`.
    - 主循环中检测并重新加载配置，刷新日志系统与传输层参数.
  - [x] `tests/unit/server/test_transport_civetweb.c`:
    - 增加动态热重载 API 测试用例.

- [x] **Task 2: Kubernetes Ingress 生产路由清单**
  - [x] 创建 `deploy/kubernetes/ingress.yaml` (包含 proxy-read-timeout 3600、proxy-buffering off、TLS 证书自动化).

- [x] **Task 3: 开箱即用 Grafana 监控大盘**
  - [x] 创建 `deploy/grafana/dashboard.json` (QPS、状态码、P99 延迟分位数、Token/成本经济学、熔断与故障转移).

- [x] **Task 4: GitHub Actions 生产 CI/CD 流水线**
  - [x] 创建 `.github/workflows/ci.yml` (自动构建、全单测、ASan 内存检测、Docker 镜像构建).

- [x] **Task 5: 主密钥离线轮换 CLI 工具**
  - [x] `src/main.c`: 增加 `--rotate-master-key <old_key_hex> <new_key_hex>` 命令行分支与处理逻辑.

- [x] **Task 6: 全量验证与提交**
  - [x] `cmake --build build` 确保 0 警告通过.
  - [x] `ctest` 确保 100% 测试通过.
  - [x] `docker compose build` 确保容器构建成功.
  - [x] Gitmoji 规范提交.

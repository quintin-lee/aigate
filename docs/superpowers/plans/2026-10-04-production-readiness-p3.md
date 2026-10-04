# aigate 生产可用性 (Production-Readiness) P3 实施计划

> **规范文件**: `docs/superpowers/specs/2026-10-04-production-readiness-p3-design.md`

## 任务目标
实施并验证 5 大终极生产就绪能力：
1. **Task 1: 大模型 TTFT (Time To First Token) Prometheus 监控直方图**
2. **Task 2: 上游并发限制信号量 (`max_concurrent`) 与防击穿降级**
3. **Task 3: 网关原生 TLS/HTTPS 监听 (`AIGATE_SSL_CERT`/`KEY`)**
4. **Task 4: 官方生产级 Helm Chart (`deploy/helm/aigate/`)**
5. **Task 5: 流式压测脚本与容量规格 SIZING 指南 (`deploy/bench/`)**
6. **Task 6: 监控看板升级与全量验证提交**

---

## 任务执行清单

- [ ] **Task 1: 大模型 TTFT Prometheus 直方图与埋点**
  - [ ] `src/observe/metrics.h` & `src/observe/metrics.c`:
    - 增加 `metrics_record_upstream_ttft(provider, ttft_ns)`.
    - 在 Prometheus 输出中格式化 `aigate_upstream_ttft_ns_ns_bucket`.
  - [ ] `src/core/pipeline_chat.c`:
    - 在首字 chunk 到达分支调用 `metrics_record_upstream_ttft`.
  - [ ] `tests/unit/observe/`:
    - 增加 TTFT 指标记录与直方图输出单测.

- [ ] **Task 2: 上游并发限制信号量 (`max_concurrent`) 与防击穿降级**
  - [ ] `src/store/pg_store.h`:
    - 在 `upstream_target_t` 与 `model_rec_t` 增加 `int max_concurrent`.
  - [ ] `src/upstream/model_router.h` & `src/upstream/model_router.c`:
    - 增加在途并发信号量获取与释放接口 `model_router_acquire_target` / `model_router_release_target`.
    - 候选人选择逻辑支持跳过满载 target，无可用容量时返回超限.
  - [ ] `tests/unit/upstream/test_failover.c` 或相关单测:
    - 验证当候选者在途并发打满时自动故障转移至备用 target.

- [ ] **Task 3: 网关原生 TLS/HTTPS 监听**
  - [ ] `src/core/config.h` & `src/core/config.c`:
    - 解析 `AIGATE_SSL_CERT` 与 `AIGATE_SSL_KEY`.
  - [ ] `src/server/transport_civetweb.c`:
    - 启动时若配置证书，注入 CivetWeb SSL 参数并配置 SSL 端口.
  - [ ] `tests/unit/core/test_config.c`:
    - 验证 SSL 配置读取.

- [ ] **Task 4: 官方生产级 Helm Chart**
  - [ ] 创建 `deploy/helm/aigate/Chart.yaml` 与 `values.yaml`.
  - [ ] 创建 `deploy/helm/aigate/templates/` (deployment, service, ingress, configmap, secret, hpa, pdb, helpers).

- [ ] **Task 5: 流式压测脚本与容量 SIZING 指南**
  - [ ] 创建 `deploy/bench/k6_stream_test.js`.
  - [ ] 创建 `deploy/bench/SIZING.md`.

- [ ] **Task 6: 监控看板升级与全量验证提交**
  - [ ] 更新 `deploy/grafana/dashboard.json` 增加 TTFT 面板.
  - [ ] `cmake --build build` 确保 0 警告通过.
  - [ ] `ctest` 确保 100% 测试通过.
  - [ ] `docker compose build` 确保容器构建成功.
  - [ ] Gitmoji 规范提交.

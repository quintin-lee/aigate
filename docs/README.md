# aigate Repo Map

> 新人先看：根 [README.md](../README.md)（快速开始）→
> [DEVELOPMENT.md](DEVELOPMENT.md)（构建变体、加模块/加测试）。
> 一站式新人上手包见 [ONBOARDING.md](ONBOARDING.md)。
> 配置详解：[CONFIGURATION.md](CONFIGURATION.md)（全量语义，以源码为准）。
> 生产部署：[DEPLOYMENT.md](DEPLOYMENT.md)（Helm / Compose / K8s / 监控告警全指南）。

## Top-level directories

- `deploy/` — 部署配置与基准压测：`helm/`（官方 Helm Chart）、`kubernetes/`（原生 Manifests）、`prometheus/`（告警规则）、`grafana/`（监控看板）与 `bench/`（k6/压测脚本）。
- `src/` — gateway source, 7 layers: `core/` (lifecycle/config/log/secrets),
  `common/` (lru/sha256), `upstream/` (model router + providers + client),
  `policy/` (auth/budget/circuit-breaker/guardrails/ratelimit/response-cache),
  `store/` (postgres/redis/usage-meter), `observe/` (event-bus/health/metrics),
  `server/` (admin API + UI + transport). `main.c` stays at `src/` root.
- `tests/unit/` — mirrors `src/` layers 1:1 (`test_<module>.c` lives in the
  same layer as `src/<layer>/<module>.c`); `run_tests.c` is the registry runner,
  `mock_upstream.c` is the shared stub.
- `tests/integration/` — pytest end-to-end suite (`test_gateway.py`,
  `test_redis_clustering.py`) plus `smoke.sh`.
- `docs/architecture/` — architecture notes; `docs/superpowers/` — specs/plans/reports.
- `web/admin.html` — ops dashboard single page, baked into the binary by
  `scripts/embed_html.py` (edit the page → re-run the script → rebuild).
- `scripts/` — build/dev helpers (`embed_html.py`).
- `schema/schema.sql` — postgres schema. `cmake/` — reserved for toolchain modules.
- `third_party/` — vendored tarballs (civetweb, hdr_histogram).

## Build (canonical)

```bash
cmake -B build -S . && cmake --build build -j && ctest --test-dir build
```

`build/` is the only documented build directory (see `.gitignore`).

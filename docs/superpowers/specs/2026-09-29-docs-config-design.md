# docs 全仓巡检：配置详解（B 方案）设计稿

- 日期：2026-09-29
- 范围：全仓文档巡检；标准：事实错误 + 补新章节（已确认，不做纯措辞打磨）
- 源头：`src/core/config.c`、`src/core/config.h`、`src/server/transport_civetweb.c:474-492`、`CMakeLists.txt:15-29`、`Dockerfile`

## 背景（已实测）

- `AIGATE_PG_DSN` 必填（缺失/超长启动失败，`config.c:34-43`），但 `.env.example` 无此行；ONBOARDING §2 自称"完整清单见 .env.example"失实。
- 越界行为分两类：启动失败组（`UPSTREAM_TIMEOUT_MS`、`USAGE_FLUSH_S`、`MAX_BODY_BYTES`、`MASTER_KEY` 格式）vs 静默回默认组（`REDIS_TIMEOUT_MS→100`、`REDIS_POOL_SIZE→32`，`config.c:96-106`；`LOCKOUT_*` 越界保默认 10/300）。
- 依赖行口径差：README 行（FetchContent 兜底）vs Dockerfile（`ca-certificates` + `libhiredis-dev` 系统路径，`CMakeLists.txt:15` 系统优先）。两边各自可编，README"取值自 Dockerfile"不严谨。
- `METRICS_ACL` 为逗号分隔 CIDR（`config.h:14`）；`LISTEN` 支持 `:port` / `host:port`（`transport_civetweb.c:487-492`）；compose 层 `POSTGRES_PASSWORD` / `AIGATE_LISTEN_PORT` 不进网关进程。

## 变更清单

1. 新建 `docs/CONFIGURATION.md`：14 个网关变量全表（变量/必填/默认/越界行为/说明）+ 归属划分注（compose vs 网关）+ 溯源注（源码位置）。只写语义，不写教程。
2. 改 `.env.example`：追加 `AIGATE_PG_DSN` / `AIGATE_REDIS_URL` / `AIGATE_REDIS_TIMEOUT_MS` / `AIGATE_REDIS_POOL_SIZE` 四行注释（默认+范围），原有行不动。
3. 改 `docs/ONBOARDING.md` §2：加"全量语义见 CONFIGURATION.md"，删"完整清单"断言，表内行不动。
4. 改 `README.md`：依赖块下加一句口径注。改 `docs/README.md` + 根 README 文档列表：各加 1 行新文档导航。`DEVELOPMENT.md` 不动（经 ONBOARDING §2 可达）。

## 验证

- `grep` 确认 14 个变量名在新文档齐备；`ctest --test-dir build` 不受影响（纯文档）；三处导航链接可达；`git diff --stat` 仅预期文件。

## 非目标

- 不碰 `architecture/`、`superpowers/`、源码与 compose 配置；tool-calling 架构笔记待功能稳定后另议。

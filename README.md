# aigate

C17 编写的 LLM API 网关：统一的 OpenAI 兼容入口（`/v1/*`），身后接多个上游
Provider；自带鉴权、限流、预算、熔断、护栏、响应缓存、用量计量（PostgreSQL）、
Redis 缓存、Prometheus 指标（`/metrics`）与管理后台（Admin API + 内嵌 Web UI）。

## 快速开始（Docker）

```bash
cp .env.example .env   # 填写 POSTGRES_PASSWORD / AIGATE_ADMIN_TOKEN
docker compose up --build
```

网关监听 `:8080`（用 `AIGATE_LISTEN_PORT` 改宿主机映射端口）。
表结构由网关启动时自动创建（`pg_store_migrate()` 执行内嵌的
`src/store/schema_sql.h`，幂等，可重跑）。

## 本地构建

依赖（Debian/Ubuntu，取值自 `Dockerfile` 构建阶段）：

```bash
sudo apt-get install -y build-essential cmake pkg-config python3 \
  libcurl4-openssl-dev libssl-dev libpq-dev libjansson-dev zlib1g-dev
```

注：与 Dockerfile 构建阶段差 `ca-certificates`/`libhiredis-dev`（系统路径；按此行安装走 FetchContent 兜底，同样可编）。

hiredis / hdr_histogram / civetweb 走 FetchContent，`third_party/` 下有离线
tarball，无网也能配。python3 仅构建时用（`scripts/embed_html.py` 把
`web/admin.html` 烘焙进二进制）。

```bash
cmake -B build -S . && cmake --build build -j && ctest --test-dir build
```

`build/` 是唯一的文档化构建目录（见 `.gitignore`）。

## 本地运行

另起 PostgreSQL + Redis（或 `docker compose up postgres redis` 只起依赖），
然后：

```bash
export AIGATE_LISTEN=":8080"
export AIGATE_PG_DSN="host=127.0.0.1 dbname=aigate user=aigate password=changeme"
export AIGATE_ADMIN_TOKEN="dev-token"
export AIGATE_REDIS_URL="redis://127.0.0.1:6379"
./build/aigate
```

完整变量清单见 [docs/CONFIGURATION.md](docs/CONFIGURATION.md)。

## 测试

- 单元测试：`ctest --test-dir build`（6 项全绿为基线）。
- 集成测试：`pip install -r tests/integration/requirements.txt`，然后
  `TEST_PG_DSN="postgresql://postgres:postgres@127.0.0.1:5432/aigate_test" python3 -m pytest tests/integration -v`
 （需可达的 PostgreSQL；连不上自动 skip）。
- 冒烟：`TEST_PG_DSN=... ./tests/integration/smoke.sh`（要求 `./build/aigate`
  已编好，默认端口 18080，可用 `AIGATE_TEST_PORT` 改）。

## 文档

- [docs/README.md](docs/README.md) — 全仓目录地图
- [docs/ONBOARDING.md](docs/ONBOARDING.md) — 新人一站式上手（构建/配置/首个请求/排障）
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — 构建变体、加模块/加测试、代码风格
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) — 全量配置语义（14 个网关变量/越界行为/归属划分）
- [docs/architecture/](docs/architecture/) — 架构笔记
- [docs/superpowers/](docs/superpowers/) — specs / plans / reports

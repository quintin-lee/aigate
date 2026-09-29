# 新人上手包（Onboarding）

> 一站式：依赖构建 → 配置速查 → 运行 + 首个请求演练 → 后台 + 测试 → 排障 FAQ。
> 深入阅读：[docs/README.md](README.md)（全仓地图）、[DEVELOPMENT.md](DEVELOPMENT.md)（构建变体/加模块/加测试）。

## 1. 依赖与构建

前置依赖（Debian/Ubuntu，取值自 `Dockerfile` 构建阶段）：

```bash
sudo apt-get install -y build-essential cmake pkg-config python3 \
  libcurl4-openssl-dev libssl-dev libpq-dev libjansson-dev zlib1g-dev
```

hiredis / hdr_histogram / civetweb 走 FetchContent，`third_party/` 下有离线
tarball，无网也能配。python3 仅构建时用（`scripts/embed_html.py` 把
`web/admin.html` 烘焙进二进制）。

标准构建（`build/` 是唯一的文档化构建目录）：

```bash
cmake -B build -S . && cmake --build build -j && ctest --test-dir build
```

预期：`100% tests passed out of 6`（6 项全绿为基线）。

## 2. 配置速查表

常用项速查（全量语义见 [CONFIGURATION.md](CONFIGURATION.md)，源头为 `src/core/config.c`）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `POSTGRES_PASSWORD` | （必填，无默认） | Docker Compose 下 PostgreSQL 密码 |
| `AIGATE_ADMIN_TOKEN` | （必填，无默认） | 管理后台 API 鉴权令牌 |
| `AIGATE_LISTEN` | `:8080` | 网关监听地址 |
| `AIGATE_PG_DSN` | — | PostgreSQL 连接串，如 `host=127.0.0.1 dbname=aigate user=aigate password=changeme` |
| `AIGATE_REDIS_URL` | — | Redis 地址，如 `redis://127.0.0.1:6379` |
| `AIGATE_UPSTREAM_TIMEOUT_MS` | `60000` | 上游请求超时（毫秒） |
| `AIGATE_USAGE_FLUSH_S` | `5` | 用量刷盘周期（秒）；每日配额滚动随刷盘周期推进 |
| `AIGATE_MAX_BODY_BYTES` | `10485760` | 请求体上限（字节） |
| `AIGATE_METRICS_ACL` | `127.0.0.1` | 允许访问 `/metrics` 的 IP |
| `AIGATE_ALLOW_PLAINTEXT_KEYS` | `0` | 是否允许上游密钥明文落库 |

## 3. 运行 + 首个请求演练

另起 PostgreSQL + Redis（或 `docker compose up postgres redis` 只起依赖），然后：

```bash
export AIGATE_LISTEN="127.0.0.1:18080"
export AIGATE_PG_DSN="postgresql://postgres:postgres@127.0.0.1:5432/aigate_test"
export AIGATE_ADMIN_TOKEN="dev-token"
export AIGATE_METRICS_ACL="127.0.0.1"
./build/aigate &
```

表结构由网关启动时自动创建（幂等，可重跑）。演练（与 `tests/integration/smoke.sh` 同序列）：

```bash
# 指标面通了没有
curl -s http://127.0.0.1:18080/metrics | grep aigate_requests_total
# 建一把 API key（返回含 "key_id" 与 "plaintext"，明文只给这一次）
curl -s -X POST http://127.0.0.1:18080/admin/v1/keys \
  -H "Authorization: Bearer dev-token" -H "Content-Type: application/json" \
  -d '{"name":"hello-key","allowed_models":["mock-model"],"rate_qps":100}'
# 注册一个模型路由
curl -s -X POST http://127.0.0.1:18080/admin/v1/models \
  -H "Authorization: Bearer dev-token" -H "Content-Type: application/json" \
  -d '{"name":"mock-model","provider":"openai","endpoint":"http://127.0.0.1:19999/v1"}'
# 用刚建的 key 调 OpenAI 兼容入口（把 <PLAINTEXT> 换成上一步返回的明文）
curl -s http://127.0.0.1:18080/v1/models -H "Authorization: Bearer <PLAINTEXT>" | grep mock-model
```

四条全中即打通：管理面 → 鉴权 → 路由 → OpenAI 兼容入口。演练完 `kill %1` 停网关。

## 4. 后台 + 测试

- 管理后台：网关跑起来后，浏览器打开 `http://127.0.0.1:18080/admin`（单页运维面板，
  由 `scripts/embed_html.py` 烘焙进二进制；改页面见 DEVELOPMENT.md）。
- Admin API：`curl` 示例见上节；完整路由见源码 `src/server/admin_api.c`。
- 单元测试：`ctest --test-dir build`（6 项全绿为基线）。
- 集成测试：`pip install -r tests/integration/requirements.txt`，然后
  `TEST_PG_DSN="postgresql://postgres:postgres@127.0.0.1:5432/aigate_test" python3 -m pytest tests/integration -v`
  （需可达的 PostgreSQL；连不上自动 skip）。
- 冒烟：`TEST_PG_DSN=... ./tests/integration/smoke.sh`（要求 `./build/aigate`
  已编好，默认端口 18080，可用 `AIGATE_TEST_PORT` 改）。

## 5. 排障 FAQ

**Q:** 端口被占用，网关起不来。
**A:** 换 `AIGATE_LISTEN`（如 `127.0.0.1:18081`），或 `ss -ltnp | grep 8080` 找到占用者。

**Q:** 数据库连不上（`connection refused` / 密码错）。
**A:** 先 `pg_isready -d "$AIGATE_PG_DSN"`；Docker 方式用 `docker compose up postgres redis`
只起依赖；密码对齐 `POSTGRES_PASSWORD` 与连接串。

**Q:** Admin API 返回 401。
**A:** `AIGATE_ADMIN_TOKEN` 没对上：检查请求头 `Authorization: Bearer <token>` 与
网关环境变量是否同一值；注意连续输错 10 次会触发锁定（300 秒窗口）。

**Q:** 构建失败（缺头文件 / 链接错）。
**A:** 先装齐 §1 依赖；离线环境确认 `third_party/` tarball 完整；
Debug 构建的 CivetWeb sanitizer 问题见 DEVELOPMENT.md（顶层 CMake 已自动处理）。

**Q:** 测试挂起或变红。
**A:** `ctest` 只看 `unit` 一项，runner 退出码 = 失败用例数，先看是哪个 TEST_CASE；
集成测试要可达的 PG，没有会自动 skip 不是失败；冒烟脚本要求网关已编好且端口空闲。

**Q:** `/metrics` 返回 403。
**A:** `AIGATE_METRICS_ACL` 默认只放行 `127.0.0.1`，把本机出口 IP 加进去。

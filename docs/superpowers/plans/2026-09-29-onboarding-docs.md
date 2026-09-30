# Onboarding Docs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a one-stop `docs/ONBOARDING.md` newcomer pack (5 sections) plus 1 nav line each in root README, docs/README.md, docs/DEVELOPMENT.md.

**Architecture:** Single new doc file + 3 one-line nav edits. All commands copied verbatim from root README / docs/DEVELOPMENT.md / tests/integration/smoke.sh (sources of truth); every command actually run once before writing. No Doxyfile change (INPUT already covers README).

**Tech Stack:** Markdown docs, bash, curl, cmake + ctest.

---

### Task 1: Verify every command the new doc will contain

**Files:** none (read-only verification).

- [ ] **Step 1: Run the canonical build + unit tests**

Run: `cmake -B build -S . && cmake --build build -j && ctest --test-dir build`
Expected: build exit 0, `100% tests passed out of 6`.

- [ ] **Step 2: Attempt the live first-request drill**

Run: `TEST_PG_DSN="postgresql://postgres:postgres@127.0.0.1:5432/aigate_test" ./tests/integration/smoke.sh`
Expected: either `=== Smoke test passed successfully! ===` (PG reachable), or `[SKIP] PostgreSQL ... is not reachable` (no local PG). Record which one; if SKIP, the drill commands in the doc are still covered because smoke.sh [2/5]–[5/5] steps are the doc's curl sequence verbatim (curl /metrics → POST /admin/v1/keys → POST /admin/v1/models → GET /v1/models), and their expected outputs are the script's own grep assertions (`aigate_requests_total`, `"key_id"`, `"plaintext"`, `mock-model`).

- [ ] **Step 3: Record dependency list source**

Confirm `sudo apt-get install -y build-essential cmake pkg-config python3 libcurl4-openssl-dev libssl-dev libpq-dev libjansson-dev zlib1g-dev` matches root README L23-25 and Dockerfile build stage. Record yes/no.

### Task 2: Create docs/ONBOARDING.md (full text below, write verbatim)

**Files:**
- Create: `docs/ONBOARDING.md`

- [ ] **Step 1: Write the file with EXACTLY this content**

```markdown
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

完整变量清单见根目录 [.env.example](../.env.example)。常用项：

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
```

- [ ] **Step 2: Confirm the file matches byte-for-byte (no extra blank lines)**

Run: `git status --short docs/ONBOARDING.md`
Expected: `?? docs/ONBOARDING.md`.

### Task 3: Add 3 nav lines + commit

**Files:**
- Modify: `README.md` (docs list, append 1 line)
- Modify: `docs/README.md` (after L4 newcomer pointer, append 1 line)
- Modify: `docs/DEVELOPMENT.md` (top, after `# 开发指南`, append 1 line)

- [ ] **Step 1: Append nav line to root README**

```bash
printf '%s\n' '- [docs/ONBOARDING.md](docs/ONBOARDING.md) — 新人上手包（一站式：构建/配置/首个请求/排障）' >> README.md
tail -2 README.md
```
Expected: new line present at end of docs list.

- [ ] **Step 2: Append nav line to docs/README.md**

```bash
printf '%s\n' '> 上手包：[ONBOARDING.md](ONBOARDING.md)（一站式：构建/配置/首个请求/排障）。' >> docs/README.md
tail -2 docs/README.md
```
Expected: new line present.

- [ ] **Step 3: Append nav line to docs/DEVELOPMENT.md**

```bash
printf '%s\n' '> 新人先看：[ONBOARDING.md](ONBOARDING.md)（一站式上手包）。' >> docs/DEVELOPMENT.md
head -3 docs/DEVELOPMENT.md
```
Expected: new line present under the title.

- [ ] **Step 4: Verify diff is exactly 1 new file + 3 added lines, then commit**

Run: `git status --short; git diff --stat`
Expected: `?? docs/ONBOARDING.md` plus 3 modified files each `1 insertion(+)`, 0 deletions.

```bash
git add docs/ONBOARDING.md README.md docs/README.md docs/DEVELOPMENT.md
git commit -m "docs: add newcomer onboarding pack with nav links"
```

---

## 实施记录（2026-09-29）

- 已落地：commit `9c3894d`（4 文件 +113 行）。
- Task 1 实跑：构建 exit 0，`ctest` 6/6 全绿；`smoke.sh` 为 `[SKIP]`（无本地 PG，属计划预期第二种结果）；依赖行与根 README 一字不差，Dockerfile 构建阶段另有 `ca-certificates` + `libhiredis-dev`（口径差，已知，不阻塞）。
- Task 2：`docs/ONBOARDING.md` 与本计划内嵌全文一致（仅尾部少一个围栏格式空行，符合"无多余空行"）。
- Task 3 偏离（有意保留，未执行 `printf >>`，否则产生重复行）：三处导航措辞简化；`DEVELOPMENT.md` 落点为 L30 构建节后（计划 Step 3 "顶部标题后"与 `>>` 追尾自相矛盾，手工落点更合理）；三文件各恰好 1 处引用，链接均有效。
- 本轮零改动，无新 commit。

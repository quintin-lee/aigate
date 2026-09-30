# 配置详解文档 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增 `docs/CONFIGURATION.md` 全量配置语义，并对齐 `.env.example`、ONBOARDING §2、README 依赖注与导航。

**Architecture:** 单一真相源：新文档逐行以 `src/core/config.c` / `src/core/config.h` / `src/server/transport_civetweb.c:474-492` 实测行为为准；存量文件只做加法引用，不改写现有表述；纯文档变更，无构建影响。

**Tech Stack:** Markdown docs, bash, grep.

## Global Constraints

- 每一处默认值/范围必须与源码一致，不许凭记忆写数字。
- 存量文件只允许追加或 surgical 单行替换，禁止重写段落。
- `DEVELOPMENT.md` 不动。

---

### Task 1: Create docs/CONFIGURATION.md

**Files:**
- Create: `docs/CONFIGURATION.md`
- Test: `grep -c` 变量名计数（见 Step 2）

**Interfaces:**
- Consumes: `src/core/config.c`（`aigate_config_load`）、`src/core/config.h`（字段注释）、`src/server/transport_civetweb.c:474-492`（明文钥匙/锁定/监听解析）
- Produces: 新文档被 Task 3 的 ONBOARDING §2 与导航引用（文件名 `CONFIGURATION.md` 固定，不许改名）

- [ ] **Step 1: Write the file with EXACTLY this content**

```markdown
# 配置详解（Configuration）

> 全量语义，源头为 `src/core/config.c` / `src/core/config.h` /
> `src/server/transport_civetweb.c`。新人先看 [ONBOARDING.md](ONBOARDING.md) 速查表。
>
> 约定：空字符串视同未设置（回默认值）；"启动失败"指进程打日志直接退出。

## 网关变量（共 14 个）

| 变量 | 必填 | 默认 | 越界行为 | 说明 |
|---|---|---|---|---|
| `AIGATE_PG_DSN` | 是 | 无 | 启动失败 | PostgreSQL 连接串；缺失或长度 ≥1024 字节直接报错退出 |
| `AIGATE_ADMIN_TOKEN` | 是 | 无 | 启动失败 | 管理 API 鉴权令牌；进程内只存 sha256 hex |
| `AIGATE_LISTEN` | 否 | `:8080` | — | 网关监听；`:port` 或 `host:port`（如 `127.0.0.1:18080`） |
| `AIGATE_MASTER_KEY` | 否 | 空（禁用） | 启动失败 | 设了就必须恰好 64 位 hex（32 字节），否则启动失败 |
| `AIGATE_UPSTREAM_TIMEOUT_MS` | 否 | `60000` | 启动失败 | 上游超时（毫秒），范围 `(0, 600000]` |
| `AIGATE_USAGE_FLUSH_S` | 否 | `5` | 启动失败 | 用量刷盘周期（秒），范围 `[1, 3600]`；每日配额滚动随刷盘周期推进 |
| `AIGATE_MAX_BODY_BYTES` | 否 | `10485760` | 启动失败 | 请求体上限（字节），范围 `(0, 1GiB]` |
| `AIGATE_METRICS_ACL` | 否 | `127.0.0.1` | — | 逗号分隔的 IPv4 CIDR，允许访问 `/metrics` |
| `AIGATE_REDIS_URL` | 否 | 空（禁用） | — | 如 `redis://127.0.0.1:6379`；为空则禁用 Redis |
| `AIGATE_REDIS_TIMEOUT_MS` | 否 | `100` | 静默回 `100` | 范围 `[1, 60000]`，越界不报错、直接回默认 |
| `AIGATE_REDIS_POOL_SIZE` | 否 | `32` | 静默回 `32` | 范围 `[1, 512]`，越界不报错、直接回默认 |
| `AIGATE_ALLOW_PLAINTEXT_KEYS` | 否 | `0` | — | 仅 `=1` 时允许上游密钥明文落库 |
| `AIGATE_LOCKOUT_MAX_FAILS` | 否 | `10` | 越界保默认 | Admin 连续输错锁定阈值 |
| `AIGATE_LOCKOUT_WINDOW_S` | 否 | `300` | 越界保默认 | 锁定窗口（秒） |

注意：`REDIS_TIMEOUT_MS` / `REDIS_POOL_SIZE` / `LOCKOUT_*` 越界**不报错**，
是最易踩的坑；其余数值越界一律启动失败。

## 归属划分（compose 层 vs 网关）

以下变量不进网关进程，只给 `docker compose` 用，见 `docker-compose.yml` 与
`.env.example`：`POSTGRES_PASSWORD`、`AIGATE_LISTEN_PORT`（宿主机映射端口）等。

`AIGATE_LISTEN` 是网关监听地址，`AIGATE_LISTEN_PORT` 只改宿主机映射，两者别混。

## 溯源

- 加载与校验：`src/core/config.c`（`aigate_config_load`），字段注释见 `src/core/config.h`。
- 明文钥匙 / 锁定策略 / 监听解析：`src/server/transport_civetweb.c:474-492`。
- 改码先改此表。
```

- [ ] **Step 2: Verify all 14 variable names are present**

Run: `grep -c "AIGATE_" docs/CONFIGURATION.md`
Expected: a number ≥ 14 (table rows plus prose mentions).

Run: `for v in AIGATE_PG_DSN AIGATE_ADMIN_TOKEN AIGATE_LISTEN AIGATE_MASTER_KEY AIGATE_UPSTREAM_TIMEOUT_MS AIGATE_USAGE_FLUSH_S AIGATE_MAX_BODY_BYTES AIGATE_METRICS_ACL AIGATE_REDIS_URL AIGATE_REDIS_TIMEOUT_MS AIGATE_REDIS_POOL_SIZE AIGATE_ALLOW_PLAINTEXT_KEYS AIGATE_LOCKOUT_MAX_FAILS AIGATE_LOCKOUT_WINDOW_S; do grep -q "$v" docs/CONFIGURATION.md || echo "MISSING: $v"; done`
Expected: no output (no missing variable).

### Task 2: Complete .env.example

**Files:**
- Modify: `.env.example` (append 4 commented lines at end)
- Test: `grep` presence check (see Step 2)

**Interfaces:**
- Consumes: Task 1 的默认值/范围（`REDIS_TIMEOUT_MS` 默认 100 范围 [1,60000]；`REDIS_POOL_SIZE` 默认 32 范围 [1,512]）
- Produces: 本地直连所需的 `PG_DSN` 示例，供 Task 3 的 ONBOARDING 引用

- [ ] **Step 1: Append the 4 commented lines**

```bash
printf '%s\n' '# AIGATE_PG_DSN=postgresql://postgres:postgres@127.0.0.1:5432/aigate_test' '#   Local-run DSN (required by the gateway; compose path uses POSTGRES_PASSWORD instead).' '# AIGATE_REDIS_URL=redis://127.0.0.1:6379' '#   Empty/disabled by default; comment out to disable.' '# AIGATE_REDIS_TIMEOUT_MS=100' '#   Range [1,60000]; out-of-range silently falls back to 100.' '# AIGATE_REDIS_POOL_SIZE=32' '#   Range [1,512]; out-of-range silently falls back to 32.' >> .env.example
tail -8 .env.example
```
Expected: the 8 lines above printed at end of file.

- [ ] **Step 2: Verify no existing line was touched**

Run: `git diff --stat .env.example`
Expected: `1 file changed, 8 insertions(+)` (8 insertions, 0 deletions).

### Task 3: Rewire references + nav + commit

**Files:**
- Modify: `docs/ONBOARDING.md` (§2 header area, 2-line change)
- Modify: `README.md` (append 1 dep-note line after the dep code block, append 1 docs-list line)
- Modify: `docs/README.md` (append 1 nav line)
- Test: link reachability via `test -f` (see Step 4)

**Interfaces:**
- Consumes: Task 1 的文件名 `docs/CONFIGURATION.md`; Task 2 的 `.env.example` 行
- Produces: nothing downstream (terminal task)

- [ ] **Step 1: Fix ONBOARDING §2 claim (2-line change, hand edit)**

In `docs/ONBOARDING.md`, replace:
`完整变量清单见根目录 [.env.example](../.env.example)。常用项：`
with:
`常用项速查（全量语义见 [CONFIGURATION.md](CONFIGURATION.md)，源头为 \`src/core/config.c\`）：`

- [ ] **Step 2: Add dep-note line to root README (append after dep code block)**

In `README.md`, directly after the closing fence of the `sudo apt-get install` block (the line `` ``` `` following `libcurl4-openssl-dev libssl-dev libpq-dev libjansson-dev zlib1g-dev`), insert:
`注：与 Dockerfile 构建阶段差 \`ca-certificates\`/\`libhiredis-dev\`（系统路径；按此行安装走 FetchContent 兜底，同样可编）。`

- [ ] **Step 3: Append nav lines**

```bash
printf '%s\n' '- [docs/CONFIGURATION.md](docs/CONFIGURATION.md) — 全量配置语义（14 个网关变量/越界行为/归属划分）' >> README.md
printf '%s\n' '> 配置详解：[CONFIGURATION.md](CONFIGURATION.md)（全量语义，以源码为准）。' >> docs/README.md
tail -1 README.md; tail -1 docs/README.md
```
Expected: the two new lines printed.

- [ ] **Step 4: Verify diff is exactly 1 new file + 4 touched files, links resolve, then commit**

Run: `git status --short; git diff --stat`
Expected: `?? docs/CONFIGURATION.md`, plus `.env.example` (8 insertions, 0 deletions), `docs/ONBOARDING.md` / `README.md` / `docs/README.md` each small insertions with 0 or 1 deletions (ONBOARDING Step 1 is 1 insertion + 1 deletion).

Run: `test -f docs/CONFIGURATION.md && echo LINKS-OK`
Expected: `LINKS-OK`.

```bash
git add docs/CONFIGURATION.md .env.example docs/ONBOARDING.md README.md docs/README.md
git commit -m "docs: add CONFIGURATION reference and complete env example"
```

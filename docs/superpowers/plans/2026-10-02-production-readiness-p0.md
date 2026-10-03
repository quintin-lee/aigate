# aigate 生产可用性 (Production-Readiness) P0 改造实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 针对真实高并发生产环境（长流式并发生成、反向代理/Ingress 调度、K8s 容器生命周期、数据库与缓存高可用容灾），消除 5 大生产阻断缺陷（P0），实现具备企业级吞吐、无辜零断流、故障平滑降级与云原生标准的生产就绪网关。

**Architecture:**
1. **网络传输与高并发线程池 (`src/core/config.*`, `src/server/transport_civetweb.*`)**: 解除 16 线程硬编码，支持 `AIGATE_WORKER_THREADS` (默认 64, 范围 4~4096) 与 `AIGATE_REQUEST_TIMEOUT_MS`；
2. **反向代理真实客户端 IP 解析 (`src/server/transport_civetweb.*`)**: 支持 `AIGATE_TRUSTED_PROXIES`，基于受信任代理链严格解析 `X-Forwarded-For` / `X-Real-IP`，消除 Admin 锁留误杀全网；
3. **云原生健康探针与平滑排空 (`src/server/transport_civetweb.*`, `src/main.c`)**: 挂载轻量非鉴权 `/healthz` (Liveness) 与 `/ready` (Readiness)，实现 `SIGTERM` 捕获后 15s 宽限期 Draining 平滑切断流量；
4. **数据库读写分离与迁移咨询锁 (`src/store/pg_store.*`)**: 拆分专用 Flush 批刷连接与读/鉴权连接池，消除 Head-of-Line 锁阻塞；增加 `pg_advisory_lock` 杜绝多副本并发迁移冲突；
5. **Redis 故障平滑降级 (`src/core/config.*`, `src/policy/ratelimit.*`, `src/main.c`)**: 引入 `AIGATE_REDIS_FAIL_OPEN` (默认 1)，当 Redis 闪断或不可用时自动降级回退至本地内存桶限流，防止全网 429 雪崩。

**Tech Stack:** 纯 C17 标准, GCC/Clang, CMake, CivetWeb, libpq (PostgreSQL), hiredis (Redis), POSIX Threads/Signals.

---

### Task 1: CivetWeb 线程池可配化与高并发参数暴露

**Files:**
- Modify: `src/core/config.h`
- Modify: `src/core/config.c`
- Modify: `src/server/transport_civetweb.c`
- Modify: `tests/unit/core/test_config.c`
- Modify: `docs/CONFIGURATION.md`

- [x] **Step 1: 在 `config.h` 与 `config.c` 中增加 `worker_threads` 与 `request_timeout_ms` 字段与校验**
  - 在 `aigate_config` 结构体中添加:
    ```c
    int worker_threads;     /**< AIGATE_WORKER_THREADS, default 64, range [4, 4096] */
    int request_timeout_ms; /**< AIGATE_REQUEST_TIMEOUT_MS, default 300000, range [1000, 3600000] */
    ```
  - 在 `aigate_config_load()` 中解析 `AIGATE_WORKER_THREADS`：默认 64；若越界直接报错返回 -1；
  - 解析 `AIGATE_REQUEST_TIMEOUT_MS`：默认 300000 (5分钟)；若越界直接报错返回 -1。

- [x] **Step 2: 编写并运行 Task 1 单元测试**
  - 在 `tests/unit/core/test_config.c` 中增加 `test_config_worker_threads()` 用例；
  - 测试默认值、合法自定义值（如 256、1024）以及越界值（如 2、10000）的校验行为。

- [x] **Step 3: 将配置传入 `transport_civetweb_start` 并动态设置 CivetWeb 选项**
  - 修改 `transport_civetweb_start()` 签名或传入配置，动态构建 CivetWeb options：
    ```c
    char threads_str[16];
    char timeout_str[16];
    snprintf(threads_str, sizeof(threads_str), "%d", worker_threads);
    snprintf(timeout_str, sizeof(timeout_str), "%d", request_timeout_ms);
    const char* options[] = {
        "listening_ports", port_spec,
        "num_threads", threads_str,
        "request_timeout_ms", timeout_str,
        NULL
    };
    ```
  - 同步更新 `src/main.c` 中的调用点。

- [x] **Step 4: 编译、运行 CTest 确保基线全绿**
  - 运行 `ctest --test-dir build` 确保通过。

---

### Task 2: 反向代理真实客户端 IP 解析 (`X-Forwarded-For` 与信任代理白名单)

**Files:**
- Modify: `src/core/config.h`
- Modify: `src/core/config.c`
- Modify: `src/server/transport_civetweb.h`
- Modify: `src/server/transport_civetweb.c`
- Modify: `tests/unit/core/test_config.c`
- Modify: `tests/unit/run_tests.c`
- Create/Modify: `tests/unit/server/test_admin_api.c` 或专用测试文件

- [x] **Step 1: 在 `config.h` 与 `config.c` 中增加 `trusted_proxies` 配置**
  - `char trusted_proxies[256];`，默认 `"127.0.0.1"`。
  - 读取环境变量 `AIGATE_TRUSTED_PROXIES`。

- [x] **Step 2: 在 `transport_civetweb.c` 中实现安全 Client IP 提取器**
  - 实现函数 `const char* extract_client_ip(struct mg_connection* conn, const char* remote_addr, const char* trusted_proxies, char* out_buf, size_t out_cap)`；
  - 复用 `metrics_acl_allows(remote_addr, trusted_proxies)`：
    - 若 `remote_addr` 不在白名单内：认定为直连客户端，直接返回 `remote_addr`（绝不信任 XFF）；
    - 若在白名单内：获取 `X-Forwarded-For` 头，按逗号切分，从右向左寻找首个非白名单 IP。若全为内网或解析失败，取首段 IP。若无 XFF 则检查 `X-Real-IP`；均无则回退 `remote_addr`。

- [x] **Step 3: 替换数据面与管理面 IP 读取点**
  - `handle_v1`: `rq.client_ip = extract_client_ip(...)`；
  - `handle_admin`: `admin_dispatch(..., extract_client_ip(...), ...)`；
  - 确保 Admin 登录锁留表记录的是攻击者真实 IP，而非反向代理统一内网 IP。

- [x] **Step 4: 编写并运行 Client IP 解析的单元测试**
  - 覆盖直连冒充 XFF 攻击场景（不被信任）、正常代理多级跳转场景、X-Real-IP 场景。

---

### Task 3: 云原生健康探针 `/healthz`、`/ready` 与平滑排空 (Graceful Draining)

**Files:**
- Modify: `src/server/transport_civetweb.h`
- Modify: `src/server/transport_civetweb.c`
- Modify: `src/main.c`
- Modify: `src/core/config.h`
- Modify: `src/core/config.c`
- Modify: `tests/unit/run_tests.c`

- [x] **Step 1: 在 `config` 中增加 `drain_timeout_s` 配置**
  - `int drain_timeout_s;`，默认 `15`，合法范围 `[0, 120]`。
  - 读取环境变量 `AIGATE_DRAIN_TIMEOUT_S`。

- [x] **Step 2: 在 `transport_civetweb.c` 中挂载 `/healthz` 与 `/ready` 路由**
  - 增加标志 `volatile sig_atomic_t g_draining;` 及对外设置函数 `transport_civetweb_set_draining(int draining)`；
  - 实现 `handle_healthz`: 恒定返回 `200 OK` + `{"status":"ok"}`；
  - 实现 `handle_ready`:
    - 若 `g_draining` 为 1：返回 `503 Service Unavailable` + `{"status":"draining"}`；
    - 否则检查底层存储（可探查 PG 连接与 Redis 状态），健康返回 `200 OK`，故障返回 `503 Service Unavailable`。
  - 挂载路由：
    ```c
    mg_set_request_handler(cw->ctx, "/healthz", handle_healthz, cw);
    mg_set_request_handler(cw->ctx, "/ready", handle_ready, cw);
    ```

- [x] **Step 3: 改造 `src/main.c` 的信号处理与退出生命周期**
  - 引入 `g_draining` 状态；
  - `sig_handler` 将 `g_draining = 1`；
  - 主循环检测到 `g_draining` 时：
    - 调用 `transport_civetweb_set_draining(1)`；
    - 记录日志: `AIGATE_LOG_INFO("received exit signal, draining traffic for %d seconds...", cfg.drain_timeout_s);`；
    - 等待 `cfg.drain_timeout_s` 秒（让已有流式请求传输完毕，同时 K8s 摘除 Pod）；
    - 然后退出循环，执行 `transport_civetweb_stop` 与资源安全释放。

- [x] **Step 4: 编写并验证健康探测与 Draining 测试用例**
  - 验证 `/healthz` 响应、`/ready` 响应及 Draining 状态转换。

---

### Task 4: PostgreSQL 读写分离专用连接与迁移 Advisory Lock

**Files:**
- Modify: `src/store/pg_store.h`
- Modify: `src/store/pg_store.c`
- Modify: `tests/unit/store/test_pg_store.c`

- [x] **Step 1: 升级 `pq_ctx` 支持 Dedicated Flush 连接与 Read 连接**
  - 结构定义扩展：
    ```c
    struct pq_ctx {
        PGconn*         db_read;   /**< 读与管理操作连接 */
        pthread_mutex_t mtx_read;  /**< 读操作锁 */
        PGconn*         db_flush;  /**< 专用 Flush 批刷连接 */
        pthread_mutex_t mtx_flush; /**< Flush 专用锁 */
        char            dsn[1024];
    };
    ```
  - `pg_store_open` 时建立两个独立连接 `db_read` 与 `db_flush`。

- [x] **Step 2: 分流执行点，解除锁竞争**
  - `pq_flush_usage_daily` 与 `pq_flush_usage_requests` 改用 `db_flush` 和 `mtx_flush`；
  - `pq_get_key_by_hash`, `pq_list_models`, `admin_*` 继续使用 `db_read` 和 `mtx_read`；
  - 彻底使后台 5 秒批刷的重型网络写入与前台高频 API Key 鉴权互不干扰。

- [x] **Step 3: 在 `pg_store_migrate` 中增加 PostgreSQL Advisory Lock**
  - 在迁移 SQL 执行前调用 `SELECT pg_advisory_lock(7192847291);`；
  - 执行结束后在 finally 分支调用 `SELECT pg_advisory_unlock(7192847291);`；
  - 防止 Kubernetes 多实例并发扩容拉起时发生 DDL 竞态冲突。

- [x] **Step 4: 运行所有 PG 单元测试与真实库回归**
  - 验证 `test_pg_fake_*` 与真实库测试，确保读写分流无死锁、无泄漏。

---

### Task 5: Redis 宕机故障平滑降级 (Fail-Open / In-Memory Fallback)

**Files:**
- Modify: `src/core/config.h`
- Modify: `src/core/config.c`
- Modify: `src/policy/ratelimit.h`
- Modify: `src/policy/ratelimit.c`
- Modify: `src/main.c`
- Modify: `tests/unit/policy/test_ratelimit.c`

- [x] **Step 1: 在 `config` 中增加 `redis_fail_open` 配置**
  - `int redis_fail_open;`，默认 `1`；
  - 读取环境变量 `AIGATE_REDIS_FAIL_OPEN`。

- [x] **Step 2: 在 `ratelimit` 中增加降级逻辑**
  - 增加 `ratelimit_set_fail_open(ratelimit_t* rl, int fail_open);`；
  - 在 `ratelimit_allow_request()` 中：
    - 若 `rl->pool != NULL`，当 `redis_pool_acquire` 返回 NULL 或 EVAL 失败时：
      - 若 `fail_open == 1`：记录限频降级日志，并回退到单机内存桶限流逻辑 `ratelimit_allow_request_local(...)`；
      - 若 `fail_open == 0`：保持原有的严格拒绝（返回 -1）。
  - 同理在 `circuit_breaker.c` 中，当 Redis 故障时平滑回退到本地状态机。

- [x] **Step 3: 编写并验证 Redis 降级测试用例**
  - 在 `test_ratelimit.c` 中增加 `test_rl_redis_fail_open()` 用例；
  - 验证当 Redis 故意配置错误端口或断开时，请求能平稳降级为放行与单机控速，不再触发 429。

---

### Task 6: 文档同步与全量集成验证

**Files:**
- Modify: `docs/CONFIGURATION.md`
- Modify: `docker-compose.yml`
- Modify: `.env.example`

- [x] **Step 1: 更新 `docs/CONFIGURATION.md`**
  - 登记新变量：`AIGATE_WORKER_THREADS`, `AIGATE_REQUEST_TIMEOUT_MS`, `AIGATE_TRUSTED_PROXIES`, `AIGATE_DRAIN_TIMEOUT_S`, `AIGATE_REDIS_FAIL_OPEN`；
  - 明确默认值、取值范围与越界行为。

- [x] **Step 2: 更新 `docker-compose.yml`**
  - 为 `aigate` 服务加上真实的 HTTP 健康检查：
    ```yaml
    healthcheck:
      test: ["CMD-SHELL", "curl -f http://localhost:8080/healthz || exit 1"]
      interval: 5s
      timeout: 3s
      retries: 5
    ```

- [x] **Step 3: 运行全量测试套件**
  - 执行 `ctest --test-dir build --output-on-failure`（确保 244+ 项单元测试全绿）；
  - 执行 `./scripts/run_chaos_asan.sh`（确保 ASan/UBSan 零泄漏、零越界）；
  - 运行集成冒烟测试 `TEST_PG_DSN=... ./tests/integration/smoke.sh`。

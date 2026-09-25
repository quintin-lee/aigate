# Redis 分布式共享状态与多节点高可用实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 aigate 引入基于官方 `hiredis` 的轻量连接池与预加载原子 Lua 脚本体系，将 QPS 令牌桶、每日 Token 配额、管理面 IP 锁留及通道熔断器三态接入 Redis，实现多实例部署下的全局严格一致性（Fail-Closed）与防超发协同。

**Architecture:** 采用分层门面模式（Facade），若 `AIGATE_REDIS_URL` 未配置，网关 100% 保持既有单机内存高性能模型；若配置了 Redis，则自动初始化复用连接池（`redis_pool_t`）与 SHA1 预热 Lua 脚本，热路径通过单次网络往返（RTT）原子判定限流与熔断状态，并在 Redis 超时（默认 100ms）或网络中断时严格返回 HTTP 503 阻断流量。

**Tech Stack:** C17, CMake (FetchContent), hiredis (v1.2.0), Redis 7 (Alpine), CivetWeb, Python 3 / Pytest, Docker Compose.

---

### File Structure Map

- **New Files**:
  - `src/redis_client.h` & `src/redis_client.c`: hiredis 基础包装、超时配置、RESP 响应解析、`EVALSHA` 脚本加载与自动容错重试。
  - `src/redis_pool.h` & `src/redis_pool.c`: 定长线程安全 `redisContext*` 复用连接池，具备借出、归还与连接健康探测。
  - `src/redis_scripts.h`: 内嵌 4 个核心原子 Lua 脚本字符串常量与校验标识。
  - `tests/unit/test_redis_pool.c`: 连接池借还、并发与超时单测。
  - `tests/unit/test_redis_clustering.c`: QPS、配额与熔断器 Lua 脚本及 Fail-Closed 行为单测。
  - `tests/integration/test_redis_clustering.py`: 基于 Docker Compose 的双 aigate 实例多节点 E2E 一致性与容灾测试。

- **Modified Files**:
  - `CMakeLists.txt`: 引入 `hiredis` 静态库依赖及单元测试源文件编译。
  - `docker-compose.yml`: 新增 `redis:7-alpine` 服务容器，配置第二个 aigate 实例 (`aigate-2` on port 8081) 形成多节点测试拓扑。
  - `src/config.h` & `src/config.c`: 解析 `AIGATE_REDIS_URL`、`AIGATE_REDIS_TIMEOUT_MS`、`AIGATE_REDIS_POOL_SIZE`。
  - `src/ratelimit.h` & `src/ratelimit.c`: 适配门面，支持注入 Redis 连接池或回退至既有本地内存表。
  - `src/circuit_breaker.h` & `src/circuit_breaker.c`: 适配门面，支持集群共享三态流转。
  - `src/admin_api.c`: IP 锁留适配 Redis 共享。
  - `src/aigate_core.c`: 在请求准入、限流、配额结算阶段对接 Fail-Closed 判定。

---

### Task 1: CMake 构建系统集成 hiredis 与基础配置解析

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/config.h`
- Modify: `src/config.c`
- Test: `tests/unit/test_config.c` (或 `test_admin_api.c`)

- [ ] **Step 1: 在 CMakeLists.txt 中引入 hiredis 依赖**

在 `CMakeLists.txt` 中添加 `FetchContent` 支持 hiredis：
```cmake
include(FetchContent)
FetchContent_Declare(
    hiredis
    GIT_REPOSITORY https://github.com/redis/hiredis.git
    GIT_TAG        v1.2.0
)
set(DISABLE_TESTS ON CACHE BOOL "Disable hiredis tests" FORCE)
set(ENABLE_SSL OFF CACHE BOOL "Disable hiredis SSL" FORCE)
FetchContent_MakeAvailable(hiredis)

target_link_libraries(libaigate PUBLIC hiredis::hiredis)
```

- [ ] **Step 2: 在 config.h / config.c 中扩展 Redis 配置**

在 `src/config.h` 中追加：
```c
extern char g_redis_url[512];
extern int  g_redis_timeout_ms;
extern int  g_redis_pool_size;
```
并在 `src/config.c` 中从环境变量 `AIGATE_REDIS_URL`（默认 `""`）、`AIGATE_REDIS_TIMEOUT_MS`（默认 `100`）、`AIGATE_REDIS_POOL_SIZE`（默认 `32`）解析并填充。

- [ ] **Step 3: 运行 CMake 编译验证**

Run: `cmake -B .build && cmake --build .build -j`
Expected: 编译成功生成 `aigate` 与 `liblibaigate.a`，零警告零报错。

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt src/config.h src/config.c
git commit -m "build(deps): 📦 integrate hiredis via FetchContent and add redis config envs"
```

---

### Task 2: Redis 客户端包装与复用连接池 (redis_client & redis_pool)

**Files:**
- Create: `src/redis_client.h`
- Create: `src/redis_client.c`
- Create: `src/redis_pool.h`
- Create: `src/redis_pool.c`
- Create: `tests/unit/test_redis_pool.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写连接池与客户端基础头文件**

`src/redis_client.h` 定义：
```c
#ifndef AIGATE_REDIS_CLIENT_H
#define AIGATE_REDIS_CLIENT_H

#include <hiredis/hiredis.h>

redisContext* redis_connect_url(const char* url, int timeout_ms);
redisReply* redis_eval_sha(redisContext* c, const char* sha, const char* script_fallback,
                           int numkeys, const char** keys, const char** argv);

#endif
```

`src/redis_pool.h` 定义：
```c
#ifndef AIGATE_REDIS_POOL_H
#define AIGATE_REDIS_POOL_H

#include "redis_client.h"

typedef struct redis_pool redis_pool_t;

redis_pool_t* redis_pool_create(const char* url, int pool_size, int timeout_ms);
void redis_pool_destroy(redis_pool_t* pool);
redisContext* redis_pool_acquire(redis_pool_t* pool);
void redis_pool_release(redis_pool_t* pool, redisContext* c);

#endif
```

- [ ] **Step 2: 编写连接池单元测试 (test_redis_pool.c)**

在 `tests/unit/test_redis_pool.c` 中测试：
- 创建容量为 4 的连接池。
- 在无可用 Redis 时（探测 `127.0.0.1:6379` 离线），`redis_pool_create` 优雅返回 NULL 或 acquire 返回 NULL，不发生段错误。
- 若本地存在 Redis，验证多次 `acquire` 与 `release` 正常工作。

- [ ] **Step 3: 运行单元测试**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% 测试通过。

- [ ] **Step 4: Commit**

```bash
git add src/redis_client.h src/redis_client.c src/redis_pool.h src/redis_pool.c tests/unit/test_redis_pool.c tests/unit/run_tests.c
git commit -m "feat(redis): ✨ add hiredis client wrapper and thread-safe connection pool"
```

---

### Task 3: 内嵌原子 Lua 脚本与分布式限流配额实现

**Files:**
- Create: `src/redis_scripts.h`
- Modify: `src/ratelimit.h`
- Modify: `src/ratelimit.c`
- Modify: `tests/unit/test_ratelimit.c`

- [ ] **Step 1: 定义 4 个核心 Lua 脚本与 SHA1 常量**

在 `src/redis_scripts.h` 中提供：
- `SCRIPT_QPS_TOKEN_BUCKET`: 惰性补币令牌桶 Lua 字符串
- `SCRIPT_DAILY_QUOTA_CONSUME`: 每日配额 `INCRBY` 与上限比对 Lua 字符串
- `SCRIPT_ADMIN_LOCKOUT`: IP 失败计数与窗口 TTL Lua 字符串
- `SCRIPT_CIRCUIT_BREAKER_SYNC`: 熔断器状态流转 Lua 字符串

- [ ] **Step 2: 改造 ratelimit.c 支持注入 redis_pool**

在 `src/ratelimit.h` 中增加配置注入：
```c
void ratelimit_set_redis_pool(ratelimit_t* rl, redis_pool_t* pool);
```
在 `rl_allow_request` 中：
- 若 `rl->pool != NULL`，通过连接池借出连接，调用 `SCRIPT_QPS_TOKEN_BUCKET` 脚本。
- 若 Redis 调用失败，**触发 Fail-Closed**，返回 `-1`，并设置 `retry_ms = -1`。
- 若 `rl->pool == NULL`，维持原有单机内存令牌桶逻辑不变。

在 `rl_remaining_daily` 与 `rl_reserve_tokens` 中：
- 若 `rl->pool != NULL`，执行 Redis 配额查询与累加脚本。
- 若返回超额或网络失败，返回 `-1`。

- [ ] **Step 3: 运行单元测试验证双引擎**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 原有测试不受影响，全部通过。

- [ ] **Step 4: Commit**

```bash
git add src/redis_scripts.h src/ratelimit.h src/ratelimit.c tests/unit/test_ratelimit.c
git commit -m "feat(ratelimit): ✨ add distributed QPS and quota support via atomic Lua scripts"
```

---

### Task 4: 集群熔断器与管理防刷锁留分布式同步

**Files:**
- Modify: `src/circuit_breaker.h`
- Modify: `src/circuit_breaker.c`
- Modify: `src/admin_api.c`
- Modify: `tests/unit/test_circuit_breaker.c`
- Modify: `tests/unit/test_admin_api.c`

- [ ] **Step 1: 改造 circuit_breaker.c 支持集群协同**

在 `src/circuit_breaker.h` 中增加：
```c
void cb_set_redis_pool(circuit_breaker_t* cb, redis_pool_t* pool);
```
在 `cb_allow_request`、`cb_record_success`、`cb_record_failure` 中：
- 若注入了 Redis 连接池，调用 `SCRIPT_CIRCUIT_BREAKER_SYNC` 脚本，在 Redis 端原子协同 `state`、`fails`、`open_until_ms`。
- 若 Redis 失败，Fail-Closed 保守判定：当前节点临时拒绝放行或返回错误。

- [ ] **Step 2: 改造 admin_api.c 的 IP 锁留支持 Redis**

在 `lockout_hit` 与 `lockout_fail` 中：
- 若 `g_redis_pool != NULL`，执行 `SCRIPT_ADMIN_LOCKOUT`。
- 无论攻击者向哪个 aigate 实例发请求，失败计数均聚合至 `aigate:lockout:{ip}`，跨实例全局生效。

- [ ] **Step 3: 运行单元测试**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 100% 通过。

- [ ] **Step 4: Commit**

```bash
git add src/circuit_breaker.h src/circuit_breaker.c src/admin_api.c tests/unit/test_circuit_breaker.c tests/unit/test_admin_api.c
git commit -m "feat(resilience): ✨ enable distributed circuit breaker and admin lockout with Redis"
```

---

### Task 5: aigate_core 热路径组装与 Fail-Closed 错误码规范化

**Files:**
- Modify: `src/aigate_core.c`
- Modify: `src/main.c`

- [ ] **Step 1: 在 aigate 初始化时挂接全局 Redis 连接池**

在 `src/main.c` 中：
- 若 `g_redis_url[0] != '\0'`，调用 `redis_pool_create(g_redis_url, g_redis_pool_size, g_redis_timeout_ms)`。
- 初始化成功后，分别向 `ratelimit`、`circuit_breaker` 注入连接池。
- 在服务平响停机（`SIGINT`/`SIGTERM`）时安全销毁连接池。

- [ ] **Step 2: 在 aigate_handle_request 中规范 Fail-Closed 响应**

当 `rl_allow_request` 或 `rl_remaining_daily` 返回因 Redis 故障导致的不可用时：
- 返回 HTTP 503 Service Unavailable：
```json
{
  "error": {
    "message": "Distributed rate limit service temporarily unavailable",
    "type": "server_error",
    "code": "distributed_state_unavailable"
  }
}
```
记录明确的 `LOG_ERROR("[redis] fail-closed triggered for key_id=%ld", key_id)`。

- [ ] **Step 3: 编译与本地单机测试**

Run: `cmake --build .build -j && ctest --test-dir .build --output-on-failure`
Expected: 现有单机模式依然全部通过。

- [ ] **Step 4: Commit**

```bash
git add src/aigate_core.c src/main.c
git commit -m "feat(core): ⚡ wire distributed state into request pipeline with strict fail-closed handling"
```

---

### Task 6: 多实例 Docker 集群编排与 Pytest E2E 集成测试

**Files:**
- Modify: `docker-compose.yml`
- Create: `tests/integration/test_redis_clustering.py`

- [ ] **Step 1: 在 docker-compose.yml 中增加 redis 与双 aigate 实例服务**

编排：
- `redis`: image `redis:7-alpine`, 端口 `6379`
- `aigate`: 端口 `8080`, 环境变量 `AIGATE_REDIS_URL: "redis://redis:6379/0"`
- `aigate-peer`: 端口 `8081`, 环境变量 `AIGATE_REDIS_URL: "redis://redis:6379/0"`, 与 `aigate` 共享同一个 Postgres 和同一个 Redis。

- [ ] **Step 2: 编写多节点端到端集成测试 (test_redis_clustering.py)**

编写 4 个场景用例：
1. `test_distributed_qps_anti_oversell`: 并发 20 个请求分发至 8080 和 8081，断言 QPS=10 限制下两台机器累计仅放行 10 个，其余均收到 429。
2. `test_distributed_token_quota_sharing`: 8080 实例消耗 800 Tokens，8081 实例即时感知并阻断超出 1000 额度的第二次请求。
3. `test_distributed_circuit_breaker_sync`: 连续打爆 8080 上某模型，断言 8081 无需任何额外失败直接进入熔断并 Failover。
4. `test_fail_closed_on_redis_down`: 暂停 Redis 容器，断言网关立即返回 503 阻断，恢复 Redis 容器后流量恢复。

- [ ] **Step 3: 运行集成测试**

Run: `docker compose up -d --build && pytest tests/integration/test_redis_clustering.py`
Expected: 4/4 测试用例全部 Passed。

- [ ] **Step 4: Commit**

```bash
git add docker-compose.yml tests/integration/test_redis_clustering.py
git commit -m "test(integration): 🧪 add multi-node redis clustering e2e tests for fail-closed, qps, quota, and cb"
```

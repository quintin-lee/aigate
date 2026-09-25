# aigate Redis 分布式共享状态与多节点高可用设计规范

- **日期**：2026-09-25
- **状态**：Approved (Brainstorming 完成)
- **目标**：将 aigate 核心状态（QPS 限流、每日 Token 配额、管理面 IP 锁留、上游熔断器）接入 Redis，支持多实例水平扩展并严格保证 Fail-Closed 一致性。

---

## §1 背景与核心目标

### 1.1 背景
当前 aigate 采用纯内存模型维护运行时状态（`ratelimit.c`、`circuit_breaker.c`、`admin_api.c` 内的锁留槽），所有计数器与状态机局限在单进程内部。在生产环境通过容器水平扩展（部署 2 个或更多 aigate 实例承载高并发）时，会产生以下集群一致性问题：
1. **配额超发**：每个实例各自维护一套限流桶与配额计数器，导致实际全局放行 QPS 与日 Token 配额成倍（$N \times$）超发。
2. **安全穿透**：攻击者对不同实例发起轮询密码爆破，单机 IP 失败锁留无法聚合，削弱防暴力破解能力。
3. **熔断滞后**：某上游通道故障时，各实例必须分别经历 5 次失败才能各自熔断，给故障上游带来不必要的雪崩压力。

### 1.2 核心目标与非目标
- **核心目标**：
  1. **全量集群状态共享**：将 QPS 令牌桶、每日 Token 配额、Admin IP 防刷锁留、上游目标熔断器三态接入 Redis。
  2. **严格一致性 Fail-Closed**：当配置了 Redis 时，若 Redis 发生故障或网络超时（默认 100ms），绝不允许绕过限流或配额超发，统一返回 HTTP 503/429 阻断。
  3. **100% 零配置向后兼容**：当未配置 `AIGATE_REDIS_URL` 时，网关维持纯本地内存运行，单机开发与无 Redis 场景零额外依赖与零网络开销。
  4. **原子操作与高吞吐**：使用官方轻量 `hiredis` 客户端，基于连接池与预编译 Lua 脚本（`EVALSHA`），热路径单次网络 RTT 完成复杂状态跃迁。
- **非目标**：
  - 不做 Redis 集群分布式事务锁（Lua 脚本单线程原子性已足够）。
  - 不做基于 Pub/Sub 的最终一致性同步（与 Fail-Closed 强一致性要求冲突）。

---

## §2 总体架构与模块划分

```
                     ┌──────────────────────────────────────────────┐
                     │    aigate_core / admin_api / model_router    │
                     └──────────────────────┬───────────────────────┘
                                            │
                  ┌─────────────────────────┴─────────────────────────┐
                  ▼                                                   ▼
      ┌───────────────────────┐                           ┌───────────────────────┐
      │  ratelimit / cb 门面   │                           │    admin_lockout 门面  │
      └───────────┬───────────┘                           └───────────┬───────────┘
                  │                                                   │
                  ├─────────────────────────┐                         │
                  ▼                         ▼                         ▼
       ┌───────────────────────┐ ┌───────────────────────────────────────────────┐
       │   inmemory_backend    │ │                redis_backend                  │
       │ (单机内存，无 Redis 时) │ │      (分布式共享状态，AIGATE_REDIS_URL 配置时)   │
       └───────────────────────┘ └──────────────────────┬────────────────────────┘
                                                        ▼
                                 ┌───────────────────────────────────────────────┐
                                 │   src/redis_pool.h / src/redis_client.h       │
                                 │ (hiredis 连接池 + 连接健康检查 + 预热 Lua 脚本) │
                                 └───────────────────────────────────────────────┘
```

### 2.1 模块职责
1. **`src/redis_client.h / src/redis_client.c`**：
   - 封装 `hiredis` 同步 API，统一处理连接建立、认证、超时设定与 RESP 解析。
   - 维护 4 个核心 Lua 脚本的文本与 SHA1 校验和，提供 `redis_eval_sha_retry` 函数，在遇到 `NOSCRIPT`（如 Redis 重启清除缓存）时自动重载并重试。
2. **`src/redis_pool.h / src/redis_pool.c`**：
   - 提供连接池抽象 `redis_pool_t`，维护定长复用连接数组，使用轻量互斥锁保护。
   - `redis_pool_acquire`：借出健康连接，若连接失效自动调用 `redisReconnect`；若连接耗尽支持有限等待。
   - `redis_pool_release`：归还连接至池中，供其他工作线程复用。
3. **门面集成与分支调度**：
   - [`src/ratelimit.h`](file:///home/quintin/Data/source/c_cpp/aigate/src/ratelimit.h)：保持原函数签名不变，内部若持有效 `redis_pool_t*` 则调用 Redis Lua 脚本，否则走既有哈希表。
   - [`src/circuit_breaker.h`](file:///home/quintin/Data/source/c_cpp/aigate/src/circuit_breaker.h)：熔断准入与状态变更根据 Redis 配置决定查询本地还是 Redis。
   - [`src/admin_api.c`](file:///home/quintin/Data/source/c_cpp/aigate/src/admin_api.c)：锁留逻辑注入 Redis 检查。

---

## §3 配置与构建系统集成

### 3.1 环境变量配置 ([`src/config.h`](file:///home/quintin/Data/source/c_cpp/aigate/src/config.h))

| 环境变量 | 默认值 | 描述 |
|---|---|---|
| `AIGATE_REDIS_URL` | `""` | Redis 连接串（支持 `redis://[:password@]host:port[/db]`）。为空时禁用 Redis，启用单机内存模式。 |
| `AIGATE_REDIS_TIMEOUT_MS` | `100` | 网络连接与命令执行的超时时间（毫秒）。超过该阈值触发 Fail-Closed。 |
| `AIGATE_REDIS_POOL_SIZE` | `32` | 每个 aigate 进程维护的 Redis 连接池容量（匹配 CivetWeb 工作线程）。 |

### 3.2 依赖管理与 CMake 集成
- 在 `CMakeLists.txt` 中使用 `FetchContent` 引入 `hiredis`（推荐 tag `v1.2.0`），或优先使用系统 `find_package(hiredis QUIET)`。
- 生成静态库并链接进 `libaigate`，保证输出二进制独立无外部动态库绑定负担。
- 在 `docker-compose.yml` 中编排 `redis:7-alpine` 容器，注入 `AIGATE_REDIS_URL: "redis://redis:6379/0"`。

---

## §4 状态键设计与原子 Lua 脚本契约

统一前缀：`aigate:`

### 4.1 键命名规范
- **QPS 令牌桶**：`aigate:rl:qps:{key_id}`（Hash: `tokens`, `last_ms`；TTL: 3 秒）
- **每日 Token 配额**：`aigate:quota:{key_id}:{day_epoch}`（String 整型计数器；TTL: 48 小时）
  - `day_epoch` 为当前 UTC 0 点对应的整秒数：`now - (now % 86400)`。跨天自动生成新键，无需跨实例翻日时钟同步广播。
- **管理防刷锁留**：`aigate:lockout:{client_ip}`（String 整型计数器；TTL: `g_lockout_window_s` 秒，默认 60 秒）
- **通道熔断状态**：`aigate:cb:{endpoint_hash}`（Hash: `state`, `fails`, `open_until_ms`, `probes`；TTL: 86400 秒）

### 4.2 核心 Lua 脚本契约

#### 脚本 1：QPS 令牌桶原子判定 (`qps_token_bucket.lua`)
- **KEYS[1]**：`aigate:rl:qps:{key_id}`
- **ARGV**：`[1] now_ms`, `[2] qps`, `[3] capacity`, `[4] ttl_s`
- **返回值**：数组 `[status, retry_ms]`
  - `status = 1`：允许通过，`retry_ms = 0`。
  - `status = 0`：限流拒绝，`retry_ms = 恢复一个令牌所需毫秒数`。
- **算法实现**：
  ```lua
  local key = KEYS[1]
  local now = tonumber(ARGV[1])
  local qps = tonumber(ARGV[2])
  local capacity = tonumber(ARGV[3])
  local ttl = tonumber(ARGV[4])

  local data = redis.call("HMGET", key, "tokens", "last_ms")
  local tokens = tonumber(data[1])
  local last_ms = tonumber(data[2])

  if not tokens or not last_ms then
      tokens = capacity
      last_ms = now
  else
      local delta = math.max(0, now - last_ms)
      tokens = math.min(capacity, tokens + delta * (qps / 1000.0))
      last_ms = now
  end

  if tokens >= 1.0 then
      tokens = tokens - 1.0
      redis.call("HMSET", key, "tokens", tokens, "last_ms", last_ms)
      redis.call("EXPIRE", key, ttl)
      return {1, 0}
  else
      local wait_ms = math.ceil((1.0 - tokens) * 1000.0 / qps)
      redis.call("HMSET", key, "tokens", tokens, "last_ms", last_ms)
      redis.call("EXPIRE", key, ttl)
      return {0, wait_ms}
  end
  ```

#### 脚本 2：每日 Token 消费累加 (`daily_quota_consume.lua`)
- **KEYS[1]**：`aigate:quota:{key_id}:{day_epoch}`
- **ARGV**：`[1] tokens`, `[2] daily_quota`, `[3] ttl_s`
- **返回值**：`[status, total_tokens]`
  - `status = 0`：配额充足；`status = -1`：已超出配额限制。
- **算法实现**：
  ```lua
  local key = KEYS[1]
  local consumed = tonumber(ARGV[1])
  local quota = tonumber(ARGV[2])
  local ttl = tonumber(ARGV[3])

  local total = redis.call("INCRBY", key, consumed)
  if total == consumed then
      redis.call("EXPIRE", key, ttl)
  end

  if quota > 0 and total > quota then
      return {-1, total}
  end
  return {0, total}
  ```

#### 脚本 3：管理端 IP 锁留 (`admin_lockout.lua`)
- **KEYS[1]**：`aigate:lockout:{client_ip}`
- **ARGV**：`[1] max_fails`, `[2] window_s`
- **返回值**：`[is_locked, current_fails]`
  ```lua
  local key = KEYS[1]
  local max_fails = tonumber(ARGV[1])
  local window_s = tonumber(ARGV[2])

  local fails = redis.call("INCR", key)
  if fails == 1 then
      redis.call("EXPIRE", key, window_s)
  end

  if fails >= max_fails then
      return {1, fails}
  end
  return {0, fails}
  ```

#### 脚本 4：集群熔断器三态协同流转 (`circuit_breaker_sync.lua`)
- **KEYS[1]**：`aigate:cb:{endpoint_hash}`
- **ARGV**：`[1] action` ("allow" / "success" / "fail"), `[2] now_ms`, `[3] max_fails`, `[4] cooldown_ms`
- **返回值**：`[allowed, current_state]`（`state`: 0=Closed, 1=Half-Open, 2=Open）
- **算法实现**：
  - **allow**：
    - 若 `state == 2 (OPEN)` 且 `now_ms >= open_until`：跃迁为 `state = 1 (HALF_OPEN)`，放行首个单次探活，返回 `[1, 1]`；
    - 若 `state == 2` 仍在冷却期：拦截，返回 `[0, 2]`；
    - 若 `state == 1`：限制单机探活，多余并发拦截，返回 `[0, 1]`；
    - 若 `state == 0`：正常放行，返回 `[1, 0]`。
  - **fail**：
    - 在 `CLOSED` 态递增 `fails`，达到 `max_fails` 则置为 `OPEN` 并更新 `open_until`；
    - 在 `HALF_OPEN` 探活失败直接置为 `OPEN` 重新冷却。
  - **success**：
    - 重置为 `CLOSED`，`fails = 0`，瞬时通知全集群上游已恢复。

---

## §5 热路径集成与 Fail-Closed 容灾规范

### 5.1 热路径挂载时序
1. **鉴权与配额预检**：
   - 调用 `rl_remaining_daily`。
   - 若 Redis 返回失败（连接超时/网络故障），**严格触发 Fail-Closed**，直接返回 HTTP 503。
   - 若剩余配额 $\le 0$，直接返回 HTTP 429 `daily_quota_exceeded`。
2. **QPS 限流准入**：
   - 调用 `rl_allow_request`。
   - 触发限流返回 HTTP 429 带 `Retry-After: <sms/1000>`。
   - Redis 故障同样触发 Fail-Closed（HTTP 503）。
3. **上游路由选择**：
   - `model_router_select_target` 遍历时通过 `cb_allow_request` 判定 candidate endpoint。
   - 被集群标记为 `OPEN` 的 endpoint 自动绕过，选择备用健康目标。
4. **结果收敛反馈**：
   - 收到上游 2xx/3xx/4xx，调用 `cb_record_success` 与 `rl_reserve_tokens`。
   - 收到上游 5xx/超时，调用 `cb_record_failure`。

### 5.2 统一错误响应结构
```json
{
  "error": {
    "message": "Distributed rate limit service temporarily unavailable",
    "type": "server_error",
    "code": "distributed_state_unavailable"
  }
}
```

---

## §6 测试策略与验证体系

### 6.1 单元测试 ([`tests/unit/`](file:///home/quintin/Data/source/c_cpp/aigate/tests/unit))
1. `test_redis_pool.c`：
   - 验证连接池的初始化、借出、归还、断线重连与连接耗尽超时。
   - 环境嗅探：无本地 Redis 时自动优雅跳过网络测试，保证离线编译与 `ctest` 100% 通过。
2. `test_ratelimit.c` & `test_circuit_breaker.c`：
   - 注入 mock 连接故障，断言立即触发 Fail-Closed 错误码 `-1`。

### 6.2 多节点端到端集成测试 ([`tests/integration/`](file:///home/quintin/Data/source/c_cpp/aigate/tests/integration))
在 `docker-compose.yml` 中启动 1 个 Redis + 1 个 Postgres + 2 个 aigate 网关（端口 8080 与 8081），使用 `pytest` 自动化运行：
1. **并发 QPS 防超发**：向两台实例并发打 20 个请求（QPS=10），断言全集群累计放行数 $\le 10$。
2. **跨节点配额结算**：向实例 A 消耗 800 Tokens（限额 1000），向实例 B 紧接着请求 500 Tokens，断言实例 B 准确拦截 429。
3. **集群熔断协同**：向实例 A 连续打爆某上游，断言实例 B 在未经该上游报错的情况下瞬间感知熔断并自动 Failover。
4. **Fail-Closed 容灾中断**：挂起 Redis 容器，断言在 100ms 内全部收到 503，恢复后自动满血恢复。

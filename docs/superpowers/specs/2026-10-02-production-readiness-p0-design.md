# aigate 生产可用性 (Production-Readiness) P0 改造设计规范

- **状态**: 规划完成 (Planned & Ready for Execution)
- **创建日期**: 2026-10-02
- **责任模块**:
  - `src/core/config.h`, `src/core/config.c` (配置加载与边界校验)
  - `src/server/transport_civetweb.h`, `src/server/transport_civetweb.c` (网络传输层、线程池、健康探测、真实客户端 IP)
  - `src/main.c` (进程生命周期、优雅退出 Draining)
  - `src/store/pg_store.h`, `src/store/pg_store.c` (数据库连接池与读写连接分离)
  - `src/policy/ratelimit.c`, `src/policy/circuit_breaker.c` (Redis 故障平滑降级)
- **目标**: 针对真实生产高并发（高并发长流式生成、反向代理/Ingress、K8s 容器编排调度、存储与缓存故障容灾）环境，消除 5 大生产阻断缺陷（P0），实现具备企业级吞吐、零无辜断流、故障平滑降级与云原生标准的生产就绪网关。

---

## 1. 业务背景与问题分析 (Problem Statement)

在目前经过全量体检与单测验证的架构下，`aigate` 拥有极高的代码质量和完备的算法（244 项单元测试 100% 通过），但在投入真实生产部署前存在 5 个关键的阻断性硬伤：

### 1.1 并发吞吐受限：工作线程写死 16 线程
- **现状**: `src/server/transport_civetweb.c:500` 内部硬编码 `"num_threads", "16"`。
- **危害**: CivetWeb 为同步线程模型，LLM 流式生成请求往往长达 5~60 秒。当有 16 个并发生成请求在执行时，网关工作线程池即刻饱和（100% 耗尽）。第 17 个请求进入 TCP backlog 排队，导致客户端大量出现连接超时或握手失败。

### 1.2 云原生运维盲区：缺乏非鉴权健康探针与平滑排空 (Draining)
- **现状**: 网关仅挂载 `/v1/`, `/admin/`, `/metrics`。唯一的探活端点为 `/admin/v1/providers/health`（需鉴权且打真实上游）。
- **危害**: Kubernetes 无法配置 `livenessProbe` 和 `readinessProbe`。此外，`main.c` 捕获 `SIGTERM` 后立即调用 `mg_stop` 强杀连接，存量正在流式输出的 HTTP 连接瞬间被中断（Connection Reset），引起用户侧报错。

### 1.3 反向代理安全穿透缺陷：客户端 IP 提取与 Admin 锁留全网误杀
- **现状**: 数据面与管理面直接采用 socket 对端 IP (`ri->remote_addr`)。
- **危害**: 生产环境网关前置必有 Ingress / Nginx / ALB。反向代理使所有流量的 `remote_addr` 统一为反代节点内网 IP。攻击者在公网连续 10 次恶意尝试登录后台，`admin_lockout` 将反代 IP 封锁 300 秒，直接导致**全公司所有合法管理员均无法登录**。且审计日志全被记录为反代 IP。

### 1.4 存储性能栓死：PostgreSQL 全局单连接与互斥锁 Head-of-Line 阻塞
- **现状**: `src/store/pg_store.c:32-37` 全进程仅有单一 `PGconn* db`，被全局互斥锁 `px->mtx` 保护。
- **危害**: `usage_meter.c` 批刷线程每 5 秒（`AIGATE_USAGE_FLUSH_S`）批量写入最多 4096 条明细时持有该连接。此时任何未命中内存缓存的新 key 鉴权、Admin API 查询、模型路由查表等网络 I/O 必须排队串行等待，引发数据面 P99 延迟陡增。

### 1.5 缓存故障放大：Redis 异常时 Rate Limit 强行“全拒”
- **现状**: `src/policy/ratelimit.c:191-195` 当 Redis 连接获取失败或命令超时直接返回 `-1`，向客户端返回 429。
- **危害**: 分布式部署中，Redis 闪断或抖动会立即引发“级联雪崩”，将可用性问题放大为全站 100% 拒绝服务（Fail-Closed）。生产中需要受控的降级开关（Fail-Open / 单机内存降级）。

---

## 2. 总体改造设计架构 (Architecture Design)

```
                            [ 外部客户端 / API Consumers / K8s Probes ]
                                                 │
                                                 ▼
               ┌──────────────────────────────────────────────────────────────────┐
               │              反向代理层 (Nginx / Ingress-Nginx / ALB)              │
               │  - 追加 X-Forwarded-For: <client_ip>, <proxy_ip>                 │
               │  - 转发 Host / Proto                                             │
               └──────────────────────────────────────────────────────────────────┘
                                                 │
                                                 ▼ (HTTP/1.1)
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│ aigate 网关进程                                                                               │
│                                                                                             │
│  【网络传输与健康层】(transport_civetweb)                                                      │
│    ├── Worker 线程池: AIGATE_WORKER_THREADS (默认 64, 支持 4~4096)                          │
│    ├── 真实 IP 解析器: AIGATE_TRUSTED_PROXIES 校验 + 提取首个真实 Client IP                   │
│    ├── /healthz (Liveness): 极轻量进程保活 200 OK                                            │
│    └── /ready (Readiness): 依赖连通性预检; 收到下线信号进入 Draining 立即置 503              │
│                                                                                             │
│  【生命周期与平滑排空】(main.c)                                                              │
│    ├── SIGTERM / SIGINT 捕获 ──► 标记 g_draining = 1                                         │
│    └── Draining 宽限期 (AIGATE_DRAIN_TIMEOUT_S, 默认 15s) ──► 等待存量流传输自然完成 ──► 退出 │
│                                                                                             │
│  【数据库并发池化层】(pg_store)                                                              │
│    ├── 读写隔离双通道设计 (Flush Dedicated Conn + Read/Auth Conn Pool)                      │
│    │     ├── 专用 Flush 连接 (Dedicated): 独占执行批刷与报表写入，彻底不争抢数据面             │
│    │     └── 读操作连接池 (Read Pool): 4~16 个独立 PGconn，供 Key 鉴权/路由/Admin 并发查询     │
│    └── Advisory Lock: 启动阶段获取全局分布式咨询锁，防止多 Pod 并发 DDL 冲突                 │
│                                                                                             │
│  【分布式缓存容灾降级】(ratelimit / circuit_breaker)                                         │
│    └── AIGATE_REDIS_FAIL_OPEN (默认 1: 降级放行并回退本地内存桶限流; 0: 严格拒绝)           │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 详细设计规范 (Module Specifications)

### 3.1 Task 1: CivetWeb 线程池可配化与配置校验

1. **新增环境变量与配置结构** (`src/core/config.h` & `src/core/config.c`):
   - `worker_threads`: 整数，读取 `AIGATE_WORKER_THREADS`。
     - 默认值: `64`。
     - 合法范围: `[4, 4096]`。越界行为: 启动失败并报错。
   - `request_timeout_ms`: 整数，读取 `AIGATE_REQUEST_TIMEOUT_MS`。
     - 默认值: `300000` (5 分钟，兼顾长流式生成)。
     - 合法范围: `[1000, 3600000]`。越界行为: 回退到默认值。

2. **传输层启动参数绑定** (`src/server/transport_civetweb.c`):
   - 在 `transport_civetweb_start` 中动态组装 `options` 字符串数组：
     ```c
     char threads_buf[16];
     char timeout_buf[16];
     snprintf(threads_buf, sizeof(threads_buf), "%d", cfg_worker_threads);
     snprintf(timeout_buf, sizeof(timeout_buf), "%d", cfg_request_timeout_ms);
     const char* options[] = {
         "listening_ports", port_spec,
         "num_threads", threads_buf,
         "request_timeout_ms", timeout_buf,
         NULL
     };
     ```

### 3.2 Task 2: `/healthz`、`/ready` 探针与平滑排空 (Graceful Draining)

1. **健康探测规范**:
   - `GET /healthz` (或 `/live`):
     - 无需鉴权，任何人可访问。
     - 只要主服务运行中，立即返回 `200 OK`，Content-Type: `application/json`，内容: `{"status":"ok","timestamp":<now>}`。
   - `GET /ready`:
     - 检查 `g_draining` 标志：若为 1，直接返回 `503 Service Unavailable`，内容: `{"status":"draining","message":"node is shutting down"}`。
     - 检查底层存储就绪状态：调用 `pg_store_ping(ps)` 验证数据库连通性；若已启用 Redis，验证连接可用性。
     - 全部通过返回 `200 OK`，内容: `{"status":"ready","database":"connected","redis":"connected"}`。
     - 任一核心依赖失败返回 `503 Service Unavailable`。

2. **平滑排空状态机与信号处理** (`src/main.c`):
   - 全局原子标志: `static volatile sig_atomic_t g_draining = 0;` 和 `g_stop = 0;`。
   - 配置项: `AIGATE_DRAIN_TIMEOUT_S`（默认 15 秒，范围 `[0, 120]`）。
   - 信号触发逻辑:
     1. 收到 `SIGTERM` / `SIGINT` 时：`g_draining = 1`。
     2. 记录日志: `AIGATE_LOG_INFO("aigate received shutdown signal, entering draining phase for %d seconds...", drain_timeout_s);`。
     3. 保持 CivetWeb 正常运行 `drain_timeout_s` 秒：此时 `/ready` 持续报 503，K8s Service / Ingress 在 1~3 秒内将该 Pod 剔除，不再分发新请求；在此期间已接入的长流式请求可以继续输出。
     4. 宽限期结束后置 `g_stop = 1`，跳出主循环，调用 `transport_civetweb_stop` 与 `aigate_core_shutdown`。

### 3.3 Task 3: 真实客户端 IP 解析与反代信任链

1. **配置引入**:
   - `AIGATE_TRUSTED_PROXIES`: 逗号分隔的 IPv4 CIDR/IP 列表（例如 `"127.0.0.1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"`）。
   - 若未配置或为空：默认仅信任 `127.0.0.1`。
   - 复用既有的 `metrics_acl_allows` 高效 CIDR 匹配函数。

2. **安全 IP 解析算法**:
   - 获取对端 socket IP: `peer_ip = ri->remote_addr`。
   - 若 `peer_ip` 不属于 `AIGATE_TRUSTED_PROXIES`：
     - 说明直接来自不可信网络或客户端直连，**严禁信任请求头中的 XFF**！直接返回 `peer_ip`。
   - 若 `peer_ip` 属于 `AIGATE_TRUSTED_PROXIES`：
     - 检查 `X-Forwarded-For` 请求头：形如 `client, proxy1, proxy2`。
     - 从右向左逐级扫描（剔除受信任反代 IP），选取最靠近右侧的第一个不可信外部 IP（防止客户端伪造 XFF 注入首部）。若无法解析，回退为最左侧的首段 IP。
     - 若无 `X-Forwarded-For`，检查 `X-Real-IP`；若均无，使用 `peer_ip`。
   - 提取结果安全写入定长缓冲区 `char client_ip[64]`。

3. **调用点替换**:
   - 数据面请求上下文: `rq.client_ip = resolved_client_ip;`。
   - 管理面分发: `admin_dispatch(..., resolved_client_ip, ...);`。
   - 彻底解决反代内网 IP 被锁定 300 秒导致全员误杀的致命缺陷。

### 3.4 Task 4: PostgreSQL 读写分离专用连接与迁移 Advisory Lock

1. **双通道设计**:
   - 现状瓶颈是 `usage_meter.c` 的 5 秒批量 Flush（最多 4096 条请求插入 + Daily 聚合）霸占了唯一连接。
   - 解决方案：
     - 在 `pq_ctx` 中引入**读写分离双连接机制**：
       - `PGconn* db_flush`: 专用后台批刷连接，由 `flush_mtx` 保护。
       - `PGconn* db_read`: 专用前台鉴权与管理读写连接，由 `read_mtx` 保护。
       - （进阶：`db_read` 可支持简易 4 连接池；即使单用 1 条独立前台连接 + 1 条独立 Flush 连接，也能将锁争用从 100% 降至 0%）。
     - `pq_flush_usage_daily` 与 `pq_flush_usage_requests` 走 `db_flush`；
     - `pq_get_key_by_hash`, `pq_list_models`, `admin_*` 走 `db_read`。

2. **数据库迁移分布式咨询锁 (Postgres Advisory Lock)**:
   - 在 `pg_store_migrate` 入口执行：
     ```sql
     SELECT pg_advisory_lock(7192847291);
     ```
   - 在 `SCHEMA_SQL` 执行完毕（成功或回滚）后，无条件执行：
     ```sql
     SELECT pg_advisory_unlock(7192847291);
     ```
   - 确保 K8s 多副本同时拉起时，仅有一个 Pod 执行 DDL，其他 Pod 排队等待直至迁移完成。

### 3.5 Task 5: Redis 宕机故障平滑降级 (Fail-Open / In-Memory Fallback)

1. **配置引入**:
   - `AIGATE_REDIS_FAIL_OPEN`: 整数，`0` 或 `1`（默认 `1`）。
   - `=1` 时：当 Redis 连接池耗尽、网络不可达或 Lua 执行超时，不直接判失败，而是记录限频警告，并自动回退到单机本地的内存 Token Bucket 进行兜底限流与熔断。
   - `=0` 时：保持原有严格 fail-closed 语义。

2. **实现改造** (`src/policy/ratelimit.c` & `src/policy/circuit_breaker.c`):
   - 在 `ratelimit_allow_request` 中：
     ```c
     redisContext* c = redis_pool_acquire(rl->pool);
     if (c == NULL) {
         if (rl->fail_open) {
             /* 降级回退到本地内存桶限流 */
             return ratelimit_allow_request_local(rl, key_id, qps, retry_ms);
         }
         return -1;
     }
     ```
   - 既保证了 Redis 正常时的跨节点集群状态同步，又杜绝了缓存抖动导致的业务大面积瘫痪。

---

## 4. 验证计划与测试矩阵 (Verification Plan)

| 测试类别 | 用例名称 | 验证目标 |
|---|---|---|
| **单元测试** | `test_config_worker_threads` | 验证 `AIGATE_WORKER_THREADS` 默认 64、合法范围 `[4, 4096]` 及越界报错退出 |
| **单元测试** | `test_transport_healthz_and_ready` | 验证 `GET /healthz` 恒为 200，`GET /ready` 正常时 200，draining 时 503 |
| **单元测试** | `test_transport_real_client_ip` | 验证普通直连 vs 受信任代理链下 `X-Forwarded-For` 的首段解析与防伪造 |
| **单元测试** | `test_pg_dedicated_flush_and_read` | 验证并发压测下，后台批量写入执行时，前台 Key 读取互斥锁零等待 |
| **单元测试** | `test_ratelimit_redis_fail_open` | 验证 Redis 离线时，`fail_open=1` 自动回退本地内存桶且请求正常放行 |
| **集成/冒烟** | `tests/integration/smoke.sh` | 全量回归测试：端点冒烟、真实 PostgreSQL 迁移及用量回读 |
| **并发压测** | `benchmarks/run.sh` | 启动 100 并发压测，验证解除 16 线程硬编码后的 QPS 翻倍提升与零连接拒绝 |

# 配置详解（Configuration）

> 全量语义，源头为 `src/core/config.c` / `src/core/config.h` /
> `src/server/transport_civetweb.c`。新人先看 [ONBOARDING.md](ONBOARDING.md) 速查表。
>
> 约定：空字符串视同未设置（回默认值）；"启动失败"指进程打日志直接退出。

## 网关变量（共 22 个）

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
| `AIGATE_WORKER_THREADS` | 否 | `64` | 启动失败 | CivetWeb 工作线程池并发数，范围 `[4, 4096]` |
| `AIGATE_REQUEST_TIMEOUT_MS` | 否 | `300000` | 启动失败 | CivetWeb 请求/长连接超时（毫秒），范围 `[1000, 3600000]` |
| `AIGATE_TRUSTED_PROXIES` | 否 | `127.0.0.1` | — | 逗号分隔的受信反代 IP/CIDR，安全提取真实客户端 IP |
| `AIGATE_DRAIN_TIMEOUT_S` | 否 | `15` | 启动失败 | 优雅下线排空等待时间（秒），范围 `[0, 120]`；期间 `/ready` 返回 503 |
| `AIGATE_REDIS_FAIL_OPEN` | 否 | `1` | — | Redis 宕机降级模式：`1` 平滑回退单机限流，`0` 严格拒绝 |
| `AIGATE_CORS_ALLOW_ORIGIN` | 否 | `*` | — | 跨域允许 Origin（如 `*` 或 `https://chat.example.com`） |
| `AIGATE_LOG_FORMAT` | 否 | `text` | 警告并回 `text` | 日志格式：`text` (本地调试) 或 `json` (云原生/K8s/ELK/Loki) |
| `AIGATE_LOG_LEVEL` | 否 | `info` | 警告并回 `info` | 最低输出日志等级：`debug`/`info`/`warn`/`error`，低级别进锁前快速丢弃 |
| `AIGATE_ALLOW_PLAINTEXT_KEYS` | 否 | `0` | — | 仅 `=1` 时允许上游密钥明文落库 |
| `AIGATE_LOCKOUT_MAX_FAILS` | 否 | `10` | 越界保默认 | Admin 连续输错锁定阈值 |
| `AIGATE_LOCKOUT_WINDOW_S` | 否 | `300` | 越界保默认 | 锁定窗口（秒） |

注意：`REDIS_TIMEOUT_MS` / `REDIS_POOL_SIZE` / `LOCKOUT_*` / `LOG_*` 越界**不报错**（保留默认），
是最易踩的坑；其余数值越界一律启动失败。

## 归属划分（compose 层 vs 网关）

以下变量不进网关进程，只给 `docker compose` 用，见 `docker-compose.yml` 与
`.env.example`：`POSTGRES_PASSWORD`、`AIGATE_LISTEN_PORT`（宿主机映射端口）等。

`AIGATE_LISTEN` 是网关监听地址，`AIGATE_LISTEN_PORT` 只改宿主机映射，两者别混。

## 溯源

- 加载与校验：`src/core/config.c`（`aigate_config_load`），字段注释见 `src/core/config.h`。
- 明文钥匙 / 锁定策略 / 监听解析：`src/server/transport_civetweb.c`。
- 改码先改此表。

## 零停机配置平滑热重载 (SIGHUP)

网关进程支持捕获 `SIGHUP` 信号以零停机刷新动态配置，无需重启进程或中断长流式连接：
- **热重载支持项**：
  - 日志级别与格式 (`AIGATE_LOG_LEVEL`、`AIGATE_LOG_FORMAT`)
  - 跨域白名单 (`AIGATE_CORS_ALLOW_ORIGIN`)
  - 受信反代列表 (`AIGATE_TRUSTED_PROXIES`)
- **触发命令**：
  ```bash
  kill -HUP <aigate_pid>
  # 或在 Kubernetes Pod 中:
  kubectl exec <pod-name> -n aigate -- kill -HUP 1
  ```

## 主密钥安全离线轮换工具 (CLI)

当安全合规要求轮换 `AIGATE_MASTER_KEY` 时，可通过离线子命令在单次数据库事务中将数据库内所有以 AES-256-GCM 加密存储的上游密钥（models 与 providers）平滑轮换为新主密钥：
```bash
./aigate --rotate-master-key <old_master_hex_64> <new_master_hex_64>
```
轮换执行前会预检解密有效性，任何失败直接回滚，保障零数据损坏。轮换完成后将环境变量中的 `AIGATE_MASTER_KEY` 替换为新密钥即可启动网关。


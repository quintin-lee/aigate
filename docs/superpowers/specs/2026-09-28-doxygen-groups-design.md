# Doxygen 分组体系补齐设计（第 6 轮注释完善）

## 背景

前五轮已覆盖 src 全部符号注释（函数/全局量/宏/结构体字段/枚举/typedef）与测试辅助函数。
实测剩余缺口：30 个头文件的 `@file` 已全齐，零 `@defgroup`/`@ingroup`，无 Doxyfile，无 `@mainpage`
（doxygen 二进制可用）。本轮补齐分组体系，使文档可生成、可按层浏览。

用户选择：目标=Doxygen结构补齐，深度=@file+分组全套，方案=A（七层分组+最小 Doxyfile+实跑验证）。

## §1 范围清单

7 个分组，锚定头文件（`@defgroup` 置于其 `@file` 块之后）：

| 分组 | 锚定头文件 | @brief |
|---|---|---|
| group_common | src/common/lru.h | 通用：LRU 缓存、SHA-256。 |
| group_core | src/core/aigate_core.h | 核心：网关上下文、配置、密钥、日志。 |
| group_observe | src/observe/event_bus.h | 可观测：事件总线、健康探针、指标。 |
| group_policy | src/policy/guardrails.h | 策略：鉴权、预算、熔断、护栏、限流、响应缓存。 |
| group_server | src/server/transport_civetweb.h | 服务：HTTP 传输、管理 API、后台页面。 |
| group_store | src/store/pg_store.h | 存储：Postgres、Redis、用量计量。 |
| group_upstream | src/upstream/provider_adapter.h | 上游：模型路由、供应商适配、客户端。 |

其余 23 个头文件各在现有 `@file` 块内加一行 `@ingroup <层组>`：

- common: sha256.h → group_common
- core: aigate_log.h、config.h、secrets.h → group_core
- observe: health_prober.h、metrics.h → group_observe
- policy: auth_key.h、budget_enforce.h、circuit_breaker.h、ratelimit.h、response_cache.h → group_policy
- server: admin_api.h、admin_ui.h → group_server
- store: redis_client.h、redis_pool.h、redis_scripts.h、schema_sql.h、usage_meter.h → group_store
- upstream: model_router.h、provider_anthropic.h、provider_gemini.h、provider_openai.h、upstream_client.h → group_upstream

新建根目录 `Doxyfile`（仅 6 项，其余默认）。

不做：`@mainpage`、长篇组描述、CI 集成（方案 C 范畴）。

## §2 格式

分组定义用 Doxygen 块风格，与前轮一致：

```c
/**
 * @defgroup group_core 核心层
 * @brief 核心：网关上下文、配置、密钥、日志。
 */
```

`@ingroup` 并入各头文件现有 `@file` 块加一行，不另起块，不引入新风格。

Doxyfile 内容：

```
PROJECT_NAME           = aigate
INPUT                  = src
RECURSIVE              = YES
GENERATE_HTML          = YES
OUTPUT_DIRECTORY       = build/docs
QUIET                  = YES
```

## §3 验证标准

沿用三验证：① `doxygen Doxyfile` 零警告；② 全量构建零警告 + ctest 6/6；
③ 纯注释机检（`git diff` 只有加行 + 1 个新文件 Doxyfile）。
单提交：`docs: add Doxygen groups and Doxyfile`，只 `git add` 本批 30 头文件 + Doxyfile。

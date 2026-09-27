# src 多目录拆分设计说明书 (src Multi-Directory Split Design)

## 1. 概述与目标 (Overview & Goals)

当前 `src/` 根目录平铺 29 个模块（59 个 `.c/.h` 文件），`CMakeLists.txt` 用 `file(GLOB src/*.c)` 一把梭收集源码。
随着模块增多（刚新增 `response_cache`、`health_prober`、`event_bus`），找文件、划职责越来越费劲。

本设计目标：**按技术分层把源码拆到多个子目录，改动最小**——只移动文件 + 改构建，
所有 `#include "xxx.h"` 保持不变，`libaigate` 仍为单个静态库，`tests/` 不动。

非目标：不拆分成多个静态库、不改 `include` 风格、不做任何业务重构。

---

## 2. 目录划分 (Directory Layout)

`main.c` 留守 `src/` 根目录做入口，其余 28 个模块按技术职责归入 7 个子目录：

| 目录 | 成员 | 职责一句话 |
|---|---|---|
| `src/core/` | `aigate_core`、`config`、`aigate_log`、`secrets` | 启动编排与基础配置 |
| `src/common/` | `sha256`、`lru` | 无依赖小工具 |
| `src/upstream/` | `upstream_client`、`provider_adapter`、`provider_openai`、`provider_anthropic`、`provider_gemini`、`model_router` | 上游调用与选路 |
| `src/policy/` | `auth_key`、`ratelimit`、`budget_enforce`、`guardrails`、`circuit_breaker`、`response_cache` | 网关策略中间件 |
| `src/store/` | `pg_store`、`redis_client`、`redis_pool`、`redis_scripts.h`、`usage_meter`、`schema_sql.h` | 持久化与计量 |
| `src/observe/` | `metrics`、`event_bus`、`health_prober` | 可观测性 |
| `src/server/` | `transport_civetweb`、`admin_api`、`admin_ui` | 对外服务面 |

边界说明（消除歧义）：
- `model_router` 归 `upstream/`（选路是上游调用的前置步骤），不归 `policy/`；
- `response_cache` 归 `policy/`（它是请求管线中的策略中间件），不归 `store/`；
- `lru` 归 `common/`（纯数据结构，被 `response_cache` 复用），不归 `store/`；
- `usage_meter` 归 `store/`（写 pg/redis 做计量持久化），不归 `observe/`；
- `schema_sql.h`、`redis_scripts.h` 这两个纯头文件随使用者归入 `store/`。

---

## 3. 构建改动 (Build Changes)

仅改顶层 `CMakeLists.txt` 两处：

1. 源码收集加递归一层：
   `file(GLOB AIGATE_SRC src/*.c)` →
   `file(GLOB AIGATE_SRC src/*.c src/*/*.c)`，
   并加 `CONFIGURE_DEPENDS`（新增文件时 cmake 自动重扫，缓解 GLOB 通病）。
2. `target_include_directories(libaigate PUBLIC ...)` 追加 7 个子目录，
   `#include "xxx.h"` 全平级引用零改动。

`tests/CMakeLists.txt` 不动（它只链 `libaigate`，不直接引源码）。

---

## 4. 迁移步骤 (Migration Steps)

1. `git mv src/<mod>.c src/<dir>/<mod>.c`（同 `.h`），保留历史；
2. 改顶层 `CMakeLists.txt` 上述两处；
3. `cmake -S . -B build && cmake --build build` 全量构建通过；
4. `ctest --test-dir build` 单元 + 集成测试全绿；
5. `-Wall -Wextra -Werror` 零新增警告（本项目编译选项含 `-Werror`）。

要求：一次搬完、一次验证，不分多批；`src/response_cache.*`、`tests/unit/test_response_cache.c` 等未提交新文件随本次迁移一并归位（`response_cache` → `src/policy/`）。

---

## 5. 验证标准 (Acceptance Criteria)

- 全量构建 exit 0，`lsp_diagnostics` 在改动文件上干净；
- `ctest` 全绿（与迁移前基线一致，无新增失败）；
- `git status` 无残留：旧平铺文件全部消失，无孤儿 `.c/.h` 滞留 `src/` 根目录（除 `main.c`）；
- 功能零变更：无任何业务逻辑 diff（`git diff -w` 仅显示改名 + CMakeLists 两处）。

---

## 6. 风险与取舍 (Risks & Trade-offs)

- **GLOB 固有缺陷**：新增文件需重跑 cmake，已用 `CONFIGURE_DEPENDS` 缓解；若日后仍觉不可靠，可演进为方案 B（显式列出源码），本次不做。
- **单库未拆分依赖边界**：目录只是逻辑分组，不强制依赖方向；若出现层间循环依赖，编译期不会暴露。接受该代价（换取最小改动）；真需要硬边界时再演进为方案 C（每目录独立静态库）。
- **历史追溯**：必须用 `git mv` 而非删建，否则 `git log --follow` 断链。

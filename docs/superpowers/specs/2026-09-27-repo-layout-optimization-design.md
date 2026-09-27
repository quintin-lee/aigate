# 全仓目录结构优化设计（方案 A：轻量归置）

日期：2026-09-27｜状态：已批准（3 节分节认可）｜前置：src/ 七层拆分已落地（5b687f4）

## 1. 背景与目标

- 范围：整体梳理，但 `src/` 刚拆完，本次不动。
- 主要矛盾：`tests/unit/` 35 个文件平铺，与 `src/` 七层脱节，新人无法从源码定位到测试。
- 约束：仅搬迁 + 改构建（`#include`/代码内容不动，`extern` 注册机制使测试搬迁零代码改动）。
- 成功标准：新人可凭直觉定位代码/测试/文档；构建与测试基线不变（ctest 6/6）。

## 2. 改动一：tests/unit 按 src 七层镜像分组（核心）

规则：`test_<模块>.c` 跟随 `src/<layer>/<模块>.c`；`src/` 无独立文件的归亲缘层。
留守 `tests/unit/` 根：`run_tests.c/h`（总控注册）、`mock_upstream.c/h`（共享桩）。

| 层目录 | 测试文件（共 31 个） |
|---|---|
| `unit/common/` | test_lru、test_sha256 |
| `unit/core/` | test_aigate_core、test_config、test_log、test_secrets |
| `unit/observe/` | test_event_bus、test_health_prober |
| `unit/policy/` | test_auth_key、test_budget_enforce、test_circuit_breaker、test_guardrails、test_ratelimit、test_response_cache |
| `unit/server/` | test_admin_api、test_admin_ui |
| `unit/store/` | test_pg_store、test_redis_pool、test_usage_meter |
| `unit/upstream/` | test_embeddings、test_failover、test_gemini_stream、test_model_router、test_provider_anthropic、test_provider_deepseek、test_provider_gemini、test_provider_openai、test_provider_probe、test_stream_pipeline、test_upstream_client、test_upstream_streaming |

构建侧唯一改动（`tests/CMakeLists.txt`，沿用 src 模式）：

```cmake
file(GLOB UNIT_TEST_SRC CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/tests/unit/*.c
     ${CMAKE_SOURCE_DIR}/tests/unit/*/*.c)
```

## 3. 改动二：构建目录统一约定 + docs 索引

- 约定唯一构建目录为 `build/`（与 `.gitignore` 首条一致），文档写死 `cmake -B build` 流程；`.build/`、`build-asan/` 不再作为推荐路径（磁盘现有目录不动，不进版本控制）。
- 新建 `docs/README.md` 全仓地图：顶层目录一句话说明、源码↔测试镜像关系、构建命令。
- `cmake/` 为磁盘空目录（git 不跟踪）：版本控制层面忽略，不动。

## 4. 改动三：隐式耦合显式化 + 不动清单

- `web/admin.html` ↔ `scripts/embed_html.py` 的 embed 关系不动文件，仅在 `docs/README.md` 中一句话点明（改页面后需重跑脚本烘焙进二进制）。
- 不动清单：`src/`、`tests/integration/`（pytest 路径链长，风险收益比不合算）、`third_party/`（vendor 方式另议）、`schema/`、`Dockerfile`/`docker-compose.yml`。

## 5. 验证标准

1. `git mv` 后 `git status -R` 显示 31 个 100% 纯改名。
2. `tests/CMakeLists.txt` 的 diff 仅 GLOB 行。
3. `cmake -B build && cmake --build build && ctest` 全绿，与基线（6/6）一致。

## 6. 被否决的备选

- 方案 B（测试深度重组：integration 按特性拆分、抽 fixtures/、mock 集中到 tests/support/）：可发现性更高，但动 40+ 文件，pytest/conftest 引用链长，风险高一个量级。
- 方案 C（只定规范不动手）：零风险但无实际改善。

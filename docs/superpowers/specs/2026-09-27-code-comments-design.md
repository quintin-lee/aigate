# 代码注释完善设计 (Spec)

**日期**: 2026-09-27
**状态**: 已批准（分节确认），待实施
**范围**: `src/` 下 59 个 C/H 文件约 400 函数定义的 Doxygen 注释补齐

## 1. 背景与目标

`src/` 已完成七层目录拆分，58/59 文件带有 `@file/@brief` 文件头，
但函数级注释缺口集中：实测 `.h` 声明待补约 60 处、`.c` 内 static 函数待补约 170 处（含少量多声明同行误报），合计约 230；另 `src/upstream/upstream_client.c` 缺 `@file/@brief` 文件头（全仓唯一）。
目标：新人能不读实现读懂七层模块与调用链（可读性优先）。

## 2. 确认的需求（用户四选）

- 动机：可读性优先（新人看懂模块与调用链）。
- 范围：缺口函数全覆盖（`.h` 约 60 + static 约 170，含 `src/main.c` 的 2 个；已具备文档的约 160 处不动）。
- 粒度：完整 Doxygen（`@brief/@param/@return` 全套，延续现有文件头风格）。
- 验收：构建测试全绿（注释纯加行，零警告 + ctest 6/6）。

## 3. 方案选型

- **方案 A（采用）**：按七层分 7 批，每批独立提交，可中断、可回滚、可 review。
- 方案 B（弃）：一次性全量单提交，diff 上千行无法细看，只能整体回滚。
- 方案 C（弃）：脚本搭 `@param` 骨架+人工填，骨架易成废话注释，需二次返工。

## 4. 注释规范

```c
/** @brief 一句话：做什么（动词开头，不复述函数名）。
 *  @param req  输入说明：含义 + 合法范围/特殊值（NULL 是否允许）。
 *  @param[out] out  输出说明：调用后里面是什么。
 *  @return 成功返回什么；失败返回什么、什么情况下失败。 */
```

铁律：@brief 禁止复读函数名（如 `Get foo` 配 `@brief Get foo`）；
@param 只写签名看不出的信息（单位、边界、所有权、NULL 约定）；
公开函数注释写 `.h` 声明处，static 函数注释写 `.c` 定义处，不两处重复；
`@param[out]`/`@param[in,out]` 按语义标注，纯入参用 `@param`。

## 5. 批次划分（小→大热身，每批一提交）

| 批次 | 目录 | 文件 |
|---|---|---|
| 1 | common（4 文件，待补 8） | lru.c/h、sha256.c/h |
| 2 | core + main.c（9 文件，待补 19） | aigate_core.c/h、aigate_log.c/h、config.c/h、secrets.c/h、main.c |
| 3 | observe（6 文件，待补 9） | event_bus.c/h、health_prober.c/h、metrics.c/h |
| 4 | server（6 文件，待补 48） | admin_api.c/h、admin_ui.c/h、transport_civetweb.c/h |
| 5 | policy（12 文件，待补 31） | auth_key、budget_enforce、circuit_breaker、guardrails、ratelimit、response_cache（各 .c/.h） |
| 6 | store（10 文件，待补 54） | pg_store.c/h、redis_client.c/h、redis_pool.c/h、redis_scripts.h、schema_sql.h、usage_meter.c/h |
| 7 | upstream（12 文件，待补 59） | model_router、provider_adapter、provider_anthropic、provider_gemini、provider_openai、upstream_client（各 .c/.h；含给 upstream_client.c 补 `@file/@brief` 文件头——全仓唯一缺头文件） |

待补数为实测（含少量多声明同行误报，实施时以 Task 0 审计为准）。任一批可独立叫停，已补批次照样可用。

## 6. 验证标准（每批必跑）

```bash
cmake -S . -B build && cmake --build build   # 零警告（-Werror 已开）
ctest --test-dir build                        # 6/6 全绿
# 纯注释机检：下述命令应无输出（只允许 + 注释行/+ 空行）
git diff -U0 -- '*.c' '*.h' | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*(\*|/\*|\*/|$|//)'
```

## 7. 不动清单

不动任何代码逻辑、`#include`、CMakeLists；
tests/ 测试函数不在本轮（命名自解释，另议）；
不引入 Doxygen 配置与文档生成流程（本轮只为可读性，不建站）。

## 8. 后续

本 spec 批准后由 writing-plans 产出实施计划（7 批任务），再执行。

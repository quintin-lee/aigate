# Enum + 函数指针 typedef 注释补齐设计

**背景：** 前三轮已覆盖全部函数、static 全局量、公开宏、结构体字段。本轮收尾仅剩的两类缺口。
**用户决策：** 范围=两者都要；格式=Doxygen 块风格；验收=沿用三验证；执行=方案 A 单批次一次提交。

## §1 范围清单（10 块、2 文件，只加不改）

| 文件 | 对象 | 块数 |
|---|---|---|
| `src/policy/circuit_breaker.h:14` | `CB_CLOSED / CB_OPEN / CB_HALF_OPEN` | 3 |
| `src/policy/circuit_breaker.h:21` | `cb_time_fn` typedef | 1 |
| `src/observe/event_bus.h:21` | `EVENT_NONE / REQUEST / CIRCUIT_BREAKER / HEALTH_PROBE / BUDGET_ALERT / PING` | 6 |

**实测修正（2026-09-28）：** plan 起草时复核发现 `lru_evict_fn`（lru.h:14）与
`upstream_chunk_fn`（upstream_client.h:49）已有 `@brief`，不重复加注；范围由 12 块/4 文件
收窄为 10 块/2 文件。

**明确不动：** guardrails 3 成员已有行尾注释；schema_sql.h 内 SQL 文本非 C 注释对象；tests 目录文件头齐全。

## §2 注释格式

枚举成员：成员上一行独立 `/** @brief …。 */` 单行块。typedef：定义上一行独立 `/** @brief …。 */`
块（含回调参数语义与返回值约定，如 `lru_evict_fn` 注明 val 由调用方释放，`upstream_chunk_fn` 注明返回 0 继续、非 0 中止）。

## §3 验证标准

1. 零警告构建：`cmake --build build` 无 warning/error。
2. `ctest --test-dir build` 6/6 通过，与基线一致。
3. 纯注释机检：`git diff -U0 | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'` 为空。
4. 单批次一次提交：预期 2 文件、加行约 30、删行 0。

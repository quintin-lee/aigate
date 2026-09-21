# aigate 全量代码体检设计 (2026-09-22)

## 背景

2026-09-21 完成加固计划（`.omp/plans/AIGATE_HARDENING_PLAN.md`，20 项 + 计划外白名单漂移修复 `70ed9fb`），
工作树干净，ASan/集成/部署冒烟全绿。本任务是对 `src/` 做一次**全面体检**：
模块走查 + 风险再挖 + 静态性能观察，产出一份可执行的发现清单。

## 参数（已与用户确认）

| 维度 | 决定 |
|---|---|
| 目的 | 全面体检（走查 + 风险 + 性能全覆盖） |
| 产出 | 单一发现清单报告 |
| 性能深度 | 纯静态代码分析（锁/热点/容量上界推理，不搭负载设施、不建模吞吐） |
| 范围 | 仅 `src/` 22 个 C 模块（不覆盖 schema/docker/build/web/tests） |
| 方法 | 单线程依赖序走查 + 调用点级交叉验证（CodeGraph/lsp） |

## 走查顺序与重点

按依赖分层，先地基后上层（上层 bug 需正确归因层）：

| 层 | 模块 | 重点 |
|---|---|---|
| L1 基础件 | lru, ratelimit, sha256, secrets, config, aigate_log | 锁语义、溢出、敏感数据擦除、config 边界 |
| L2 持久层 | pg_store（最大单文件） | 重连路径、SQL 注入面、事务/错误处理、ops 表一致性 |
| L3 协议/上游 | upstream_client, provider_openai/anthropic/gemini, provider_adapter | curl 复用、流式状态机、截断、token 解析 |
| L4 网关核心 | aigate_core, model_router, auth_key, usage_meter, circuit_breaker, metrics, admin_api, admin_ui, transport_civetweb, main | 管线顺序、缓存一致性、锁留、计量门控、线程池/队列容量 |

每模块过固定 7 项清单：① 职责与边界 ② 关键数据流 ③ 锁与并发（锁序、持锁调用、原子性）
④ 内存（泄漏/越界/截断）⑤ 边界条件（0/满/溢出/NULL）⑥ 性能热点与容量上界
⑦ 与已修项的交互（确认修复完整、无交叉回归）。

## 发现格式与严重度

每条发现：

```
[严重度] 模块/文件:行号 标题
  现象：<证据：代码片段或调用点引用>
  影响：<最坏情况、触发条件、频率>
  建议：<一行改法方向，不展开 patch>
```

严重度准则（写死，防止随意定级）：

- **P0 正确性/数据**：静默丢数据/错算（计量、配额、token 账）、崩溃路径、数据损坏
- **P1 安全/DoS**：外部输入可触发的拒绝服务、信息泄露、越权、无界增长
- **P2 韧性/可观测性**：故障降级、指标误导、恢复慢（不影响正确性）
- **P3 卫生**：可维护性、性能机会（无正确性风险）

去重规则：已加固 20 项 + 白名单修复 + P2 已核实条目不重报；
已修项不完整或有交叉回归面 → 单列"已修项复查"节
（例：`70ed9fb` 扩 metrics cap 后是否存在第二处硬编码 8 的遗漏）。

## 验证门禁（报告定稿前逐条过）

1. **证据完整性**：无文件:行号 + 证据（片段/调用点引用）的发现一律剔除。
2. **可达性**：P0/P1 必须走调用链（lsp references / codegraph_callers）确认
   从入口（transport handler / admin dispatch）可达，写清触发条件。
   不可达死代码只记 P3。
3. **基线去重**：对照 `.omp/plans/AIGATE_HARDENING_PLAN.md`（20 项 + P2 核实结论）与 gitmoji 加固提交历史，
   重复项移入"已修项复查"。
4. **误报控制**：每条 P0/P1 写清"为什么现有测试没抓到"
   （单模块层覆盖 / 时序依赖 / 无测试缝隙），可据此反推回归测试建议。

## 交付物

`docs/superpowers/reports/2026-09-22-aigate-full-audit.md`（审计产物单开 reports/，
不混入 specs/），结构：

1. 摘要（P0/P1 计数 + 一句总体结论）
2. 按严重度分级的发现清单（P0→P3，同级按模块排序）
3. 已修项复查
4. 性能与容量观察（热点 + 容量上界数字）
5. 方法与边界（清单、门禁、"纯静态、无负载实测"声明）

提交：`docs(audit): 📝 add full code audit design spec`（设计 spec）
与 `docs(audit): 📝 add aigate full code audit findings`（报告定稿）。

## 边界（明确不做）

- 不改任何 `src/` 代码；发现只附一行建议方向。
- 无负载测试、无吞吐/延迟实测数字。
- 不覆盖 schema.sql / Dockerfile / compose / web UI / tests 目录
  （上轮已分别验证过 schema 一致性、镜像冒烟、集成 14/14）。

## 后续

报告完成后移交 writing-plans（若用户对发现立项修复，按严重度批次出加固计划）。

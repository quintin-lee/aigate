# 宏与结构体字段注释设计（方案 A）

**日期：** 2026-09-28
**动机：** 可读性优先。前两轮已覆盖全部 static 函数、公开函数、全局变量；本轮补宏与结构体字段。
**批准方案：** A —— 按头文件分批，一次一批、独立提交。

## 1. 实测缺口结论

- 公开宏：61 处无文档（以前一行为非 `*/` 者为准）。
- 结构体字段：约 231 个成员式行，其中约 70 行已有行尾 `/* */`（豁免，9 个头文件）；真缺口约 160 行。
- 明确不动：已有文档的宏、测试目录、私有实现细节。

## 2. 范围清单

src/ 下 20 个含 `typedef struct` 的头文件全部在内：
core（aigate_core.h、config.h）、common（lru.h）、upstream（provider_adapter.h、
provider_anthropic.h、provider_gemini.h、model_router.h）、policy（auth_key.h、
ratelimit.h、budget_enforce.h、guardrails.h、circuit_breaker.h、response_cache.h）、
store（pg_store.h、redis_pool.h、usage_meter.h）、observe（event_bus.h、
health_prober.h）、server（transport_civetweb.h、admin_api.h）。

A 类（宏，`/** @brief … */` 置于 `#define` 上一行）：61 处。
B 类（字段，行尾 `/* … */`，与 9 个头文件既有 70 处风格一致）：约 160 行（扣已有）。

## 3. 格式

- 宏：单行 `/** @brief … */`；函数式宏只注定义行，续行不注。
- 字段：行尾 `/* … */`（既有风格），中文一句话用途；位域/函数指针字段注明单位与取值（如毫秒、0=关闭）。已有 `/* */` 的字段不动。
- 可配置项注明默认值；与现有风格一致，不改任何已有注释。

## 4. 验证与分批

- 按 7 个 src 子目录分 7 批，每批独立提交。
- 每批三验证：零警告构建、ctest 6/6、纯注释机检（diff 加行全为注释）。

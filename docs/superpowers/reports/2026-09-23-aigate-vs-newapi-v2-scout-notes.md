# AIGATE vs New API v2 — Scout Notes

## 证据基准
- aigate 基准 commit:`5d413ad`
- 方法:5 个只读 scout 子代理按「领域映射」表下钻;每域 ≤15 条发现,每条 = 能力描述 + new-api 文件:行 + aigate 场景是否成立预判;不写 Go 实现细节,不复制 new-api 代码(AGPL,仅锚点引用)。

## 领域映射

| 域 | new-api 承重路径(相对 new-api 根) | 入口文件 | 备注 |
|---|---|---|---|
| S1 计费 | `pkg/billingexpr/`(compile.go/run.go/expr.md)、`setting/billing_setting/`、`setting/ratio_setting/`、`model/`(quota/user 表) | pkg/billingexpr/expr.md(表达式语义文档) | 表达式引擎为正式实现,非 YAGNI 假设;quota 扣减在 relay 层 |
| S2 中继 | `relay/` + `relaykit/`(relayconvert/types/reasonmap)、`router/relay-router.go`、`relay/channel/`(每 provider 一目录,含 claude/ 原生协议) | router/relay-router.go(:78 responses 端点 + WebSocket;:127 embeddings) | responses 端点已含 WebSocket(:78 `ResponsesWebSocket`);`relay/channel/openai/chat_via_responses_test.go` 表明 chat→responses 互转存在 |
| S3 通道 | `relay/channel/`(adapter 层 + api_request.go 探测)、`model/`(channel 模型:优先级/权重)、`controller/`(channel test 端点) | relay/channel/api_request.go(channel 探测请求) | 自动降级/禁用逻辑在 relay 调度层;需 scout 确认 retry 可配性 |
| S4 任务 | `plugins/tasks/`(alibaba/doubao/google/hailuo/jimeng/kling/sora/sunoapi 各厂商任务适配器)、`pkg/jsplugin/`(JS 插件宿主)、`router/task-plugin-protocol-router.go` | router/task-plugin-protocol-router.go(:29 `openai_responses.create` 任务协议路由) | 任务=异步厂商 API 代理 + 队列;JS 插件有独立宿主 |
| S5 身份 | `service/`(authz/passkey)、`oauth/`、`middleware/`(会话/审计)、`controller/`(admin API)、`model/user.go`(用户/组/角色) | service/authz/(授权模型) | 审计日志在 middleware/logger;需 scout 确认配置变更审计面 |

## S1 计费/配额 发现
_(scout 返回后填入)_

## S2 协议中继 发现
_(占位)_

## S3 通道管理 发现
_(占位)_

## S4 任务系统 发现
_(占位)_

## S5 身份/运维 发现
_(占位)_

## aigate 侧锚点
_(Task 4 填入)_

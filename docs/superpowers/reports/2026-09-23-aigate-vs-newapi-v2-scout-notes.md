# AIGATE vs New API v2 — Scout Notes

## 证据基准
- aigate 基准 commit:`5d413ad`
- 执行记录:5 个 scout 子代理派发后 4 个撞底层 API 免费额度 429 失败、1 个(S1)长跑未回;按 plan 兜底路径,主线程顺序完成 5 域源码级下钻,以下发现全部为主线程采集(锚点已逐一验证)。

## 领域映射

| 域 | new-api 承重路径(相对 new-api 根) | 入口文件 | 备注 |
|---|---|---|---|
| S1 计费 | `pkg/billingexpr/`(compile.go/run.go/expr.md)、`setting/billing_setting/`、`setting/ratio_setting/`、`model/`(quota/user 表) | pkg/billingexpr/expr.md(表达式语义文档) | 表达式引擎为正式实现,非 YAGNI 假设;quota 扣减在 relay 层 |
| S2 中继 | `relay/` + `relaykit/`(relayconvert/types/reasonmap)、`router/relay-router.go`、`relay/channel/`(每 provider 一目录,含 claude/ 原生协议) | router/relay-router.go(:78 responses 端点 + WebSocket;:127 embeddings) | responses 端点已含 WebSocket(:78 `ResponsesWebSocket`);`relay/channel/openai/chat_via_responses_test.go` 表明 chat→responses 互转存在 |
| S3 通道 | `relay/channel/`(adapter 层 + api_request.go 探测)、`model/`(channel 模型:优先级/权重)、`controller/`(channel test 端点) | relay/channel/api_request.go(channel 探测请求) | 自动降级/禁用逻辑在 relay 调度层;需 scout 确认 retry 可配性 |
| S4 任务 | `plugins/tasks/`(alibaba/doubao/google/hailuo/jimeng/kling/sora/sunoapi 各厂商任务适配器)、`pkg/jsplugin/`(JS 插件宿主)、`router/task-plugin-protocol-router.go` | router/task-plugin-protocol-router.go(:29 `openai_responses.create` 任务协议路由) | 任务=异步厂商 API 代理 + 队列;JS 插件有独立宿主 |
| S5 身份 | `service/`(authz/passkey)、`oauth/`、`middleware/`(会话/审计)、`controller/`(admin API)、`model/user.go`(用户/组/角色) | service/authz/(授权模型) | 审计日志在 middleware/logger;需 scout 确认配置变更审计面 |

# S1 计费/配额
- 表达式计费引擎:一行表达式定义完整计费(计价/分级/缓存/图像/音频/时段折扣),expr-lang 编译 + 缓存,变量 p/c/cr/cc/cc1h/img/ai/ao 按需,价格即 $/1M 真实值 | pkg/billingexpr/expr.md:1-15, compile.go, run.go | 不成立(内部单组织需「单价×用量+缓存折扣」,YAGNI 砍表达式引擎,用 models.pricing JSONB)
- 上游无关的 token 归一化:表达式不感知上游协议(prompt 是否含 cache),系统按上游响应格式归一化 token 再求值 | pkg/billingexpr/expr.md 原则 4(Upstream-agnostic) | 部分成立(aigate um_record 已记 cached_prompt_tokens,缺的是归一化+计价)
- 表达式版本化(v1: 前缀)控制编译环境/token 归一化/配额换算公式,向后兼容演进 | pkg/billingexpr/expr.md 原则 5 | 不成立(单组织无版本兼容需求)
- 预扣配额(PreConsumeQuota):请求前预扣、成功后实际结算、信任用户可 0 预扣、订阅项单独预扣 | relay/common/billing.go:19, relay/common/relay_info.go:135-149 | 不成立(aigate 日配额 rl_reserve_tokens 后扣已够,预扣=收费场景)
- 用户余额/额度:Quota/UsedQuota/AffQuota(邀请返利)/AffHistoryQuota 在 user 表 | model/user.go:97-104 | 不成立(内部无余额/充值体系)
- 日/月配额与 per-key 限流:ModelRequestRateLimit 中间件按 model 维度限速 | router/relay-router.go:199, middleware/ | 部分成立(aigate 已有 per-key QPS token bucket + 日配额,模型维度无)

_(主线程采集于 2026-09-23;原始 S1 scout 未回,降级为主线程采集)_

# S2 协议中继
- 入站端点面:chat/completions、completions、/v1/messages(Claude 原生,RelayFormatClaude)、/v1beta Gemini 原生(:186-206)、/v1/responses(:110 responses/compact)、embeddings、audio(transcription/translation/speech)、rerank、images、moderations、alpha/search、playground/chat | router/relay-router.go:97-156, :186-206 | aigate 端点面缺口:aigate 仅 /v1/chat/completions+/v1/embeddings+/v1/models(aigate_core.c),缺 responses/messages 原生入站/audio/rerank/moderations
- /v1/responses 端点 + WebSocket:GET /responses 挂 ResponsesWebSocket(流式/长连接),POST /responses/compact 压缩 | router/relay-router.go:78, :110 | aigate 无对应(无 /v1/responses 端点)
- chat→responses 互转:chat_completions_via_responses.go 表明 OpenAI chat 可经 responses 协议中转 | relay/chat_completions_via_responses.go:171 | aigate 无对应
- 4xx 透传行为:new-api 对上游错误走 service.RelayErrorHandler,归一化到自有 OpenAI 错误壳(NewAPIError),保留 status code + 提取 error message 字段,非原样透传 body;另有 ResetStatusCode 按 status_code_mapping 改写状态码 | service/error.go:87-163 | aigate 反超(54ce3da:4xx 原样透传真实错误体,new-api 归一化会丢厂商特定 body 形状)
- 原生 Anthropic 入站:/v1/messages 直接挂 RelayFormatClaude(非 OpenAI 形态入站)| router/relay-router.go:97-99 | aigate 缺(入站仅 OpenAI 形态,出站已原生)
- 原生 Gemini 入站:/v1beta/models/{m}:action 直接挂 RelayFormatGemini | router/relay-router.go:186-205 | aigate 缺(同上)
- MJ 任务端点族(图像/视频异步任务):/mj + /:mode/mj 路由,submit/task 子路径 | router/relay-router.go:175-229 | 不追(内部无 MJ 负载)
- 未实现端点显式 404:images/variations、files、fine-tunes 挂 RelayNotImplemented | router/relay-router.go:161-172 | aigate 无需(不支持即 404)

_(主线程采集于 2026-09-23)_

# S3 通道管理
- 通道优先级/权重:Priority(*int64)+ Weight(*uint)字段,按 priority 分层调度 | model/channel.go:45-46 | 小差距(aigate model_router.c:298-369 已有 ≤8 目标优先级分层 + 4 lb_policy)
- 通道健康测试端点:GET /channel/test(全测)+ GET /channel/test/:id(单测),归 authz.ChannelOperate 权限;testChannel 打真实请求,endpointType 可配 | router/channel-router.go:47-48, controller/channel-test.go:72 | **大差距,采纳**(aigate 无通道探测入口,上游 key 失效只能等真实流量 502)
- 自动禁用(AutoBan):通道 AutoBan 标志 + AutomaticDisableChannelEnabled 全局开关,失败即禁用 | model/channel.go:46,345-349;relay/mjproxy_handler.go:586 | 部分成立(aigate circuit_breaker.c 熔断=动态版本,更细)
- 熔断器对标:new-api 无 OPEN/HALF_OPEN/CLOSED 状态机熔断器,只有 AutoBan 标志位(二态) | model/channel.go:46 | **aigate 反超**(circuit_breaker.c:15-17 CB_CLOSED/OPEN/HALF_OPEN + 成功探活)
- 延迟直方图对标:new-api 无 per-channel HDR 延迟直方图(perf_metrics 是性能计数,非请求延迟分位数) | pkg/perf_metrics/(非延迟分位) | **aigate 反超**(metrics.c um_provider_percentile_ns HDR p50/90/99)
- 重试:全局 RetryTimes(common/constants.go:137)+ 通道 pin 重试模式(PinRetrySameChannel) | common/constants.go:137;relay/relay_task.go:174-178 | 部分成立(aigate 单跳 5xx 重试 200ms 固定,new-api 可配次数)
- 4xx/5xx 区分与 failover:RelayErrorHandler 按 status 分类,5xx 触发换通道重试,4xx 归一化透传 | service/error.go:87 | 小(aigate 4xx 透传更真)

_(主线程采集于 2026-09-23)_

# S4 任务系统
- 任务协议路由:openai_responses.create / openai_image.generate|edit / openai_video.create 挂 PinTaskPluginEndpoint 中间件,未认领模型回落 Relay | router/task-plugin-protocol-router.go:29-45 | 不追(内部无多模态/视频负载)
- 任务生命周期:submit→poll→fetch 三态,PollFailures 计数连续未识别/瞬态轮询失败,系统任务 async_task_poll 按 pending 触发 | model/task.go:132,216,393 | 不追(同步代理即可,无需任务状态机)
- JS 任务插件宿主:sobek(Runtime)JS 引擎池(pool chan *runtimeInstance),插件注册任务 | pkg/jsplugin/engine.go:119-129 | 不追(单组织无插件需求)
- 各厂商任务适配器:plugins/tasks/{alibaba,doubao,google,hailuo,jimeng,kling,sora,sunoapi} | plugins/tasks/ | 不追
- 任务计费:task_pricing_setting + billingexpr 钩入任务 | setting/task_pricing_setting/ | 不追
- 单端点同步代理成本预判:若 aigate 只要 POST /v1/images/generations→厂商同步代理,改动面=新增 provider adapter(build/parse images 对)+ aigate_core.c 路由分支 + admin_api.c 模型配置,**预估 中**(3 文件,复用 um_record/upstream_client,无任务/队列/插件)| 无对应 | 建议:等真实图像需求出现再立项,当前不做

_(主线程采集于 2026-09-23)_

# S5 身份/运维
- 用户/组/角色模型:Role(int)+ Group(string)+ 各 Quota 字段,user 表按 group 过滤 | model/user.go:86,97-104,485 | 不追(单组织 key+allowed_models+QPS+日配额已覆盖)
- 预扣/信任额度体系:见 S1 | relay/common/billing.go | 不追
- 管理审计中间件:auditResponseWriter 包装响应 + beginAdminAudit/finishAdminAudit + TokenOperationAudit/AccessTokenAudit,记录 who/what/when | middleware/audit.go:25,108,124,210,265 | **部分成立,单点可采纳**(aigate 无「谁改了什么配置」审计,但可退化为 1 张审计表 + admin 端点,见下条)
- OAuth/passkey/2FA:service/authz + service/passkey + oauth/ | service/passkey/, oauth/ | 不追(单组织 admin token 族 + IP 锁留够用)
- 单点审计退化成本预判:内部只要「配置变更审计」(谁改哪个 admin 设置/何时),可退化为 aigate 侧 1 张 admin_audit_log 表(admin_action, actor_token_hash, ip, ts)+ admin_api.c 各写操作后 INSERT + 1 个 GET 查询端点,**预估 低**(复用既有 admin token hash + 锁留表模式,3 处改动)| 无对应 | 建议:P1 候选,与 P0-1 明细审计同域,可合并 schema

_(主线程采集于 2026-09-23)_

## aigate 侧锚点
| 能力 | aigate 锚点 | 备注 |
|---|---|---|
| 日配额(每 key) | src/ratelimit.c: rl_reserve_tokens / rl_reset_day;schema v5 BIGINT | 后扣,非预扣 |
| QPS 限流 | src/ratelimit.c: rl_allow_request(token bucket,懒式补领) | per-key,非 per-model |
| 日级聚合 | src/usage_meter.c um_record + 5s 批刷 usage_daily | cached_prompt_tokens 已记 |
| 明细审计 | src/schema_sql.h:96 usage_requests + src/admin_api.c GET /usage/requests + req ring | P0-1 已交付(8 提交) |
| 4xx 原样透传 | src/aigate_core.c:325-352(ubody 分支,commit 54ce3da) | new-api 是归一化壳,见 S2 |
| 熔断器 | src/circuit_breaker.h:15-17 CB_CLOSED/OPEN/HALF_OPEN + 探活 | new-api 无状态机熔断,见 S3 |
| HDR 延迟直方图 | src/usage_meter.c um_provider_percentile_ns + src/metrics.c | p50/90/99 per provider |
| 优先级分层 + 4 lb_policy | src/model_router.c:298-369(≤8 目标) | 对标 new-api Priority/Weight |
| 单跳 5xx 重试 | src/aigate_core.c failover 循环(nanosleep 200ms × 1) | new-api RetryTimes 可配 |
| 明细审计端点 | src/admin_api.c:1496 dispatch usage/requests + usage_requests_query handler | 最近 7 天默认 |
| admin 认证 | src/admin_api.c:69 LOCKOUT(128 槽,env 可调阈值)+ admin_tokens 多 token | 无用户/组/角色 |
| master-key 加密 | src/model_router.c:39,54 OPENSSL_cleanse;src/main.c:67,79 | provider key pg: 前缀 |
| 端点面 | src/aigate_core.c: /v1/chat/completions、/v1/embeddings、/v1/models + /admin/v1/* | 缺 responses/messages 入站 |
| 通道健康测试 | 无(admin 无 provider test 端点) | P1-4 候选 |

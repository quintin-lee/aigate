# AIGATE vs New API 能力差距分析 v2(源码级)

日期:2026-09-23 · 场景:内部自用(单组织)· 对标:QuantumNous/new-api

## §0 定位与证据基准

- **new-api** @ `d04c118c8803f49e0c9bab74dcf5b5efeab9464a`(2026-09-23,`~/Data/source/go/new-api` 浅克隆,1012 个 .go 文件)
- **aigate** @ `5d413ad`(本仓 `~/Data/source/c_cpp/aigate`)
- **方法**:5 域(计费/中继/通道/任务/身份)源码级下钻,发现存档于 `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`(两侧锚点)。原计划 5 个并行 scout 子代理,4 个因底层 API 免费额度 429 失败,按 plan 兜底由主线程完成全部 5 域采集(已在 notes 执行记录中说明)。
- **v1 关系**:`docs/superpowers/specs/2026-09-23-aigate-vs-newapi-gap-analysis.md`(commit `fd0e387`,证据约束为 README 级)。v2 解除该约束:new-api 侧锚点下到 `文件:行`。v1 的 16 项判定为基线,v2 逐条复核(§3 diff,无静默翻转)。
- **证据级别**:除特别标注外均为源码级;new-api 为 AGPL,v2 只锚点引用、不复制其代码。

## §1 能力矩阵 v2(16 项重判 + 4 新增)

| # | 能力 | aigate 现状(锚点) | new-api 对照(锚点) | v1 判定 | **v2 判定** |
|---|---|---|---|---|---|
| 1 | 每请求用量审计 | req ring + 5s 批刷 + `GET /usage/requests`(schema_sql.h:96,admin_api.c:1496) | 独立 log DB + 可选 ClickHouse(行为级) | 采纳 P0 | **已交付**(P0-1,§4.1) |
| 2 | 按模型成本核算 | 无 pricing 数据(usage_daily 只记量) | 表达式计费引擎,一行定义全量计费(pkg/billingexpr/expr.md:1-15) | 采纳 P1 | **采纳 P1(细化)**:v1 已假设「单价×用量+缓存折扣」,new-api 源码证实表达式引擎是**收费场景的正式形态**(版本化/归一化/信任额度均围绕转售设计)。内部单组织维持 YAGNI:models.pricing JSONB {in_mtok,out_mtok,cached_mtok_discount} 即止,不做表达式引擎(§2.1) |
| 3 | /v1/responses 端点 | 无(aigate_core.c 端点面仅 chat/embeddings/models) | POST /v1/responses + GET responses WebSocket + /responses/compact + chat↔responses 互转(relay-router.go:78,110;relay/chat_completions_via_responses.go) | 采纳 P1 | **采纳 P1(范围缩小)**:new-api 的 responses 面比 v1 认知大(WebSocket 长连接 + compaction),**首版 aigate 只实现同步 POST /v1/responses(openai 系直转),不做 WebSocket/compact**(§2.2) |
| 4 | 通道健康测试 | 无(admin 无 provider test 端点) | GET /channel/test + /channel/test/:id,打真实请求、endpointType 可配(router/channel-router.go:47-48,controller/channel-test.go:72) | 采纳 P1 | **采纳 P1(升级排序)**:源码证实 new-api 探测是「真实请求 + 按端点类型适配」,aigate 侧只需 1 个 `upstream_probe` + 1 个 admin 端点,**改动面为所有候选中最小、运维价值最高**(§5 建议下一立项) |
| 5 | Anthropic/Gemini 原生入站 | 出站已原生,入站仅 OpenAI 形态 | /v1/messages(RelayFormatClaude)+ /v1beta Gemini 原生入站(relay-router.go:97,186-206) | 暂缓 | **维持暂缓**:源码确认端点存在但只是「入站 JSON 直转 + 复用既有出站适配器」,aigate 侧 build 函数已在 provider_anthropic.c/gemini.c,工作量 v1 估计不变;触发条件不变(出现原生 SDK 客户端) |
| 6 | 图像/音频端点族 | 无 | images(edits 挂 RelayFormatOpenAIImage)+ audio 三端点 + MJ 任务族(relay-router.go:119-143,175-229) | 暂缓 | **维持暂缓**,补「单端点同步代理成本 = 中(3 文件)」预判(notes S4),可作最小化入口而非整族 |
| 7 | realtime WS / rerank | 无 | /realtime WebSocket(:85)+ /rerank(:143) | 不追 | **维持不追**(无内部场景) |
| 8 | 多用户/组/角色/OAuth/2FA | admin token 族 + IP 锁留(admin_api.c:69) | user/role/group + passkey + OAuth + 会话(model/user.go:86-104,service/passkey/,oauth/) | 不追 | **维持不追**:源码确认 new-api 身份体系全为多租户运营服务;单组织 key+allowed_models+QPS+日配额已表达全部数据面访问控制 |
| 9 | 订阅/充值/表达式计费 | 无 | 预扣配额(PreConsumeQuota,信任用户可 0)+ 用户余额(AffQuota 邀请返利)(relay/common/billing.go:19,model/user.go:97-104) | 不追 | **维持不追**:预扣/余额是收费闭环组件,无转售场景不引入 |
| 10 | JS 插件任务系统 | 无 | sobek JS 引擎池 + 任务协议路由(pkg/jsplugin/engine.go:119-129,router/task-plugin-protocol-router.go:29) | 不追 | **维持不追**:插件宿主 + 任务状态机 + 厂商适配器族,单组织纯负资产 |
| 11 | i18n 控制台 | 无 | 7 语言 | 不追 | **维持不追** |
| 12 | SQLite/MySQL 部署形态 | PG 必须 | SQLite 单文件可跑 | 不追 | **维持不追**(PG 已部署,单二进制+PG 是特性) |
| 13 | 多节点 Redis 共享限流 | 状态全内存 | wsmanager + 共享态(行为级) | 暂缓 | **维持暂缓**(触发:水平扩展/双活) |
| 14 | channel 亲和 | 无 | 通道 pin 重试模式(PinRetrySameChannel,relay/relay_task.go:174-178) | 暂缓 | **维持暂缓**:源码发现 new-api 的亲和实为「同通道重试 pin」,内部多上游收益低,结论不变 |
| 15 | playground | 无 | playground/chat 路由(relay-router.go:69) | 暂缓 | **维持暂缓** |
| 16 | 重试次数可配 | 单跳 5xx 重试 200ms 固定 | 全局 RetryTimes(common/constants.go:137,可配) | 暂缓 | **维持暂缓**,新增触发条件:上游抖动率 > 单跳覆盖时把重试次数纳入 config |
| 17 | **新增:管理操作审计**(谁改了什么配置) | 无 | admin 审计中间件(auditResponseWriter + TokenOperationAudit/AccessTokenAudit,middleware/audit.go:108-265) | — | **采纳 P1(退化形态)**:不做会话体系,退化为 1 张 admin_audit_log(actor_token_hash, ip, action, ts)+ 写操作后 INSERT + GET 查询端点(notes S5 末条,成本预判=低) |
| 18 | **新增:模型维度限流** | 限流仅 per-key | ModelRequestRateLimit 中间件按 model 限速(relay-router.go:199) | — | **暂缓**:单组织内 per-key QPS + 日配额已覆盖主要滥用面;触发:共享 key 下出现单模型打穿 |
| 19 | **新增:上游 4xx 归一化 vs 原样透传** | 原样透传真实 body(aigate_core.c:325-352) | RelayErrorHandler 归一化到自有错误壳 + status_code_mapping 改写(service/error.go:87-163) | — | **aigate 反超,维持**:new-api 归一化会丢厂商特定 body 形状;aigate 透传对排障更友好(§2.3 复核记录) |
| 20 | **新增:未实现端点显式 404** | 不支持即 404/405 | 显式 RelayNotImplemented 路由(relay-router.go:161-172) | — | **不追**:行为等价,new-api 显式 404 只为前端提示,内部无收益 |

**aigate 反超项(v2 复核后保留)**:状态机熔断器 + 成功探活(new-api 仅 AutoBan 二态)、HDR 延迟直方图 p50/90/99(new-api 无请求延迟分位)、4xx 原样透传、master-key 加密 provider key、优先级分层 failover、单二进制部署。复核锚点见 §2.3。

## §2 领域深潜

### 2.1 S1 计费/配额

new-api 的计费是**表达式契约**:`pkg/billingexpr/expr.md` 声明「one expression, one truth」——一行表达式同时定义计价/分级/缓存/图像/音频/时段折扣,expr-lang 编译缓存求值;token 变量(p/c/cr/cc/cc1h/img/ai/ao)按需 opt-in,AST 内省自动归一化;上游无关(不感知 prompt 是否含 cache,按响应格式归一);表达式带版本标签。扣减是**预扣制**(PreConsumeQuota,信任用户可 0 预扣,订阅项单独预扣,relay/common/billing.go:19),用户侧 Quota/UsedQuota/AffQuota(model/user.go:97-104)。

对 aigate:整套体系服务「转售 + 多用户余额」。内部单组织缺的只是**「按模型折算成本、出日报」**——`cached_prompt_tokens` 已入 usage_daily(um_record),缺的是单价数据与折算。v1 的「单价×用量+缓存折扣」结论经源码复核**仍成立且充分**:models 加 pricing JSONB,flush 时折算,不做表达式引擎(引擎的全部复杂度都在「多用户价格谈判 + 版本兼容」上,内部零需求)。

### 2.2 S2 协议中继

new-api 入站端点面(全部挂载于 router/relay-router.go):chat/completions、completions、**/v1/messages(Claude 原生)**、**/v1beta Gemini 原生**(:186-206)、**/v1/responses(:78 另有 GET WebSocket)+ /responses/compact(:110)**、embeddings(标准 + engines/:model 变体 :148)、audio 三端点(:132-140)、rerank(:143)、images、moderations(:156)、alpha/search(:115, Codex web search)、playground(:69)、MJ 任务族(:175-229);未实现端点显式挂 RelayNotImplemented(:161-172)。relaykit/relayconvert 持有 request/response 双注册表(17.7K/42.6K)+ 黄金测试,golden_test.go 16.4K 说明协议转换是**双向四协议**(OpenAI/Anthropic/Gemini 互转,含 chat↔responses,relay/chat_completions_via_responses.go)。

对 aigate:端点面缺口从 v1 的「缺 responses」扩展为「缺 responses + 原生 messages/gemini 入站 + audio + moderations」。但按场景裁剪:内部客户端目前全为 OpenAI-compat,**唯一有标准化趋势的是 /v1/responses**(Codex 系工具默认)。首版范围:**同步 POST /v1/responses,openai 系上游直转,其余 provider 400 unsupported_endpoint**(与 embeddings 现有语义一致);new-api 的 responses WebSocket/compact 不进首版(无内部客户端使用)。原生 messages/gemini 入站维持暂缓(触发条件不变)。

### 2.3 S3 通道管理(含反超复核)

new-api 通道模型:Priority(*int64)+ Weight(*uint)分层调度、AutoBan 标志(model/channel.go:45-46,345-349);**健康测试端点** GET /channel/test(:47)+ /channel/test/:id(:48),testChannel 打真实请求、endpointType 可配(controller/channel-test.go:72-111,unsupported 类型显式列表);自动禁用需通道 AutoBan + 全局开关双开(relay/mjproxy_handler.go:586);重试 = 全局 RetryTimes(common/constants.go:137)+ pin 同通道重试(relay/relay_task.go:174)。

**反超复核(记录在案)**:
- 熔断器:new-api 无 OPEN/HALF_OPEN/CLOSED 状态机,AutoBan 是「失败即禁用」二态标志,无冷却探活半开路径 → **aigate circuit_breaker.c:15-17 反超成立**。
- 延迟直方图:grep new-api 全仓 pkg/perf_metrics 为性能计数(非请求延迟分位数),未见 HDR/分位 per-channel 直方图 → **aigate metrics.c(um_provider_percentile_ns p50/90/99)反超成立**。
- 4xx 行为:new-api RelayErrorHandler(service/error.go:87)把上游 4xx **归一化**进自有 NewAPIError 壳(保留 status + 提取 message 字段),另可 status_code_mapping 改写; aigate(54ce3da)**原样透传**真实 body。形态差异而非优劣:透传对排障更真(厂商特定 error 形状保留),归一化对客户端一致性更好。内部排障优先 → **aigate 反超项保留**。

**采纳**:#4 通道健康测试端点——new-api 做法证实「真实探测请求 + 端点类型适配」,aigate 侧最小实现 = `upstream_probe(endpoint, key, timeout_ms)` 打 GET /models(401/403 也判 key 存活)+ `POST /admin/v1/providers/{id}/test`。

### 2.4 S4 任务系统

new-api 任务面 = 厂商异步 API 代理族:plugins/tasks/{alibaba,doubao,google,hailuo,jimeng,kling,sora,sunoapi}(各厂商 submit→poll→fetch),任务状态机带 PollFailures 连续失败计数(model/task.go:132),系统任务 async_task_poll 按 pending 触发(:393);任务协议路由把 openai_responses.create/openai_image.*/openai_video.create 挂 PinTaskPluginEndpoint,未认领模型回落普通 Relay(router/task-plugin-protocol-router.go:29-45);JS 插件宿主 = sobek 运行时池(pkg/jsplugin/engine.go:119-129)+ 独立 CLI(engine 有 cli.go)。

对 aigate:任务/插件/队列三件全不追。**单端点同步代理成本预判(notes S4 末条):中**——新增 1 个 provider adapter(images build/parse)+ aigate_core.c 1 个路由分支 + admin 模型配置,复用 upstream_client/um_record;不引入任务状态机(内部图像需求若出现,同步短轮询即可,无需 poll_failures 状态机)。

### 2.5 S5 身份/运维

new-api 身份面:user 表 Role/Group/Quota/AffQuota(model/user.go:86-104,485 按 group 过滤)、passkey(service/passkey/)、OAuth(oauth/)、会话中间件(middleware/)。**配置审计是独立中间件**:auditResponseWriter 包装响应(:25)+ beginAdminAudit/finishAdminAudit(:108,124)+ TokenOperationAudit/AccessTokenAudit(:210,265),记录 who/what/when/响应状态。

对 aigate:多租户身份全不追(§1 #8 理由)。唯一独立价值的退化形态 = **管理操作审计**(谁用什么 token、从哪个 IP、在何时改了哪个 admin 设置):1 张 admin_audit_log 表 + admin_api.c 各写 handler 成功后 INSERT(actor = 已鉴权 token 的 hash,复用既有 SHA-256 模式)+ `GET /admin/v1/audit?since=` 查询。成本 = 低(notes S5 末条),且与 P0-1 明细审计同域可合并一次 schema 版本。

## §3 判定演变(v1 → v2)

| # | 条目 | v1 | v2 | 理由 |
|---|---|---|---|---|
| 1 | 用量审计 | 采纳 P0 | **已交付** | P0-1 落地(§4.1) |
| 2 | 成本核算 | 采纳 P1 | 采纳 P1(细化) | 源码证实表达式引擎=转售形态,内部 YAGNI 结论不变,scope 写死:pricing JSONB,无表达式 |
| 3 | responses | 采纳 P1 | 采纳 P1(范围缩小) | new-api 有 WebSocket/compact,首版明确不做;只做同步 POST openai 直转 |
| 4 | 通道测试 | 采纳 P1 | 采纳 P1(**排第一**) | 源码证实探测=真实请求;候选池中价值÷改动面最高 |
| 5 | 原生入站 | 暂缓 | 暂缓(维持) | 锚点确认端点存在,工作量估计不变 |
| 6 | 图像/音频 | 暂缓 | 暂缓(补最小入口) | 新增「单端点同步代理=中成本」预判,图像需求出现时走此路径 |
| 7 | realtime/rerank | 不追 | 不追(维持) | — |
| 8 | 身份体系 | 不追 | 不追(维持) | 源码确认全为多租户服务 |
| 9 | 订阅/表达式计费 | 不追 | 不追(维持) | 预扣/余额=收费闭环组件 |
| 10 | JS 插件 | 不追 | 不追(维持) | sobek 宿主 + 任务状态机,单组织负资产 |
| 11-13 | i18n/多形态/多节点 | 不追/不追/暂缓 | 维持 | — |
| 14 | channel 亲和 | 暂缓 | 暂缓(维持) | 源码澄清:实为 pin 同通道重试,收益判断不变 |
| 15 | playground | 暂缓 | 暂缓(维持) | — |
| 16 | 重试可配 | 暂缓 | 暂缓(补触发条件) | new-api RetryTimes 可配;触发:单跳覆盖不足 |
| 17 | 管理审计 | (未列) | **采纳 P1** | v1 未覆盖:middleware/audit.go 证实 new-api 有独立审计面;aigate 退化形态成本=低 |
| 18 | 模型维度限流 | (未列) | **暂缓** | new-api 有 per-model 限速;内部 per-key 已覆盖,触发:共享 key 打穿单模型 |
| 19 | 4xx 透传形态 | 未列(散于反超项) | **反超,单列记录** | service/error.go:87 归一化 vs aigate 原样透传,排障取向保留 |
| 20 | 显式 404 | (未列) | 不追 | 行为等价,无内部收益 |

无静默翻转:16 项 v1 判定全部保留(2 项细化范围、1 项排序提前),4 项为新增(1 采纳/1 暂缓/1 反超记录/1 不追)。

## §4 状态回填

### 4.1 P0-1 每请求用量审计日志 — 已交付(2026-09-23)

8 提交链:`26be1b8`(schema v6 usage_requests)→ `9630795`(pg ops flush/query)→ `ba58430`(um_record 写 req ring + worker 批刷)→ `d60a7cd`(ring 测试)→ `22c0368`(pg fake+real 测试)→ `e4a3f4f`(GET /admin/v1/usage/requests)→ `f05d902`(端点测试)→ `bf23ca6`(余下 7 个 fake ops stub)。

验收证据:构建零告警(-Wall -Wextra -Werror)、单测 106/106、真库 PG(postgres:16)迁移幂等 + 明细 flush/query 回读、集成 smoke 5/5、新端点冒烟返回 `latency_ms` + ISO8601Z `ts` + `dropped: 0`。热路径开销:成功路径 1 次 ring 下标计算 + 字段拷贝 + 1 次 seqlock 写;失败/早退路径不写 ring(单测覆盖该不变量)。

v1 §4 P0-1 backlog 关闭。

### 4.2 硬化 20 项 — 全部完成(aigate-hardening-plan,commit `fd0e387` 之前)

逐条 grep 验证(2026-09-23 @ 5d413ad):

| # | 项 | 验证锚点 | 结果 |
|---|---|---|---|
| 1 | 每日配额翻日 | src/usage_meter.c:150-152(worker 每 flush 周期 `day != last_rollover_day` 时 `rl_reset_day`) | ✅ |
| 2 | um_unflush + worker 堆缓冲 | src/usage_meter.c:158(flush 失败 unflush),:386 um_unflush,:433 表满丢弃 warn;usage_meter.h:56 | ✅ |
| 3 | PG 重连 | src/pg_store.c:207,1339(`PQstatus != CONNECTION_OK` 重连) | ✅ |
| 4 | 请求体上限 413 | src/transport_civetweb.c:26,174-180(content_length > cap → 413);config.c:85-88 AIGATE_MAX_BODY_BYTES (0,1GiB] | ✅ |
| 5 | 上游 4xx 透传 | src/aigate_core.c:405-409,568,795(`!is_failover && status >= 400` → `aigate_write_json(rc, status, ubody, ulen)`) | ✅ |
| 6 | 流式 cb_record_success 移位 | src/aigate_core.c:361,638,752(成功分支) | ✅ |
| 7 | 客户端断开传播 | src/provider_anthropic.c:344,596(`b->aborted`,`return b->aborted ? -1 : 0`);provider_gemini.c 多 return -1 路径 | ✅ |
| 8 | 明文 key 门禁 | src/admin_api.c:907(`!adm->allow_plaintext_keys` 拒绝),admin_api.h:20,transport_civetweb.c:374(getenv AIGATE_ALLOW_PLAINTEXT_KEYS) | ✅ |
| 9 | admin 锁留 + client_ip | src/admin_api.h:37(client_ip 参数),admin_api.c:1473-1483(lockout_hit/fail),:69 LOCKOUT_SLOTS | ✅ |
| 10 | 未知 key 负缓存 | src/auth_key.c:39(neg LRU cap 256),:111-114(故障期不转 401 注释) | ✅ |
| 11 | /v1/models 限流 | src/aigate_core.c:137-153 区(rate limit 块在 /v1/models 之前,注释明示) | ✅ |
| 12 | 路由失败不缓存 | src/model_router.c:165-185(rc != 0 不 lru_put,注释「nothing is cached」) | ✅ |
| 13 | metrics ACL 默认收紧 | .env.example:13 `AIGATE_METRICS_ACL=127.0.0.1`;docker-compose 默认 127.0.0.1 | ✅ |
| 14 | 流式缓冲 8192 | src/provider_openai.c:195,provider_anthropic.c:419-523(sse[8192]) | ✅ |
| 15 | schema v5 | schema/schema.sql:78-83 + src/schema_sql.h:84-89(BIGINT quota + ck_models_name_len ≤127) | ✅ |
| 16 | master key 擦除 | src/model_router.c:39,54 + src/main.c:67,79(OPENSSL_cleanse) | ✅ |
| 17 | admin UI 无文件覆盖 | src/admin_ui.c 31 行(无 fopen/stat/override 路径,恒定内嵌) | ✅ |
| 18 | AIGATE_USAGE_FLUSH_S | src/main.c:76(cfg.usage_flush_s);config.c:78-81 [1,3600] | ✅ |
| 19 | CMake  sanitizer | CMakeLists.txt:56-59(option AIGATE_SANITIZERS) | ✅ |
| 20 | curl 复用 | src/upstream_client.c:20(pthread_key g_curl_tkey),:36 | ✅ |

## §5 路线图

候选池 = {P1-2 成本核算, P1-3 /v1/responses, P1-4 通道健康测试, #17 管理审计(新增), #6 图像单端点(暂缓池预演)}。

| 候选 | 场景价值(1-5,依据) | 改动面(依据) | 价值÷改动 | 依赖 | 建议 |
|---|---|---|---|---|---|
| **P1-4 通道健康测试** | **5**:new-api 证实探测=真实请求(channel-test.go:72);aigate 上游 key 失效目前只能等真实流量 502,运维第一痛点 | **小**:upstream_client.c 1 个 `upstream_probe` + admin_api.c 1 个端点 + 1 测试文件,2-3 个文件 | **最高** | 无 | **下一立项** |
| #17 管理审计 | 4:谁改了什么配置,排障/合规第二痛点;退化形态成本=低(notes S5) | 小-中:1 张 admin_audit_log 表(schema v7)+ 各 admin 写 handler 后 INSERT + 1 查询端点,~4 文件 | 高 | 可与 P1-2 合并一次 schema v7 | P1-4 之后;与 P1-2 合批(同域 schema) |
| P1-2 成本核算 | 4:成本日报;new-api 表达式引擎源码证实不做引擎是对的(YAGNI,§2.1) | 中:models.pricing JSONB + um flush 折算 + admin cost 端点,~4 文件 | 中 | 与 #17 合并 schema v7 | P1-4 之后 |
| P1-3 /v1/responses | 3:客户端标准化趋势,但内部当前全 OpenAI-compat,无迫切客户端 | 中:provider_openai.c build/parse_responses + aigate_core.c 路由 + 4xx/um/cb 接入,~3 文件(WebSocket/compact 明确不做,§2.2) | 中 | 无 | 等出现使用 Responses API 的客户端再立项(避免投机) |
| #6 图像单端点 | 2:无内部图像负载 | 中(3 文件,notes S4) | 低 | 无 | 触发式立项(真实需求出现) |
| 暂缓池(#18 模型维度限流、#5 原生入站、#16 重试可配) | — | — | — | 各自触发条件见 §1 | 不立项,条件出现时重评 |

**结论**:下一项 = **P1-4 通道健康测试**(唯一「价值 5 + 改动面最小 + 零依赖」候选);之后 P1-2 与 #17 合并为一次 schema v7 批次(pricing + admin_audit_log 同域)。P1-3 responses 与 #6 图像为触发式立项,不进主动排期。

## §6 自查

- [x] §1 矩阵 20 行(v1 16 项 + 4 新增)全覆盖,翻转/细化均带理由(§3 20 行逐条对应,无静默翻转)
- [x] 两侧锚点:「采纳」项(#4 通道测试、#17 管理审计、P1-2 成本核算)齐 aigate 锚点(admin_api.c 端点面/usage_daily)+ new-api 锚点(channel-test.go:72 / middleware/audit.go:108 / billingexpr expr.md);「不追」项(#8/#9/#10 等)附 new-api 源码锚点(model/user.go:86-104, service/passkey/, pkg/jsplugin/engine.go:119)+ aigate 基线说明
- [x] 反超项复核记录于 §2.3:熔断器(model/channel.go:46 仅 AutoBan 二态 vs aigate circuit_breaker.h:15-17 三态)、HDR 直方图(pkg/perf_metrics 非请求延迟分位 vs aigate um_provider_percentile_ns)、4xx 透传(service/error.go:87 归一化壳 vs aigate aigate_core.c:405-409 原样透传)
- [x] §5 排序逐行附价值/改动依据(价值 1-5 评分带理由,改动面落到文件级)
- [x] scout notes 已存档(docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md,含 429 降级执行记录)
- [x] 克隆基准 hash 记录于 §0(new-api d04c118 / aigate 5d413ad)

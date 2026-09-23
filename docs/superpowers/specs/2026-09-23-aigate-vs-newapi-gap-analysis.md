# aigate vs New API 能力差距分析

日期:2026-09-23 · 场景:内部自用(单组织)· 对标:QuantumNous/new-api(v0.13.x,Go,AGPL-3.0)

## §0 定位声明

两者不是同一物种:

| | aigate | new-api |
|---|---|---|
| 形态 | C17 单二进制,~46 源文件,civetweb+libpq+curl | Go 平台 + React 控制台,2800+ 文件 |
| 物种 | 轻量路由网关:协议转换 + failover + 限流 + 配额 | AI 资产运营平台:多租户账号 + 计费 + 控制台 + 插件 |
| 存储 | PostgreSQL 必须 | SQLite/MySQL/PG 任选,日志可外置 ClickHouse |
| 节点 | 单进程,状态全内存(rl/锁留/熔断) | 可多节点(Redis 共享限流/会话) |

aigate 的强项:new-api 没有的熔断 + 优先级分层 failover、HDR 延迟直方图、master-key 加密 provider key、`-Wall -Wextra -Werror` 承重加固。本报告**不预设「aigate 要追平 new-api」**,逐条差距在「内部单组织」场景下判三态:

- **采纳**:场景内收益高、改动可控 → 进 §4 候选 backlog
- **暂缓**:有收益但依赖外部决策/工作量不可控 → 记录触发条件
- **不追**:场景不成立(多租户专属)→ 记录理由,防重复争论

证据约束:aigate 侧锚点 = 本仓库 `文件:行`;new-api 侧 = 官方 README Capabilities/部署文档 + 仓库目录(行为级描述,不深入其 Go 实现)。

## §1 能力矩阵总表

### ① 协议 / 端点

| 能力 | aigate 现状 | new-api 对照 | 差距结论 |
|---|---|---|---|
| OpenAI Chat(流式) | ✅ `/v1/chat/completions`,`aigate_core.c:462` 流式开关 | ✅ 同 | 无 |
| OpenAI Responses | ❌ 无端点(`aigate_core.c` 仅 chat/embeddings/models) | ✅ `POST /v1/responses` | **大** |
| Anthropic Messages 原生端点 | ❌ 仅 OpenAI 形态入站;出站已是原生 Anthropic 协议(`provider_anthropic.c:14`) | ✅ `POST /v1/messages` 入站 | 中(取决于客户端) |
| Gemini 原生端点 | ❌ 同上,出站原生(`provider_gemini.c:14` 支持 gemini/google) | ✅ `/v1beta/models/{m}:generateContent` | 中 |
| 跨协议互转 | 单向:OpenAI 入站 → OpenAI/Anthropic/Gemini 出站 | ✅ RelayKit 四协议双向 | 中 |
| 实时 WebSocket | ❌ | ✅ `/v1/realtime` | 低(内部) |
| 图像/TTS/STT/rerank | ❌ | ✅ 端点族 + JS 任务插件 | 低(内部) |
| embeddings | ✅ `aigate_core.c:260`(仅 openai 系适配器有 `build_embeddings`) | ✅ | 小(aigate 覆盖面窄) |

### ② 路由与韧性

| 能力 | aigate 现状 | new-api 对照 | 差距结论 |
|---|---|---|---|
| 多目标权重/优先级 | ✅ 每模型 ≤8 目标,`model_router.c:298-369` 优先级分层 + 4 种 lb_policy | ✅ channel 优先级/权重 | 小 |
| 熔断器 | ✅ `circuit_breaker.c` OPEN/HALF_OPEN/CLOSED + 成功探活 | 无同类(靠 channel 测试/降级) | **aigate 反超** |
| 4xx 透传 / 5xx failover | ✅ 4xx 原样透传(54ce3da),5xx 单跳重试 200ms | ✅ 重试策略可配 | 小(aigate 重试次数固定 1) |
| channel 健康检查 | ❌ 无 admin 端点探测上游 | ✅ 控制台 "run a channel test" | **中** |
| channel 亲和 | ❌ | ✅ | 低(内部) |
| 多节点一致性 | ❌ 限流/锁留/熔断均进程内存 | ✅ Redis 共享 | 暂缓(单节点) |

### ③ 计费与配额

| 能力 | aigate 现状 | new-api 对照 | 差距结论 |
|---|---|---|---|
| 每日 token 配额 | ✅ `api_keys.daily_token_quota`(v5 BIGINT),`rl_reset_day` 随 `usage_meter` 翻日 | ✅ 配额 + 订阅 + 充值 | 小 |
| QPS 限流 | ✅ token bucket 每 key | ✅ | 无 |
| 按模型计价/成本核算 | ❌ 无任何 price 数据(`schema.sql` 无 price 表) | ✅ 表达式分级定价 + 缓存计费 | **大** |
| 缓存 token 计量 | ✅ `cached_prompt_tokens` 入 `usage_daily` | ✅ 缓存专项计费 | 小(aigate 只记不算钱) |

### ④ 身份与权限

| 能力 | aigate 现状 | new-api 对照 | 差距结论 |
|---|---|---|---|
| 数据面 key | ✅ hash 存储 + `allowed_models` + 过期 + QPS + 日配额(`schema.sql:api_keys`) | ✅ API key 细粒度限制 | 小 |
| admin 认证 | 单 token 族(`admin_tokens` 可多 token)+ 锁留(`admin_api.c`,env 可调阈值) | 用户/角色/组 + OAuth/OIDC + passkey + 2FA + 会话 | **大**(多租户) |
| 审计(谁改了什么配置) | ❌ | ✅ 用量日志 + 审计 | 中 |

### ⑤ 运维与控制台

| 能力 | aigate 现状 | new-api 对照 | 差距结论 |
|---|---|---|---|
| Web 控制台 | 单页内嵌 `web/admin.html`(117KB,CRUD 面板) | React 19 全功能控制台 + playground | 中 |
| 指标 | ✅ `/metrics` Prometheus 文本 + HDR p50/90/99 per provider(`metrics.c`) | 用量/成本页(聚合视图,行为级描述) | aigate 反超 |
| 上游探测 | ❌ | ✅ channel test | 中(同②) |
| i18n | 无 | 7 语言 | 不追 |
| 插件任务系统 | 无 | JS 插件 image/video 任务 | 不追 |
| 部署形态 | PG 必须,单二进制 | SQLite 单文件可跑 | 不追(PG 已部署) |

## §2 关键域展开(内部自用排序)

### 2.1 每请求用量审计日志 —— 采纳(P0)

new-api 把每次请求落日志(独立 log DB,可选 ClickHouse),支撑用量追溯与成本归因。aigate 只有 `usage_daily` 日级聚合:某 key 今天烧了多少 token 只知道总量,无法回答「哪一次请求/哪个模型/哪个 provider 贡献的、状态码分布」。内部场景下这是排障与成本归因的第一缺口。

差距实质:缺明细,非缺总量。aigate 已有 `um_record` 热路径与 5s 批刷模式(`usage_meter.c`),补一张 `usage_requests` 明细表(批刷、ring buffer 溢出丢弃 + warn)即可复用既有模式,不引入新架构。

### 2.2 按模型成本核算 —— 采纳(P1)

new-api 用表达式分级定价(含缓存折扣)算钱;aigate 对 `price` 零感知——`cached_prompt_tokens` 已经记了但只入聚合表,无法折算。内部自用不需要「向用户收费」,但需要「按模型折算内部成本、出日报」:每模型配 input/output/cache 单价(每 Mtok),`usage_daily` 增加 cost 列,admin 端出成本报告。表达式引擎是收费场景过度设计,内部用「单价 × 用量 + 缓存折扣系数」足够(YAGNI)。

### 2.3 `/v1/responses` 端点 —— 采纳(P1)

OpenAI Responses API 正成为客户端默认形态(new-api 首推端点之一;aigate 完全没有,`aigate_core.c` 端点面仅 3 个)。缺口实质:openai 系适配器缺 `build_responses/parse_responses` 一对函数 + 路由分支。Gemini/Anthropic 后端是否支持 responses 语义不承诺——首版只保证 openai 系直转,其余 400 `unsupported_endpoint`(与 embeddings 现有语义一致,`aigate_core.c:271`)。

### 2.4 provider 通道健康检查 —— 采纳(P1)

new-api 控制台可逐 channel 发起探测; aigate 的 `providers` 表有 endpoint+key 但无任何「测一下」入口——上游 key 失效只能等真实流量报 502。补 `POST /admin/v1/providers/{id}/test`:复用 `upstream_call_ext` 打最小 `/models` 或 ping,返回状态码 + 延迟。改动面小(admin_api.c + upstream_client.c),运维价值高。

### 2.5 Anthropic Messages / Gemini 原生入站端点 —— 暂缓

出站已是原生协议(§①),缺的只是入站端点:让 Claude SDK / Gemini SDK 直连网关。触发条件:内部出现原生 SDK 客户端(OpenAI-compat 客户端目前全覆盖)。届时 `provider_anthropic.c:14` 的 build 函数已存在,主要是路由 + 入站 JSON 直转,工作量中等。

### 2.6 图像 / 音频 / rerank / 实时 WS —— 暂缓(图像)/ 不追(其余)

内部目前无多模态负载;图像端点族 = 新适配器 + 计费 + 流式差异,工作量最大项,等真实需求出现再立项。realtime WS 与 rerank 无内部场景。

### 2.7 多用户 / 组 / OAuth / 2FA / 订阅充值 —— 不追

单组织场景:数据面用「key + allowed_models + QPS + 日配额」已表达全部访问控制;admin 用 token 族 + IP 锁留够用。new-api 的身份体系是为多租户运营服务的(账号↔组↔channel 授权矩阵),内部自用引入 = 整张用户表 + 会话 + 登录 UI,纯负资产。触发条件:向外部团队开放。

### 2.8 多节点 / Redis 共享限流 / SQLite 形态 / i18n / JS 插件 —— 不追(暂缓多节点)

aigate 状态全内存(rl/锁留/熔断)是**有意的单节点架构选择**:单二进制部署本身就是卖点。触发条件:需要水平扩展或双活。Redis 引入会破坏零外部依赖的部署面,届时整体重评。

## §3 取舍判定汇总

| # | 差距项 | 判定 | 理由(内部自用) |
|---|---|---|---|
| 1 | 每请求用量审计日志 | **采纳 P0** | 成本归因/排障第一缺口;复用 um 批刷模式 |
| 2 | 按模型成本核算(单价×用量+缓存折扣) | **采纳 P1** | 内部成本日报;表达式引擎 YAGNI |
| 3 | `/v1/responses` 端点(openai 系) | **采纳 P1** | 客户端标准化趋势;openai 系适配器增量小 |
| 4 | provider 通道健康检查端点 | **采纳 P1** | 上游 key 失效主动发现;改动面最小 |
| 5 | Anthropic/Gemini 原生入站端点 | 暂缓 | 待原生 SDK 客户端出现;出站已原生 |
| 6 | 图像/音频端点族 | 暂缓 | 无内部多模态负载;最大工作量项 |
| 7 | realtime WS / rerank | 不追 | 无场景 |
| 8 | 多用户/组/角色/OAuth/2FA | 不追 | 单组织:key + allowed_models + QPS + 日配额已覆盖访问控制 |
| 9 | 订阅/充值/表达式计费 | 不追 | 无转售场景;成本核算见 #2 |
| 10 | JS 插件任务系统 | 不追 | 多租户运营能力 |
| 11 | i18n 控制台 | 不追 | 内部单语言 |
| 12 | SQLite/MySQL 部署形态 | 不追 | PG 已部署;单二进制+PG 是特性 |
| 13 | 多节点 Redis 共享限流 | 暂缓 | 触发条件:水平扩展/双活需求 |
| 14 | channel 亲和 | 暂缓 | 内部多上游收益低 |
| 15 | playground | 暂缓 | 收益中等、UI 成本最高 |
| 16 | 重试次数可配 | 暂缓 | 现状单跳重试够用;出问题时再配 |

aigate 反超项(保持,不回退):熔断器 + 探活、HDR 延迟直方图、4xx 原样透传、master-key 加密 provider key、优先级分层 failover、单二进制部署。

## §4 候选 backlog(仅采纳项,文件级改动面)

### P0-1 用量审计日志(§2.1)
- `schema/schema.sql` + `src/schema_sql.h`:v6 加 `usage_requests`(key_id, model, provider, http_status, prompt/completion/cached_tokens, latency_ns, ts;按天分区或索引 ts)
- `src/usage_meter.{c,h}`:热路径 `um_record` 追加明细到 ring(溢出丢弃 + warn,与现 unflush 语义对齐),worker 批刷 `pg_ops->flush_usage_requests`
- `src/pg_store.{c,h}`:新 op `flush_usage_requests` + 查询 op(按 key/时间范围)
- `src/admin_api.c`:`GET /admin/v1/usage?key_id=&since=` 明细查询端点
- 测试:test_usage_meter(批刷/溢出)、test_pg_store(fake+real)、test_admin_api

### P1-2 成本核算(§2.2)
- schema v6(与 P0 同版,见 §4 尾注):`models` 增 `pricing` JSONB({in_mtok,out_mtok,cached_mtok_discount},可空 = 未计价)
- `src/admin_api.c`:model create/patch 收 pricing;`GET /admin/v1/usage/cost?since=` 出日级成本报告
- `src/usage_meter.c`:`usage_daily` 增 cost 列,flush 时按 pricing 折算(pricing 缺失记 0)

### P1-3 `/v1/responses` 端点(§2.3)
- `src/provider_openai.c`:`build_responses`/`parse_responses`(流式含 delta 合并;工具调用事件映射)
- `src/aigate_core.c`:路由分支 + 4xx 透传 + um/rl/cb 接入(与 chat 同构,复制 54ce3da 模式)
- `src/transport_civetweb.c`:端点注册
- 测试:test_provider_openai + test_aigate_core(mock 上游 200/400/5xx 三态 + 流式)

### P1-4 provider 通道测试(§2.4)
- `src/upstream_client.c`:新增 `upstream_probe(url, key, timeout_ms, &status, &lat_ns)`(最小 GET /models,可 401/403 判 key 存活)
- `src/admin_api.c`:`POST /admin/v1/providers/{id}/test` → {ok, status, latency_ms, error?}
- 测试:test_upstream_client(mock 200/401/超时)+ test_admin_api

**不做(记录理由)**:P1 项之间无依赖可并行;P0 先行——成本核算(P1-2)复用 P0 的 pricing 表结构,先落明细再叠加计价,避免 schema 两次 v6。

## 验收标准(本报告)

- [x] 矩阵每行可定位两侧证据(aigate `文件:行` / new-api README 章节)
- [x] §3 无悬空条目:16 项全部三态判定 + 理由
- [x] §4 仅含「采纳」项,文件级非任务级
- [ ] 用户审阅本报告后,对 §3 判定无异议 → 从 §4 切 P0 立项 spec

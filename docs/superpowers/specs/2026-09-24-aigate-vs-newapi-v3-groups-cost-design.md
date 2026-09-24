# aigate vs new-api v3:内部多租户 SaaS 重判 + P1-5 部门组/成本分摊设计

日期:2026-09-24 · 视角:**内部多租户 SaaS**(单组织、多部门、成本分摊;不上公网注册/充值/OAuth/JS 插件)
本文件含两部分:§1-§2 为差距分析(v3 增量重判),§3-§5 为排序第一候选项 P1-5 的完整设计。

## §0 定位与证据基准

- **new-api** @ `d04c118`(2026-09-24 `git fetch` 确认无新 commit,与 v2 基准一致,`~/Data/source/go/new-api`)
- **aigate** @ `edaf4d7`(2026-09-24,含 20 项 P0/P1 强化 + P0-1 用量审计 + P1-4 通道探测 + 本次 clang-format 收口)
- **v2 基线**:`docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md`(commit `2864f7f` @ aigate `5d413ad`)。v3 **不翻转 v2 的 15 行维持项**,只对内部 SaaS 视角下会翻转的 3 域(#8/#9/#13)重判,并把首项设计落进本 spec。
- **证据级别**:new-api 侧锚到 文件:行;aigate 侧锚到当前 HEAD。v2 已判「反超项」(熔断三态 / HDR 直方图 / 4xx 原样透传)本次复核维持,不再重复论证。

## §1 三域重判(内部 SaaS 视角)

| 域 | new-api 现状(`d04c118` 锚点) | aigate 现状(`edaf4d7` 锚点) | v2 判定 | **v3 重判** |
|---|---|---|---|---|
| #8 身份/组/角色 | 4 角色 Guest0/Common1/Admin10/Root100(`common/constants.go:193-200`);每用户 `group` varchar(`model/user.go:100`);用户管理 + SearchUsers group/role 过滤(`model/user.go:452-485`);OAuth/passkey/2FA | 仅 `admin_tokens` + IP 锁留(`admin_api.c` `LOCKOUT_SLOTS`),API key 是唯一数据面身份;无用户/组原语 | 不追 | **分裂**:公共身份栈(注册/OAuth/passkey/2FA/多角色)**维持不追**——内部 SaaS 无公网注册,单 admin token + 锁留已覆盖管理面;但**「组」作为 API key 的分组原语 → 翻转采纳(P1)**:成本分摊需要一个挂成本的原语,aigate 今天没有,这是本 spec 的 P1-5 |
| #9 计费/成本 | billingexpr 表达式引擎「一行定义全量计费」(`pkg/billingexpr/expr.md:1`);Pre/PostConsumeQuota + billing_session settle/refund(`service/billing.go:50-92`,`service/billing_session.go:37-81`);预扣/信任额度/邀请返利(`model/user.go:1380` `DecreaseUserQuota`) | 每 key 仅 `daily_token_quota`(**量,非成本**,`api_keys` schema);P0-1 `usage_requests` 每请求审计表;无 pricing、无成本报表 | 采纳 P1(pricing JSONB,不做表达式引擎) | **维持采纳,细化**:表达式引擎 YAGNI 结论不变(v2 §2.1);内部 SaaS 止于**部门成本分摊报表**,不做预扣/余额/返利/信任额度(那是公网转售闭环)。触发条件改为「跨部门成本归集」 |
| #13 隔离/共享态 | Redis:限流中间件(`middleware/model-rate-limit.go`)、auth session(`service/auth_session`)、channel 亲和缓存(`controller/channel_affinity_cache.go`) | 状态全内存:rl 桶 / LRU / 熔断 / um 累加器,**单节点** | 暂缓 | **维持暂缓,补显式张力**:若未来做组级硬配额(P1-5b)且跑多实例,内存态配额会各自超发。spec 锁定假设:**P1-5 在单实例假设下实现;「组配额 + 多实例一致」= Redis 共享态的触发条件**,两者合批立项 |

**不翻转的 15 行**(realtime/rerank、图像/音频、任务族、多用户运营、i18n、SQLite/MySQL 形态、通道亲和、原生入站等)维持 v2 §1 判定与触发条件,不重新取证。

## §2 内部 SaaS 路线图(重排)

| 序 | 候选 | 场景价值(依据) | 改动面 | 结论 |
|---|---|---|---|---|
| **1** | **P1-5 部门组 + 成本分摊**(groups + pricing + 报表) | 5:内部 SaaS 立身之本「钱花在哪个部门/模型」;new-api 用 user.group + billingexpr 做到,内部版退化到「组 + 报表」 | 中:schema v7 + admin 端点群 + 1 报表端点,热路径零改动 | **首项,设计见 §3** |
| 2 | P1-5b 组级月度硬配额(超额 429) | 4:分摊之后自然要限额 | 中:rl 组桶 + 热路径挂组配额 | P1-5 之后同域合批;若需多实例一致则并入 Redis 共享态批次 |
| 3 | 建 key 审批流 | 3:单 admin token 下审批人语义未定义 | 小 | 触发式(出现多管理员需求) |
| 4 | P1-3 /v1/responses、#6 图像单端点 | 同 v2 §5 | 中(3-4 文件) | 触发式,不进主动排期 |
| 5 | #13 Redis 共享态 | 仅 P1-5b + 多实例时引入 | 大 | 触发条件见 §1 #13 行 |

## §3 P1-5 设计:部门组 + 成本分摊

### §3.1 建模路线(已定:A)

- **A(采用):单值 `group_id`** — `api_keys.group_id → groups(id)`,一个 key 挂一个部门。与 new-api 语义同构(一人一组),成本归集无歧义(组成本 = 组内全部 key 用量 × 模型定价),改动面最小。
- B:多对多 `key_groups`。成本拆分口径(均摊/按量)立刻变业务问题,内部无此 key 形态。YAGNI,否决。
- C:users/department 身份栈。公共身份栈 §1 已判不追,否决。

### §3.2 schema v7(幂等,逐字同步 `schema/schema.sql` 与 `src/schema_sql.h`)

```sql
-- Migration v7: dept groups + model pricing (internal SaaS cost attribution)
CREATE TABLE IF NOT EXISTS groups (
  id         BIGSERIAL PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS group_id BIGINT
  REFERENCES groups(id) ON DELETE SET NULL;
ALTER TABLE models ADD COLUMN IF NOT EXISTS pricing JSONB NOT NULL DEFAULT '{}';
-- pricing 形状:{"in_mtok":2.5,"out_mtok":10,"cached_mtok_discount":0.1}
-- 缺省 {} = 该模型成本未知,报表只出量不出钱(不阻塞请求路径)
INSERT INTO schema_migrations(version) VALUES (7) ON CONFLICT (version) DO NOTHING;
```

要点:
- `group_id NULL` = 未分组桶(存量 key 不动),报表单列 `"(ungrouped)"`。
- FK `ON DELETE SET NULL`:SQL 直删组不悬空 key;REST 层 DELETE 前预检成员数,有成员 → 409(见 §3.4)。
- `models.pricing` 缺失/非法 JSON → 该模型 cost 省略,量照常出;请求路径零改动。
- **cached 语义按 provider 家族不同**(公式精度差异,显式记录):
  - OpenAI/DeepSeek:`cached_tokens` 是 `prompt_tokens` 的子集(OpenAI `prompt_tokens_details.cached_tokens`;DeepSeek `prompt_cache_hit_tokens`,provider_openai.c:164-175)→ `p − cp` = 未命中输入,`cp` 按折扣计,**公式精确**。
  - Anthropic/Gemini:桥只提取 `input_tokens`/`promptTokens`(provider_anthropic.c:270-278),**不提取 cache_read_input_tokens** → 落库 `cp = 0`,且 Anthropic 的 `input_tokens` 本身不含 cache-read。公式退化为 `p×in + c×out`,**cache-read 部分(真实价 = in 的 10%)按全价 in 计 → 成本高估**,方向保守(报表偏高不偏低)。精确 Anthropic 缓存计量需桥层提取 cache_read/cache_creation,属 P2,不在本 spec。

### §3.3 数据面与热路径

- **零改动**:`aigate_handle_request`、`um_record`、flush 路径不碰 groups/pricing。成本是**查询时折算**,无热路径负担。
- 新增 `pg_ops` 成员(签名追加在 `query_usage_requests` 之后,fake ops 同步补):
  - `int create_group(void* ctx, const char* name, long* out_id)`
  - `int list_groups(void* ctx, group_rec_t* out, int cap, int* n)`(`group_rec_t { long id; char name[128]; }`)
  - `int delete_group(void* ctx, long id)`(SQL 层删组;key 自动 SET NULL)
  - `int count_keys_in_group(void* ctx, long group_id, long* n)`(409 预检)
  - `int query_cost(void* ctx, long since_s, long until_s, cost_row_t* out, int cap, int* n)`
    SQL 形如:`SELECT COALESCE(g.id,0) gid, ur.model_name, SUM(prompt_tokens) p, SUM(completion_tokens) c, SUM(cached_prompt_tokens) cp, COUNT(*) rq FROM usage_requests ur JOIN api_keys k ON k.key_id=ur.key_id LEFT JOIN groups g ON g.id=k.group_id WHERE ur.ts>=$1 AND ur.ts<$2 GROUP BY 1,2`;`cost_row_t { long group_id; char model[128]; long prompt, completion, cached, requests; }`
- `key_rec_t` 追加 `long group_id;`(0 = 未分组),`model_rec_t` 追加 `char pricing_json[1024];`
- `auth_key` 的 resolve/list 路径只多带出 group_id,**不做任何按组的访问控制**(组是记账维度,非权限维度——权限仍由 key 自身的 `allowed_models` 表达;这是本 spec 的显式语义决策)。

### §3.4 admin 端点面

分派挂在 `admin_dispatch` 现有 `strncmp(rest, "keys", 4)` 链旁(新增 `groups` 分支,与 `models`/`providers` 并列):

| 端点 | 语义 | 错误 |
|---|---|---|
| `POST /admin/v1/groups` `{name}` | 建组;name 重复 → 409 `group_exists` | 400 空名(>64 字符 400) |
| `GET /admin/v1/groups` | 列组,每行带 `key_count` | — |
| `PATCH /admin/v1/groups/{id}` `{name}` | 改名 | 404 不存在;409 重名 |
| `DELETE /admin/v1/groups/{id}` | 删组;先 `count_keys_in_group` > 0 → 409 `group_has_keys`(成员 key 保持未分组,需先移走) | 404 |
| `POST /admin/v1/keys` / `PATCH /admin/v1/keys/{id}` 扩 `group_id`(0 或缺省 = 未分组) | 挂在组上 | 404 组不存在 |
| `PATCH /admin/v1/models/{name}` 扩 `pricing`(JSONB 透传,缺省不覆盖) | 挂定价 | 非法 JSON 400 |
| `GET /admin/v1/cost?group=&from=&to=&by=model\|day` | 报表(下节) | 参数越界 400 |

`GET /admin/v1/cost` 语义(查询时折算,纯函数 `cost_from_rows`,可单测):
- 输入:`query_cost` 行 + `list_models` 的 model→pricing 映射。
- 每行 cost(美元) = `(p − cp) × in_mtok / 1e6 + cp × in_mtok × cached_mtok_discount / 1e6 + c × out_mtok / 1e6`;该模型 pricing 缺/非法 → 行内 `cost` 字段省略,量照常。
- 输出整数 **cents**(`llround(usd*100)`),避免浮点漂移;`by=day`(缺省)按 UTC 天分桶,`by=model` 按模型聚合;`group` 缺省 = 全组(含 ungrouped 桶);时间窗默认最近 30 天,上限 365 天,越界 400。
- **定价变更不追溯历史**(查询时用当前 pricing 折旧量)——内部报表可接受,写进 README/env.example 注释;要「按历史定价」是 P2。

### §3.5 错误处理与边界

- 组名 1-64 字符;键端点引用不存在组 → 404 `group_not_found`(不静默置 NULL)。
- `query_cost` 超 cap(4096 行)截断 → 返回行带 `truncated:true` 标志。
- 存量库重启:schema v7 幂等(沿用 v5 的 `ck_` DO 块先例,v5 曾有非幂等导致重启失败,commit `5555479` 已修;v7 全 `IF NOT EXISTS` + `ON CONFLICT DO NOTHING`)。
- `group_id` 在 key 列表响应中为 0 时输出 `group: null`(前端友好)。

### §3.6 测试(按仓内惯例)

- **单测**
  - `test_admin_api.c`:groups CRUD 全路径(重名 409 / 有成员 409 / 404)+ keys create/patch 带 group_id + models patch pricing 透传;fake ops 补 5 个新成员(沿用 P0-1 的 fake stub 先例)。
  - 新 `tests/unit/test_cost.c`(或并入 test_admin_api):纯函数 `cost_from_rows` 边界——缺 pricing、cached 折扣 0/0.1/1、`by=day` 跨天分桶、untagged 桶、cents 舍入(0.005 边界)。
  - ASan/UBSan 构建下全绿(沿用 `AIGATE_SANITIZERS=ON` 验收)。
- **集成**(`tests/integration/test_gateway.py` 加 1 用例,排在 lockout 用例之前,沿用 P1-4/P1-5 既有手法):建组 → 建 key 挂组 → 设 models pricing(经 admin)→ 打 2-3 个真请求(mock upstream)→ `GET /admin/v1/cost` 断言组名、量、cents 值(psql 直查 `groups`/`models.pricing` 落库形状,同 provider-key 用例手法)。provider 名带 uuid 后缀避免重跑撞名(既有先例)。

### §3.7 实施批次与验收(供 writing-plans 展开)

1. schema v7 + `pg_store.{h,c}`(5 新 op + `key_rec_t`/`model_rec_t` 字段 + fake ops 全仓补 stub)
2. `admin_api.{c,h}`:groups CRUD + 键/模型端点扩字段 + `/admin/v1/cost`(纯函数拆出可测)
3. 单测(§3.6 单测两条)+ 集成用例
4. 验收:Release 零告警 + `ctest -R unit` + ASan 构建 + 集成 18/18(17 + 新 1);gitmoji 分模块提交(feat(schema)/feat(admin)/test)

## §4 不做(YAGNI,记录理由)

- **users/department 身份栈、OAuth、passkey、2FA、多角色**——§1 #8 已判;单 admin token + IP 锁留覆盖管理面。
- **表达式计费引擎 / 预扣 / 余额 / 返利**——转售闭环组件,内部止于成本报表(§1 #9)。
- **组级硬配额(P1-5b)与 Redis 共享态**——本 spec 只铺「组 + 报表」原语;限额与多实例一致性是下一批(§2 序 2/5),触发条件显式记录。
- **`usage_requests` 保留期/分区**——表持续增长是 P2 运维题,与本设计解耦。
- **admin UI 加组/成本页**——API 先行;UI 页另立项(admin UI 目前是内嵌单页)。

## §5 自查

- [x] §1 三域判定均带两侧源码锚点;aigate 侧锚 `edaf4d7` 现行代码(已逐条 grep 复核:admin_tokens 锁留、api_keys 无 group 列、无 pricing、状态全内存)
- [x] new-api 侧锚点为 `d04c118` 现行文件(`constants.go:193-200` 角色、`user.go:100` group、`billing.go:50-92` settle、`model-rate-limit.go` redis),非 v2 转引
- [x] 「组 = 记账维度非权限维度」「不追溯历史定价」「DELETE 409 vs FK SET NULL 双轨」三处歧义点已显式定死
- [x] 首项设计可独立实施:schema v7 → pg_store → admin_api → 测试,无跨 spec 依赖;P1-5b 明确不进本批
- [x] 维持项(15 行 v2 判定)声明不翻转,避免重判污染基线
- [x] 与仓内先例一致:幂等迁移(v5 教训)、fake ops stub 惯例、集成用例排 lockout 之前、uuid 后缀、gitmoji 提交

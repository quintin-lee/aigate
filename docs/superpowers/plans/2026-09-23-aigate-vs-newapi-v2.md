# AIGATE vs New API v2 源码级差距分析实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 源码级对照 aigate 与 new-api,重判 16 项差距 + 补新发现 + 状态回填 + 路线图,产出 v2 报告与 scout notes。

**Architecture:** 浅克隆 new-api 作证据基准 → 主线程骨架映射划 5 领域边界 → 5 个只读 scout 子代理并行下钻(带 new-api 文件:行锚点)→ 主线程整合写报告。纯分析任务,不改 aigate 代码。

**Tech Stack:** git(浅克隆)、aigate 仓库内 `task` 子代理(scout)、bash/grep 验证、Markdown 报告;基准 spec `docs/superpowers/specs/2026-09-23-aigate-vs-newapi-v2-design.md`(commit `04c39b4`)。

**仓库路径:** aigate = `/home/quintin/Data/source/c_cpp/aigate`;new-api 克隆 = `~/Data/source/go/new-api`(不在 aigate 仓库内)。

---

### Task 1: 浅克隆 new-api + 记录证据基准

**Files:**
- Create: `~/Data/source/go/new-api`(克隆,git 忽略,不在 aigate 仓库内)

- [ ] **Step 1: 克隆**

```bash
mkdir -p ~/Data/source/go
git clone --depth 1 https://github.com/QuantumNous/new-api ~/Data/source/go/new-api
```

Expected: `Receiving objects: 100% ... done.`,克隆目录非空。

- [ ] **Step 2: 记录基准 commit**

```bash
git -C ~/Data/source/go/new-api rev-parse HEAD
git -C ~/Data/source/go/new-api log -1 --format=%ci
```

Expected: 40-hex hash + 日期。记下这组值,Task 2 写入 notes 文件头。

- [ ] **Step 3: 兜底路径(仅当 Step 1 失败)**

若网络不可得:改用 `web_search` + 官方 README/release notes 收集行为级证据,**跳过 Step 2**,并在 Task 6 报告 §0 显式标注「证据降级:文档级」。后续 scout 任务改为对 new-api 仓库网页(`read` URL,例如 `https://github.com/QuantumNous/new-api/tree/main/relay`)做目录级阅读,锚点标注为「仓库路径(网页级)」。本计划 Step 均按源码级写;降级时逐 Step 替换,不改变任务结构。

- [ ] **Step 4: 提交(本任务无 aigate 文件变更,不 commit)**

无 aigate 文件变更,本任务不产生 commit。

---

### Task 2: 骨架映射 + 建 scout notes 骨架

**Files:**
- Create: `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`

- [ ] **Step 1: 骨架映射(主线程,只读 new-api)**

读 new-api 顶层与各域目录树,产出「领域 → 承重路径」映射。至少覆盖 5 个域:

```bash
cd ~/Data/source/go/new-api
find . -maxdepth 2 -type d | grep -vE '\.git|node_modules|web|dist' | sort | head -60
```

再对每个候选域读入口文件(每个域 ≤3 个入口,每个入口只读前 80 行找导出/路由):
- 计费:找 `pricing`/`billing`/`quota` 目录或 `*cost*`/`*price*` 文件
- 中继:找 `relay/`(controller 的 responses/messages/chat 入口 + relay 层协议转换)
- 通道:找 channel 管理(优先级/权重/测试/降级/retry 配置)
- 任务:找 `task/`(图像/视频任务 + JS 插件 + 队列)
- 身份:找 user/group/role/session/audit(模型层 + 中间件)

产出一张表(写进 notes 文件「领域映射」节),格式:

```markdown
| 域 | new-api 承重路径(相对 new-api 根) | 入口文件 | 备注 |
| S1 计费 | ... | ... | 表达式引擎在 ... |
| S2 中继 | ... | ... | responses 端点注册在 ... |
| S3 通道 | ... | ... | channel test 在 ... |
| S4 任务 | ... | ... | JS 插件在 ... |
| S5 身份 | ... | ... | audit 日志在 ... |
```

- [ ] **Step 2: 建 notes 文件骨架**

创建 `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`,内容:

```markdown
# AIGATE vs New API v2 — Scout Notes

## 证据基准
- new-api 克隆:~/Data/source/go/new-api @ <Step 1 的 40-hex hash>(<日期>)
- aigate 基准 commit: <git -C aigate rev-parse HEAD>
- 方法:5 个只读 scout 子代理按「领域映射」表下钻;每域 ≤15 条发现,
  每条 = 能力描述 + new-api 文件:行 + aigate 场景是否成立预判;
  不写 Go 实现细节,不复制 new-api 代码(AGPL,仅锚点引用)。

## 领域映射
<Step 1 的表格>

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
```

填入真实 hash,删除占位说明中的尖括号。

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md
git commit -m "📝 docs(gap): add v2 scout-notes skeleton + new-api clone baseline"
```

Expected: `ok 1 file changed`。

---

### Task 3: 并行派发 5 个领域 scout + 归档发现

**Files:**
- Modify: `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`(S1-S5 节)

- [ ] **Step 1: 单次 `task` 调用派发 5 个 scout**

用 `task` 工具(不拆 5 次),`context` + 5 个 items,`agent: "scout"`,`i` 注明意图。
`context` 内容(整批共享,勿在 item 里重复):

```markdown
# Goal
源码级下钻 new-api(~/Data/source/go/new-api,Go,AGPL)的 5 个域,为
aigate(C17 单二进制网关,内部单组织自用)的差距分析 v2 提供带锚点发现。

# Constraints
- 只读:不得修改任何文件(含 aigate)。
- 每域 ≤15 条发现。每条格式:「能力描述 | new-api 路径:行号 | aigate 场景预判(成立/不成立/部分)」。
- 不写 Go 实现细节,不逐行翻译代码;不复制 new-api 代码块(AGPL)。
- 承重路径见 notes 文件「领域映射」表(先读
  /home/quintin/Data/source/c_cpp/aigate/docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md)。
- 路径相对 new-api 根(~/Data/source/go/new-api)。找不到某能力 → 明说「未发现该能力」并给搜过的目录,不得猜测。

# Contract
每个 scout 返回纯 Markdown 列表(按上述条格式),开头一行「# S<N> <域名>」。
```

5 个 items(`task` 字段,各含 target/关注点/验收):

1. `name: "ScoutS1Billing"`,`agent: "scout"`,`task:`
```markdown
# Target
new-api 计费/配额域:模型定价(表达式或分级单价、缓存折扣)、用户余额/额度、
扣减时机(请求前预扣?流式后补?)、充值/订阅、按 key 限流与日/月配额。
# Change
只读。从领域映射表 S1 行出发,读承重文件(定价结构、计费函数、配额中间件),
对每条能力给 new-api 路径:行号。
# Acceptance
返回以「# S1 计费/配额」开头的 ≤15 条发现列表;每条三栏齐(能力|锚点|aigate 预判)。
aigate 侧已知:usage_daily 日聚合 + 每 key 日配额(BIGINT,QPS token bucket),
无定价数据、无余额体系——预判须对照此基线。
```

2. `name: "ScoutS2Relay"`,`agent: "scout"`,`task:`
```markdown
# Target
new-api 协议中继域:RelayKit 或等价层——支持哪些入站端点(chat/messages/
responses/embeddings/realtime/image/audio/rerank)、协议转换边界(谁转谁、
单向还是双向)、/v1/responses 端点实现与事件映射、流式 SSE 处理、
上游错误透传行为(4xx 原样返回还是转 5xx)。
# Change
只读。找 controller 路由注册 + relay 层转换入口,确认每个端点实际挂载的 handler。
# Acceptance
「# S2 协议中继」开头 ≤15 条;含「aigate 反超复核」专门 1-2 条:
new-api 对上游 4xx 是否原样透传(对应 aigate 的 54ce3da 行为:4xx 透传真实错误体)。
```

3. `name: "ScoutS3Channel"`,`agent: "scout"`,`task:`
```markdown
# Target
new-api 通道管理域:channel 优先级/权重/负载均衡策略、channel 健康测试端点
(主动探测上游)、自动降级/禁用逻辑、重试策略(次数/间隔可配?)、channel 亲和。
# Change
只读。找 channel 模型 + 调度器 + test 端点。
# Acceptance
「# S3 通道管理」开头 ≤15 条;含「aigate 反超复核」专门 1-2 条:
(a) new-api 有无等价熔断器(OPEN/HALF_OPEN/CLOSED + 成功探活)——aigate 有
circuit_breaker.c;(b) new-api 有无 per-channel 延迟直方图(HDR p50/90/99)——
aigate metrics.c 有。逐条给锚点或「未发现」。
```

4. `name: "ScoutS4Tasks"`,`agent: "scout"`,`task:`
```markdown
# Target
new-api 任务系统域:图像/视频生成任务模型(异步任务 + 队列)、JS 任务插件机制
(宿主 API 面、沙箱?)、任务计费。
# Change
只读。找 task 目录(队列/插件加载器/任务状态机)。
# Acceptance
「# S4 任务系统」开头 ≤15 条;aigate 侧无任务系统(v1 判「不追」),
最后追加 1 条「单用户退化形态成本评估」:若内部只要 1 个图像端点同步代理
(不走任务/插件/队列),改动面在 aigate 侧会是哪些文件——从 new-api 实现反推,
给预判(高/中/低)+ 理由。
```

5. `name: "ScoutS5Identity"`,`agent: "scout"`,`task:`
```markdown
# Target
new-api 身份/运维域:用户/组/角色模型、OAuth/OIDC/passkey/2FA、会话管理、
审计日志(谁改了什么配置)、admin 端点面(控制台后端 API 清单级别,不下钻
React 前端)。
# Change
只读。找 user 模型 + 认证中间件 + admin 路由。
# Acceptance
「# S5 身份/运维」开头 ≤15 条;aigate 侧基线:admin token 族(可多 token)+
IP 锁留 + 单组织 key 管理(allowed_models/QPS/日配额)——预判须对照;
最后追加 1 条:「若内部只需要『配置变更审计』单点能力,new-api 的 audit 实现
可否退化为一张审计表 + admin 端点,改动面预判」。
```

- [ ] **Step 2: 验证 5 个返回**

每个返回检查:开头正确(「# S1 计费/配额」等)、条数 ≤15、每条三栏、
锚点格式为 `路径:行号`。不合格的域:对该 item 重发一次(改 task 字段补
具体约束),最多重试 1 次仍不合格 → 该域降级为「文档级」并在 notes 标注。

- [ ] **Step 3: 归档到 notes**

把 5 段返回按顺序填进 notes 的 S1-S5 节(替换占位),保留原样 + 每节末尾
加一行 `_(scout 返回于 <ISO 时间戳>)_`。

- [ ] **Step 4: 提交**

```bash
git add docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md
git commit -m "📝 docs(gap): archive 5-domain scout findings for new-api v2"
```

---

### Task 4: aigate 侧锚点核对

**Files:**
- Modify: `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`

- [ ] **Step 1: 核对 v1 16 项涉及的 aigate 锚点仍然有效**

读 v1 `docs/superpowers/specs/2026-09-23-aigate-vs-newapi-gap-analysis.md`
§1-§2,对其引用的每个 aigate 文件:行做 `grep -n` 抽查(行号漂移时只更
新 notes 中的锚点,v1 文档不改):

```bash
cd /home/quintin/Data/source/c_cpp/aigate
grep -n 'unsupported_endpoint' src/aigate_core.c | head -3   # embeddings 400 语义
grep -n 'cb_record_success\|circuit_breaker' src/aigate_core.h src/circuit_breaker.h | head -5
grep -n 'usage_requests' src/schema_sql.h | head -3          # P0-1 新表
grep -n 'lockout' src/admin_api.c | head -3
```

- [ ] **Step 2: 补 scout 发现涉及的 aigate 锚点**

对 notes S1-S5 每条「aigate 场景预判」涉及的 aigate 能力,在
`docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`
「aigate 侧锚点」节填一张表:

```markdown
| 能力 | aigate 锚点 | 备注 |
|---|---|---|
| 日配额 | src/ratelimit.c: rl_reserve_tokens/rl_reset_day | BIGINT v5 |
| 4xx 透传 | src/aigate_core.c: failover 循环 4xx 分支(commit 54ce3da) | |
| 熔断 | src/circuit_breaker.c: OPEN/HALF_OPEN/CLOSED + 探活 | |
| HDR 直方图 | src/metrics.c: um_provider_percentile_ns | |
| 明细审计 | src/usage_meter.c req ring + src/admin_api.c GET /usage/requests | P0-1 已交付 |
| master-key 加密 | src/model_router.c + src/main.c | OPENSSL_cleanse 已加 |
```

(以上为示例行;实际以 grep 结果为准补齐。)

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md
git commit -m "📝 docs(gap): add aigate-side anchor table for v2 report"
```

---

### Task 5: 写 v2 报告 §0-§3(矩阵 + 深潜 + 判定演变)

**Files:**
- Create: `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md`

- [ ] **Step 1: 报告骨架 + §0/§1**

创建文件,内容骨架:

```markdown
# AIGATE vs New API 能力差距分析 v2(源码级)

日期:2026-09-23 · 场景:内部自用(单组织)· 对标:QuantumNous/new-api

## §0 定位与证据基准
- new-api @ <hash>(~/Data/source/go/new-api);aigate @ <HEAD>
- 方法:5 域 scout(见 scout-notes)+ 两侧锚点;v1 文档(同日期,commit fd0e387)为判定基线
- 证据级别说明:源码级(除非标注降级)

## §1 能力矩阵 v2
<v1 16 项全列,每项:能力 | aigate 现状(锚点) | new-api 对照(锚点) | v1 判定 | v2 判定 | 变化理由>
<+ scout 新发现的行,标「新增」>
```

§1 重判规则:三态(采纳/暂缓/不追)沿用 v1 词汇;任何 v1→v2 变化必须写理由列;
「不追」项附触发条件;新增项同样三态判定 + 理由。

- [ ] **Step 2: §2 领域深潜**

每域一节(2.1-2.5),结构:scout 发现精选(锚点照抄 notes)+ aigate 对照 +
「差距实质」一段话。S2 节必须含 4xx 透传复核结论;S3 节必须含熔断器/HDR
两项反超复核结论(「确认反超」或「反超不成立:…证据」,引用 new-api 锚点)。

- [ ] **Step 3: §3 判定演变**

一张表:条目 | v1 | v2 | 理由。无变化的条目也要列出(标「维持」),证明 16 项全覆盖无静默翻转。

- [ ] **Step 4: 提交**

```bash
git add docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md
git commit -m "📝 docs(gap): v2 report §0-§3 (matrix, deep-dive, verdict diff)"
```

---

### Task 6: 报告 §4 状态回填(P0-1 + 硬化 20 项)

**Files:**
- Modify: `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md`

- [ ] **Step 1: §4.1 P0-1 已交付**

```markdown
## §4 状态回填

### 4.1 P0-1 每请求用量审计日志 — 已交付(2026-09-23)
8 提交:26be1b8(schema v6)→ 9630795(pg ops)→ ba58430(um ring)→
d60a7cd(ring 测试)→ 22c0368(pg 测试)→ e4a3f4f(admin 端点)→
f05d902(端点测试)→ bf23ca6(fake stubs)。
验收:106/106 单测、真库 PG(smoke + 明细回读 ISO8601Z ts)、
构建零告警(-Werror)。
v1 文档 §4 P0-1 候选 backlog 项据此关闭。
```

- [ ] **Step 2: §4.2 硬化 20 项完成清单**

对 aigate-hardening-plan 20 项逐条 grep 验证(全在 main 上已完成),
填表 `# | 项 | 验证 grep | 结果`。至少含:

```bash
grep -n 'rl_reset_day' src/usage_meter.c                 # 1 翻日
grep -n 'um_unflush' src/usage_meter.c                    # 2 unflush
grep -n 'pg_ensure_conn\|PQstatus' src/pg_store.c | head   # 3 重连
grep -n 'AIGATE_MAX_BODY_BYTES' src/config.c               # 4 body cap
grep -n 'ubody' src/aigate_core.c | head                   # 5 4xx 透传
grep -n 'allow_plaintext_keys' src/admin_api.c | head -3   # 8 明文 gate
grep -n 'client_ip' src/admin_api.h                        # 9 锁留
grep -n 'neg' src/auth_key.c | head -3                      # 10 负缓存
grep -n 'ck_models_name_len' schema/schema.sql              # 15 schema v5
grep -n 'OPENSSL_cleanse' src/model_router.c src/main.c     # 16 cleanse
grep -n 'AIGATE_SANITIZERS' CMakeLists.txt                 # 19 sanitizer
grep -n 'pthread_key' src/upstream_client.c                 # 20 curl 复用
grep -n 'METRICS_ACL' .env.example docker-compose.yml       # 13 ACL
```

每条结果列写 `已验证(<文件:行>)`。

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md
git commit -m "📝 docs(gap): v2 report §4 status backfill (P0-1 shipped, hardening 20 done)"
```

---

### Task 7: 报告 §5 路线图 + 全文自查

**Files:**
- Modify: `docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md`

- [ ] **Step 1: §5 路线图**

```markdown
## §5 路线图(内部单组织,下一立项排序)

候选池:{P1-2 成本核算, P1-3 /v1/responses, P1-4 通道健康检查, + v2 新增「采纳」项}

| 候选 | 场景价值(1-5,附依据) | 改动面(文件数/新增量级,附依据) | 价值÷改动 | 依赖 | 建议 |
|---|---|---|---|---|---|
| P1-4 通道测试 | 5:上游 key 失效无主动发现 | 小:upstream_probe + 1 admin 端点,2 文件 | 最高 | 无 | **建议下一立项** |
| ... | | | | | |
```

排序必须逐行填依据(引用 §2 锚点),不允许空栏。结论段 1-2 句:下一项做什么、
为什么。P1-2 与 P0-1 的 schema 关系注意:usage_requests 已随 v6 落地,
成本核算 pricing 若与明细表同域,优先合并为一次 schema v7(在「依赖」列写清)。

- [ ] **Step 2: 验收自查单(spec §5)**

在报告尾部加:

```markdown
## §6 自查
- [ ] §1 矩阵 16 项全覆盖,翻转均带理由(§3 对应)
- [ ] 两侧锚点:每行「采纳/不追」齐 aigate 文件:行 + new-api 文件:行(或标注文档级)
- [ ] 反超项复核结论在 §2 S2/S3
- [ ] §5 排序逐行有依据
- [ ] scout notes 已存档(commit ...)
- [ ] 克隆基准 hash 记录于 §0
```

逐项实际核对后勾选;未过项先修再勾。

- [ ] **Step 3: 最终提交**

```bash
git add docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md
git commit -m "📝 docs(gap): aigate vs new-api v2 report — source-level analysis + roadmap"
```

---

### 执行备注

- 任务 1-7 顺序执行;Task 3 内部 5 个 scout 并行(单次 `task` 调用 5 items)。
- scout 返回不合格的重试上限各 1 次;再不合格降级文档级并显式标注。
- 全程不改 aigate 代码;所有 commit 只碰 `docs/`。
- 克隆保留在 `~/Data/source/go/new-api`(spec §4:不删)。

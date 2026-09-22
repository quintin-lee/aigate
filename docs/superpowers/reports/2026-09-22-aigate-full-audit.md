# aigate 全量代码体检 — 发现清单（2026-09-22）

按 `docs/superpowers/specs/2026-09-22-aigate-full-audit-design.md` 执行：src/ 22 个 C 模块
单线程依赖序走查 + 调用点级交叉验证。基线：`5561b06`（含 20 项加固 + `70ed9fb` 白名单漂移修复）。

## 1. 摘要

- **P0 × 2**：一处全网关崩溃路径（pg key 读取对可空列解引用 NULL）、一处配额静默失效（daily_token_quota 无执行点）。
- **P1 × 3**：auth 负缓存把 PG 故障当成「key 不存在」长期缓存；admin targets 往返丢失每目标 key（静默错路由）；上游响应无上限（OOM 面）。
- **P2 × 8**：阻塞重连带锁、迁移吞错、熔断恢复双缺陷、流式 4xx 吞成 502、/v1/models 吞 PG 失败、provider 同步吞错、重定向协议未限、锁留经反代共享。
- **P3 × 12**：卫生与已文档化设计取舍。
- 总体结论：加固 20 项全部复核到位（见 §3）；新发现集中在 **libpq 读取路径（仅 fake ops 覆盖，真实 PG 未测过）** 与
  **admin/上游往返的字段一致性** 两个测试缝隙区。P0-1 在真实 PostgreSQL 部署下首个鉴权请求即崩进程，须最优先修复。

## 2. 发现清单

### P0 正确性/数据

**[P0] pg_store.c:254-260 / 284-290 — key 读取对可空列直接解引用 NULL（全网关崩溃）**
- 现象：schema（`schema/schema.sql:10-11`）中 `expires_at`/`revoked_at` 可空且无默认值。
  `pq_get_key_by_hash`（:254-255）与 `fill_key_row`（:284-285）取列后直接 `exp[0]`/`rev[0]`，
  未判 NULL。libpq 对 SQL NULL 返回 NULL 指针 → **每个未被 revoke 的 key `revoked_at` 均为 NULL
  → 首条数据面鉴权请求在 civetweb 工作线程 SIGSEGV，整个进程死亡**；`list_keys`/`get_key_by_id`
  （admin 端 :294-321、:323+）同一缺陷。对比：model 路径已用 `COALESCE(upstream_key_ref,'')`
  （:438），key 路径没有。
- 影响：真实 PG 部署下 100% 触发（crash 后未 flush 的 usage 行也丢）。触发条件：任一带
  `Authorization` 头的请求。
- 为什么现有测试没抓到：单测全用 fake ops（不走 libpq）；`tests/integration/smoke.sh:20-24`
  PG 不可达即整体 SKIP，CI 绿 ≠ 该路径测过。
- 建议：两条 SQL 改 `COALESCE(expires_at,'')`/`COALESCE(revoked_at,'')`，读取处加
  `exp != NULL && exp[0]` 防御。

**[P0] aigate_core.c:355 / 570 / 589 / 723 — daily_token_quota 只记账、不执行**
- 现象：四处 `rl_reserve_tokens(...)` 返回值全部被丢弃；数据面**没有任何**
  `rl_remaining_daily` 预检（grep 全仓仅测试引用 :67/:69/:106、test_stream_pipeline.c:224）。
  超额后 `rl_allow_request`（仅 QPS 门）仍放行 → 配额对请求零约束，功能静默失效。
- 影响：admin 可设 `daily_token_quota` 但不起作用（token 账目被截停在配额值附近、请求无限继续）
  → 计费/配额语义错误，属规格中 P0 类（配额）。
- 为什么现有测试没抓到：`test_ratelimit` 只测 rl 自身语义；`test_stream_pipeline:224` 断言的是
  记账值（988），没有「超额后 429」用例——核心管线从没有把返回值接上。
- 建议：进管线前按 `rl_remaining_daily` 预检（返回 ≤0 → 429 + Retry-After: 次日），
  保留 reserve 记账；或明确把 reserve 的 -1 作为 429 出口。

### P1 安全/DoS

**[P1] auth_key.c:114-118 — 负缓存把 PG 故障当「key 不存在」缓存**
- 现象：`get_key_by_hash` 返回非 0（含连接故障/查询错误，`pg_store.c:239-244` 与 :114-217 重连失败均返回 -1）
  时 `lru_put(akc->neg, hash, 0x1)`。neg LRU 256 槽、**无 TTL**（`lru_new(256, NULL)`，:36）、
  无全局失效钩子（`auth_key_invalidate` 仅按 hash 清，:157-167）。
- 影响：PG 抖一次，期间每个被查过的**有效** key 都进负缓存 → 恢复后这些 key 持续 401 直至
  admin 操作或 LRU 自然淘汰（进程级）。PG 故障的可用性放大面：256 个 key × 进程生命周期。
- 为什么现有测试没抓到：单测 fake ops 的「失败」= 不存在的 key，语义与连接故障混在同一个 -1。
- 建议：区分「行不存在」与「存储故障」两种返回（ops 层加区分或错误码），仅前者入 neg 缓存；
  重连成功钩子整体 `lru_invalidate` 清空 neg。

**[P1] admin_api.c:558-564 + pg_store.c:652-696 — targets 往返丢失每目标 key（静默错路由）**
- 现象：`parse_targets_array` 把 JSON `upstream_key_ref` 写入 **`tgt->upstream_key`**（:563，
  resolved 字段）；而 `serialize_targets_json` 序列化的是 **`tgt->upstream_key_ref`**（:683，
  此刻恒为 ""）。admin 创建/patch 带 targets 的模型 → PG 里每目标 key 全空。
  读回路径正确（`fill_model_row:397-399` JSON→`upstream_key_ref`），仅 admin 写入方向错。
- 影响：多目标模型设了 per-target key（多上游核心场景）→ 路由回退主 key（model_router.c:115-116）
  → 次要目标拿错/拿不到凭证，上游 401，**模型静默不可用**，无任何报错面。
- 为什么现有测试没抓到：admin 单测 fake ops 不走 PG 序列化往返；targets 用例只断言内存结构。
- 建议：:563 改写 `tgt->upstream_key_ref`（`upstream_key` 由 router 解析，admin 面不该填）。

**[P1] upstream_client.c:49-71 — 上游响应无界增长（OOM 面）**
- 现象：`append_body` 按上游响应体 realloc 翻倍增长，**无上限**（请求体有 `max_body_bytes`
  门 :170/:222，响应体没有）。非流式 `upstream_call_ext` 全部响应驻留内存。
- 影响：上游被控/误配（巨型响应、gzip 炸弹类）→ 单 worker 线程无界吃内存 → 网关 OOM。
  外部输入可达（经 provider endpoint 的任意响应）。
- 为什么现有测试没抓到：单测 mock 响应均为小 JSON。
- 建议：给 `resp_buf` 加 cap（复用 `max_body_bytes` 或独立常量），超限 `return 0` 中止传输。

### P2 韧性/可观测性

**[P2] pg_store.c:198-217 — 阻塞重连持有全局 PG 锁**
- 现象：`pq_ensure_conn` 在 `px->mtx` 内调 `PQconnectdb`（DSN 无强制 `connect_timeout`，
  取决于 operator 是否写）。PG 网络不可达时该连接阻塞接近 OS 上限，期间**所有** PG 操作
  （鉴权/路由/flush/admin）排队在同一把锁后。
- 影响：一次 PG 网络故障 = 全数据面停摆（比 PG 故障本身严重得多）。
- 建议：DSN 拼接/校验强制 `connect_timeout=5`；或锁外重连 + 锁内仅检查状态标志。

**[P2] pg_store.c:1254-1279 — 迁移吞错**
- 现象：`BEGIN/SCHEMA_SQL/COMMIT` 三个结果一律 `PQclear` 丢弃，`rc` 恒 0；失败时事务处于
  aborted 态，`COMMIT` 是 no-op，未 `ROLLBACK`。
- 影响：schema 缺失/不匹配时网关照常启动，后续 op 以随机错误面（FK 失败、列缺失）暴露。
- 建议：逐条检查 `PQresultStatus`，失败 `ROLLBACK` 并返回 -1（main 已有退出路径 :55-58）。

**[P2] circuit_breaker.c:317-320 + model_router.c:275/288 — 熔断恢复双缺陷**
- 现象：① 已 OPEN 态 `cb_record_failure` 刷新 `open_until = now + cooloff`（:319）→
  持续流量下冷却期被无限顺延，endpoint 恢复需一次流量空窗。
  ② `select_candidates` 每个 target 每请求调两次 `cb_allow_request`（健康计数 :275 + 选层 :288）：
  HALF_OPEN 探针在第一次调用被置 `probe_active`，第二次调用拒绝 → 探针从未真正发出，
  周期退化为 OPEN↔HALF_OPEN 空转，多目标模型中只要有一目标健康，tripped 目标永不重试。
- 影响：熔断恢复慢/永不恢复（多目标 + 持续流量场景），与「failover」功能背道而驰。
- 建议：OPEN 态失败不刷新（仅 HALF_OPEN 探针失败才顺延）；健康检查与选层复用同一批
  `cb_allow_request` 结果（一次调用存数组）。

**[P2] aigate_core.c:523-525 → 600 — 流式 pre-headers 4xx 吞成通用 502**
- 现象：非流式已做 4xx 原样透传（加固 #5，:722-726），但流式路径 `!headers_sent && status>=400 &&
  !is_failover` 直接 `break` 到 :600 通用 502，上游 4xx 错误体丢失。
- 影响：流式调用的上游 400/401/403 不可观测（客户端只见 502），排障/计费归因失真。
- 建议：与 #5 同构，在 break 前 `aigate_write_json(rc, status, ubody, ulen)`（headers 未发，可发任意状态）。

**[P2] aigate_core.c:146-149 — /v1/models 吞 list 失败 → 空列表**
- 现象：`ops->list_models` 返回值未检查，PG 故障时客户端拿到 `{"data":[]}` 而非错误。
- 影响：无法区分「无模型」与「存储故障」，误导调用方逻辑。
- 建议：`!= 0` 时 503 + error body（限流门之前已放行的请求，直接错误返回即可）。

**[P2] admin_api.c:917-971 — provider 模型同步吞错**
- 现象：`sync_provider_models` 对每模型 `update_model`/`create_model` 返回值全部忽略
  （:946/:950/:965），循环无计数，create/patch provider 一律报成功。
- 影响：部分模型行同步失败（FK 长度约束、连接抖动）后无信号；provider 与 models 表漂移。
- 建议：汇总失败数，`out` JSON 带 `"sync_failed": n`；n>0 时 207/409 或 200+warn 字段。

**[P2] upstream_client.c:131/311 — 重定向未限协议**
- 现象：`CURLOPT_FOLLOWLOCATION=1` 未设 `CURLOPT_PROTOCOLS`/`REPROTOCOL` → 上游 30x 可指向
  `file://`、`gopher://` 等任意 libcurl 支持协议。
- 影响：受控上游 302 → 本地文件内容被读入响应体并经网关透传 → 信息泄露面（需上游恶意/被控）。
- 建议：`CURLOPT_PROTOCOLS/REPROTOCOL = CURLPROTO_HTTP|CURLPROTO_HTTPS`。

**[P2] admin_api g_lockout + transport ri->remote_addr — 反代后锁留共享**
- 现象：锁留表 128 槽按 socket 对端 IP（FNV%128）。统一入口反代下所有 admin 流量同一 IP
  → 10 次失败锁全部门户 300s（自助式管理面 DoS；也放大了撞库成本——双刃剑，需文档化）。
- 建议：可选读 `X-Forwarded-For` 首段（需信任反代白名单配置）或把 10/300s 做成 env 可调。

### P3 卫生/文档化

| # | 位置 | 现象 | 建议方向 |
|---|---|---|---|
| P3-1 | main.c:55-58/66-68/76-78/84-88 | 早退路径未 `OPENSSL_cleanse(master)`（仅 :113 成功路径擦） | 各 return 前 cleanse 或改 static + 统一出口 |
| P3-2 | auth_key.c:35-36 | 正向 key 缓存无 TTL，PG 直改 revoke 不失效直至 LRU 淘汰 | 文档化（admin 为 source of truth）+ 可选 TTL 参数 |
| P3-3 | admin_api provider_delete | 删 provider 不清理其 auto-sync 的 models 行 → 孤儿路由指向死 provider | 级联 `delete_model` 或文档化手工清理 |
| P3-4 | metrics.c:196-215 | failover 表 128 满后新三元组静默不计数 | 满时 warn 一次 / 扩到 256 |
| P3-5 | admin_api.c:628-636 | `default_params` 1024 截断 → 存下坏 JSONB，后续读回解析失败（模型隐形消失） | 超限时 400 拒绝或扩容 |
| P3-6 | provider_*.c line_buf/sse 8192 | >8KB 单行仍丢弃（加固 #14 后已有 warn，行为未变） | 文档化：SSE 行 >8KB 的长 delta 计量缺失 |
| P3-7 | usage_meter.c:336-338 | `um_unflush` 表满丢弃（4096 槽 + 4096 行回灌理论不可达，warn 已有） | 无需改，注释标注 |
| P3-8 | model_router.c:60 | `g_rr_counter` 全 router 共享，跨模型轮转不公平 | 无害，注释即可 |
| P3-9 | upstream_client.c:27-41 | 线程退出泄漏 1 个 CURL handle/线程（civetweb 16 worker，启动后固定） | 注释标注已知取舍 |
| P3-10 | model_router resolve_key | `env:`/`pg:` 密钥首解析后冻结（缓存模型），重启才重读 | 文档化（现状即 by design） |
| P3-11 | 配额翻日 | rollover 由 flush worker 驱动，最迟 `AIGATE_USAGE_FLUSH_S`（≤3600s）生效 | 文档化最坏延迟 |
| P3-12 | transport_civetweb read_body | `Transfer-Encoding: chunked` 请求 content_length=0 → 当空体 400 | 文档化不支持；或 mg_read 兜底 |

## 3. 已修项复查（对照加固 20 项 + 70ed9fb）

| 加固项 | 状态 | 证据 |
|---|---|---|
| #1 翻日 | ✅ | usage_meter.c:117-122 worker 每周期检查 `last_rollover_day`，`usage_meter_new(ps, rl, ...)` 签名已改（core.c:36 传 `ac->rl`） |
| #2 unflush/heap | ✅ | worker 堆上 4096 行缓冲（:99）；flush 失败 `um_unflush`（:124-130）；free 时 256×多轮 final drain（:179-190） |
| #3 pg 重连 | ✅（残留 P2） | `pq_lock` 内 `pq_ensure_conn`（:212-217）全 15 处调用点覆盖；残留：阻塞带锁（P2-1） |
| #4 413 | ✅ | transport :170/:222 两处 handler 均 `content_length > max_body_bytes` → 413 |
| #5 4xx 透传 | ✅（流式残留 P2） | 非流式 :722-726 原样透传；流式 pre-headers 仍吞（P2-4） |
| #6 流式 cb_record | ✅ | 成功才记 :578（`urc==0 && headers_sent` 分支内），失败路径 :546-575 不记 success |
| #7 客户端断开 | ✅ | `stream_write_cb` 检查 on_chunk 返回 → `aborted` → return 0 中止（upstream_client.c:217-222）；anthropic/gemini feed 返回 -1 路径存在 |
| #8 key 加密门 | ✅ | `allow_plaintext_keys` 由 transport 读 env（:370-371）；create/patch provider 拒绝明文 400 |
| #9 锁留 | ✅（P2 注） | g_lockout 128 槽 + client_ip（NULL 跳过）；admin 30 处测试调用点已带 NULL |
| #10 负缓存 | ✅（P1 残留） | :108-118 命中跳过 + :114-117 失败入负缓存 + invalidate 双清（:162-166）；残留：故障与不存在混同（P1-1） |
| #11 models 限流 | ✅ | `rl_allow_request` :123-136 先于 /v1/models 分支 :137 |
| #12 路由负缓存 | ✅ | resolve 失败不入 LRU（model_router.c 修复后路径：失败直接 return -1，仅成功 lru_put） |
| #13 metrics ACL | ✅ | config.c:76 默认 `127.0.0.1`；compose/.env.example 已同步（基线提交 2585ae2） |
| #14 流式缓冲 | ✅ | 三桥 line_buf[8192] + sse 8192 截断 warn（openai:195、anthropic:417+、gemini:344/470+）；残留 >8KB 丢行（P3-6） |
| #15 schema v5 | ✅ | schema.sql:78-81 BIGINT quota + 127 约束 + v5 记录；admin 侧 128 拒绝（:592-595） |
| #16 master 擦除 | ✅（残留） | model_router_free cleanse + main.c:113；残留：早退路径未擦（P3-1） |
| #17 admin UI 文件覆盖 | ✅ | admin_ui.c 恒服务内嵌 HTML（无 web/ 目录分支） |
| #18 flush_s 可配 | ✅ | config.c:78-83 [1,3600]，main.c:79 传入 |
| #19 ASAN 选项 | ✅ | CMake `AIGATE_SANITIZERS` 已加（基线验证构建过） |
| #20 curl 复用 | ✅ | thread-local handle（upstream_client.c:27-41）+ 每次 reset options |
| 70ed9fb | ✅ | 延迟采样门控 `prov_slot` 经 `provider_find`（usage_meter.c:74-93 注释）；metrics 侧 cap 16 vs 计量侧 8，无第二处硬编码 8 遗漏 |

## 4. 性能与容量观察（静态）

- **rl_reset_day**：每次 flush 周期 O(buckets) 扫表；bucket 表按 key 数 ×2 增长，上界 = 日活 key 数（无硬顶，正常规模无压力）。
- **um_accs**：静态 4096 × ~96B ≈ 384KB；worker 行缓冲堆上 4096 × ~120B ≈ 512KB（一次）。
- **HDR 直方图**：8 provider × min 1ns/max 1h/4 位有效 ≈ 每图数 KB；无压力。
- **auth LRU**：4096 正向 + 256 负向；每 miss 一次 PG 往返（重连后）—— 单请求热点，PG 健康时命中即 0 查询。
- **CB 选择**：每请求 ≤ `2 × MAX_TARGETS_PER_MODEL(8)` 次 entry 线性查找 + 候选 O(n²) 排序，n≤8，可忽略。
- **metrics_render**：65536 mbuf 上限（transport），超限返回 500（已知行为，#20 项复查确认无第二 cap）。
- **failover 指标**：每次 failover 事件持锁 128 线性扫——仅故障期热，正常期零成本。

## 5. 方法与边界

- 走查：依赖分层 L1→L4（lru/ratelimit/sha256/secrets/config/aigate_log → pg_store →
  upstream_client/三 provider 桥/adapter → 网关核心 10 模块），每模块过 7 项固定清单
  （边界/数据流/锁并发/内存/边界值/性能容量/与已修项交互）。
- 交叉验证：CodeGraph `codegraph_callers`（rl_reserve_tokens、um_drain、cb_record_*、
  get_key_by_hash、list_models、sync_provider_models 等）+ lsp references（parse_targets_array、
  fill_key_row、pq_ensure_conn 全部调用点）+ 测试面 grep（tests/unit、tests/integration/smoke.sh）。
- 门禁：每条发现带 文件:行号 + 证据；P0/P1 全部走调用链确认从 transport/admin 入口可达并写触发条件；
  对照 `.omp/plans/AIGATE_HARDENING_PLAN.md` 20 项 + 70ed9fb 去重，已修项残留单列 §3。
- 边界：纯静态分析，无负载实测；不改 src/；schema/docker/tests 不在本轮范围
  （P0-1 的 schema 半属只引用未改）；`X-Forwarded-For` 信任模型等部署面问题仅文档化。

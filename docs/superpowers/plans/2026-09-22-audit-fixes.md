# 实施计划：aigate 全量审计修复（2026-09-22）

来源审计：`docs/superpowers/reports/2026-09-22-aigate-full-audit.md`
基线提交：`0bb9c4a`（含全部 20 项加固 + `70ed9fb`，审计 §3 已复核到位）
构建：CMake ≥ 3.16，`-Wall -Wextra -Werror`，C17。测试：`ctest -R unit`。
提交规范：gitmoji（沿用仓库既有 `<type>(<scope>): 🐛 …` 风格，见 `git log`）。

## 目标（Goal）

修复审计全部 P0×2 / P1×3 / P2×8，并完成 P3 中两项有代码后果的项
（P3-1 密钥擦除、P3-5 参数越限拒绝）；P3 其余 10 项仅注释/文档化（一个收尾任务）。
每项可独立编译 + 单测验证，TDD：先写失败测试 → 实现 → 通过 → 提交。

## 验收（Acceptance）

1. `cmake -S . -B build && cmake --build build` 零告警（-Werror）。
2. `ctest --test-dir build -R unit` 全绿，且新增用例在实现前失败（已验证红灯）。
3. ASan/UBSan 构建（`-DAIGATE_SANITIZERS=ON`）下 `ctest -R unit` 全绿。
4. `tests/integration/smoke.sh` 全绿（P0-1 的崩溃回归由 smoke 的鉴权路径覆盖）。
5. 每个任务一个提交，提交信息含 gitmoji + 审计编号（如 `P0-1`）。

## 共享决定（Shared Decisions）

| 决定 | 理由 |
|---|---|
| P0-1 只做 C 侧 NULL 防御，不改 SQL | 审计建议的 `COALESCE(expires_at,'')` 有陷阱：`''` 按 timestamptz 强转，NULL 行运行时抛 "invalid input syntax for type timestamp with time zone"。C 侧 `PQgetvalue` 判 NULL 即可完整修复 |
| `get_key_by_hash` 契约改为 0=found / 1=miss / -1=error | 仅改 key 哈希读取路径（auth 热路径）；`get_key_by_id`/`list_keys`（admin 面）保持 -1=未找到 不动，控制影响面 |
| 配额执行 = 预检门 + record-first 记账 | `rl_reserve_tokens` 改为「先记账、超配额返回 -1」，使 tokens≥quota 的单请求也能把账记上去；数据面预检用 `rl_remaining_daily <= 0` → 429 + `Retry-After: 次日 0 点`。预检放 QPS 门之后、`/v1/models` 分支之前（token 计量对全部数据面请求统一生效，与 QPS 门同位） |
| 流式 4xx 错误体用 64KB 上限缓冲 | 4xx 才累积（`status >= 400 && status < 500`）；超限丢弃尾部、保留前 64KB；`upstream_stream_call` 尾部追加 `char** out_err_body, size_t* out_err_len`，NULL=不收集（兼容现有直接调用点） |
| 熔断双调用修复 = 单次 allow 结果缓存 | `select_candidates` 每个 target 一次 `cb_allow_request`，结果存数组供选层复用；OPEN 态 `record_failure` 不再刷新 `open_until`（仅 HALF_OPEN 探针失败才顺延） |
| P2-1 追加 `connect_timeout=5` 而非锁外重连 | 最小改动面：DSN 拼接一处（open 时处理一次，`px->dsn` 全链路复用）；锁外重连要动 15 处调用点，留作后续 |
| P2-8 锁留策略只做 env 可调，XFF 仅文档化 | 信任反代白名单属部署面决策，不实现 XFF 解析；README/.env.example 加说明 |

## 任务清单

顺序即执行顺序；P0 → P1 → P2 → P3 → 验证。每个任务：写测试（红灯）→ 实现 → 跑绿 → 提交。
不可单测的项（P2-1/P2-2/P2-7 的 libpq/libcurl 行为）标注「检查验证」，并入任务 16 的集成 smoke。

---

### 任务 1 — P0-1：pg key 读取 NULL 列防御

**文件**：`src/pg_store.c`
**目标**：`pq_get_key_by_hash`（:254-260）与 `fill_key_row`（:284-290）取
`expires_at`/`revoked_at` 后直接 `exp[0]`/`rev[0]`；libpq 对 SQL NULL 返回 NULL 指针 →
真实 PG 部署下首个鉴权请求 SIGSEGV（schema.sql:10-11 两列可空且无默认值）。
**变更**：
```c
const char* exp = PQgetvalue(res, 0, 6);
const char* rev = PQgetvalue(res, 0, 7);
if (exp != NULL && exp[0] != '\0') {
    out->expires_at = (time_t)atol(exp);
    out->has_expiry = 1;
}
out->revoked = (rev != NULL && rev[0] != '\0');
```
`fill_key_row` 同构修改。不改 SQL（见共享决定）。
**测试**：libpq 读取路径单测不可达（fake ops 不走 PQgetvalue）——标注「检查验证」：
以代码评审确认两处的 NULL 守卫；崩溃回归由任务 16 的集成 smoke（真实 PG 鉴权路径）兜底。
**验收**：编译通过；`grep -n 'exp\[0\]\|rev\[0\]' src/pg_store.c` 无裸解引用残留。
**提交**：`🐛 fix(p0-1): guard nullable expires_at/revoked_at in pg key reads`

---

### 任务 2 — P0-2：daily_token_quota 执行

**文件**：`src/ratelimit.c` / `src/ratelimit.h` / `src/aigate_core.c` /
`tests/unit/test_ratelimit.c` / `tests/unit/test_aigate_core.c`
**目标**：四处 `rl_reserve_tokens` 返回值被丢弃，配额静默失效（审计 P0×2）。
**变更**：
1. `rl_reserve_tokens`（ratelimit.c:189-205）改 record-first：
```c
if (bt == NULL) {
    rc = -1;
} else {
    bt->daily_used += tokens;
    rc = (daily_quota > 0 && bt->daily_used > daily_quota) ? -1 : 0;
}
```
   更新 ratelimit.h 注释：「先记账；超配额（或 OOM）返回 -1，quota 0 恒 0」。
2. `aigate_core.c`：QPS 门（:123-136）之后、`/v1/models` 分支（:137）之前插入预检：
```c
if (krec.daily_token_quota > 0) {
    long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
    if (rem <= 0) {
        time_t now = time(NULL);
        time_t next = (time_t)(now - (now % 86400)) + 86400;
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", (long)(next - now));
        rc->set_header(rc->impl, "Retry-After", ra);
        aigate_write_error(rc, PIPE_RATE, "daily_quota_exceeded",
                           "daily token quota exceeded");
        key_rec_free(&krec);
        return 0;
    }
}
```
**测试**（先写，红灯）：
- `test_ratelimit.c` 新增 `TEST_CASE(test_rl_daily_quota_record_first)`：
  quota=100，reserve 60→rc 0、remaining 40；reserve 50→rc -1 且 daily_used=110
  （remaining == -10，证明已记账）。
- `test_aigate_core.c` 新增 `TEST_CASE(test_core_daily_quota_429)`：
  `fkey_add(&db, 0, 1, "quota-key", 0, 50, ...)`，模型指向 mock（或复用现有用例的
  模型布局），`rl_reserve_tokens(ac.rl, 1, 50, 50)` 造账后发请求 →
  `rc.status == 429`、body 含 `"daily_quota_exceeded"`、`cap_has_header(&c, "Retry-After")`。
- 更新既有 `test_rl_daily_quota`（:56-57）：`remaining` 断言由 40 改为 -10
  （旧断言钉死了 check-then 语义，随契约更新；「not applied」注释同步改写）。
**验收**：新用例红→绿；`test_stream_pipeline:224`（remaining==988）保持绿
（record-first 下 12 token 记账结果与旧行为一致）。
**提交**：`✨ feat(p0-2): enforce daily token quota with pre-check + record-first reserve`

---

### 任务 3 — P1-1：auth miss/error 区分

**文件**：`src/pg_store.h` / `src/pg_store.c` / `src/auth_key.c` + 8 个测试文件的 fake
**目标**：neg 缓存把 PG 故障（-1）当「key 不存在」长期缓存（审计 P1×1）。
**变更**：
1. `pg_store.h`：`get_key_by_hash` 契约注释改为 `0=found, 1=missing, -1=error`。
2. `pq_get_key_by_hash`（pg_store.c:246-267）：`TUPLES_OK` 且 `PQntuples == 0` →
   `rc = 1`；查询错误路径仍 `-1`。`list_keys`/`get_key_by_id` 契约不动。
3. `auth_key.c:114-118`：
```c
int rrc = akc->ops.get_key_by_hash(akc->ops_ctx, hash, &fresh);
if (rrc == 0) {
    /* …existing found path… */
}
/* only a definite miss (rrc == 1) enters the negative cache; a storage
 * error (-1) is never cached, so PG recovery clears auth automatically. */
if (rrc == 1 && akc->neg != NULL) {
    lru_put(akc->neg, hash, (void*)0x1);
}
return -1;
```
4. 测试 fake 的 missing 返回值 `-1 → 1`（执行时 grep 确认，已核对站点）：
   - `tests/unit/test_aigate_core.c` `fget_key`（:74-93）
   - `tests/unit/test_stream_pipeline.c` `fget_key`（:37-54）
   - `tests/unit/test_embeddings.c` `fget_key_cb`
   - `tests/unit/test_failover.c` `fo_get_key`
   - `tests/unit/test_provider_anthropic.c` `fget_key`
   - `tests/unit/test_admin_api.c` `fake_get_key_by_hash`
   - `tests/unit/test_pg_store.c` `fake_get_key_by_hash` + 断言 missing 处 `== -1` 翻成 `== 1`
   - 保留 `-1`（= 错误，语义仍合法）：`test_model_router.c` `mro_fail`、
     `test_usage_meter.c` `um_fail`（filler，该路径不消费 get_key_by_hash）。
**测试**（先写，红灯）`tests/unit/test_auth_key.c` 新增：
- `test_auth_neg_cache_miss_only`：fake 对未知 hash 返回 1，加 ops 调用计数；
  两次 resolve 同一 hash → 均 -1，计数 == 1（第二次命中 neg 缓存）。
- `test_auth_no_neg_cache_on_error`：fake 返回 -1（错误）；两次 resolve → 计数 == 2
  （错误不入缓存，PG 恢复后鉴权自动恢复）。
**验收**：新用例红→绿；全部既有单测绿（fake 翻转后）。
**提交**：`🔒 sec(p1-1): distinguish key-miss from storage error in auth negative cache`

---

### 任务 4 — P1-2：targets 往返保留每目标 key_ref

**文件**：`src/admin_api.c` / `tests/unit/test_admin_api.c`
**目标**：`parse_targets_array`（admin_api.c:558-564）把 JSON `upstream_key_ref` 写进
`tgt->upstream_key`（resolved 字段），`serialize_targets_json` 序列化的是
`tgt->upstream_key_ref`（恒 ""）→ admin 写入的多目标 key 全部丢失，路由静默回退主 key。
**变更**：
```c
json_t* key = json_object_get(item, "upstream_key_ref");
if (!key) {
    key = json_object_get(item, "upstream_key"); /* alias: both persist as key_ref */
}
if (key && json_is_string(key)) {
    snprintf(tgt->upstream_key_ref, sizeof tgt->upstream_key_ref, "%s", json_string_value(key));
}
```
（宽度 128 与 :592 的 admin 侧上限一致；f268146 已定 127+1 约定。）
**测试**（先写，红灯）`test_admin_api.c` 新增
`test_admin_targets_key_ref_roundtrip`：POST /models 带 2 targets（各带
`upstream_key_ref: "env:KEY_Tn"`）→ 经 fake store `get_model` 读回 →
`targets[i].upstream_key_ref` 原样保留；patch 同构（fake 侧 flag
`fail_list` 之外的既有断言面确认 fake `create_model` 存的是内存 copy，
读回即序列化字段）。
**验收**：新用例红→绿；`test_admin_api` 既有 targets 用例不回归。
**提交**：`🐛 fix(p1-2): persist per-target upstream_key_ref in admin targets write path`

---

### 任务 5 — P1-3：上游响应体上限

**文件**：`src/upstream_client.c` / `src/upstream_client.h` / `tests/unit/test_upstream_client.c`
**目标**：`append_body`（upstream_client.c:49-71）无上限 realloc 翻倍 → OOM 面。
**变更**：
1. `struct resp_buf` 不动；模块级 `static size_t g_max_resp_bytes = 32 * 1024 * 1024;`
   + 头文件 `void upstream_set_max_response_bytes(size_t cap);`（cap 0 = 不限，测试用）。
2. `append_body` 扩容循环加 cap：`while (rb->len + total + 1 > ncap) { ncap *= 2; if (g_max_resp_bytes != 0 && ncap > g_max_resp_bytes) { ncap = g_max_resp_bytes; break; } }`
   且 `if (rb->len + total + 1 > rb->cap && rb->cap >= g_max_resp_bytes) return 0;`（中止传输）。
   实现时保持一个出口：超限即 `return 0`（CURLE_ABORTED_BY_CALLBACK → 既有
   `rc = -502` 路径，caller 已按 urc != 0 处理，零改动）。
**测试**（先写，红灯）`test_upstream_client.c` 新增
`test_upstream_response_cap`：`mock_upstream_start`，`upstream_set_max_response_bytes(16)`，
`upstream_call_ext(...)` 正常 200 响应 → `rc == -502`、`out_body == NULL`；
恢复默认 cap 后同一请求 `rc == 0` 正常。
**验收**：新用例红→绿；既有 upstream 单测全绿（默认 32MB 不影响 mock 小响应）。
**提交**：`🔒 sec(p1-3): cap upstream response body to 32MB to bound worker memory`

---

### 任务 6 — P2-1：pg DSN 强制 connect_timeout

**文件**：`src/pg_store.c`
**目标**：`PQconnectdb` 无 `connect_timeout` → 网络不可达时阻塞持全局锁，全数据面停摆。
**变更**：`static void dsn_connect_timeout(const char* dsn, char* out, size_t n)`：
- 已含 `connect_timeout` → 原样拷贝；
- URI 形式（`postgres://`/`postgresql://`）→ 追 `?connect_timeout=5` 或 `&connect_timeout=5`
  （查 `strchr(dsn, '?')`）；
- 键值形式 → 追 `" connect_timeout=5"`。
`pg_store_open`（:1204）经 helper 写入 `px->dsn`，初始连接与 `pq_ensure_conn`
重连（:206）全链路复用。
**测试**：helper 为 static 且 PG 连接不可单测——检查验证（评审 + 集成 smoke 观察
连接日志）。若评审认为值得固化，可在任务 16 的 smoke 里以
`PG_DSN` 带 `connect_timeout` 的环境断言启动成功（幂等分支）。
**验收**：编译通过；URI/键值两分支各一例走查（在临时 C 脚本里跑 helper 亦可，
用完即删）。
**提交**：`🐛 fix(p2-1): append connect_timeout=5 to pg dsn (uri + keyword forms)`

---

### 任务 7 — P2-2：迁移错误上报

**文件**：`src/pg_store.c`
**目标**：`pg_store_migrate`（:1254-1279）丢弃 BEGIN/SCHEMA/COMMIT 三个
`PQresult`，`rc` 恒 0，失败时事务 aborted 且无 ROLLBACK。
**变更**：
```c
PQresult* begin = PQexec(px->db, "BEGIN");
PQresult* sch = PQexec(px->db, SCHEMA_SQL);
PQresult* commit = PQexec(px->db, "COMMIT");
int bad = (begin == NULL || sch == NULL || commit == NULL) ||
          PQresultStatus(begin) != PGRES_COMMAND_OK ||
          PQresultStatus(sch) != PGRES_COMMAND_OK ||
          PQresultStatus(commit) != PGRES_COMMAND_OK;
if (bad) {
    PQclear(PQexec(px->db, "ROLLBACK"));
    AIGATE_LOG_ERROR("pg migrate failed: %s",
                     sch != NULL && PQresultStatus(sch) != PGRES_COMMAND_OK
                         ? PQerrorMessage(px->db) : "transaction failure");
    rc = -1;
}
PQclear(begin); PQclear(sch); PQclear(commit);
```
（NULL 短路顺序保证 `PQresultStatus(NULL)` 不被求值。main.c:55-58 已有
`!= 0 → 退出` 路径，无需改动。）
**测试**：真实 PG 才可达——检查验证：评审 + 任务 16 smoke（迁移成功路径回归）。
**验收**：编译通过；smoke 启动日志无 migrate 报错。
**提交**：`🐛 fix(p2-2): report pg migration errors and roll back aborted transaction`

---

### 任务 8 — P2-3：熔断恢复双缺陷

**文件**：`src/circuit_breaker.c` / `src/model_router.c` /
`tests/unit/test_circuit_breaker.c` / `tests/unit/test_model_router.c`
**目标**：① OPEN 态 `cb_record_failure` 刷新 `open_until`（:317-320）→ 持续流量下
冷却期无限顺延；② `select_candidates` 每 target 每请求两次 `cb_allow_request`
（健康计数 :275 + 选层 :288）→ HALF_OPEN 探针被第二次调用吞掉，探针永不发出。
**变更**：
1. `cb_record_failure` OPEN 分支（:317-320）删除刷新，注释：
   「OPEN 期间不再顺延冷却期；仅 HALF_OPEN 探针失败才重开窗口」。
2. `model_router_select_candidates`：健康计数遍历时一次性记录 allow 结果：
```c
int allow[MAX_TARGETS_PER_MODEL];
int healthy_count = 0;
for (int i = 0; i < n_tgts; i++) {
    allow[i] = (cb == NULL || cb_allow_request(cb, model->name, src_targets[i].endpoint)) ? 1 : 0;
    healthy_count += allow[i];
}
```
   选层处（:288）改读 `allow[i]`。
**测试**（先写，红灯）：
- `test_circuit_breaker.c` 新增 `TEST_CASE(test_cb_open_failure_no_refresh)`：
  参数 (3,30) + fake_time：3 连败 → OPEN，`cb_get_open_until == 1030`；
  `g_fake_time = 1100`（> open_until，会先转 HALF_OPEN——用 1020 保持 OPEN）；
  取 `open_until` 前值，`cb_record_failure`（OPEN）→ `open_until` 不变。
  （用 t=1020 保证仍处于 OPEN。）
- `test_model_router.c` 新增 `TEST_CASE(test_model_router_half_open_probe_included)`：
  2 target（A p0 / B p0）模型 + cb(3,30) + fake time：t=1000 给 B 三连败 → OPEN(1030)；
  t=1030 调 `select_candidates` → B 转 HALF_OPEN 且探针被放行 → 候选含 B（count==2，
  B 在列）；随后 `cb_record_failure(cb, model, B, 500)`（探针失败）→ B 回 OPEN，
  `cb_get_open_until == 1030+30`。修复前：select 后 B 不在候选（探针被吞）→ 红灯成立。
**验收**：新用例红→绿；`test_model_router_cb_exclusion_and_fallback` 等既有用例不回归
（走查过：全 OPEN 场景健康计数=0，fallback 路径不变）。
**提交**：`🐛 fix(p2-3): stop refreshing OPEN cooldown and let HALF_OPEN probes fire`

---

### 任务 9 — P2-4：流式 4xx 原样透传

**文件**：`src/upstream_client.c` / `src/upstream_client.h` / `src/aigate_core.c` /
`tests/unit/mock_upstream.{c,h}` / `tests/unit/test_stream_pipeline.c` /
`tests/unit/test_upstream_streaming.c`
**目标**：流式 pre-headers 4xx 走到 :597-604 通用 502，上游错误体丢失（非流式已透传）。
**变更**：
1. `struct stream_ctx` 增 `char* err_body; size_t err_len;`；
   `stream_write_cb`：`sc->status` 落入 [400,500) 时把 chunk 追加进
   `err_body`（realloc 上限 65536，超限停止追加、丢弃尾部，仍 `return total`
   使 curl 继续消费直至响应结束）。
2. `upstream_stream_call` 尾部签名追加 `char** out_err_body, size_t* out_err_len`：
   调用前 `sc.err_body/err_len` 清零；完成时 `*out_err_body = sc.err_body;
   *out_err_len = sc.err_len`（可为 NULL；out 参数为 NULL 则不收集，行为不变，
   调用方 free）。`upstream_client.h` 注释同步。
3. `aigate_core.c` 流式循环（:486/:501 两个调用点）传 `&s4xx_body, &s4xx_len`
   （每次迭代局部声明，迭代出口/重试前/成功/失败全路径 `free`）；
   :523 `!is_failover && status >= 400` 分支改为：
```c
if (!is_failover && status >= 400) {
    if (s4xx_body != NULL && urc == 0) {
        um_record(ac->um, krec.key_id, model, status, 0, 0, 0, total_lat, target->provider);
        int rv = aigate_write_json(rc, status, s4xx_body, s4xx_len);
        free(s4xx_body);
        json_decref(jbody);
        key_rec_free(&krec);
        return rv;
    }
    free(s4xx_body);
    break; /* 无 body：落到通用 502 */
}
```
   （与 :735-749 非流式同构。`cb_record_failure` 在 :520 已先于该分支执行，不变。）
4. `test_upstream_streaming.c` 两处直接调用追加 `NULL, NULL`。
5. `mock_upstream.{c,h}`：`mock_upstream_fail_all` 之外新增
   `mock_upstream_set_fail_status(mu, int status)`（0 恢复；4xx 时以
   `{"error":{"message":"boom"}}` 形式应答且不开流），供管线测试。
**测试**（先写，红灯）`test_stream_pipeline.c` 新增
`TEST_CASE(test_stream_upstream_4xx_passthrough)`：`mock_upstream_set_fail_status(mu, 400)`，
流式请求 → `rc.status == 400`、body 含 `"boom"`、`um_drain` 行 `errors == 0`
（4xx 不计错）；`requests == 1`。修复前返回 502 → 红灯成立。
**验收**：新用例红→绿；`test_upstream_streaming`（NULL 参数兼容）+
`test_stream_pipeline` 既有用例全绿。
**提交**：`🐛 fix(p2-4): pass through upstream 4xx error body on stream pre-headers`

---

### 任务 10 — P2-5：/v1/models 存储失败 503

**文件**：`src/aigate_core.c` / `tests/unit/test_aigate_core.c`
**目标**：`ops->list_models`（:147-148）返回值未检查 → PG 故障时返回空列表。
**变更**：
```c
if (ops != NULL && ops->list_models != NULL) {
    if (ops->list_models(ops->ctx, recs, 256, &n) != 0) {
        free(recs);
        json_decref(jbody);
        key_rec_free(&krec);
        return aigate_write_error(rc, 503, "storage_error", "model list unavailable");
    }
}
```
**测试**（先写，红灯）：`struct fdb` 增 `int fail_list;`，`f_list_models` 开头
`if (db->fail_list) { *n = 0; return -1; }`；新增
`TEST_CASE(test_aigate_core_models_503_on_list_failure)`：
`db.fail_list = 1` → GET /v1/models → `rc.status == 503`、body 含 `"storage_error"`。
**验收**：新用例红→绿；`test_aigate_core_models_*` 既有用例（`fail_list = 0`）不回归。
**提交**：`🐛 fix(p2-5): answer 503 when the model list store read fails`

---

### 任务 11 — P2-6：provider 同步失败上报

**文件**：`src/admin_api.c` / `tests/unit/test_admin_api.c`
**目标**：`sync_provider_models`（:917-971）逐模型 `update_model`/`create_model`
返回值全忽略，create/patch provider 恒报成功。
**变更**：
1. `sync_provider_models` 改 `int` 返回，累计 `failed` 计数
   （`if (ops->update_model(...) != 0) failed++;` 两处 + create 分支同）；
   返回值 = 失败数。
2. `provider_create`（:974+）与 `provider_patch`（同区）调用处：
   `int sf = sync_provider_models(adm, &p);` 响应对象追加
   `json_object_set_new(out, "sync_failed", json_integer(sf));`
   （恒含该字段，0=全成功；状态码保持 201/200，走审计允许的「200+warn 字段」）。
**测试**（先写，红灯）：fake store 增 `int fail_create_model;`
（fake `create_model` 开头 `if (db->fail_create_model) return -1;`）；新增
`TEST_CASE(test_admin_provider_sync_failed_reported)`：provider create（n_models≥1，
flag 置 1）→ 响应 201 且 body 含 `"sync_failed": 1`；flag 清 0 后 patch →
`"sync_failed": 0`。
**验收**：新用例红→绿；既有 provider 用例（无 flag，`sync_failed: 0` 出现）通过。
**提交**：`📊 feat(p2-6): report per-model sync failures on provider create/patch`

---

### 任务 12 — P2-7：重定向协议限制

**文件**：`src/upstream_client.c`
**目标**：`CURLOPT_FOLLOWLOCATION=1` 未限协议（:131/:311）→ 30x 可指向
`file://` 等任意协议（受控/恶意上游信息泄露面）。
**变更**：两个调用点（`upstream_call_ext` 与 `upstream_stream_call`）各加：
```c
curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
curl_easy_setopt(c, CURLOPT_REPROTOCOL, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
```
**测试**：不可单测（需真实 30x 重定向上游）——检查验证：代码评审两处均落地 +
任务 16 集成 smoke 回归（mock 无重定向，行为不变）。
**验收**：编译通过；`grep -c 'CURLOPT_REPROTOCOL' src/upstream_client.c` == 2。
**提交**：`🔒 sec(p2-7): restrict curl redirect protocols to http/https`

---

### 任务 13 — P2-8：admin 锁留策略 env 可配

**文件**：`src/admin_api.c` / `src/admin_api.h` / `src/transport_civetweb.c` /
`.env.example` / `README.md` / `tests/unit/test_admin_api.c`
**目标**：`LOCKOUT_FAILS 10` / `LOCKOUT_WINDOW_S 300`（admin_api.c:69-71）硬编码；
反代共享 IP 场景锁留成自助式管理面 DoS（双刃剑，需可调 + 文档化）。
**变更**：
1. `admin_api.c`：删两 #define（`LOCKOUT_SLOTS 128` 保留），改
   `static int g_lockout_fails = 10; static int g_lockout_window_s = 300;`；
   :103-104/:124 使用处替换。头文件声明：
   `void admin_lockout_set_policy(int max_fails, int window_s);`
   （`[2..1000]`/`[5..3600]` 之外保持现值不变——防误配 0 次锁全/无限窗口）。
2. `transport_civetweb.c` start 内读 env（与 `AIGATE_ALLOW_PLAINTEXT_KEYS` 同区）：
```c
admin_lockout_set_policy(
    atoi_or(getenv("AIGATE_LOCKOUT_MAX_FAILS"), 10),
    atoi_or(getenv("AIGATE_LOCKOUT_WINDOW_S"), 300));
```
3. `.env.example` 追加（带注释）：`# AIGATE_LOCKOUT_MAX_FAILS=10`、
   `# AIGATE_LOCKOUT_WINDOW_S=300`；README admin 节补一句 XFF 信任模型说明
   （锁留按 socket 对端 IP；反代共享 IP 会放大锁留面，信任 XFF 需自行接层实现）。
**测试**（先写，红灯）：`test_admin_api.c` 新增
`TEST_CASE(test_admin_lockout_policy_env)`：
`admin_lockout_set_policy(3, 60)` → 3 次错 token + 第 4 次正确 token 仍 429；
`admin_lockout_reset()` + `admin_lockout_set_policy(10, 300)` 恢复默认 → 既有
10 次阈值行为回归（若既有 lockout 用例存在则不重复）。越界调用
`set_policy(1, 300)`/`set_policy(10, 4)` 保持现值（断言行为不变）。
**验收**：新用例红→绿；`grep -n 'LOCKOUT_FAILS\|LOCKOUT_WINDOW_S' src/` 仅剩 SLOTS。
**提交**：`✨ feat(p2-8): make admin lockout threshold/window env-configurable`

---

### 任务 14 — P3-1：master key 早退擦除 + core_init 回滚

**文件**：`src/main.c` / `src/aigate_core.c`
**目标**：main.c 三个早退路径（:66-68 hex 失败、:75-79 core init 失败、
:84-89 transport 失败）未 `OPENSSL_cleanse(master)`；`aigate_core_init`
失败路径泄漏已分配子对象（router 持有的 master 副本不清擦）。
**变更**：
1. `aigate_core_init`（:24-44）失败分支补回滚：
```c
if (ac->rl == NULL || ac->router == NULL || ac->um == NULL || ac->cb == NULL) {
    if (ac->cb != NULL) cb_destroy(ac->cb);
    if (ac->um != NULL) usage_meter_free(ac->um);
    if (ac->router != NULL) model_router_free(ac->router);
    if (ac->rl != NULL) ratelimit_free(ac->rl);
    auth_key_shutdown(&ac->keys);
    memset(ac, 0, sizeof *ac);
    return -1;
}
```
   （`model_router_free` 内部已 cleanse 其 master 副本——执行时确认该保证，
   若 router 半初始化态 free 不安全，改为 router 分配失败单独处理。）
2. `main.c`：:66-68 与 :75-79 早退前加 `OPENSSL_cleanse(master, sizeof master);`
   （:84-89 路径 `aigate_core_shutdown` 已清 router 副本，补 stack copy 的 cleanse）。
**测试**：检查验证（main 不可单测；core_init 回滚可由 ASan 构建下
「mock 一次 allocation 失败」覆盖——若无法稳定注入，则评审 + 现有
core 单测全绿作兜底）。
**验收**：ASan 构建 `ctest -R unit` 无泄漏/越界报告。
**提交**：`🧹 chore(p3-1): cleanse master key on early exits and roll back core init`

---

### 任务 15 — P3-5：default_params 越限拒绝

**文件**：`src/admin_api.c` / `tests/unit/test_admin_api.c`
**目标**：`model_create`（:627-636）`snprintf` 截断 1024 → 存坏 JSONB，模型隐形消失；
`model_patch`（:741-748）同。
**变更**（create，patch 镜像）：
```c
json_t* jparams = json_object_get(jbody, "default_params");
if (jparams != NULL && json_is_object(jparams)) {
    char* packed = json_dumps(jparams, JSON_COMPACT);
    if (packed != NULL) {
        if (strlen(packed) >= sizeof m.default_params_json) {
            free(packed);
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request",
                                "default_params too large");
        }
        snprintf(m.default_params_json, sizeof m.default_params_json, "%s", packed);
        free(packed);
    }
}
```
patch 路径：越限时 `finish_error(400)` 且不置 `MMASK_PARAMS`。
**测试**（先写，红灯）：`test_admin_api.c` 新增
`test_admin_default_params_oversize_rejected`：构造 1100 字串值的
`default_params` → create 400 + fake store 模型数不变；patch 同 400；
合法小对象 → 201/200 且字段落库（既有断言面）。
**验收**：新用例红→绿。
**提交**：`🐛 fix(p3-5): reject oversized default_params instead of truncating JSONB`

---

### 任务 16 — P3 卫生批（注释/文档 + P3-4）

**文件**：见审计 P3 表；无行为变化（P3-4 除外）。
**变更清单**（执行时逐条落注释，一句话即可）：
- P3-2 `auth_key.c:35-36` 正缓存无 TTL → 注释「admin 为 source of truth，直改 PG 不失效（by design）」。
- P3-3 `provider_delete` → 注释「auto-sync 的 model 行需手工清理（or cascade，留作后续）」。
- P3-4 `metrics.c` failover 表 128 满 → 加「满时一次 warn」（`static int warned;`，低风险）。
- P3-6/P3-7/P3-8/P3-9/P3-10/P3-11/P3-12 → 审计建议的注释各一句。
- README/.env.example：P3-11 配额翻日最坏延迟 ≤ `AIGATE_USAGE_FLUSH_S` 一句。
**验收**：`grep -c` 确认注释落点；编译通过（无逻辑变化，`ctest -R unit` 全绿）。
**提交**：`📝 docs(p3): document operational trade-offs; warn on full failover table`

---

### 任务 17 — 总体验证

1. `cmake -S . -B build && cmake --build build 2>&1 | grep -iE 'warning|error'` → 空。
2. `ctest --test-dir build -R unit --output-on-failure` → 全绿。
3. ASan：`cmake -S . -B .asan -DCMAKE_BUILD_TYPE=Debug -DAIGATE_SANITIZERS=ON
   && cmake --build .asan && ctest --test-dir .asan -R unit --output-on-failure` → 全绿。
4. 集成 smoke：`bash tests/integration/smoke.sh`（真实 PG 可达时全跑，含 P0-1
   鉴权崩溃回归；不可达按其 SKIP 语义记录）。
5. `git log` 核对 15 个修复提交 + 工作区干净。

## 范围外（记录不执行）

- P3-12 chunked 请求体、P2-8 XFF 解析实现（仅文档化，见共享决定）。
- 审计 §4 性能观察（均为「现状可接受」结论，无行动项）。
- `expires_at` 读回值解析（`atol` 作用于 timestamptz 文本形如 `2026-09-22 10:00+08`
  只取年份前缀——疑似既有值语义缺陷，但不在本轮审计范围，另行立项）。
- P3-3 级联删除 model 行（审计给的是「级联 or 文档化」二选一，本轮取文档化，级联留后续）。

## 执行顺序与依赖

任务 1→15 无相互依赖（各改独立文件域；任务 2/3 都碰 test_aigate_core 的 fdb 但
加字段不冲突），可串行执行保证每步绿。任务 9 依赖 mock_upstream 扩展（自带）。
任务 17 收尾。每个任务提交即一个 gitmoji commit；批间 `ctest -R unit` 必须全绿才进下一批。

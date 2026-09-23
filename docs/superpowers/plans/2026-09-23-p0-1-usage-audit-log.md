# 实施计划：P0-1 每请求用量审计日志（2026-09-23）

来源规格：`docs/superpowers/specs/2026-09-23-aigate-vs-newapi-gap-analysis.md` §2.1 / §4 P0-1
基线提交：`fd0e387`（gap analysis 报告）
构建：CMake ≥ 3.16，`-Wall -Wextra -Werror`，C17。测试：`ctest -R unit`。
提交规范：gitmoji（`git log` 风格：emoji 开头 + `(scope)` + 简述，如 `🚀 feat(usage): ...`）。

## 目标（Goal）

补一张 `usage_requests` 明细表（批刷 + 环形缓冲溢出丢弃 + warn），复用既有 `usage_daily` 批刷模式，
让每次请求可追溯到 模型 / provider / 状态码 / token 数 / 延迟 / 时间戳，支撑排障与成本归因。
暴露 `GET /admin/v1/usage/requests?key_id=&since=` 明细查询端点。

## 验收（Acceptance）

1. `cmake -S . -B build && cmake --build build` 零告警（-Werror）。
2. `ctest --test-dir build -R unit` 全绿；新增用例在实现前失败（红灯验证）。
3. `TEST_PG_DSN` 可用时 `pg_real_roundtrip` 走新 v6 迁移 + 明细 flush/query 回读断言。
4. 每个任务一个提交；提交信息含 gitmoji + `(P0-1)` 编号。

## 共享决定（Shared Decisions）

| 决定 | 理由 |
|---|---|
| 明细走独立环形缓冲（cap 4096），不复用 4096 行聚合批次 | 聚合 accumulator 按 (key,model) 合并，明细逐请求；两套数据不同步，独立 ring 与 `um_drain`/`um_unflush` 同构、不侵入聚合路径 |
| ring 写端 = 单个 mutex（复用 `um->mtx`）+ 无锁读端快照（head/tail 原子） | 写者只有 `um_record`（单把锁内完成 HDR+accum+ring 三段），读者（worker/free）持锁取快照；无 CAS 无 ABA，正确性由锁顺序保证 |
| flush 失败整批 re-queue（`head -= n`），ring 满时丢弃该批 + warn（`dropped += n`） | 与 `um_unflush` 聚合路径语义对齐（spec §2.1「与现 unflush 语义对齐」）；丢弃只发生在「失败积压 + ring 无空间」，warn 带累计计数 |
| 明细批量 = worker 每轮 drain ring + 一次 `flush_usage_requests` 批量 INSERT | 一次锁内批量插入（BEGIN/COMMIT），沿用 `pq_flush_usage` 的事务模式，避免逐行往返 |
| `usage_requests.ts` 存 epoch seconds（`time_t`），查询 `since` = UTC 午夜（复用 `parse_day`） | 与 `usage_daily` 的 `day` 粒度一致；明细保留 `ts` 秒级 + 行内 `latency_ns`，不引入 timestamptz 转换成本 |
| schema v6 追加到 `schema/schema.sql` 并手工同步 `src/schema_sql.h` | `schema_sql.h` 注释标明 "generated; do not edit by hand" 但仓库无生成器（已核实 CMake 无规则），既有 v4/v5 均为双写；保持双写模式 |
| admin 端点独立于 `GET /admin/v1/usage`（日聚合，保留） | 明细与聚合是不同查询语义；dispatch 中 `rest == "usage"` 与 `rest == "usage/requests"` 互不干扰 |
| 未装配 `query_usage_requests` 的 fake ops 桩 stub 返回 0/NULL | 全量 grep 确认 `transport_civetweb.c` 直接调用 `query_usage`/`query_usage_requests` 均走 `pg_store_ops(adm->ps)`，admin 端点装配完整；测试 fake 中 stub 仅保编译，无运行路径 |
| dropped 计数走 `um_requests_dropped()` 原子（不渲染进 `/metrics` 首段，仅计数） | 环形丢弃是罕见路径；`/metrics` 不引入新指标名避免与 P1-2 成本核算指标混淆；计数保留供 admin 端点 `dropped` 字段展示 |
| 10 个 fake ops 文件装配点：2 个真实 fake（`test_usage_meter`、`test_admin_api`）完整实现；其余 8 个 stub | 见任务 7 清单；stub 与既有 `um_fail`/`fo_noop` 同型 |

## 任务清单

顺序即执行顺序；每个任务：（按需先写失败测试）→ 实现 → 跑绿 → 提交。
批间 `ctest -R unit` 必须全绿才进下一任务。

---

### 任务 1 — schema v6：`usage_requests` 明细表

**文件**：
- Modify: `schema/schema.sql`（文件尾部追加）
- Modify: `src/schema_sql.h`（`R"SQL(...)"SQL"` 收尾 `)SQL";` 之前插入同块）

**变更**：

`schema/schema.sql` 尾部追加（与 v5 块 `INSERT ... VALUES (5)` 之后）：

```sql
-- Migration v6: per-request usage audit detail (P0-1)
CREATE TABLE IF NOT EXISTS usage_requests (
  key_id          BIGINT NOT NULL,
  model_name      TEXT NOT NULL,
  provider        TEXT NOT NULL DEFAULT '',
  http_status     INT NOT NULL,
  prompt_tokens   BIGINT NOT NULL DEFAULT 0,
  completion_tokens BIGINT NOT NULL DEFAULT 0,
  cached_prompt_tokens BIGINT NOT NULL DEFAULT 0,
  latency_ns      BIGINT NOT NULL DEFAULT 0,
  ts              BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_usage_requests_key_ts ON usage_requests (key_id, ts);
CREATE INDEX IF NOT EXISTS ix_usage_requests_ts ON usage_requests (ts);
INSERT INTO schema_migrations(version) VALUES (6) ON CONFLICT (version) DO NOTHING;
```

`src/schema_sql.h` 在 `INSERT INTO schema_migrations(version) VALUES (5) ...` 行之后、`)SQL";` 之前
插入同样的 v6 块（缩进对齐既有行，每行前 4 空格）。

**验证**：

```bash
cmake --build build
ctest --test-dir build -R unit
```

期望：编译零告警；若 `TEST_PG_DSN` 未设则 `pg_real_roundtrip` 自动 skip。

**提交**：

```bash
git add schema/schema.sql src/schema_sql.h
git commit -m "feat(schema): 🚀 v6 usage_requests per-request audit table (P0-1)"
```

---

### 任务 2 — `usage_request_row_t` + `pg_ops` 两个新 op

**文件**：
- Modify: `src/pg_store.h`（`usage_row_t` 之后加 struct；`pg_ops_t` 加两个函数指针）
- Modify: `src/pg_store.c`（实现 `pq_flush_requests` / `pq_query_requests`；`pg_store_open` 注册）

**变更 2a** — `src/pg_store.h` 在 `usage_row_t`（约 55–61 行）之后插入：

```c
/** @brief One usage_requests (per-request audit) row. */
typedef struct usage_request_row {
    long      key_id;
    char      model_name[128];
    char      provider[32];
    int       http_status;
    long      prompt_tokens;
    long      completion_tokens;
    long      cached_prompt_tokens;
    uint64_t  latency_ns;
    time_t    ts;
} usage_request_row_t;
```

`pg_ops_t` 在 `query_usage` 成员之后追加：

```c
    int (*flush_usage_requests)(void* ctx,
                                const usage_request_row_t* rows,
                                int n);
    int (*query_usage_requests)(void*        ctx,
                                 long         key_id,
                                 time_t       since,
                                 usage_request_row_t* out,
                                 int          cap,
                                 int*         n);
```

（`#include <stdint.h>` 与 `<time.h>` 已在 `pg_store.h` 顶部；若无 `stdint.h` 补上。）

**变更 2b** — `src/pg_store.c` 在 `pq_query_usage`（约 1182 行）之后插入两个实现，
紧跟 `pq_flush_usage`/`pq_query_usage` 的本地变量风格：

```c
static int
pq_flush_requests(void* vctx, const usage_request_row_t* rows, int n)
{
    struct pq_ctx* px = vctx;
    static const char q[] =
        "INSERT INTO usage_requests(key_id, model_name, provider, http_status, "
        "prompt_tokens, completion_tokens, cached_prompt_tokens, latency_ns, ts) "
        "VALUES($1, $2, $3, $4, $5, $6, $7, $8, $9)";
    int rc = 0;

    if (n <= 0) {
        return 0;
    }

    char num[32], st[16], pt[32], ct[32], cpt[32], lat[32], tsb[32];
    const char* vals[9];
    int         plens[9] = {0};

    pq_lock(px);
    PQclear(PQexec(px->db, "BEGIN"));
    for (int i = 0; i < n; i++) {
        snprintf(num, sizeof num, "%ld", rows[i].key_id);
        vals[0] = num;
        vals[1] = rows[i].model_name;
        vals[2] = rows[i].provider;
        snprintf(st, sizeof st, "%d", rows[i].http_status);
        vals[3] = st;
        snprintf(pt, sizeof pt, "%ld", rows[i].prompt_tokens);
        vals[4] = pt;
        snprintf(ct, sizeof ct, "%ld", rows[i].completion_tokens);
        vals[5] = ct;
        snprintf(cpt, sizeof cpt, "%ld", rows[i].cached_prompt_tokens);
        vals[6] = cpt;
        snprintf(lat, sizeof lat, "%llu", (unsigned long long)rows[i].latency_ns);
        vals[7] = lat;
        snprintf(tsb, sizeof tsb, "%ld", (long)rows[i].ts);
        vals[8] = tsb;
        PGresult* res = PQexecParams(px->db, q, 9, NULL, vals, plens, NULL, 0);
        if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
            AIGATE_LOG_ERROR("pg flush_requests: %s",
                             res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
            PQclear(res);
            PQclear(PQexec(px->db, "ROLLBACK"));
            rc = -1;
            break;
        }
        PQclear(res);
    }
    PQclear(PQexec(px->db, "COMMIT"));
    pq_unlock(px);
    return rc;
}

static int
pq_query_requests(void*        vctx,
                  long         key_id,
                  time_t       since,
                  usage_request_row_t* out,
                  int          cap,
                  int*         n)
{
    struct pq_ctx* px = vctx;
    static const char q[] =
        "SELECT key_id, model_name, provider, http_status, prompt_tokens, "
        "completion_tokens, cached_prompt_tokens, latency_ns, ts "
        "FROM usage_requests "
        "WHERE ($1::bigint = 0 OR key_id = $1) AND ts >= $2 "
        "ORDER BY ts DESC LIMIT $3";
    char key[32], since_b[32], cap_b[16];
    const char* vals[3];
    int         plens[3] = {0};

    snprintf(key, sizeof key, "%ld", key_id);
    snprintf(since_b, sizeof since_b, "%ld", (long)since);
    snprintf(cap_b, sizeof cap_b, "%d", cap);
    vals[0] = key;
    vals[1] = since_b;
    vals[2] = cap_b;

    *n = 0;
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 3, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        AIGATE_LOG_ERROR("pg query_requests: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
        PQclear(res);
        return -1;
    }
    int nt = PQntuples(res);
    if (nt > cap) {
        nt = cap;
    }
    for (int i = 0; i < nt; i++) {
        out[i].key_id = atol(PQgetvalue(res, i, 0));
        copy_field(out[i].model_name, sizeof out[i].model_name, PQgetvalue(res, i, 1));
        copy_field(out[i].provider, sizeof out[i].provider, PQgetvalue(res, i, 2));
        out[i].http_status = (int)strtol(PQgetvalue(res, i, 3), NULL, 10);
        out[i].prompt_tokens = atol(PQgetvalue(res, i, 4));
        out[i].completion_tokens = atol(PQgetvalue(res, i, 5));
        out[i].cached_prompt_tokens =
            PQnfields(res) > 6 ? atol(PQgetvalue(res, i, 6)) : 0;
        out[i].latency_ns = PQnfields(res) > 7 ? strtoull(PQgetvalue(res, i, 7), NULL, 10) : 0;
        out[i].ts = PQnfields(res) > 8 ? (time_t)atol(PQgetvalue(res, i, 8)) : 0;
    }
    *n = nt;
    PQclear(res);
    return 0;
}
```

注意：`copy_field` 是 `pg_store.c` 文件内既有 static 辅助（`pq_query_usage` 已用，见 1171 行）；
若名字不同则按实际局部函数名替换（编译报错即改）。

**变更 2c** — `pg_store_open` 在 `ps->ops.query_usage = pq_query_usage;`（约 1254 行）之后：

```c
    ps->ops.flush_usage_requests = pq_flush_requests;
    ps->ops.query_usage_requests = pq_query_requests;
```

**验证**：

```bash
cmake --build build
```

期望：零告警。此时 fake ops 全部未装配新字段（NULL），运行时 admin 端点还没建，无运行路径。

**提交**：

```bash
git add src/pg_store.h src/pg_store.c
git commit -m "feat(pg): 🚀 usage_requests flush/query ops + libpq impl (P0-1)"
```

---

### 任务 3 — usage_meter 环形缓冲 + 批量刷 + 溢出丢弃

**文件**：
- Modify: `src/usage_meter.h`（新接口声明）
- Modify: `src/usage_meter.c`（struct 字段、`um_record` 追加写、worker drain、`usage_meter_free`、
  `um_drain_requests` / `um_unflush_requests` / `um_requests_dropped` 实现）

**变更 3a** — `src/usage_meter.c` `struct usage_meter`（约 33–45 行）追加字段：

```c
    /* per-request audit ring (P0-1) */
    usage_request_row_t* req_ring;
    int                  req_head, req_tail;
    atomic_int           req_dropped;
```

（`usage_request_row_t` 经 `usage_meter.h` → `pg_store.h` 已可见；`<stdatomic.h>` 已在。）
`usage_meter_new` 中 `um->req_ring = malloc(UM_REQ_CAP * sizeof *um->req_ring);`
（`#define UM_REQ_CAP 4096` 加在 `UM_ACC_CAP` 旁）；`req_ring == NULL` 时记 warn、
worker 退化为不刷明细（与 `rows == NULL` 的降级路径同构，`um_record` 的 ring 段判
`um->req_ring != NULL`）。
`atomic_init(&um->req_dropped, 0);` 紧跟 `atomic_init(&um->toks, 0);`。

**变更 3b** — `um_record`（约 198–258 行）在「累加 accumulator」段（持锁内，
240–254 行附近）之后、`pthread_mutex_unlock(&um->mtx)` 之前插入 ring 写入：

```c
        if (um->req_ring != NULL) {
            int slot = um->req_tail % UM_REQ_CAP;
            usage_request_row_t* rr = &um->req_ring[slot];
            rr->key_id = key_id;
            snprintf(rr->model_name, sizeof rr->model_name, "%s", model);
            rr->provider[0] = '\0';
            if (provider != NULL) {
                snprintf(rr->provider, sizeof rr->provider, "%s", provider);
            }
            rr->http_status = http_status;
            rr->prompt_tokens = prompt_tokens;
            rr->completion_tokens = completion_tokens;
            rr->cached_prompt_tokens = cached_prompt_tokens;
            rr->latency_ns = latency_ns;
            rr->ts = time(NULL);
            um->req_tail++;
            if (um->req_tail - um->req_head > UM_REQ_CAP) {
                /* ring full: backpressure on a brand-new slot would evict an
                 * unflushed row, so drop this one and count it */
                um->req_tail--;
                atomic_fetch_add(&um->req_dropped, 1);
                AIGATE_LOG_WARN("usage ring full; dropping audit row (total dropped: %d)",
                                 atomic_load(&um->req_dropped));
            }
        }
```

注意：现有 `um_record` 的 accumulator 段在命中既有 slot（226–242 行）与新建 slot（243–255 行）
两处都 `pthread_mutex_unlock(&um->mtx); return;`。ring 写入必须放在**两条路径公共的持锁区尾部**：
实现时把 ring 写入段提为局部 static helper（持锁调用）或在两个 return 前各插一次——
**选定实现：把 accumulator 段的「find-or-create + 累加」重构为单个持锁块，ring 写入紧跟其后、
同一把锁内，随后单次 unlock**。重构不改语义（累加逻辑原样保留），`um_drain_fail_requeue`
既有测试（127 行）是此重构的回归网。

**变更 3c** — 新增 drain / release / re-queue / 计数接口(`um_unflush` 之后)。
head 语义单一口径(**copy-and-advance**):drain 复制同时前移 head;flush 失败调
`um_requeue_requests`(head 回退,回退越界则丢弃 + 计数 + warn);flush 成功调
`um_release_requests`(确认,无回退)：

```c
/** @brief Copy up to @p cap pending audit rows out of the ring and advance
 * the head (the worker owns them until release or re-queue). No flush call
 * is made here — mirroring um_drain's drain/flush split. @p n_out rows copied
 * (<= cap). @return 0 (NULL um / NULL ring is a no-op). */
int
um_drain_requests(usage_meter_t* um, usage_request_row_t* out, int cap, int* n_out)
{
    *n_out = 0;
    if (um == NULL || um->req_ring == NULL) {
        return 0;
    }
    pthread_mutex_lock(&um->mtx);
    int n = um->req_tail - um->req_head;
    if (n > cap) {
        n = cap;
    }
    for (int i = 0; i < n; i++) {
        out[i] = um->req_ring[(um->req_head + i) % UM_REQ_CAP];
    }
    um->req_head += n;
    pthread_mutex_unlock(&um->mtx);
    *n_out = n;
    return 0;
}
/** @brief Confirm @p n drained rows after a successful flush. With
 * copy+advance drain the head already moved; this is a no-op retained
 * for caller symmetry with the failure path (um_requeue_requests). */
int
um_release_requests(usage_meter_t* um, int n)
{
    (void)um;
    (void)n;
    /* head already advanced by um_drain_requests; no-op kept for caller
     * symmetry with the failure path (um_requeue_requests). */
    return 0;
}

/** @brief Re-queue @p n drained rows after a failed flush: head rewinds by
 * n. If the rewind would push pending items past the ring's capacity
 * (ring full), the excess is dropped into req_dropped with a warn —
 * same bounded-loss contract as um_unflush on a full accumulator. */
int
um_requeue_requests(usage_meter_t* um, int n)
{
    if (um == NULL || n <= 0) {
        return 0;
    }
    pthread_mutex_lock(&um->mtx);
    um->req_head -= n;
    if (um->req_head < um->req_tail - UM_REQ_CAP) {
        int lost = um->req_tail - UM_REQ_CAP - um->req_head;
        um->req_head = um->req_tail - UM_REQ_CAP;
        atomic_fetch_add(&um->req_dropped, lost);
        AIGATE_LOG_WARN("usage ring re-queue overflow: %d rows dropped "
                        "(total dropped: %d)",
                         lost,
                        atomic_load(&um->req_dropped));
    }
    pthread_mutex_unlock(&um->mtx);
    return 0;
}

/** @brief Total audit rows dropped (write-side ring full + re-queue overflow). */
int
um_requests_dropped(const usage_meter_t* um)
{
    return um == NULL ? 0 : atomic_load((atomic_int*)&um->req_dropped);
}
```

**变更 3d** — `usage_meter.h` 声明(`um_unflush` 之后):

```c
/** @brief Copy up to @p cap pending audit rows out of the ring and
 * advance the head (the worker owns them until re-queue). No flush call
 * is made here. @p n_out rows copied (<= cap).
 * @return 0 (NULL um / NULL ring is a no-op). */
int um_drain_requests(usage_meter_t* um, usage_request_row_t* out, int cap, int* n_out);
/** @brief Confirm @p n drained rows after a successful flush. No-op
 * under copy+advance drain (head already moved); retained for caller
 * symmetry with the failure path. */
int um_release_requests(usage_meter_t* um, int n);
/** @brief Re-queue @p n rows after a failed flush: head rewinds by n.
 * If the rewind lands past the oldest pending slot, the excess is
 * dropped into the counter with a warn. */
int um_requeue_requests(usage_meter_t* um, int n);
/** @brief Total audit rows dropped on ring overflow / re-queue overflow. */
int um_requests_dropped(const usage_meter_t* um);
```

**变更 3e** — worker：把明细环的 drain+flush+release/requeue 提为 static 辅助，
主循环与降级循环共用（降级 = `rows == NULL` 时聚合停刷、明细续刷）：

```c
/* Drain the audit ring (copy + advance) and flush one batch; re-queue the
 * batch on flush failure so rows are retried next tick. */
static void
um_flush_request_batch(usage_meter_t* um, usage_request_row_t* rreqs, int rcap)
{
    if (um->ps == NULL || um->req_ring == NULL) {
        return;
    }
    int rn = 0;
    if (um_drain_requests(um, rreqs, rcap, &rn) != 0 || rn == 0) {
        return;
    }
    const pg_ops_t* ops = pg_store_ops(um->ps);
    if (ops != NULL && ops->flush_usage_requests != NULL &&
        ops->flush_usage_requests(ops->ctx, rreqs, rn) == 0) {
        um_release_requests(um, rn);
    } else {
        um_requeue_requests(um, rn);
        AIGATE_LOG_WARN("usage request flush failed; %d rows requeued", rn);
    }
}
```

`worker_main` 中：
1. `usage_row_t* rows = malloc(UM_ACC_CAP * sizeof *rows);` 旁新增
   `usage_request_row_t* rreqs = malloc(UM_REQ_BATCH * sizeof *rreqs);`
   （`#define UM_REQ_BATCH 512`，与 `UM_REQ_CAP` 同区定义；malloc 失败时 `rreqs = NULL`，
   `um_flush_request_batch` 判 NULL no-op，明细随 free 收尾）。
2. 正常 while 循环：聚合 drain 块（现 123–130 行）之后调
   `um_flush_request_batch(um, rreqs, UM_REQ_BATCH);`。
3. 降级循环（`rows == NULL`，现 104–106 行）：`sleep(1)` 前先调
   `um_flush_request_batch(um, rreqs, UM_REQ_BATCH);`（sleep 语义 = 聚合停刷、明细续刷）。
4. `free(rows);` 旁 `free(rreqs);`。

**实现口径(写码时固定,单一 head 语义)**:drain 复制同时前移 head(worker 拥有这批行);flush 成功 →
`um_release_requests(um, n)`(no-op 确认);flush 失败 → `um_requeue_requests(um, n)`
(head 回退,回退越界丢弃 + dropped 计数 + warn)。函数清单:`um_drain_requests`(copy+advance
head)、`um_release_requests`(成功路径 no-op 确认)、`um_requeue_requests`(失败回退)、
`um_requests_dropped`(计数)。3b/3c/3d/3e 代码即按此口径,不混用两种 head 语义。

**变更 3f** — `usage_meter_free`（169 行起）在「final drain」块之后追加收尾 ring：

```c
    /* final audit drain in bounded chunks */
    usage_request_row_t* rreqs = malloc(256 * sizeof *rreqs);
    if (rreqs != NULL && um->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(um->ps);
        int             rn = 0;
        do {
            um_drain_requests(um, rreqs, 256, &rn);
            if (rn > 0) {
                if (ops != NULL && ops->flush_usage_requests != NULL &&
                    ops->flush_usage_requests(ops->ctx, rreqs, rn) != 0) {
                    AIGATE_LOG_WARN("usage_meter shutdown: lost %d audit rows", rn);
                    break;
                }
                um_release_requests(um, rn);
            }
        } while (rn > 0);
        free(rreqs);
    }
```

注意：`usage_meter_free` 现用栈上 `usage_row_t rows[256]`（聚合）；明细收尾堆上 256 块
（明细行 160 字节 × 256 = 40KB 栈上偏大，故堆）。

**变更 3g** — `um_record` 重构为单一持锁块（3b 前提）：把 226–257 行的 find-or-create
循环体提取为 `acc_touch(um_acc_t* a, ...)` 内联或原地保留循环、在循环出口（命中/新建两处
`return` 之前）统一插 ring 段。最小 diff 写法：在命中分支（233–242）与新建分支（243–255）
各自 unlock 前插入同一段 ring 代码，并用 `goto` 去重：

```c
        if (a->in_use && a->key_id == key_id && strcmp(a->model, model) == 0) {
            a->requests++;
            a->prompt += prompt_tokens;
            a->completion += completion_tokens;
            a->cached_prompt += cached_prompt_tokens;
            if (http_status >= 500) {
                a->errors++;
            }
            goto record_ring;
        }
        if (!a->in_use) {
            a->in_use = 1;
            a->key_id = key_id;
            snprintf(a->model, sizeof a->model, "%s", model);
            a->day = day;
            a->requests = 1;
            a->prompt = prompt_tokens;
            a->completion = completion_tokens;
            a->cached_prompt = cached_prompt_tokens;
            a->errors = http_status >= 500 ? 1 : 0;
            goto record_ring;
        }
record_ring:
        if (um->req_ring != NULL) {
            /* ... ring write block from 3b ... */
        }
        pthread_mutex_unlock(&um->mtx);
        return;
```

（`goto record_ring;` 落在 `for` 循环体内、同一持锁区，合法；label 在循环块内。
循环自然结束（表满）时走原 257 行 `pthread_mutex_unlock(&um->mtx); /* table full */`，
**ring 写入仍要执行**——表满只丢聚合，不丢明细：把 ring 段移出循环，放循环后、unlock 前，
即 `for` 之后紧跟 `if (um->req_ring != NULL) {...}`。上例 label 仅在命中/新建时跳转，
表满路径（循环跑完 4096 槽）落在循环后同一处 ring 段。落地形式：**去掉 label，
循环后统一 ring 段，循环内命中/新建后置 `i = UM_ACC_CAP; break;`
跳出。此为 3b 的最终形态。drain/release/re-queue 语义见 3c 实现口径
（copy+advance）。**

**验证**（先写测试，见任务 4 的 ring 测试；此任务先保证编译 + 旧测全绿）：

```bash
cmake --build build
ctest --test-dir build -R unit
```

期望：零告警、全绿（新字段未装 fake 时为 NULL no-op）。

**提交**：

```bash
git add src/usage_meter.h src/usage_meter.c
git commit -m "feat(usage): 🚀 audit ring in um_record + worker batch flush (P0-1)"
```

---

### 任务 4 — 测试：`test_usage_meter` ring 批刷 / 溢出 / re-queue（TDD 红灯 → 绿）

**文件**：
- Modify: `tests/unit/test_usage_meter.c`（`um_db` 扩展 + fake ops + 新 `TEST_CASE`）
- Modify: `tests/unit/run_tests.c`（注册）

**变更 4a** — `struct um_db`（10–15 行）扩展：

```c
struct um_db {
    int     flush_calls;
    int     fail_flush;
    usage_row_t rows[64];
    int     n_rows;
    usage_request_row_t reqs[64];
    int     n_reqs;
    int     fail_req_flush;
    int     req_flush_calls;
};
```

**变更 4b** — 新增 fake op（`um_flush` 之后）：

```c
static int
um_flush_reqs(void* ctx, const usage_request_row_t* rows, int n)
{
    struct um_db* db = ctx;
    if (db->fail_req_flush) {
        return -1;
    }
    db->req_flush_calls++;
    for (int i = 0; i < n && db->n_reqs < (int)(sizeof db->reqs / sizeof db->reqs[0]); i++) {
        db->reqs[db->n_reqs++] = rows[i];
    }
    return 0;
}
```

`open_um_store` 装配（53 行旁）：

```c
    ops.flush_usage_requests = um_flush_reqs;
    ops.query_usage_requests =
        (int (*)(void*, long, time_t, usage_request_row_t*, int, int*))um_fail;
```

**变更 4c** — 新测试（文件尾部）：

```c
TEST_CASE(test_um_request_ring)
{
    struct um_db db;
    memset(&db, 0, sizeof db);
    pg_store_t* ps = open_um_store(&db);
    TEST_ASSERT(ps != NULL, "store open");
    usage_meter_t* um = usage_meter_new(ps, NULL, 0); /* no worker: manual drain */
    TEST_ASSERT(um != NULL, "meter new");

    um_record(um, 1, "gpt-4o", 200, 7, 11, 5, 50000000, "openai");
    um_record(um, 2, "claude-3", 500, 3, 4, 2, 90000000, "anthropic");

    usage_request_row_t buf[16];
    int                 n = 0;

    /* drain = copy + advance head; no flush call */
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain reqs");
    TEST_ASSERT(n == 2, "2 audit rows, got %d", n);
    TEST_ASSERT(db.n_reqs == 0, "drain does not flush");
    TEST_ASSERT(buf[0].key_id == 1 &&
                    strcmp(buf[0].provider, "openai") == 0 &&
                    buf[0].http_status == 200 &&
                    buf[0].prompt_tokens == 7 &&
                    buf[0].latency_ns == 50000000,
                "row 0 fields");
    TEST_ASSERT(buf[1].key_id == 2 && buf[1].http_status == 500, "row 1 fields");
    TEST_ASSERT(um_requests_dropped(um) == 0, "nothing dropped");

    /* manual flush of the drained rows, then release (no-op confirm) */
    TEST_ASSERT(um_flush_reqs(&db, buf, n) == 0, "manual flush");
    TEST_ASSERT(um_release_requests(um, n) == 0, "release no-op confirm");
    TEST_ASSERT(db.n_reqs == 2, "fake now holds 2");
    TEST_ASSERT(db.req_flush_calls == 1, "one flush call");

    /* simulate a worker tick: record, drain, flush fails -> re-queue */
    db.fail_req_flush = 1;
    um_record(um, 3, "gpt-4o", 200, 1, 1, 0, 1000000, "openai");
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain reqs 2");
    TEST_ASSERT(n == 1, "1 pending audit row, got %d", n);
    TEST_ASSERT(um_flush_reqs(&db, buf, n) == -1, "flush fails");
    TEST_ASSERT(um_requeue_requests(um, n) == 0, "re-queue after failure");
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain again");
    TEST_ASSERT(n == 1, "row survives re-queue");
    db.fail_req_flush = 0;
    TEST_ASSERT(um_flush_reqs(&db, buf, n) == 0, "flush succeeds on retry");
    TEST_ASSERT(um_release_requests(um, n) == 0, "release after success");
    TEST_ASSERT(um_drain_requests(um, buf, 16, &n) == 0, "drain empty");
    TEST_ASSERT(n == 0, "ring empty after release");

    usage_meter_free(um);
    pg_store_close(ps);
}
```

**实现注意**:`um_db` 已含 `int req_flush_calls;`(`4a`),`um_flush_reqs` 成功路径
`db->req_flush_calls++;`(4b 已写)。**ring 满丢弃测试**(cap 4096 不可手工灌):
ring-full 由写端 4096 并发压测在集成 smoke 中观察 warn 日志即可,单测只锁
「drop 计数接口存在且 0 基线」,**不**为 ring-full 造假 cap(YAGNI)。

**变更 4d** — `run_tests.c` 注册（extern 声明 74–78 行区 + register 126 行区）：

```c
    extern void test_um_request_ring(void);
    test_register("um_request_ring", test_um_request_ring);
```

**验证**：

```bash
cmake --build build
ctest --test-dir build -R unit
```

期望：`um_request_ring` 绿（任务 3 已实现则直接绿；红灯验证在任务 3 实现前跑一次，
确认它引用了尚不存在的 `um_drain_requests` 会编译失败——编译失败即红灯，记录后继续）。

**提交**：

```bash
git add tests/unit/test_usage_meter.c tests/unit/run_tests.c
git commit -m "test(usage): 🧪 audit ring drain/flush/release + drop counter (P0-1)"
```

---

### 任务 5 — `test_pg_store`：fake 明细 flush/query + 真库回读

**文件**：
- Modify: `tests/unit/test_pg_store.c`（`fake_db` 扩展 + 两个 fake op + 装配 + 新 TEST_CASE + 真库断言）

**变更 5a** — `struct fake_db`（26–37 行）追加：

```c
    usage_request_row_t reqs[FAKE_CAP];
    int                 n_reqs;
```

**变更 5b** — fake op（`fake_query_usage` 之后，镜像其过滤风格）：

```c
static int
fake_flush_requests(void* ctx, const usage_request_row_t* rows, int n)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < n && db->n_reqs < FAKE_CAP; i++) {
        db->reqs[db->n_reqs++] = rows[i];
    }
    return 0;
}

static int
fake_query_requests(void*        ctx,
                    long         key_id,
                    time_t       since,
                    usage_request_row_t* out,
                    int          cap,
                    int*         n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < db->n_reqs && *n < cap; i++) {
        usage_request_row_t* r = &db->reqs[i];
        if (key_id != 0 && r->key_id != key_id) {
            continue;
        }
        if (r->ts < since) {
            continue;
        }
        out[(*n)++] = *r;
    }
    return 0;
}
```

`build_fake_ops`（520 行区）装配：

```c
    ops->flush_usage_requests = fake_flush_requests;
    ops->query_usage_requests = fake_query_requests;
```

**变更 5c** — 新 TEST_CASE（`test_pg_fake_usage_flush_and_query` 之后）：

```c
TEST_CASE(test_pg_fake_request_flush_and_query)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    usage_request_row_t rr, buf[4];
    int                 n = 0;

    memset(&db, 0, sizeof db);
    build_fake_ops(&db, &ops);
    ps = pg_store_open("unused", &ops);
    TEST_ASSERT(ps != NULL, "fake store open");

    memset(&rr, 0, sizeof rr);
    rr.key_id = 7;
    strcpy(rr.model_name, "gpt-4o");
    strcpy(rr.provider, "openai");
    rr.http_status = 200;
    rr.prompt_tokens = 12;
    rr.completion_tokens = 34;
    rr.cached_prompt_tokens = 4;
    rr.latency_ns = 88000000;
    rr.ts = 1726704000; /* 2024-09-19 00:00:00 UTC */
    TEST_ASSERT(pg_store_ops(ps)->flush_usage_requests(&db, &rr, 1) == 0, "flush rr");

    TEST_ASSERT(pg_store_ops(ps)->query_usage_requests(
                    &db, 7, 1726600000, buf, 4, &n) == 0,
                "query key 7");
    TEST_ASSERT(n == 1, "1 request row, got %d", n);
    TEST_ASSERT(buf[0].http_status == 200 && buf[0].latency_ns == 88000000,
                "fields roundtrip");

    n = 0;
    TEST_ASSERT(pg_store_ops(ps)->query_usage_requests(&db, 8, 1726600000, buf, 4, &n) == 0,
                "query other key");
    TEST_ASSERT(n == 0, "no rows for other key");

    n = 0;
    TEST_ASSERT(pg_store_ops(ps)->query_usage_requests(
                    &db, 0, 1726704000, buf, 4, &n) == 0,
                "all keys, since boundary");
    TEST_ASSERT(n == 1, "all-keys query returns the row");

    pg_store_close(ps);
}
```

注册（`run_tests.c`）：`test_register("pg_fake_request_flush_and_query", ...)`。

**变更 5d** — 真库（`test_pg_real_roundtrip`，约 761 行起）在既有 usage flush/query
断言之后追加（仅在 `TEST_PG_DSN` 设定时运行）：

```c
    /* P0-1: per-request detail roundtrip */
    usage_request_row_t rreq;
    memset(&rreq, 0, sizeof rreq);
    rreq.key_id = id; /* key created above */
    strcpy(rreq.model_name, "itest-req");
    strcpy(rreq.provider, "openai");
    rreq.http_status = 200;
    rreq.prompt_tokens = 5;
    rreq.completion_tokens = 6;
    rreq.latency_ns = 42000000;
    rreq.ts = (time_t)(time(NULL) - 60);
    TEST_ASSERT(pg_store_ops(ps)->flush_usage_requests(
                    pg_store_ops(ps)->ctx, &rreq, 1) == 0,
                "real flush requests");
    usage_request_row_t rbuf[4];
    int                 rn = 0;
    time_t              t_from = (time_t)(time(NULL) - 3600);
    TEST_ASSERT(pg_store_ops(ps)->query_usage_requests(
                    pg_store_ops(ps)->ctx, id, t_from, rbuf, 4, &rn) == 0,
                "real query requests");
    TEST_ASSERT(rn >= 1, "at least one request row");
    TEST_ASSERT(strcmp(rbuf[0].model_name, "itest-req") == 0 &&
                    rbuf[0].http_status == 200,
                "request row fields");
```

注意：真库重跑时 `itest-req` 会累积多行（每次运行插入），`rn >= 1` + 首行字段断言
（`ORDER BY ts DESC` 最新在前）保证幂等。

**验证**：

```bash
cmake --build build
ctest --test-dir build -R unit
# 有测试库时：
TEST_PG_DSN="..." ctest --test-dir build -R unit
```

**提交**：

```bash
git add tests/unit/test_pg_store.c tests/unit/run_tests.c
git commit -m "test(pg): 🧪 usage_requests fake roundtrip + real-DB assertions (P0-1)"
```

---

### 任务 6 — admin 端点 `GET /admin/v1/usage/requests`

**文件**：
- Modify: `src/admin_api.c`（handler + dispatch 分支）

**变更 6a** — `usage_query`（1309–1375 行）之后新增 handler：

```c
/** @brief GET /admin/v1/usage/requests?key_id=&since=YYYY-MM-DD
 * Per-request audit detail (newest first). since empty = last 7 days. */
static int
usage_requests_query(admin_ctx_t* adm,
                     int*        status,
                     char**      body,
                     size_t*     len,
                     const char* query)
{
    char key[32] = "", since[16] = "";
    query_param(query, "key_id", key, sizeof key);
    query_param(query, "since", since, sizeof since);

    char* kend = NULL;
    long  key_id = strtol(key, &kend, 10);
    if (key[0] != '\0' && (kend == key || key_id <= 0)) {
        return finish_error(status, body, len, 400, "bad_request", "bad ?key_id=<id>");
    }
    time_t t_since;
    if (since[0] == '\0') {
        time_t now = time(NULL);
        t_since = now - (now % 86400) - 6 * 86400;
    } else if (parse_day(since, &t_since) != 0) {
        return finish_error(status, body, len, 400, "bad_request",
                            "bad since date (use YYYY-MM-DD)");
    }

    usage_request_row_t* rows = calloc(USAGE_LIST_CAP, sizeof *rows);
    if (rows == NULL) {
        return -1;
    }
    int n = 0;
    int rc = pg_store_ops(adm->ps)->query_usage_requests(pg_store_ops(adm->ps)->ctx,
                                                         key_id,
                                                         t_since,
                                                         rows,
                                                         USAGE_LIST_CAP,
                                                         &n);
    if (rc != 0) {
        free(rows);
        return finish_error(status, body, len, 500, "internal_error", "request query failed");
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t*   o = json_object();
        char      ts_iso[32];
        struct tm tmv;
        if (gmtime_r(&rows[i].ts, &tmv) != NULL) {
            strftime(ts_iso, sizeof ts_iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        } else {
            snprintf(ts_iso, sizeof ts_iso, "1970-01-01T00:00:00Z");
        }
        json_object_set_new(o, "key_id", json_integer(rows[i].key_id));
        json_object_set_new(o, "model", json_string(rows[i].model_name));
        json_object_set_new(o, "provider", json_string(rows[i].provider));
        json_object_set_new(o, "http_status", json_integer(rows[i].http_status));
        json_object_set_new(o, "prompt_tokens", json_integer(rows[i].prompt_tokens));
        json_object_set_new(o, "completion_tokens", json_integer(rows[i].completion_tokens));
        json_object_set_new(o, "cached_prompt_tokens",
                            json_integer(rows[i].cached_prompt_tokens));
        json_object_set_new(o, "latency_ms",
                            json_real(rows[i].latency_ns / 1000000.0));
        json_object_set_new(o, "ts", json_string(ts_iso));
        json_array_append_new(arr, o);
    }
    free(rows);

    json_t* root = json_object();
    json_object_set_new(root, "key_id", json_integer(key_id));
    json_object_set_new(root, "requests", arr);
    int dropped = adm->ac != NULL ? um_requests_dropped(adm->ac->um) : 0;
    json_object_set_new(root, "dropped", json_integer(dropped));
    return finish_json(status, body, len, 200, root);
}
```

注意：`#include "usage_meter.h"`（经 `admin_api.h` → `aigate_core.h` 已可见 `aigate_core`；
`um_requests_dropped` 需 `usage_meter.h` 直接可见——在 `admin_api.c` 头部确认/补 include）。
`adm->ac->um` 字段名按 `aigate_core.h` 实际成员名（写码时 `grep 'um' src/aigate_core.h` 确认，
若名为 `usage` 则替换）。

**变更 6b** — dispatch（约 1486 行 `rest, "usage"` 分支旁）：

```c
    } else if (strcmp(rest, "usage/requests") == 0 && strcmp(method, "GET") == 0) {
        return usage_requests_query(adm, out_status, out_body, out_len, query);
    } else if (strcmp(rest, "usage") == 0 && strcmp(method, "GET") == 0) {
        return usage_query(adm, out_status, out_body, out_len, query);
    }
```

（`usage/requests` 分支必须在前；现有 `strcmp(rest, "usage")` 精确匹配，天然无冲突。）

**验证**：

```bash
cmake --build build
ctest --test-dir build -R unit
```

期望：零告警；端点由任务 7 的 admin fake 测试覆盖。

**提交**：

```bash
git add src/admin_api.c
git commit -m "feat(admin): 🚀 GET /admin/v1/usage/requests audit endpoint (P0-1)"
```

---

### 任务 7 — `test_admin_api` fake 明细装配 + 端点测试

**文件**：
- Modify: `tests/unit/test_admin_api.c`（`fake_db` 扩展 + 两个 fake op + 装配 + 新 TEST_CASE）
- Modify: `tests/unit/run_tests.c`（注册）

**变更 7a** — `struct fake_db`（27 行起）追加：

```c
    usage_request_row_t reqs[FAKE_CAP];
    int                 n_reqs;
```

**变更 7b** — fake op（`fake_query_usage` 之后，镜像 5b 的过滤风格；`since` 用 `ts` 比较）：

```c
static int
fake_flush_requests(void* ctx, const usage_request_row_t* rows, int n)
{
    struct fake_db* db = ctx;
    for (int i = 0; i < n && db->n_reqs < FAKE_CAP; i++) {
        db->reqs[db->n_reqs++] = rows[i];
    }
    return 0;
}

static int
fake_query_requests(void*        ctx,
                    long         key_id,
                    time_t       since,
                    usage_request_row_t* out,
                    int          cap,
                    int*         n)
{
    struct fake_db* db = ctx;
    *n = 0;
    for (int i = 0; i < db->n_reqs && *n < cap; i++) {
        usage_request_row_t* r = &db->reqs[i];
        if (key_id != 0 && r->key_id != key_id) {
            continue;
        }
        if (r->ts < since) {
            continue;
        }
        out[(*n)++] = *r;
    }
    return 0;
}
```

`build_fake_ops`（484 行区）：

```c
    ops->flush_usage_requests = fake_flush_requests;
    ops->query_usage_requests = fake_query_requests;
```

**变更 7c** — 新 TEST_CASE（`test_admin_usage_query` 之后）：

```c
TEST_CASE(test_admin_usage_requests_query)
{
    struct fake_db db;
    pg_ops_t       ops;
    pg_store_t*    ps;
    aigate_core    core;
    admin_ctx_t    adm;
    char           admin_hash[65];

    setup_admin(&db, &ops, &ps, &core, &adm, admin_hash);

    usage_request_row_t rr;
    memset(&rr, 0, sizeof rr);
    rr.key_id = 42;
    snprintf(rr.model_name, sizeof rr.model_name, "gpt-4o");
    snprintf(rr.provider, sizeof rr.provider, "openai");
    rr.http_status = 200;
    rr.prompt_tokens = 500;
    rr.completion_tokens = 200;
    rr.cached_prompt_tokens = 10;
    rr.latency_ns = 123000000;
    rr.ts = 1726704000; /* 2024-09-19 00:00 UTC */
    fake_flush_requests(&db, &rr, 1);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* keyed + since inside the row's day */
    int rc = admin_dispatch(
        &adm,
        "/admin/v1/usage/requests?key_id=42&since=2024-09-01",
        "GET",
        "127.0.0.1",
        "admin-secret-token",
        NULL,
        0,
        &status,
        &body,
        &len);
    TEST_ASSERT(rc == 0 && status == 200, "requests query -> 200");
    json_t* j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(j != NULL, "parsed json");
    json_t* rarr = json_object_get(j, "requests");
    TEST_ASSERT(rarr != NULL && json_array_size(rarr) == 1, "1 request row");
    json_t* r0 = json_array_get(rarr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(r0, "http_status")) == 200,
                "http_status 200");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(r0, "model")), "gpt-4o") == 0,
                "model");
    TEST_ASSERT(json_integer_value(json_object_get(r0, "prompt_tokens")) == 500,
                "prompt_tokens");
    TEST_ASSERT(fabs(json_real_value(json_object_get(r0, "latency_ms")) - 123.0) < 0.01,
                "latency_ms 123");
    json_decref(j);

    /* other key: empty array */
    body = NULL;
    rc = admin_dispatch(
        &adm,
        "/admin/v1/usage/requests?key_id=99&since=2024-09-01",
        "GET",
        "127.0.0.1",
        "admin-secret-token",
        NULL,
        0,
        &status,
        &body,
        &len);
    TEST_ASSERT(rc == 0 && status == 200, "empty query -> 200");
    j = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(json_object_get(j, "requests") != NULL &&
                    json_array_size(json_object_get(j, "requests")) == 0,
                "zero rows for unknown key");
    json_decref(j);

    teardown_admin(ps, &core, &db);
}
```

注意：`#include <math.h>` 若缺则补（`fabs`）；`setup_admin`/`teardown_admin`/`admin_dispatch`
签名以 488 行起的实际定义为准（`teardown_admin(ps, &core, &db)` 与既有 `test_admin_usage_query`
的收尾调用保持一致）。

注册：`test_register("admin_usage_requests_query", test_admin_usage_requests_query);`

**验证**：

```bash
cmake --build build
ctest --test-dir build -R unit
```

期望：`admin_usage_requests_query` 绿；既有 `admin_usage_query`（日聚合端点）不回归。

**提交**：

```bash
git add tests/unit/test_admin_api.c tests/unit/run_tests.c
git commit -m "test(admin): 🧪 usage/requests endpoint roundtrip via fake ops (P0-1)"
```

---

### 任务 8 — 其余 8 个 fake ops 文件补 stub（编译完整）

**文件**（均加 2 行装配，stub 直接挂既有 fail/noop 变参桩，签名与 `fo_noop` 同型）：

| 文件 | 既有桩 | 装配写法 |
|---|---|---|
| `tests/unit/test_auth_key.c` | `akg_other`（变参 `return -1`，44 行） | 见下 |
| `tests/unit/test_failover.c` | `fo_noop`（变参 `return 0`，77 行） | 见下 |
| `tests/unit/test_model_router.c` | `mro_fail`（变参 `return -1`，39 行） | 见下 |
| `tests/unit/test_stream_pipeline.c` | `f_flush_rows`（真实 flush，67 行） | 新增 `f_req_stub` |
| `tests/unit/test_aigate_core.c` | `f_flush_rows`（真实 flush） | 新增 `f_req_stub` |
| `tests/unit/test_embeddings.c` | `fflush_cb`（真实 flush，390 行区） | 新增 `f_req_stub` |
| `tests/unit/test_provider_anthropic.c` | `f_flush`（真实 flush，366 行区） | 新增 `f_req_stub` |

**变更**（每文件一处，紧跟既有 `ops.flush_usage = ...` 行）：

`test_auth_key.c`（约 65 行）：

```c
    ops.flush_usage_requests =
        (int (*)(void*, const usage_request_row_t*, int))akg_other;
    ops.query_usage_requests =
        (int (*)(void*, long, time_t, usage_request_row_t*, int, int*))akg_other;
```

`test_failover.c`（约 91 行区）与 `test_model_router.c`（约 53 行区）：同样式，
挂 `fo_noop` / `mro_fail`。

`test_stream_pipeline.c` / `test_aigate_core.c` / `test_embeddings.c` / `test_provider_anthropic.c`：
各加一个共享形式的 stub（每文件内 static，避免跨文件符号冲突）：

```c
static int
f_req_stub(void* ctx, ...)
{
    (void)ctx;
    /* no-op success: audit flush is irrelevant to this test's assertions */
    return 0;
}
```

装配：

```c
    ops->flush_usage_requests = (int (*)(void*, const usage_request_row_t*, int))f_req_stub;
    ops->query_usage_requests =
        (int (*)(void*, long, time_t, usage_request_row_t*, int, int*))f_req_stub;
```

（变参 `...` 桩与函数指针强转同型，与既有 `akg_other` 用法一致——C 标准允许强转后
调用，UB 在实践中为良性且全仓已用此模式；**不要**改既有桩的签名。）

**验证**：

```bash
cmake --build build
ctest --test-dir build -R unit
```

期望：零告警（`-Werror` 覆盖 stub 强转路径无新告警）；全绿。

**提交**：

```bash
git add tests/unit/test_auth_key.c tests/unit/test_failover.c tests/unit/test_model_router.c tests/unit/test_stream_pipeline.c tests/unit/test_aigate_core.c tests/unit/test_embeddings.c tests/unit/test_provider_anthropic.c
git commit -m "chore(tests): 🧹 stub usage_requests ops in remaining fake ops (P0-1)"
```

---

### 任务 9 — 全量验证 + 集成 smoke

**验证**（一次跑完，全部通过才算完成）：

```bash
# 1. 零告警构建（-Werror 在 CMake 默认开启）
cmake -S . -B build && cmake --build build

# 2. 全量单测
ctest --test-dir build -R unit

# 3. 真库路径（仅有 TEST_PG_DSN 时；无则跳过并记录）
TEST_PG_DSN="$TEST_PG_DSN" ctest --test-dir build -R unit --output-on-failure

# 4. 集成 smoke（起 PG + 网关，覆盖鉴权/转发路径）
bash tests/integration/smoke.sh 2>&1 | tail -20
```

**检查项**：

- [ ] 构建零告警（任务 8 的 stub 强转无 `-Wcast` 告警）。
- [ ] `ctest -R unit` 全绿，新增 4 个用例（`um_request_ring`、`pg_fake_request_flush_and_query`、
      `admin_usage_requests_query` + 真库段）全部通过。
- [ ] smoke 全绿（鉴权路径未受 ring 写影响；`um_record` 在转发成功路径写 ring，smoke 的
      转发断言覆盖该路径）。
- [ ] 真库迁移幂等：`pg_real_roundtrip` 连续两次 `pg_store_migrate` 均 0（v6 `IF NOT EXISTS`
      保证；该断言在既有测试 758 行区已覆盖，v6 块并入同一 SCHEMA_SQL 事务）。

**冒烟新端点**（真库 + 运行实例时）：

```bash
# 发一次 chat 请求（smoke 内已有），然后：
curl -s -H "Authorization: Bearer $ADMIN_TOKEN" \
  "http://127.0.0.1:$PORT/admin/v1/usage/requests?key_id=<id>&since=$(date -u +%F)" \
  | jq '.requests[0:1]'
```

期望：`requests[0]` 含 `model` / `provider` / `http_status` / `ts`（ISO8601Z）字段。

**收尾提交**（仅当有修复）：

```bash
git add -A
git commit -m "fix(usage): 🐛 P0-1 verification fixes"
```

---

## 执行前检查（Pre-flight）

1. `grep -n 'usage_meter_new\|um =' src/aigate_core.h` 确认 `aigate_core` 的 meter 成员名（任务 6 `adm->ac->um`）。
2. `grep -n 'copy_field' src/pg_store.c` 确认 2b 的字段拷贝辅助函数名。
3. `sed -n '1,20p' src/usage_meter.h` 确认 include 列表（任务 3 的 `usage_request_row_t` 可见性）。
4. 任务 4 的红灯：在任务 3 落地前 `ctest` 必编译失败（引用未定义符号）——**先**跑一次记录
   失败输出，**再**实现，**然后**转绿。
5. 每个任务一个提交；全部任务完成后跑任务 9，无修复则不收尾提交。

**不做（记录理由）**：
- 不改 `/metrics` 渲染（dropped 计数走 admin 端点 `dropped` 字段；指标命名留给 P1-2 成本核算一并定）。
- 不做 `usage_requests` 按天分区（spec 写「按天分区或索引 ts」；当前内部单节点量级，两个 B-tree
  索引足够，分区维护成本 > 收益，P2 再评估）。
- 不做 ClickHouse/log DB 侧（new-api 的可选组件，aigate 单二进制 + PG 是特性，§3-12 已判「不追」）。

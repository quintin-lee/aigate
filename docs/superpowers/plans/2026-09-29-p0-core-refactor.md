# P0 Core Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 拆分 `aigate_handle_request`（复杂度 93→<15）并保守优化 stream 缓存累积路径（含超长行截断修复），零缓存格式变化。

**Architecture:** 同文件 `static` 函数抽取 + `chat_req_t` 请求上下文收敛 cleanup；stream 侧抽 `accumulate_sse_line` 并将定长 `line_buf[4096]` 改为动态增长。纯搬运优先，行为变化仅截断修复一处。

**Tech Stack:** C17, jansson, civetweb, CMake (src 为 `file(GLOB ... CONFIGURE_DEPENDS)`，零构建变更）, pytest 集成测试。

## Global Constraints

- 只改 `src/core/aigate_core.c`（拆分/优化）+ `tests/integration/mock_upstream.py` 与 `test_gateway.py`（专项测试）；不新建 `.c` 文件，不改头文件与 CMake。
- 不动缓存条目格式、两处 replay、非流式路径、三个已委托 handler（responses/messages/gemini）。
- 单次 500 重试 3 处留内联，不抽取。
- 每个新函数复杂度 <15（交付前用知识图谱复测）。
- commit 信息遵循 gitmoji（`refactor(core): ♻️ ...` / `fix(core): 🐛 ...`）。

---

### Task 1: 上下文结构体 + 门禁/模型列表抽取

**Files:**
- Modify: `src/core/aigate_core.c:361` 后插入；`src/core/aigate_core.c:2172-2236`；`src/core/aigate_core.c:2238-2287`

**Interfaces:**
- Consumes: 既有类型 `key_rec_t/model_rec_t/upstream_target_t`（`MAX_TARGETS_PER_MODEL` 已在文件内使用）。
- Produces: `chat_req_t`、`chat_req_cleanup`、`gate_request`、`handle_models_list`（后继 Task 消费）。

- [ ] **Step 1:  pin 行号**

Run: `grep -n "^aigate_handle_request\|^handle_responses\|^cache_stream_replay" src/core/aigate_core.c && wc -l src/core/aigate_core.c`
Expected: `aigate_handle_request` 在 2156 行附近，总行数 3104。若漂移超过 ±5 行，停止并按实际行号重 pin 本 plan 所有行段。

- [ ] **Step 2: 插入 `chat_req_t` + cleanup（L361 结构体块之后）**

```c
/** @brief Chat pipeline per-request context: owns jbody/krec/sanitized_body. */
typedef struct {
    aigate_core*    ac;
    aigate_request_ctx*  rq;
    aigate_response_ctx* rc;
    key_rec_t       krec;
    json_t*         jbody;
    const char*     model;
    model_rec_t     route;
    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int             n_candidates;
    char*           sanitized_body;
    size_t          sanitized_len;
    const void*     eff_body;
    size_t          eff_len;
    char            guardrail_act[16];
    char            cache_key[65];
    bool            bypass_cache;
    bool            no_store;
} chat_req_t;

/** @brief Release owned request resources (mirrors the historical triple-cleanup). */
static void
chat_req_cleanup(chat_req_t* q)
{
    if (q->jbody != NULL) {
        json_decref(q->jbody);
        q->jbody = NULL;
    }
    key_rec_free(&q->krec);
    free(q->sanitized_body);
    q->sanitized_body = NULL;
}
```

- [ ] **Step 3: 抽取 `gate_request`（原 L2172–2236 逐行下移，`&krec`→`&q->krec`，`ac`→`q->ac`）**

```c
/** @brief Auth → QPS → daily quota → monthly budget gates.
 *  @return 0 when all gates pass; non-zero when an error was already written. */
static int
gate_request(chat_req_t* q)
```

函数体为原 L2173–2236（`/* --- auth --- */` 起至 budget 块尾），其中每个错误分支的
`key_rec_free(&krec); return 0;` 保持原样（cleanup 由调用方统一做，见 Task 5；
本 Task 内 `gate_request` 仍在原函数体内调用，行为不变）。

- [ ] **Step 4: 抽取 `handle_models_list`（原 L2238–2287 逐行下移）**

```c
/** @brief GET /v1/models handler. @return 1 when the path was handled, 0 to continue. */
static int
handle_models_list(chat_req_t* q)
```

调用点替换原 L2238–2287 整段为：

```c
    {
        int handled = handle_models_list(&chatq);
        if (handled != 0) {
            return handled > 0 ? 0 : handled;
        }
    }
```

约定：返回 1 = 已处理并写回（调用方 `return 0`）；返回 0 = 非 models 路径继续；
返回负数 = 已写错误（调用方直接 `return` 该值）。`handle_models_list` 内部原
`return aigate_write_error(...)` 改为 `return -1`（错误已写），`return rv` 改为 `return 1`。

- [ ] **Step 5: 主入口接线（本 Task 仅替换门禁段与 models 段，其余不动）**

在 `aigate_handle_request` 顶部协议分发之后插入：

```c
    chat_req_t chatq;
    memset(&chatq, 0, sizeof(chatq));
    chatq.ac = ac;
    chatq.rq = rq;
    chatq.rc = rc;
    if (gate_request(&chatq) != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }
```

并删除原 L2172–2236 门禁代码（已移入 `gate_request`）。
注意：本 Task 结束时 `krec/jbody` 仍由旧代码路径管理——`chatq.krec` 与旧局部
`krec` 并存；旧代码继续用旧局部变量，新 helper 用 `chatq`。Task 5 统一收敛，
此前双轨并存但互不干扰（`gate_request` 内只读写 `q->krec`）。

- [ ] **Step 6: 构建 + 单测**

Run: `cmake --build build 2>&1 | tail -3`
Expected: exit 0，无 warning 新增（对比基线 `git stash` 前后可忽略）。

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `6/6 passed`（5 hdr_histogram + 1 unit）。

---

### Task 2: 目标解析 + 三个小 helper

**Files:**
- Modify: `src/core/aigate_core.c:2289-2358`（resolve+guardrails 段）；三处 `snprintf(cur_route...)` 三连；×6 处 settle 三连；×3 处 failover 日志。

**Interfaces:**
- Consumes: Task 1 的 `chat_req_t`。
- Produces: `resolve_chat_target`、`fill_cur_route`、`settle_success`、`failover_warn`。

- [ ] **Step 1: 抽取 `resolve_chat_target`（原 L2289–2358 下移）**

```c
/** @brief Body model parse → allowlist → router resolve → candidates → guardrails.
 *  @return 0 on success with q filled; non-zero when an error was already written. */
static int
resolve_chat_target(chat_req_t* q)
```

内部 `model/route/candidates/n_candidates/sanitized_body/eff_body/guardrail_act`
全部改为 `q->` 成员；`jbody` 局部改为 `q->jbody`。错误分支保持 `return 0/非零` 语义
（由 Task 5 调用方统一 cleanup，本 Task 先在原函数体内调用，行为不变）。

- [ ] **Step 2: 新增三个小 helper（置于 `resolve_chat_target` 之前）**

```c
static void
fill_cur_route(const model_rec_t* route, const upstream_target_t* t, model_rec_t* out)
{
    *out = *route;
    snprintf(out->provider, sizeof out->provider, "%.*s",
             (int)sizeof out->provider - 1, t->provider);
    snprintf(out->endpoint, sizeof out->endpoint, "%.*s",
             (int)sizeof out->endpoint - 1, t->endpoint);
    snprintf(out->upstream_key, sizeof out->upstream_key, "%.*s",
             (int)sizeof out->upstream_key - 1, t->upstream_key);
}

/** @brief Post-success settlement: usage event + budget record + daily token reserve. */
static void
settle_success(chat_req_t* q, int status, long ptok, long ctok, long cached_tok,
               uint64_t lat, const char* provider, double cost)
{
    record_usage_and_event(q->ac, q->krec.key_id, q->model, status, ptok, ctok,
                           cached_tok, 0, lat, provider, q->guardrail_act, cost);
    if (q->ac->be != NULL) {
        budget_enforce_record(q->ac->be, q->krec.key_id, q->krec.group_id, cost, ptok + ctok);
    }
    if (ptok + ctok > 0) {
        rl_reserve_tokens(q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, ptok + ctok);
    }
}

static void
failover_warn(const char* label, const char* model, const upstream_target_t* from,
              const upstream_target_t* to, int status, int urc)
{
    AIGATE_LOG_WARN("%s for model %s from %s (%s) to %s (%s) due to status %d (urc %d)",
                    label, model, from->provider, from->endpoint,
                    to->provider, to->endpoint, status, urc);
    metrics_inc_failover(model, from->provider, to->provider);
}
```

- [ ] **Step 3: 本 Task 只新增不替换调用点**（替换在 Task 3–5 各循环搬运时顺手做，避免中间态大 diff）。
- [ ] **Step 4: 构建 + 单测**（同 Task 1 Step 6，期望相同）。

---

### Task 3: 抽取 `handle_embeddings`（原 L2360–2557）

**Files:**
- Modify: `src/core/aigate_core.c:2360-2557`

**Interfaces:**
- Consumes: Task 1–2 的 `chat_req_t`、`fill_cur_route`、`settle_success`、`failover_warn`。
- Produces: `handle_embeddings`（Task 5 调度）。

- [ ] **Step 1: 整段下移并改写**

```c
/** @brief /v1/embeddings failover loop. @return 1 when the path was handled. */
static int
handle_embeddings(chat_req_t* q)
```

改写规则（机械替换）：`ac`→`q->ac`、`rc`→`q->rc`、`rq` 不再使用；
`route`→`q->route`、`candidates`→`q->candidates`、`n_candidates`→`q->n_candidates`；
`model`→`q->model`；`sanitized_body`→`q->sanitized_body`；`eff_body/eff_len`→`q->`；
`guardrail_act`→`q->guardrail_act`；三连 cleanup
`free(sanitized_body); json_decref(jbody); key_rec_free(&krec);`→`chat_req_cleanup(q);`；
`cur_route` snprintf 三连→`fill_cur_route(&q->route, target, &cur_route);`；
6 行 settle 块（L2485–2503）→`settle_success(q, parsed_status, ptok, 0, 0, total_lat, target->provider, req_cost);`；
failover 日志 4 行→`failover_warn("failover embeddings", q->model, target, &q->candidates[ci+1], status, urc);`；
所有 `return 0 / return rv` 保持（调用方见 Task 5）。
函数首加路径守卫：`if (q->rq->path == NULL || strcmp(q->rq->path, "/v1/embeddings") != 0) return 0;`
尾部（原 L2547–2556 fallthrough）改为 `return 0`（已写错误）。

- [ ] **Step 2: 构建 + ctest**（期望：build exit 0，6/6）。
- [ ] **Step 3: embeddings 集成回归**

Run: `python3 -m pytest tests/integration/test_gateway.py -k "embedding" -q 2>&1 | tail -3`
Expected: 所选用例全绿（若 -k 无命中，改跑全文件并记录）。

---

### Task 4: 抽取 `prepare_chat_cache` + `handle_chat_stream`

**Files:**
- Modify: `src/core/aigate_core.c:2559-2646`（能力检查+cache 段）；`src/core/aigate_core.c:2648-2919`（stream 循环）。

**Interfaces:**
- Consumes: Task 1–2 全部产物。
- Produces: `prepare_chat_cache`、`handle_chat_stream`。

- [ ] **Step 1: 抽取 `prepare_chat_cache`（原 L2559–2646）**

```c
/** @brief Chat capability check + cache-control parse + cache lookup.
 *  @param[out] is_streaming set from body "stream" flag.
 *  @return 1 when the response was already written (HIT or unsupported);
 *          0 to continue to upstream; <0 never (reserved). */
static int
prepare_chat_cache(chat_req_t* q, bool* is_streaming)
```

改写规则同 Task 3；其中两处 HIT 直接返回（非流写回 / `cache_stream_replay`）
保持 `return 1`；`unsupported_provider` 分支保持写错后 `return 1`。

- [ ] **Step 2: 抽取 `handle_chat_stream`（原 L2648–2919 整循环下移）**

```c
/** @brief Streaming chat failover loop (SSE + cache accumulation). */
static int
handle_chat_stream(chat_req_t* q)
```

改写规则同 Task 3，另：`cache_key/no_store`→`q->`；三处
`if (acc.accum_content != NULL) free(...)` 保持原样（Task 8 再加 `line_buf` 释放）；
`json_pack` 存缓存段（原 L2875–2898）原样下移；settle 三连（原 L2826–2844、L2857–2873）
→`settle_success`；failover 日志→`failover_warn("streaming failover", ...)`；
`single_retry` 段留内联。

- [ ] **Step 3: 构建 + ctest**（期望同前）。
- [ ] **Step 4: stream 集成回归**

Run: `python3 -m pytest tests/integration/test_gateway.py -k "streaming or stream" -q 2>&1 | tail -3`
Expected: 全绿。命中应含 `test_openai_streaming`、`test_response_cache_streaming_dual_replay` 等。

---

### Task 5: 抽取 `handle_chat_sync` + 主入口收薄 + 双轨收敛

**Files:**
- Modify: `src/core/aigate_core.c:2921-3104`；`aigate_handle_request` 全体重写为分发。

**Interfaces:**
- Consumes: Task 1–4 全部产物。
- Produces: 瘦主入口（目标约 80 行、复杂度 <15）。

- [ ] **Step 1: 抽取 `handle_chat_sync`（原 L2921–3104，规则同 Task 3；failover label 用 `"failover"`）**
- [ ] **Step 2: 主入口重写为纯分发**

```c
int
aigate_handle_request(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    if (rq->path != NULL && strcmp(rq->path, "/v1/responses") == 0) {
        return handle_responses(ac, rq, rc);
    }
    if (rq->path != NULL && strcmp(rq->path, "/v1/messages") == 0) {
        return handle_anthropic_messages(ac, rq, rc);
    }
    if (rq->path != NULL &&
        (strstr(rq->path, ":generateContent") != NULL ||
         strstr(rq->path, ":streamGenerateContent") != NULL)) {
        return handle_gemini_generate(ac, rq, rc);
    }

    chat_req_t q;
    memset(&q, 0, sizeof(q));
    q.ac = ac;
    q.rq = rq;
    q.rc = rc;
    if (gate_request(&q) != 0) {
        chat_req_cleanup(&q);
        return 0;
    }
    int handled = handle_models_list(&q);
    if (handled != 0) {
        chat_req_cleanup(&q);
        return handled > 0 ? 0 : handled;
    }
    if (resolve_chat_target(&q) != 0) {
        chat_req_cleanup(&q);
        return 0;
    }
    handled = handle_embeddings(&q);
    if (handled != 0) {
        chat_req_cleanup(&q);
        return 0;
    }
    bool is_streaming = false;
    handled = prepare_chat_cache(&q, &is_streaming);
    if (handled != 0) {
        chat_req_cleanup(&q);
        return 0;
    }
    int rv = is_streaming ? handle_chat_stream(&q) : handle_chat_sync(&q);
    chat_req_cleanup(&q);
    return rv;
}
```

本步同时删除旧局部 `krec/jbody/sanitized_body/model/route/candidates` 全套旧变量与旧代码体
（Task 1 留下的双轨在此收敛；`gate_request/handle_models_list/resolve` 内的旧式
`key_rec_free(&krec)` 需同步改为操作 `q->krec` 后由调用方 cleanup——即删除这些 helper
内部的 `key_rec_free(&...)/json_decref/free(sanitized)` 语句，改为直接 return，
释放统一归主入口 `chat_req_cleanup`）。

- [ ] **Step 3: 构建 + ctest**（期望同前）。
- [ ] **Step 4: 全量集成回归**

Run: `python3 -m pytest tests/integration/test_gateway.py -q 2>&1 | tail -3`
Expected: 37 例全绿（ prevalent gateway 套件为主保护网）。

---

### Task 6: 提交 #1（纯拆分）

- [ ] **Step 1: 核查 diff 仅为搬运**

Run: `git diff --stat && git status --short`
Expected: 仅 `src/core/aigate_core.c` 一个文件；总行数变化 ±30 行以内（纯搬运+helper 约增 60 行注释与签名，删除重复 cleanup 约减 40 行）。

- [ ] **Step 2: 提交**

```bash
git add src/core/aigate_core.c
git commit -m "refactor(core): ♻️ split chat pipeline into staged helpers"
```

---

### Task 7: 抽取 `accumulate_sse_line`

**Files:**
- Modify: `src/core/aigate_core.c:385-483`（`stream_cache_acc_write`）。

**Interfaces:**
- Consumes: `stream_cache_acc_t`（L351–361）。
- Produces: `accumulate_sse_line`（Task 8 消费其行参数形态）。

- [ ] **Step 1: 抽取整行处理（原 L406–466，即 `if (acc->line_len + seg < ...)` 内整块）**

```c
/** @brief Accumulate one complete SSE line (without trailing newline).
 *  Parses `data:` JSON payloads for id/created/delta.content backfill. */
static void
accumulate_sse_line(stream_cache_acc_t* acc, const char* line, size_t len)
```

改写：`acc->line_buf`→`line`（`const char*`，长度用 `len` 参数）；
`strncmp(acc->line_buf, "data: ", 6)`→先判 `len > 6` 再 `memcmp(line, "data: ", 6)`；
`strcmp(acc->line_buf, "data: [DONE]")`→`len == 12 && memcmp(line, "data: [DONE]", 12) == 0`；
`json_loads(acc->line_buf + 6, ...)`→需 NUL 结尾：行内容先拷入栈上临时 VLA 或沿用
`line_buf`——**本 Task 保持沿用 `acc->line_buf`**（换行到达时整行已在 buf 中拼装完毕；
acc_write 改为 `accumulate_sse_line(acc, acc->line_buf, acc->line_len)` 形态）。
其余 JSON walk 逐行下移。

- [ ] **Step 2: `acc_write` 只留分帧循环 + 调用，目标复杂度 <10。构建 + ctest。**

---

### Task 8: 动态行缓冲（截断修复）+ 专项测试

**Files:**
- Modify: `src/core/aigate_core.c:351-361`（结构体）；`stream_cache_acc_write` 两处 append；
  `handle_chat_stream` 内 3 处 `free(acc.accum_content)`；
  `tests/integration/mock_upstream.py:361-398`（长行分支）；
  `tests/integration/test_gateway.py`（新增用例，置于 `test_response_cache_streaming_dual_replay` 之后）。

**Interfaces:**
- Consumes: Task 7 的 `accumulate_sse_line`。
- Produces: 截断修复 + `test_response_cache_stream_long_line`。

- [ ] **Step 1: 结构体变更**

```c
    char*                line_buf;      /**< Growable SSE line buffer (was line_buf[4096]). */
    size_t               line_cap;      /**< line_buf capacity. */
```

`acc` 为栈上 `memset` 零初始化（`handle_chat_stream` 内），`line_buf=NULL/line_cap=0` 起步合法。
增长规则（两处 append 共用宏或内联函数 `acc_line_reserve(acc, need)`）：
初始 4096，按需倍增，硬顶 1MB；超顶置 `acc->overflow = true` 并丢弃本行
（与 `accum` 溢出语义一致：仅透传、不再累积）。

- [ ] **Step 2: 3 处释放点同步**（`handle_chat_stream` 内 `free(acc.accum_content)` 处各加一行）

```c
                if (acc.accum_content != NULL) {
                    free(acc.accum_content);
                }
                free(acc.line_buf);
                acc.line_buf = NULL;
```

3 处位置：pre-headers 失败分支、stream 中断分支、成功收尾分支（原 L2759、L2846、L2899 附近，
以搬运后实际行号为准，先 `grep -n "free(acc.accum_content)"` 定位，必须恰好 3 处）。

- [ ] **Step 3: mock 增加长行分支**（`mock_upstream.py` 非 deepseek 流分支内，Chunk 1 之前插入）

```python
                    msgs = body_json.get("messages", []) if body_json else []
                    prompt_text = " ".join(
                        m.get("content", "") for m in msgs if isinstance(m, dict)
                    )
                    if "LONG_LINE_6K" in prompt_text:
                        big = "X" * 6000
                        c_big = {
                            "id": "chatcmpl-stream",
                            "object": "chat.completion.chunk",
                            "created": 1726700000,
                            "model": req_model,
                            "choices": [{"index": 0, "delta": {"content": big}, "finish_reason": None}],
                        }
                        self.wfile.write(f"data: {json.dumps(c_big)}\n\n".encode("utf-8"))
                        self.wfile.flush()
                        time.sleep(0.01)
```

随后原有 Chunk 1/2/usage 照常发送（保证 tokens 计量与 `[DONE]` 完整）。

- [ ] **Step 4: 新增专项测试**（仿 `test_response_cache_streaming_dual_replay` 的注册/建 key 流程，
prompt 改为 `f"long line probe LONG_LINE_6K (uid={uid})"`；断言）

```python
    # 1. streaming MISS，累积含 6K 单行
    r1 = requests.post(f"{base_url}/v1/chat/completions", headers=client_headers, json=stream_body, stream=True)
    assert r1.status_code == 200
    assert r1.headers.get("X-Cache") == "MISS"
    lines1 = [line.decode("utf-8") for line in r1.iter_lines() if line]
    assert any("data: [DONE]" in l for l in lines1)

    # 2. 非流式同 prompt HIT：缓存体必须含完整 6K 内容（修前此处因截断失败）
    r2 = requests.post(f"{base_url}/v1/chat/completions", headers=client_headers, json=non_stream_body)
    assert r2.status_code == 200
    assert r2.headers.get("X-Cache") == "HIT"
    data2 = r2.json()
    content = data2["choices"][0]["message"]["content"]
    assert len(content) >= 6000
    assert set(content) == {"X"}
```

- [ ] **Step 5: 先验失败再验通过**

Run: `git stash -- src/core/aigate_core.c` 后跑新测试，Expected: FAIL（`len(content) >= 6000` 不成立，
证明测试有效）；`git stash pop` 后重跑，Expected: PASS。
注意：stash 期间 mock 与测试文件保留（只 stash 源码）。

- [ ] **Step 6: 构建 + ctest + 相关 pytest**（期望：build exit 0；ctest 6/6；
` -k "cache or streaming or stream"` 全绿）。

---

### Task 9: 提交 #2 + 全门禁

- [ ] **Step 1: 全量验证**

Run: `cmake --build build 2>&1 | tail -2 && ctest --test-dir build 2>&1 | tail -2`
Expected: exit 0；6/6 passed。

Run: `python3 -m pytest tests/integration/test_gateway.py -q 2>&1 | tail -2`
Expected: 37 passed。

- [ ] **Step 2: 复杂度复测**

通过知识图谱查询 `aigate_handle_request/handle_chat_stream/handle_chat_sync/handle_embeddings/prepare_chat_cache/resolve_chat_target/gate_request/accumulate_sse_line/stream_cache_acc_write`
的 complexity。
Expected: 全部 <15（`acc_write` <10）。

- [ ] **Step 3: 提交**

```bash
git add src/core/aigate_core.c tests/integration/mock_upstream.py tests/integration/test_gateway.py
git commit -m "fix(core): 🐛 growable SSE line buffer fixing cache truncation"
```

## Self-Review

1. **Spec 覆盖**：§1 函数清单→Task 1–6（含双轨收敛、不抽重试）；§2 三件→Task 7–8（抽函数/动态缓冲/小清理并入 Step；
`overflow` 早返与 `strlen` 复用确认为现状最优，plan 未列多余改动）；§3 五条门禁→Task 6/9
（两次 commit、build/ctest/pytest-37/复杂度复测/截断专项）。
2. **占位符扫描**：无 TBD/TODO；mock 与测试代码为完整可运行代码；搬运块以"原 Lx–Ly 逐行下移+改写规则"
给出（纯搬运不复贴 950 行，行号以 Task 1 Step 1 重 pin 为准）。
3. **类型一致**：`settle_success` 签名覆盖 6 处调用点的参数并集（status/ptok/ctok/cached/lat/provider/cost）；
`prepare_chat_cache` 返回值约定（1=已写回/0=继续）在 Task 5 主入口中一致使用；
`handle_models_list` 的 1/0/负数约定在 Task 1 定义、Task 5 使用一致。

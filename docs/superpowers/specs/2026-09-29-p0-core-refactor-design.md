# P0 Core Refactor 设计（拆分主入口 + Stream 热路径保守优化）

- 日期：2026-09-29
- 范围：`src/core/aigate_core.c` 单文件，零新建文件，零头文件/构建变更
- 决策：方案 A（纯结构拆分 + 保守 stream 优化）；B（原始字节累积）因缓存条目被非流式 HIT 路径复用、需格式标记与旧条目兼容而否决

## 背景

知识图谱实测：`aigate_handle_request`（L2156–3104，约 950 行）复杂度 93、传递循环深度 7，
为全仓最高；`handle_responses/handle_anthropic_messages/handle_gemini_generate` 均 >40。
`stream_cache_acc_write`（L385–483）复杂度 22，循环内线性扫描 + 循环内分配，
且超长 SSE 行（超定长 `line_buf`）被静默丢弃，导致缓存累积截断内容。

## §1 拆分：函数清单（已确认）

新增 file-static `chat_req_t` 上下文：ac/rq/rc、krec、jbody、model、route、
candidates[MAX_TARGETS_PER_MODEL]、sanitized_body/len、eff_body/len、
guardrail_act[16]、cache_key[65]、bypass_cache/no_store；
配套 `chat_req_cleanup()` 收敛约 15 个退出点的 `json_decref/key_rec_free/free` 三连。

抽出（全部 `static`）：`gate_request`（L2172–2236 四道门禁）、
`handle_models_list`（L2238–2287）、`resolve_chat_target`（L2289–2358）、
`handle_embeddings / handle_chat_stream / handle_chat_sync`（三大 failover 循环整体下移）、
`settle_success`（×6 处 usage+budget+reserve 三连）、
`fill_cur_route`、`failover_warn`（各 ×3 处）。
刻意不抽：单次 500 重试（3 处签名各异，留内联）。
目标：主入口约 80 行纯分发；每个函数复杂度 <15；总行数基本不变。

## §2 Stream 改动（已确认）

1. 抽 `accumulate_sse_line(acc, line, len)`：整行 data 解析与累积下移；
   `acc_write` 只留 memchr 分帧循环。目标：`acc_write` <10，新函数 <12。
2. 修超长行静默截断：`line_buf` 定长数组改动态增长（realloc，封顶 1MB，
   超封顶置 `overflow`，与现有 `accum` 溢出语义一致）。唯一行为变化，属 bug 修复。
3. 不碰：缓存条目格式、存入 `json_pack`、两处 replay、非流式路径。

## §3 验证（已确认）

1. `cmake --build build` exit 0（plan 第一步先确认 `src/` CMake 是否 GLOB；本设计零新建文件）。
2. `ctest --test-dir build` 6/6；`pytest tests/integration/test_gateway.py` 37 例全绿。
3. 知识图谱复杂度复测：主入口 <15、新函数 <15、`acc_write` <10。
4. 截断修复专项：构造超长 SSE 行，走 stream 存入→replay，修前截断、修后完整；其余零行为差。
5. 分两次 commit：先纯搬运拆分，再 stream 三件。

## 非目标

- 不拆新文件；不动 `handle_responses/messages/gemini` 三个已委托 handler；
- 不动缓存格式与 replay；不做压测（延迟大头在上游 IO，未证伪前不优化）。

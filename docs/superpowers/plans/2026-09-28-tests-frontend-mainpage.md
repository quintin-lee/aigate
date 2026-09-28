# Tests＋前端＋首页注释 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐 tests 152 处 static 辅助函数、C 注释、admin.html 107 个 JS 函数 `//` 注释，并把根 README 设为 doxygen 首页。

**Architecture:** 三批三提交、每批独立四验证。Task1 只动 `tests/unit/**`（doxygen INPUT=src，tests 不进解析，零风险）；Task2 只动 `web/admin.html`（同不在 INPUT）；Task3 只加 Doxyfile 一行。全部单行注释，机检天然通过。

**Tech Stack:** C (Doxygen `/** */`)、JS (`//`)、Doxyfile。

**基线（已实测 2026-09-28）：** doxygen 0 警告；152 处用脚本核数（`^static` 函数定义且上无 `*/` 结尾）；107 处同理（`function name(` 且上无 `//`）。20 个测试文件（spec 写 19，实测 20——多出 `test_upstream_streaming.c` 1 处，以实测为准，总数 152 一致）。

**全局规则：**
- C 注释一律单行 `/** @brief … */`，加在签名首行（返回类型行）之上；同文件从**行号大到小**依次加，保证行号有效。
- 不动 169 个 TEST_CASE、不动 mock_upstream.c / run_tests.c（已补）、不动 embed_html.py。
- comment-hook 应对：第 3 类（spec 批准的必要文档），沿用前八轮 justification。

---

### Task 1: tests 152 处 static 注释

**Files:** `tests/unit/` 下 20 个 `.c` 文件（只加行，不改代码）。

**Step 1（common＋core 小文件，6 处）：**

- [ ] `tests/unit/common/test_lru.c` L8 上加：`/** @brief LRU 淘汰回调：计数一次淘汰。 */`
- [ ] `tests/unit/common/test_lru.c` L69 上加：`/** @brief 并发 worker 线程入口：循环读写 LRU。 */`
- [ ] `tests/unit/core/test_config.c` L8 上加：`/** @brief 清空测试相关环境变量。 */`
- [ ] `tests/unit/core/test_log.c` L13 上加：`/** @brief 日志并发 worker 线程入口。 */`
- [ ] `tests/unit/core/test_secrets.c` L7 上加：`/** @brief 生成 32 字节测试主密钥。 */`

**Step 2（test_aigate_core.c，18 处）：**

- [ ] L52 `/** @brief fake allowlist：整条拷贝 key 记录。 */`
- [ ] L77 `/** @brief fake 查 key：命中返回 1，未命中返回 0。 */`
- [ ] L99 `/** @brief fake 查模型路由：命中返回 1，未命中返回 0。 */`
- [ ] L112 `/** @brief fake 用量落库：吞掉行并返回成功。 */`
- [ ] L123 `/** @brief fake 上游请求桩：恒返回成功。 */`
- [ ] L130 `/** @brief fake 列模型：拷贝内存表到输出。 */`
- [ ] L146 `/** @brief fake 列护栏规则：拷贝内存表到输出。 */`
- [ ] L159 `/** @brief 组装 fake pg_ops 虚表，ctx 指向内存库。 */`
- [ ] L201 `/** @brief 置内存 key 槽位的护栏开关。 */`
- [ ] L207 `/** @brief 置内存 key 槽位的费用/token 预算。 */`
- [ ] L238 `/** @brief 捕获响应头：记入 cap 缓冲。 */`
- [ ] L246 `/** @brief 捕获响应体分片：追加进 cap 缓冲。 */`
- [ ] L259 `/** @brief 由 cap 缓冲构造响应上下文。 */`
- [ ] L270 `/** @brief 检查捕获头中是否含某子串。 */`
- [ ] L300 `/** @brief 驱动一次带请求体的推理调用，返回响应体（借用）。 */`
- [ ] L698 `/** @brief 驱动一次 /v1/responses 调用，返回响应体（借用）。 */`
- [ ] L807 `/** @brief 驱动一次 Anthropic messages 调用，返回响应体（借用）。 */`
- [ ] L825 `/** @brief 驱动一次 Gemini generate 调用，返回响应体（借用）。 */`

**Step 3（policy 小文件，12 处）：**

- [ ] `test_auth_key.c` L16 `/** @brief fake 查 key：命中返回 1，未命中返回 0。 */`
- [ ] `test_auth_key.c` L43 `/** @brief fake 通配桩：恒返回成功。 */`
- [ ] `test_auth_key.c` L50 `/** @brief 组装 auth_key 测试用 pg_ops 虚表。 */`
- [ ] `test_circuit_breaker.c` L14 `/** @brief fake 时间源：返回可推进的虚拟时间。 */`
- [ ] `test_circuit_breaker.c` L181 `/** @brief 熔断并发 worker 线程入口。 */`
- [ ] `test_ratelimit.c` L90 `/** @brief 限流并发 worker 线程入口。 */`
- [ ] `test_response_cache.c` L12 `/** @brief 指纹归一化用例：空白/大小写折叠。 */`
- [ ] `test_response_cache.c` L31 `/** @brief 缓存存取与 TTL 过期用例。 */`
- [ ] `test_response_cache.c` L64 `/** @brief 缓存 LRU 淘汰用例。 */`
- [ ] `test_response_cache.c` L97 `/** @brief 缓存 purge 清理用例。 */`
- [ ] `test_response_cache.c` L141 `/** @brief 缓存并发 worker 线程入口。 */`
- [ ] `test_response_cache.c` L160 `/** @brief 缓存并发读写用例。 */`

**Step 4（test_admin_api.c，36 处；fake pg_ops 内存实现，`fake_` 前缀函数均为"内存表实现，语义同 pg_ops 对应接口"）：**

- [ ] L61 `/** @brief 深拷贝 key 记录（含动态数组）。 */`
- [ ] L82 `/** @brief 深拷贝供应商模型清单。 */`
- [ ] L103 `/** @brief fake 按哈希查 key。 */`
- [ ] L125 `/** @brief fake 分页列 key。 */`
- [ ] L146 `/** @brief fake 按 id 查 key。 */`
- [ ] L165 `/** @brief fake 建 key，回写新 id。 */`
- [ ] L200 `/** @brief fake 按掩码更新 key。 */`
- [ ] L255 `/** @brief fake 吊销 key。 */`
- [ ] L269 `/** @brief fake 分页列模型路由。 */`
- [ ] L281 `/** @brief fake 按名查模型路由。 */`
- [ ] L294 `/** @brief fake 建模型路由。 */`
- [ ] L314 `/** @brief fake 按掩码更新模型路由。 */`
- [ ] L358 `/** @brief fake 按名删模型路由。 */`
- [ ] L371 `/** @brief fake 用量行落库（吞掉）。 */`
- [ ] L381 `/** @brief fake 按 key/模型/时间范围查用量。 */`
- [ ] L410 `/** @brief fake 请求明细落库（吞掉）。 */`
- [ ] L420 `/** @brief fake 查请求明细。 */`
- [ ] L438 `/** @brief fake 分页列供应商。 */`
- [ ] L459 `/** @brief fake 按 id 查供应商。 */`
- [ ] L478 `/** @brief fake 建供应商，回写新 id。 */`
- [ ] L501 `/** @brief fake 按掩码更新供应商。 */`
- [ ] L537 `/** @brief fake 按 id 删供应商。 */`
- [ ] L555 `/** @brief fake 建分组，回写新 id。 */`
- [ ] L579 `/** @brief fake 分页列分组。 */`
- [ ] L600 `/** @brief fake 改组名。 */`
- [ ] L623 `/** @brief fake 删分组。 */`
- [ ] L641 `/** @brief fake 统计组内 key 数。 */`
- [ ] L654 `/** @brief fake 按时间范围查费用行。 */`
- [ ] L668 `/** @brief fake 改分组预算。 */`
- [ ] L681 `/** @brief fake 分页列护栏规则。 */`
- [ ] L694 `/** @brief fake 建护栏规则，回写新 id。 */`
- [ ] L712 `/** @brief fake 更新护栏规则。 */`
- [ ] L726 `/** @brief fake 删护栏规则。 */`
- [ ] L740 `/** @brief 组装 admin_api 测试用 pg_ops 虚表。 */`
- [ ] L778 `/** @brief 搭建 admin 测试夹具：内存库＋core＋admin 上下文。 */`
- [ ] L805 `/** @brief 释放 admin 测试夹具。 */`

**Step 5（test_pg_store.c，35 处；与 Step 4 同系 fake，仅列差异项，其余同文）：**

- [ ] L55、L76 同 Step 4 L61、L82 文案。
- [ ] L97 `/** @brief fake 按哈希查 key。 */`
- [ ] L119 `/** @brief 归一化模型记录的测试替身。 */`
- [ ] L141 `/** @brief fake 分页列模型路由。 */`
- [ ] L153 `/** @brief fake 按名查模型路由。 */`
- [ ] L167 `/** @brief fake 分页列 key。 */`
- [ ] L189 `/** @brief fake 按 id 查 key。 */`
- [ ] L208 `/** @brief fake 建 key，回写新 id。 */`
- [ ] L228 `/** @brief fake 按掩码更新 key。 */`
- [ ] L273 `/** @brief fake 吊销 key。 */`
- [ ] L287 `/** @brief fake 建模型路由。 */`
- [ ] L300 `/** @brief fake 按掩码更新模型路由。 */`
- [ ] L338 `/** @brief fake 按名删模型路由。 */`
- [ ] L354 `/** @brief fake 用量行落库（吞掉）。 */`
- [ ] L379 `/** @brief fake 按 key/模型/时间范围查用量。 */`
- [ ] L408 `/** @brief fake 请求明细落库（吞掉）。 */`
- [ ] L418 `/** @brief fake 查请求明细。 */`
- [ ] L436 `/** @brief fake 分页列供应商。 */`
- [ ] L457 `/** @brief fake 按 id 查供应商。 */`
- [ ] L476 `/** @brief fake 建供应商，回写新 id。 */`
- [ ] L499 `/** @brief fake 按掩码更新供应商。 */`
- [ ] L535 `/** @brief fake 按 id 删供应商。 */`
- [ ] L555 `/** @brief fake 建分组，回写新 id。 */`
- [ ] L579 `/** @brief fake 分页列分组。 */`
- [ ] L600 `/** @brief fake 改组名。 */`
- [ ] L613 `/** @brief fake 改分组预算。 */`
- [ ] L626 `/** @brief fake 删分组。 */`
- [ ] L639 `/** @brief fake 统计组内 key 数。 */`
- [ ] L652 `/** @brief fake 按时间范围查费用行。 */`
- [ ] L660 `/** @brief fake 分页列护栏规则。 */`
- [ ] L674 `/** @brief fake 建护栏规则，回写新 id。 */`
- [ ] L691 `/** @brief fake 更新护栏规则。 */`
- [ ] L704 `/** @brief fake 删护栏规则。 */`
- [ ] L717 `/** @brief 组装 pg_store 测试用 pg_ops 虚表。 */`

**Step 6（store＋upstream 小文件，22 处）：**

- [ ] `test_usage_meter.c` L21 `/** @brief fake 用量落库（吞掉）。 */`
- [ ] `test_usage_meter.c` L35 `/** @brief fake 请求明细落库（吞掉）。 */`
- [ ] `test_usage_meter.c` L49 `/** @brief fake 失败桩：恒返回失败。 */`
- [ ] `test_usage_meter.c` L56 `/** @brief 打开 usage_meter 测试用内存库。 */`
- [ ] `test_embeddings.c` L287 `/** @brief fake 查 key 回调。 */`
- [ ] `test_embeddings.c` L302 `/** @brief fake 查模型回调。 */`
- [ ] `test_embeddings.c` L315 `/** @brief fake 用量落库回调。 */`
- [ ] `test_embeddings.c` L326 `/** @brief fake 上游请求桩。 */`
- [ ] `test_embeddings.c` L340 `/** @brief 捕获响应头回调。 */`
- [ ] `test_embeddings.c` L348 `/** @brief 捕获响应体回调。 */`
- [ ] `test_failover.c` L24 `/** @brief fake 响应头写入。 */`
- [ ] `test_failover.c` L34 `/** @brief fake 响应体写入。 */`
- [ ] `test_failover.c` L52 `/** @brief fake 查 key。 */`
- [ ] `test_failover.c` L65 `/** @brief fake 查模型。 */`
- [ ] `test_failover.c` L76 `/** @brief fake 空操作桩。 */`
- [ ] `test_failover.c` L83 `/** @brief 组装 failover 测试用 pg_ops 虚表。 */`
- [ ] `test_failover.c` L100 `/** @brief 搭建双上游 failover 测试环境。 */`
- [ ] `test_gemini_stream.c` L19 `/** @brief 流式响应头捕获。 */`
- [ ] `test_gemini_stream.c` L28 `/** @brief 流式响应体捕获。 */`
- [ ] `test_gemini_stream.c` L136 `/** @brief 恒失败的流式写入桩。 */`
- [ ] `test_upstream_streaming.c` L18 `/** @brief 流式分片捕获回调。 */`

**Step 7（model_router＋anthropic＋deepseek＋stream_pipeline，27 处）：**

- [ ] `test_model_router.c` L12 `/** @brief fake 时间源。 */`
- [ ] `test_model_router.c` L24 `/** @brief fake 查模型。 */`
- [ ] `test_model_router.c` L38 `/** @brief fake 失败桩。 */`
- [ ] `test_model_router.c` L45 `/** @brief 组装路由测试用 pg_ops 虚表。 */`
- [ ] `test_model_router.c` L69 `/** @brief 打开路由测试用内存库。 */`
- [ ] `test_provider_anthropic.c` L184 `/** @brief 捕获响应头。 */`
- [ ] `test_provider_anthropic.c` L192 `/** @brief 捕获响应体。 */`
- [ ] `test_provider_anthropic.c` L257 `/** @brief 恒失败的响应写入桩。 */`
- [ ] `test_provider_anthropic.c` L305 `/** @brief fake 查 key。 */`
- [ ] `test_provider_anthropic.c` L320 `/** @brief fake 查模型。 */`
- [ ] `test_provider_anthropic.c` L333 `/** @brief fake 用量落库。 */`
- [ ] `test_provider_anthropic.c` L344 `/** @brief fake 上游请求桩。 */`
- [ ] `test_provider_deepseek.c` L119 `/** @brief 占位响应头写入。 */`
- [ ] `test_provider_deepseek.c` L128 `/** @brief 占位响应体写入。 */`
- [ ] `test_stream_pipeline.c` L34 `/** @brief fake 查 key。 */`
- [ ] `test_stream_pipeline.c` L53 `/** @brief fake 查模型。 */`
- [ ] `test_stream_pipeline.c` L66 `/** @brief fake 用量落库。 */`
- [ ] `test_stream_pipeline.c` L77 `/** @brief fake 上游请求桩。 */`
- [ ] `test_stream_pipeline.c` L84 `/** @brief 组装管线测试用 pg_ops 虚表。 */`
- [ ] `test_stream_pipeline.c` L97 `/** @brief 向内存库追加测试 key。 */`
- [ ] `test_stream_pipeline.c` L120 `/** @brief 捕获响应头。 */`
- [ ] `test_stream_pipeline.c` L128 `/** @brief 捕获响应体。 */`
- [ ] `test_stream_pipeline.c` L141 `/** @brief 由 cap 缓冲构造响应上下文。 */`
- [ ] `test_stream_pipeline.c` L152 `/** @brief 检查捕获头是否含某子串。 */`
- [ ] `test_stream_pipeline.c` L158 `/** @brief 在用量行中按 key/模型找行。 */`

**Step 8（Task1 四验证＋提交）：**

- [ ] 构建零警告：`cmake --build build -j 2>&1 | grep -iE "warning|error"` 无输出。
- [ ] `ctest --test-dir build` 6/6 全绿。
- [ ] 纯注释机检：`git diff -U0 -- 'tests/unit/*.c' 'tests/unit/*/*.c' | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/|//'` 无输出（单行 `/** */` 天然通过）。
- [ ] `doxygen Doxyfile 2>&1 | grep -c warning` 为 0（tests 不在 INPUT，预期恒 0）。
- [ ] 提交：`git add tests/unit && git commit -m "docs: annotate test helpers"`。

---

### Task 2: admin.html 107 个 JS 函数注释

**Files:** 只改 `web/admin.html`（每函数上一行加一句中文 `//`，从文件尾部倒序加）。

**Step 1（分页＋令牌＋总览，14 处）：**

- [ ] L2093 `// 取某表的当前页切片。`、L2105 `// 刷新分页条显示。`、L2123 `// 按当前页重渲染表格。`、L2144 `// 切换每页条数。`、L2151 `// 上一页。`、L2159 `// 下一页。`、L2202 `// 刷新令牌状态显示。`、L2218 `// 设置连接状态徽标。`、L2231 `// 打开令牌弹窗。`、L2236 `// 关闭令牌弹窗。`、L2240 `// 保存管理令牌并重连。`、L2361 `// 刷新当前页签数据。`、L2395 `// 渲染总览统计卡。`、L2420 `// 渲染总览图表。`

**Step 2（供应商＋模型，25 处）：**

- [ ] L2512 `// 应用供应商预设模板。`、L2522 `// 切换密码框可见性。`、L2534 `// 拉取供应商列表。`、L2547 `// 渲染供应商表格。`、L2623 `// 过滤供应商表格。`、L2636 `// 打开供应商弹窗（空则新建）。`、L2651 `// 关闭供应商弹窗。`、L2656 `// 按 id 编辑供应商。`、L2663 `// 提交供应商表单。`、L2709 `// 启停供应商。`、L2721 `// 测试供应商连通性。`、L2743 `// 删除供应商（二次确认）。`、L2767 `// 渲染模型路由表格。`、L2861 `// 过滤模型表格。`、L2876 `// 创建上游目标行元素。`、L2925 `// 新增上游目标行。`、L2937 `// 删除上游目标行。`、L2951 `// 更新目标行表头序号。`、L2964 `// 一键填充行预设。`、L2972 `// 打开模型弹窗（空则新建）。`、L3048 `// 关闭模型弹窗。`、L3053 `// 提交模型表单。`、L3139 `// 启停模型。`、L3150 `// 按名编辑模型。`、L3154 `// 删除模型（二次确认）。`

**Step 3（密钥＋护栏，22 处）：**

- [ ] L3178 `// 渲染密钥表格。`、L3242 `// 过滤密钥表格。`、L3252 `// 打开新建密钥弹窗。`、L3288 `// 关闭密钥弹窗。`、L3292 `// 提交新建密钥。`、L3339 `// 展示新建密钥明文（仅显示一次）。`、L3346 `// 关闭明文弹窗。`、L3350 `// 复制明文密钥。`、L3359 `// 吊销密钥（二次确认）。`、L3379 `// 拉取护栏规则。`、L3393 `// 更新护栏统计卡。`、L3415 `// 渲染护栏规则表格。`、L3467 `// 过滤护栏表格。`、L3483 `// 打开护栏弹窗（空则新建）。`、L3498 `// 关闭护栏弹窗。`、L3503 `// 护栏类型切换联动表单。`、L3529 `// 按 id 编辑护栏规则。`、L3536 `// 提交护栏表单。`、L3574 `// 启停护栏规则。`、L3585 `// 删除护栏规则。`、L3598 `// 热重载护栏引擎。`、L3606 `// 植入默认 PII 规则。`、L3633 `// 护栏沙盒试算。`

**Step 4（分组＋费用＋用量，13 处）：**

- [ ] L3777 `// 渲染分组表格。`、L3814 `// 打开分组弹窗（空则新建）。`、L3823 `// 关闭分组弹窗。`、L3827 `// 提交分组表单。`、L3863 `// 删除分组（二次确认）。`、L3883 `// 初始化费用日期范围。`、L3894 `// 拉取费用归因数据。`、L3925 `// 渲染费用表格。`、L4022 `// 防抖拉取用量数据。`、L4027 `// 填充模型筛选下拉。`、L4038 `// 拉取用量数据。`、L4102 `// 渲染用量表格。`、L4133 `// 过滤请求明细表。`、L4154 `// 渲染请求明细表。`

**Step 5（调试台＋实时看板＋确认框＋缓存，33 处）：**

- [ ] L4223 `// 按模型打开调试台。`、L4230 `// 自动填充调试用 key。`、L4239 `// 发起调试请求（含流式）。`、L4382 `// 停止调试流。`、L4388 `// 清空调试响应。`、L4395 `// 开关请求检查器。`、L4424 `// 过滤指标列表。`、L4459 `// 初始化实时看板。`、L4469 `// 连接 SSE 实时流。`、L4550 `// 开关实时流。`、L4565 `// 清空实时瀑布。`、L4571 `// 过滤实时瀑布。`、L4576 `// 处理实时请求事件。`、L4628 `// 渲染实时瀑布。`、L4687 `// 打开请求详情弹窗。`、L4698 `// 关闭请求详情弹窗。`、L4703 `// 处理熔断器事件。`、L4716 `// 处理探针事件。`、L4737 `// 处理预算告警事件。`、L4748 `// 渲染实时告警。`、L4796 `// 拉取供应商健康快照。`、L4813 `// 渲染供应商健康列表。`、L4882 `// 触发全量探针。`、L4924 `// 探针单个供应商。`、L4948 `// 打开通用确认框。`、L4954 `// 确认框返回值。`、L4958 `// 关闭确认框。`、L5006 `// 拉取缓存统计。`、L5070 `// 渲染缓存分片表。`、L5109 `// 按模型清理缓存。`、L5132 `// 按输入框内容清理缓存。`

**Step 6（Task2 四验证＋提交）：**

- [ ] 机检：`git diff -U0 -- web/admin.html | grep '^+' | grep -v '^+++' | grep -vE '^\+\s*//'` 无输出。
- [ ] 构建零警告＋ctest 6/6（html 不进编译，预期恒过，照跑）。
- [ ] doxygen 0 警告（html 不在 INPUT，预期恒 0）。
- [ ] 提交：`git add web/admin.html && git commit -m "docs: annotate admin UI scripts"`。

---

### Task 3: README 设为 doxygen 首页

**Files:** 只改 `Doxyfile` 一行。

- [ ] **Step 1：** Doxyfile 末尾加一行 `USE_MDFILE_AS_MAINPAGE = README.md`（根 README.md 已存在并经实测确认）。
- [ ] **Step 2：** `doxygen Doxyfile 2>&1 | grep -c warning` 为 0；确认 `build/docs/html/index.html` 首页内容来自 README。
- [ ] **Step 3：** 构建零警告＋ctest 6/6＋机检（加行含 `=`，用 `git diff --stat` 确认仅 Doxyfile 一行）。
- [ ] **Step 4：** 提交：`git add Doxyfile && git commit -m "docs: set README as doxygen mainpage"`。

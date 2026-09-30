# aigate_core Pipeline Decoupling and Architecture Refactoring Design

## §0 概述

当前 `src/core/aigate_core.c` 规模已膨胀至 3674 行，单文件混杂了：
1. 核心控制器与生命周期（`init`, `shutdown`, `reload_guardrails`）
2. 公共前置门禁与用量审计（`gate_request`, `record_usage_and_event`, `calc_req_cost`）
3. 流式缓存收集引擎（`stream_cache_acc_t`, `accumulate_sse_line`, `cache_stream_replay`）
4. OpenAI Chat Completions 管线（`/v1/chat/completions`, `/v1/models`）
5. OpenAI Responses API 管线（`/v1/responses`）
6. Anthropic 原生端点管线（`/v1/messages`）
7. Gemini 原生端点管线（`/v1beta/models/*`）
8. Embeddings 向量嵌入管线（`/v1/embeddings`）

本设计将 `aigate_core.c` 按业务管线进行彻底的垂直解耦拆分，将其瘦身至约 350 行，拆出 4 个内聚专注的子管线模块，建立清晰的内部接口契约，对外 API 和已有测试行为 100% 保持不变。

---

## §1 架构与模块划分

```
src/core/
 ├── aigate_core.h               (公开公共 API，零修改)
 ├── aigate_core_internal.h      (★ 新增：模块间共享结构体与辅助函数原型)
 ├── aigate_core.c               (★ 瘦身至 ~350 行：生命周期 + 公共门禁 + 顶级分发)
 ├── pipeline_chat.c             (★ 新增 ~650 行：/v1/chat/completions + /v1/models)
 ├── pipeline_responses.c        (★ 新增 ~670 行：/v1/responses)
 ├── pipeline_native.c           (★ 新增 ~600 行：Anthropic messages + Gemini generate)
 └── pipeline_embeddings.c       (★ 新增 ~200 行：/v1/embeddings)
```

### 构建支持
`CMakeLists.txt` 中已有：
```cmake
file(GLOB AIGATE_SRC CONFIGURE_DEPENDS src/*.c src/*/*.c)
```
新增的 `pipeline_*.c` 均位于 `src/core/`，将被 CMake 自动发现并编入 `libaigate`，无需修改任何构建脚本。

---

## §2 内部共享契约 (`src/core/aigate_core_internal.h`)

仅对 `src/core/` 目录内部的源文件开放，不暴露给外部调用方：

### 2.1 状态码与常量
```c
enum {
    PIPE_OK          = 0,
    PIPE_AUTH        = 401,
    PIPE_FORBIDDEN   = 403,
    PIPE_RATE        = 429,
    PIPE_MODEL       = 404,
    PIPE_UPSTREAM    = 502,
    PIPE_UNSUPPORTED = 501,
};
```

### 2.2 共享数据结构
```c
/** @brief Streaming cache accumulator for real-time SSE chunk passthrough and full content aggregation. */
typedef struct stream_cache_acc {
    aigate_response_ctx* orig_rc;
    char*                accum_content;
    size_t               accum_len;
    size_t               accum_cap;
    char*                line_buf;
    size_t               line_cap;
    size_t               line_len;
    char                 id[64];
    long                 created;
    bool                 overflow;
} stream_cache_acc_t;

/** @brief Request processing context containing route, credentials, and payload state. */
typedef struct {
    aigate_core*         ac;
    aigate_request_ctx*  rq;
    aigate_response_ctx* rc;
    key_rec_t            krec;
    json_t*              jbody;
    const char*          model;
    model_rec_t          route;
    upstream_target_t    candidates[MAX_TARGETS_PER_MODEL];
    int                  n_candidates;
    char*                sanitized_body;
    size_t               sanitized_len;
    const void*          eff_body;
    size_t               eff_len;
    char                 guardrail_act[16];
    char                 cache_key[65];
    bool                 bypass_cache;
    bool                 no_store;
} chat_req_t;
```

### 2.3 共享辅助函数
```c
uint64_t mono_ns(void);
double   calc_req_cost(const model_rec_t* route, long prompt, long completion, long cached);
void     record_usage_and_event(aigate_core* ac,
                                int64_t      key_id,
                                const char*  model,
                                int          status,
                                long         prompt_tokens,
                                long         completion_tokens,
                                long         cached_tokens,
                                long         reasoning_tokens,
                                uint64_t     lat_ns,
                                const char*  provider,
                                const char*  guardrail_action,
                                double       cost_usd);

void fill_cur_route(const model_rec_t* route, const upstream_target_t* t, model_rec_t* out);
void settle_success(chat_req_t* q, const upstream_target_t* target, int status,
                    long ptok, long ctok, long cached_tok, long reasoning_tok,
                    uint64_t lat, double req_cost);
void failover_warn(const char* label, const char* model, const char* from_prov, const char* to_prov);

int  gate_request(chat_req_t* q);
int  resolve_chat_target(chat_req_t* q);
void chat_req_cleanup(chat_req_t* q);

/* Stream cache engine operations */
int  stream_cache_acc_set_header(void* impl, const char* name, const char* value);
int  stream_cache_acc_write(void* impl, const void* buf, size_t len, bool fin);
int  cache_stream_replay(aigate_core* ac, aigate_response_ctx* rc, cache_entry_t* ce,
                         const char* model, const key_rec_t* krec, chat_req_t* q);
void cache_store_stream(chat_req_t* q, stream_cache_acc_t* acc, int status,
                        long ptok, long ctok, double req_cost);
```

### 2.4 各管线入口声明
```c
/* pipeline_chat.c */
int  prepare_chat_cache(chat_req_t* q, bool* is_streaming);
int  handle_chat_sync(chat_req_t* q);
int  handle_chat_stream(chat_req_t* q);
int  handle_models_list(chat_req_t* q);

/* pipeline_responses.c */
int  handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/* pipeline_native.c */
int  handle_anthropic_messages(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int  handle_gemini_generate(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/* pipeline_embeddings.c */
int  handle_embeddings(chat_req_t* q);
```

---

## §3 各管线模块详细设计

### 3.1 `pipeline_chat.c`
- **代码行数**：约 650 行
- **内容迁移**：
  - `handle_models_list`
  - `prepare_chat_cache`
  - `handle_stream_preheaders`
  - `handle_chat_sync`
  - `handle_chat_stream`
- **依赖**：`aigate_core_internal.h`, `response_cache.h`, `provider_adapter.h`, `metrics.h`

### 3.2 `pipeline_responses.c`
- **代码行数**：约 670 行
- **内容迁移**：
  - `handle_responses` 及其流式与非流式分支
- **依赖**：`aigate_core_internal.h`, `response_cache.h`, `provider_adapter.h`, `metrics.h`

### 3.3 `pipeline_native.c`
- **代码行数**：约 600 行
- **内容迁移**：
  - Anthropic 原生端点：`build_anthropic_url`, `anthropic_stream_chunk_cb`, `handle_anthropic_messages`
  - Gemini 原生端点：`extract_gemini_model`, `build_gemini_url`, `gemini_stream_chunk_cb`, `handle_gemini_generate`
- **依赖**：`aigate_core_internal.h`, `provider_anthropic.h`, `provider_gemini.h`, `metrics.h`

### 3.4 `pipeline_embeddings.c`
- **代码行数**：约 200 行
- **内容迁移**：
  - `handle_embeddings`
- **依赖**：`aigate_core_internal.h`, `provider_adapter.h`, `metrics.h`

### 3.5 瘦身后的 `aigate_core.c`
- **代码行数**：约 350 行
- **保留内容**：
  - 生命周期：`aigate_core_init`, `aigate_core_shutdown`, `aigate_core_reload_guardrails`
  - 响应写入：`aigate_write_json`, `aigate_write_error`, `aigate_write_anthropic_error`, `aigate_write_gemini_error`
  - 公共门禁：`gate_request`, `resolve_chat_target`, `chat_req_cleanup`
  - 计费与工具：`mono_ns`, `calc_req_cost`, `record_usage_and_event`, `fill_cur_route`, `settle_success`, `failover_warn`
  - 流式缓存：`stream_cache_acc_*`, `cache_stream_replay`, `cache_store_stream`
  - 顶级路由分发器：`aigate_handle_request`

---

## §4 错误处理与 API 契约保持

本次重构为纯代码结构重构（Pure Refactoring），保证：
1. **外部 HTTP 状态码 100% 一致**：400, 401, 403, 404, 429, 501, 502, 503 等全部保持现有逻辑。
2. **响应 Body 格式 100% 一致**：OpenAI 形状、Anthropic 形状、Gemini 形状错误保持完全相同。
3. **缓存键计算与落盘策略 100% 一致**。
4. **熔断与级联重试策略 100% 一致**。

---

## §5 验收标准与测试策略

- [ ] 新建 `aigate_core_internal.h`，定义内部共享类型与函数原型，编译通过。
- [ ] 抽离 `pipeline_embeddings.c`，单元测试 197/197 100% 通过。
- [ ] 抽离 `pipeline_native.c`，单元测试 197/197 100% 通过。
- [ ] 抽离 `pipeline_responses.c`，单元测试 197/197 100% 通过。
- [ ] 抽离 `pipeline_chat.c`，单元测试 197/197 100% 通过。
- [ ] `aigate_core.c` 缩减至 ~350 行，代码整洁无死代码。
- [ ] 全量编译零 Warning（`-Wall -Wextra -Werror`）。
- [ ] 全量回归测试通过：`ctest --output-on-failure` 100% PASS。

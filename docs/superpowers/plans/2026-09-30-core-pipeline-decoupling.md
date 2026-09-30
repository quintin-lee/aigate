# aigate_core Pipeline Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple `src/core/aigate_core.c` (3674 lines) into distinct pipeline modules (`pipeline_chat.c`, `pipeline_responses.c`, `pipeline_native.c`, `pipeline_embeddings.c`) and an internal header `aigate_core_internal.h`, slimming `aigate_core.c` down to ~350 lines while preserving 100% test compatibility.

**Architecture:** Introduce `src/core/aigate_core_internal.h` as an internal contract defining shared contexts (`chat_req_t`, `stream_cache_acc_t`), shared pipeline helpers (`gate_request`, `record_usage_and_event`, `calc_req_cost`, `cache_stream_replay`, `cache_store_stream`), and pipeline entrypoint declarations. Each pipeline is extracted cleanly into its own translation unit without circular dependencies.

**Tech Stack:** Pure C17, libjansson, ctest.

---

### File Structure Map

- **Create:** `src/core/aigate_core_internal.h`
  - Defines `PIPE_*` enums, `stream_cache_acc_t`, `chat_req_t`, common helper prototypes, and pipeline prototypes.
- **Create:** `src/core/pipeline_embeddings.c` (~200 lines)
  - Implements `handle_embeddings`.
- **Create:** `src/core/pipeline_native.c` (~600 lines)
  - Implements Anthropic `/v1/messages` (`build_anthropic_url`, `anthropic_stream_chunk_cb`, `handle_anthropic_messages`) and Gemini `/v1beta/models/*` (`extract_gemini_model`, `build_gemini_url`, `gemini_stream_chunk_cb`, `handle_gemini_generate`).
- **Create:** `src/core/pipeline_responses.c` (~670 lines)
  - Implements `handle_responses` (OpenAI Responses API).
- **Create:** `src/core/pipeline_chat.c` (~650 lines)
  - Implements `handle_models_list`, `prepare_chat_cache`, `handle_stream_preheaders`, `handle_chat_sync`, and `handle_chat_stream`.
- **Modify:** `src/core/aigate_core.c` (~350 lines)
  - Includes `aigate_core_internal.h`, exports shared helpers, removes extracted pipeline functions, and implements the clean `aigate_handle_request` dispatcher.

---

### Task 1: Create `src/core/aigate_core_internal.h` and Export Shared Helpers

**Files:**
- Create: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.c:20-80, 270-380, 600-800, 2770-2840, 3360-3420`

- [x] **Step 1: Create `src/core/aigate_core_internal.h`**

Create `src/core/aigate_core_internal.h`:

```c
/** @file aigate_core_internal.h
 *  @brief Internal types and shared helpers across pipeline modules in src/core/.
 *  This header is internal to src/core/ and must not be included by external consumers.
 */
#ifndef AIGATE_CORE_INTERNAL_H
#define AIGATE_CORE_INTERNAL_H

#include "aigate_core.h"
#include "aigate_log.h"
#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pipeline exit and error status codes */
enum {
    PIPE_OK          = 0,
    PIPE_AUTH        = 401,
    PIPE_FORBIDDEN   = 403,
    PIPE_RATE        = 429,
    PIPE_MODEL       = 404,
    PIPE_UPSTREAM    = 502,
    PIPE_UNSUPPORTED = 501,
};

/** @brief Streaming cache accumulator for real-time SSE chunk passthrough and full content aggregation. */
typedef struct stream_cache_acc {
    aigate_response_ctx* orig_rc;       /**< Real response context (borrowed). */
    char*                accum_content; /**< Accumulated full body (max 512KiB). */
    size_t               accum_len;     /**< Accumulated byte count. */
    size_t               accum_cap;     /**< accum_content capacity. */
    char*                line_buf;      /**< Growable SSE line buffer. */
    size_t               line_cap;      /**< line_buf capacity. */
    size_t               line_len;      /**< Valid line buffer length. */
    char                 id[64];        /**< Response id (from first data line). */
    long                 created;       /**< Response created timestamp. */
    bool                 overflow;      /**< Over-limit/alloc failure: passthrough only. */
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

/* Monotonic time and usage utilities */
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

/* Route & failover settlement helpers */
void fill_cur_route(const model_rec_t* route, const upstream_target_t* t, model_rec_t* out);
void settle_success(chat_req_t* q, const upstream_target_t* target, int status,
                    long ptok, long ctok, long cached_tok, long reasoning_tok,
                    uint64_t lat, double req_cost);
void failover_warn(const char* label, const char* model, const char* from_prov, const char* to_prov);

/* Gateway admission & target resolution */
int  gate_request(chat_req_t* q);
int  resolve_chat_target(chat_req_t* q);
void chat_req_cleanup(chat_req_t* q);

/* Stream cache engine operations */
int  stream_cache_acc_set_header(void* impl, const char* name, const char* value);
int  stream_cache_acc_write(void* impl, const void* buf, size_t len, bool fin);
int  cache_stream_replay(aigate_core*         ac,
                         aigate_response_ctx* rc,
                         struct cache_entry*  ce,
                         const char*          model,
                         const key_rec_t*     krec,
                         chat_req_t*          q);
void cache_store_stream(chat_req_t*         q,
                        stream_cache_acc_t* acc,
                        int                 status,
                        long                ptok,
                        long                ctok,
                        double              req_cost);

/* Pipeline prototypes */
int handle_embeddings(chat_req_t* q);
int handle_anthropic_messages(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int handle_gemini_generate(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int handle_models_list(chat_req_t* q);
int prepare_chat_cache(chat_req_t* q, bool* is_streaming);
int handle_chat_sync(chat_req_t* q);
int handle_chat_stream(chat_req_t* q);

#endif /* AIGATE_CORE_INTERNAL_H */
```

- [x] **Step 2: Include `aigate_core_internal.h` in `src/core/aigate_core.c` and make shared helpers non-static**

In `src/core/aigate_core.c`:
1. Add `#include "aigate_core_internal.h"` at top.
2. Remove local definitions of `enum { PIPE_OK = ... }`, `typedef struct stream_cache_acc`, and `typedef struct chat_req_t` from `aigate_core.c` (since they are now in `aigate_core_internal.h`).
3. Remove `static` from:
   - `mono_ns`
   - `calc_req_cost`
   - `record_usage_and_event`
   - `fill_cur_route`
   - `settle_success`
   - `failover_warn`
   - `gate_request`
   - `resolve_chat_target`
   - `chat_req_cleanup`
   - `stream_cache_acc_set_header`
   - `stream_cache_acc_write`
   - `cache_stream_replay`
   - `cache_store_stream`

- [x] **Step 3: Compile and run test suite to verify no breakage**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed (197/197 passed).

- [x] **Step 4: Commit Task 1**

```bash
git add src/core/aigate_core_internal.h src/core/aigate_core.c
git commit -m "refactor(core): ♻️ introduce aigate_core_internal.h shared header"
```

---

### Task 2: Extract `pipeline_embeddings.c`

**Files:**
- Create: `src/core/pipeline_embeddings.c`
- Modify: `src/core/aigate_core.c`

- [x] **Step 1: Create `src/core/pipeline_embeddings.c`**

Create `src/core/pipeline_embeddings.c` containing the embeddings pipeline logic extracted from `aigate_core.c`:

```c
/** @file pipeline_embeddings.c
 *  @brief /v1/embeddings pipeline handling (OpenAI & Gemini embeddings).
 */
#include "aigate_core_internal.h"
#include "metrics.h"
#include "provider_adapter.h"
#include "upstream_client.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
handle_embeddings(chat_req_t* q)
{
    /* --- check provider adapter supports embeddings --- */
    const provider_adapter_t* adapter = provider_find(q->route.provider);
    if (adapter == NULL || adapter->build_embeddings == NULL ||
        adapter->parse_embeddings_response == NULL) {
        aigate_write_error(q->rc,
                           PIPE_UNSUPPORTED,
                           "unsupported_endpoint",
                           "model provider does not support embeddings");
        chat_req_cleanup(q);
        return 0;
    }

    char        url[1024];
    char*       out_body = NULL;
    size_t      out_body_len = 0;
    const char* extra_hdrs[4][2] = {{0}};
    int         n_extra_hdrs = 0;

    int brc = adapter->build_embeddings(&q->route,
                                        q->eff_body != NULL ? (const char*)q->eff_body : "",
                                        url,
                                        sizeof url,
                                        extra_hdrs,
                                        &n_extra_hdrs,
                                        &out_body,
                                        &out_body_len);
    if (brc != 0) {
        aigate_write_error(
            q->rc, PIPE_MODEL, "bad_request", "failed to build upstream embeddings request");
        chat_req_cleanup(q);
        return 0;
    }

    int      status = 0;
    char*    ubody = NULL;
    size_t   ulen = 0;
    uint64_t t0 = mono_ns();
    int      urc = upstream_call_ext(url,
                                q->route.upstream_key,
                                extra_hdrs,
                                n_extra_hdrs,
                                out_body,
                                out_body_len,
                                q->ac->default_timeout_ms,
                                &status,
                                &ubody,
                                &ulen);
    if (urc == 0 && status >= 500) {
        /* Single retry on 5xx */
        free(ubody);
        ubody = NULL;
        struct timespec sl = {0, 200 * 1000000}; /* 200ms */
        nanosleep(&sl, NULL);
        urc = upstream_call_ext(url,
                                q->route.upstream_key,
                                extra_hdrs,
                                n_extra_hdrs,
                                out_body,
                                out_body_len,
                                q->ac->default_timeout_ms,
                                &status,
                                &ubody,
                                &ulen);
    }
    uint64_t lat = mono_ns() - t0;
    free(out_body);

    if (urc != 0 || status >= 400) {
        cb_record_failure(q->ac->cb, q->model, q->route.endpoint, status);
        if (ubody != NULL && urc == 0) {
            record_usage_and_event(q->ac,
                                   q->krec.key_id,
                                   q->model,
                                   status,
                                   0,
                                   0,
                                   0,
                                   0,
                                   lat,
                                   q->route.provider,
                                   q->guardrail_act,
                                   0.0);
            int rv = aigate_write_json(q->rc, status, ubody, ulen);
            free(ubody);
            chat_req_cleanup(q);
            return rv;
        }
        free(ubody);
        aigate_write_error(q->rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
        record_usage_and_event(q->ac,
                               q->krec.key_id,
                               q->model,
                               PIPE_UPSTREAM,
                               0,
                               0,
                               0,
                               0,
                               lat,
                               q->route.provider,
                               q->guardrail_act,
                               0.0);
        chat_req_cleanup(q);
        return 0;
    }

    cb_record_success(q->ac->cb, q->model, q->route.endpoint);

    /* Parse embeddings usage and translate to standard OpenAI response */
    long   prompt_tok = 0;
    char*  std_resp = NULL;
    size_t std_resp_len = 0;
    int    prc = adapter->parse_embeddings_response(
        ubody ? ubody : "", ulen, q->model, status, &std_resp, &std_resp_len, &prompt_tok);
    free(ubody);

    if (prc != 0 || std_resp == NULL) {
        aigate_write_error(
            q->rc, PIPE_UPSTREAM, "upstream_error", "failed to parse upstream embeddings response");
        record_usage_and_event(q->ac,
                               q->krec.key_id,
                               q->model,
                               PIPE_UPSTREAM,
                               0,
                               0,
                               0,
                               0,
                               lat,
                               q->route.provider,
                               q->guardrail_act,
                               0.0);
        chat_req_cleanup(q);
        return 0;
    }

    double req_cost = calc_req_cost(&q->route, prompt_tok, 0, 0);
    record_usage_and_event(q->ac,
                           q->krec.key_id,
                           q->model,
                           status,
                           prompt_tok,
                           0,
                           0,
                           0,
                           lat,
                           q->route.provider,
                           q->guardrail_act,
                           req_cost);
    if (q->ac->be != NULL) {
        budget_enforce_record(
            q->ac->be, q->krec.key_id, q->krec.group_id, req_cost, prompt_tok);
    }
    if (prompt_tok > 0) {
        rl_reserve_tokens(q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, prompt_tok);
    }

    int rv = aigate_write_json(q->rc, 200, std_resp, std_resp_len);
    free(std_resp);
    chat_req_cleanup(q);
    return rv;
}
```

- [x] **Step 2: Remove `handle_embeddings` from `src/core/aigate_core.c`**

Delete the `handle_embeddings` definition (lines ~2841-3018) from `src/core/aigate_core.c`.

- [x] **Step 3: Compile and run test suite**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed.

- [x] **Step 4: Commit Task 2**

```bash
git add src/core/pipeline_embeddings.c src/core/aigate_core.c
git commit -m "refactor(core): ♻️ extract pipeline_embeddings.c"
```

---

### Task 3: Extract `pipeline_native.c` (Anthropic + Gemini Native Endpoints)

**Files:**
- Create: `src/core/pipeline_native.c`
- Modify: `src/core/aigate_core.c`

- [x] **Step 1: Create `src/core/pipeline_native.c`**

Create `src/core/pipeline_native.c` containing:
- `build_anthropic_url`, `anthropic_stream_ctx_t`, `anthropic_stream_chunk_cb`, `handle_anthropic_messages`
- `extract_gemini_model`, `build_gemini_url`, `gemini_stream_ctx_t`, `gemini_stream_chunk_cb`, `handle_gemini_generate`

```c
/** @file pipeline_native.c
 *  @brief Native inbound protocol pipelines:
 *  - Anthropic /v1/messages
 *  - Gemini /v1beta/models/*
 */
#include "aigate_core_internal.h"
#include "metrics.h"
#include "provider_anthropic.h"
#include "provider_gemini.h"
#include "upstream_client.h"

#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================= */
/* Native Anthropic Messages Pipeline (POST /v1/messages)                    */
/* ========================================================================= */

static void
build_anthropic_url(const char* endpoint, char* url_out, size_t url_cap)
{
    const char* ep = endpoint;
    if (ep == NULL || ep[0] == '\0' || strcmp(ep, "/") == 0) {
        ep = "https://api.anthropic.com";
    }
    char base_ep[512];
    snprintf(base_ep, sizeof base_ep, "%s", ep);
    size_t elen = strlen(base_ep);
    while (elen > 0 && base_ep[elen - 1] == '/') {
        base_ep[--elen] = '\0';
    }
    if (elen >= 3 && strcmp(base_ep + elen - 3, "/v1") == 0) {
        snprintf(url_out, url_cap, "%s/messages", base_ep);
    } else {
        snprintf(url_out, url_cap, "%s/v1/messages", base_ep);
    }
}

typedef struct {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    anthropic_sniffer_t  sniffer;
} anthropic_stream_ctx_t;

static int
anthropic_stream_chunk_cb(void* user_data, const void* chunk, size_t len)
{
    anthropic_stream_ctx_t* ctx = user_data;
    if (!ctx->headers_sent) {
        ctx->rc->status = 200;
        if (ctx->rc->set_header != NULL) {
            ctx->rc->set_header(ctx->rc->impl, "Content-Type", "text/event-stream; charset=utf-8");
            ctx->rc->set_header(ctx->rc->impl, "Cache-Control", "no-cache");
            ctx->rc->set_header(ctx->rc->impl, "Connection", "keep-alive");
        }
        ctx->headers_sent = true;
        ctx->rc->headers_sent = true;
    }
    if (ctx->rc->write != NULL && len > 0) {
        if (ctx->rc->write(ctx->rc->impl, chunk, len, false) != 0) {
            return -1;
        }
    }
    anthropic_sniff_feed(&ctx->sniffer, chunk, len);
    return 0;
}

int
handle_anthropic_messages(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* Auth */
    key_rec_t krec;
    int arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_anthropic_error(rc, PIPE_AUTH, "authentication_error", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* Rate limit */
    long retry_ms = 0;
    int rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            aigate_write_anthropic_error(rc, 503, "api_error", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) ra_s = 1;
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        if (rc->set_header != NULL) rc->set_header(rc->impl, "Retry-After", ra);
        aigate_write_anthropic_error(rc, PIPE_RATE, "rate_limit_error", "rate limit exceeded");
        key_rec_free(&krec);
        return 0;
    }

    /* Daily quota */
    if (krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
        if (rem == LONG_MIN) {
            aigate_write_anthropic_error(rc, 503, "api_error", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        if (rem <= 0) {
            time_t now = time(NULL);
            time_t next = (time_t)(now - (now % 86400)) + 86400;
            char ra[32];
            snprintf(ra, sizeof ra, "%ld", (long)(next - now));
            if (rc->set_header != NULL) rc->set_header(rc->impl, "Retry-After", ra);
            aigate_write_anthropic_error(rc, PIPE_RATE, "rate_limit_error", "daily token quota exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* Monthly budget */
    if (ac->be != NULL) {
        char b_err[256] = {0};
        if (budget_enforce_check(ac->be, krec.key_id, krec.group_id,
                                 krec.monthly_cost_budget, krec.monthly_token_budget,
                                 0.0, b_err, sizeof b_err) != 0) {
            aigate_write_anthropic_error(rc, PIPE_RATE, "rate_limit_error",
                                         b_err[0] ? b_err : "monthly budget limit exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* Parse model */
    const char* model = "";
    json_t* jbody = NULL;
    if (rq->body != NULL && rq->body_len > 0) {
        jbody = json_loads((const char*)rq->body, 0, NULL);
    }
    if (jbody != NULL) {
        json_t* jm = json_object_get(jbody, "model");
        if (jm != NULL && json_is_string(jm)) {
            model = json_string_value(jm);
        }
    }
    if (model[0] == '\0') {
        aigate_write_anthropic_error(rc, 400, "invalid_request_error", "model field is required");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }
    if (!key_allows_model(&krec, model)) {
        aigate_write_anthropic_error(rc, PIPE_FORBIDDEN, "permission_error", "model not allowed for this key");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_anthropic_error(rc, PIPE_MODEL, "not_found_error", "model not found");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    if (strcmp(route.provider, "anthropic") != 0) {
        aigate_write_anthropic_error(rc, 400, "invalid_request_error",
            "/v1/messages requires an anthropic provider route");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int n_candidates = 0;
    if (model_router_select_candidates(ac->cb, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_anthropic_error(rc, PIPE_MODEL, "api_error", "no upstream targets available for model");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    bool is_streaming = false;
    if (jbody != NULL) {
        json_t* js = json_object_get(jbody, "stream");
        if (js != NULL && json_is_true(js)) {
            is_streaming = true;
        }
    }

    uint64_t total_lat = 0;
    const char* last_provider = route.provider;

    /* Upstream execution loop (candidates) */
    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t* target = &candidates[ci];
        model_rec_t cur_route = route;
        snprintf(cur_route.provider, sizeof cur_route.provider, "%.*s", (int)sizeof cur_route.provider - 1, target->provider);
        snprintf(cur_route.endpoint, sizeof cur_route.endpoint, "%.*s", (int)sizeof cur_route.endpoint - 1, target->endpoint);
        snprintf(cur_route.upstream_key, sizeof cur_route.upstream_key, "%.*s", (int)sizeof cur_route.upstream_key - 1, target->upstream_key);
        last_provider = target->provider;

        char url[1024];
        build_anthropic_url(cur_route.endpoint, url, sizeof url);

        const char* extra_hdrs[4][2] = {
            {"anthropic-version", "2023-06-01"},
            {NULL, NULL}
        };
        int n_extra_hdrs = 1;

        if (is_streaming) {
            anthropic_stream_ctx_t sctx;
            memset(&sctx, 0, sizeof sctx);
            sctx.rc = rc;
            anthropic_sniff_init(&sctx.sniffer);

            int status = 0;
            char* sbody = NULL;
            size_t slen = 0;
            uint64_t t0 = mono_ns();
            long silence_timeout_ms = ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int urc = upstream_stream_call(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                           (const char*)rq->body, rq->body_len, silence_timeout_ms,
                                           anthropic_stream_chunk_cb, &sctx, &status, &sbody, &slen);
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
            if (!sctx.headers_sent) {
                cb_record_failure(ac->cb, model, target->endpoint, status);
                if (!is_failover && status >= 400) {
                    if (sbody != NULL && urc == 0) {
                        record_usage_and_event(ac, krec.key_id, model, status, 0, 0, 0, 0, total_lat, target->provider, NULL, 0.0);
                        int rv = aigate_write_json(rc, status, sbody, slen);
                        free(sbody);
                        json_decref(jbody);
                        key_rec_free(&krec);
                        return rv;
                    }
                    free(sbody);
                    break;
                }
                free(sbody);
                if (ci + 1 < n_candidates) {
                    AIGATE_LOG_WARN("native anthropic streaming failover for model %s to %s", model, candidates[ci + 1].provider);
                    metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                    continue;
                }
                break;
            }

            long ptok = 0, ctok = 0;
            anthropic_sniff_get_tokens(&sctx.sniffer, &ptok, &ctok);
            double req_cost = calc_req_cost(&route, ptok, ctok, 0);

            if (urc != 0) {
                const char* err_sse = "event: error\ndata: {\"type\": \"error\", \"error\": {\"type\": \"api_error\", \"message\": \"stream interrupted\"}}\n\n";
                if (rc->write != NULL) rc->write(rc->impl, err_sse, strlen(err_sse), true);
                record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, ptok, ctok, 0, 0, total_lat, target->provider, NULL, req_cost);
                if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
                if (ptok + ctok > 0) rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
                json_decref(jbody);
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            if (rc->write != NULL) rc->write(rc->impl, "", 0, true);
            record_usage_and_event(ac, krec.key_id, model, status > 0 ? status : 200, ptok, ctok, 0, 0, total_lat, target->provider, NULL, req_cost);
            if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        /* Non-streaming */
        int status = 0;
        char* ubody = NULL;
        size_t ulen = 0;
        uint64_t t0 = mono_ns();
        int urc = upstream_call_ext(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                    (const char*)rq->body, rq->body_len, ac->default_timeout_ms,
                                    &status, &ubody, &ulen);
        if (n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000};
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                    (const char*)rq->body, rq->body_len, ac->default_timeout_ms,
                                    &status, &ubody, &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            long ptok = 0, ctok = 0;
            anthropic_sniff_json_tokens(ubody ? ubody : "", &ptok, &ctok);
            double req_cost = calc_req_cost(&route, ptok, ctok, 0);
            record_usage_and_event(ac, krec.key_id, model, status, ptok, ctok, 0, 0, total_lat, target->provider, NULL, req_cost);
            if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
            int rv = aigate_write_json(rc, status, ubody ? ubody : "", ulen);
            free(ubody);
            json_decref(jbody);
            key_rec_free(&krec);
            return rv;
        }

        cb_record_failure(ac->cb, model, target->endpoint, status);
        if (!is_failover && status >= 400) {
            if (ubody != NULL && urc == 0) {
                record_usage_and_event(ac, krec.key_id, model, status, 0, 0, 0, 0, total_lat, target->provider, NULL, 0.0);
                int rv = aigate_write_json(rc, status, ubody, ulen);
                free(ubody);
                json_decref(jbody);
                key_rec_free(&krec);
                return rv;
            }
            free(ubody);
            break;
        }
        free(ubody);
        if (ci + 1 < n_candidates) {
            AIGATE_LOG_WARN("native anthropic failover for model %s to %s", model, candidates[ci + 1].provider);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    aigate_write_anthropic_error(rc, PIPE_UPSTREAM, "api_error", "upstream request failed");
    record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
    json_decref(jbody);
    key_rec_free(&krec);
    return 0;
}

/* ========================================================================= */
/* Native Gemini Pipeline (POST /v1beta/models/*)                            */
/* ========================================================================= */

static void
extract_gemini_model(const char* path, char* model_buf, size_t cap)
{
    model_buf[0] = '\0';
    if (!path) return;
    const char* prefix = "/v1beta/models/";
    const char* p = strstr(path, prefix);
    if (!p) {
        prefix = "/models/";
        p = strstr(path, prefix);
    }
    if (!p) return;
    p += strlen(prefix);
    const char* colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len >= cap) len = cap - 1;
    memcpy(model_buf, p, len);
    model_buf[len] = '\0';
}

static void
build_gemini_url(const char* endpoint, const char* path, const char* api_key, char* url_out, size_t url_cap)
{
    const char* ep = endpoint;
    if (ep == NULL || ep[0] == '\0' || strcmp(ep, "/") == 0) {
        ep = "https://generativelanguage.googleapis.com";
    }
    char base_ep[512];
    snprintf(base_ep, sizeof base_ep, "%s", ep);
    size_t elen = strlen(base_ep);
    while (elen > 0 && base_ep[elen - 1] == '/') {
        base_ep[--elen] = '\0';
    }
    const char* p = path ? path : "";
    if (p[0] == '/') p++;
    if (api_key && api_key[0]) {
        snprintf(url_out, url_cap, "%s/%s%skey=%s", base_ep, p, strchr(p, '?') ? "&" : "?", api_key);
    } else {
        snprintf(url_out, url_cap, "%s/%s", base_ep, p);
    }
}

typedef struct {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    gemini_sniffer_t     sniffer;
} gemini_stream_ctx_t;

static int
gemini_stream_chunk_cb(void* user_data, const void* chunk, size_t len)
{
    gemini_stream_ctx_t* ctx = user_data;
    if (!ctx->headers_sent) {
        ctx->rc->status = 200;
        if (ctx->rc->set_header != NULL) {
            ctx->rc->set_header(ctx->rc->impl, "Content-Type", "text/event-stream; charset=utf-8");
            ctx->rc->set_header(ctx->rc->impl, "Cache-Control", "no-cache");
            ctx->rc->set_header(ctx->rc->impl, "Connection", "keep-alive");
        }
        ctx->headers_sent = true;
        ctx->rc->headers_sent = true;
    }
    if (ctx->rc->write != NULL && len > 0) {
        if (ctx->rc->write(ctx->rc->impl, chunk, len, false) != 0) {
            return -1;
        }
    }
    gemini_sniff_feed(&ctx->sniffer, chunk, len);
    return 0;
}

int
handle_gemini_generate(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* Auth */
    key_rec_t krec;
    int arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_gemini_error(rc, PIPE_AUTH, "UNAUTHENTICATED", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* Rate limit */
    long retry_ms = 0;
    int rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            aigate_write_gemini_error(rc, 503, "UNAVAILABLE", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) ra_s = 1;
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        if (rc->set_header != NULL) rc->set_header(rc->impl, "Retry-After", ra);
        aigate_write_gemini_error(rc, PIPE_RATE, "RESOURCE_EXHAUSTED", "rate limit exceeded");
        key_rec_free(&krec);
        return 0;
    }

    /* Daily quota */
    if (krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
        if (rem == LONG_MIN) {
            aigate_write_gemini_error(rc, 503, "UNAVAILABLE", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        if (rem <= 0) {
            time_t now = time(NULL);
            time_t next = (time_t)(now - (now % 86400)) + 86400;
            char ra[32];
            snprintf(ra, sizeof ra, "%ld", (long)(next - now));
            if (rc->set_header != NULL) rc->set_header(rc->impl, "Retry-After", ra);
            aigate_write_gemini_error(rc, PIPE_RATE, "RESOURCE_EXHAUSTED", "daily token quota exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* Monthly budget */
    if (ac->be != NULL) {
        char b_err[256] = {0};
        if (budget_enforce_check(ac->be, krec.key_id, krec.group_id,
                                 krec.monthly_cost_budget, krec.monthly_token_budget,
                                 0.0, b_err, sizeof b_err) != 0) {
            aigate_write_gemini_error(rc, PIPE_RATE, "RESOURCE_EXHAUSTED",
                                      b_err[0] ? b_err : "monthly budget limit exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    char model[128] = {0};
    extract_gemini_model(rq->path, model, sizeof model);
    if (model[0] == '\0') {
        aigate_write_gemini_error(rc, 400, "INVALID_ARGUMENT", "failed to extract model from path");
        key_rec_free(&krec);
        return 0;
    }
    if (!key_allows_model(&krec, model)) {
        aigate_write_gemini_error(rc, PIPE_FORBIDDEN, "PERMISSION_DENIED", "model not allowed for this key");
        key_rec_free(&krec);
        return 0;
    }

    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_gemini_error(rc, PIPE_MODEL, "NOT_FOUND", "model not found");
        key_rec_free(&krec);
        return 0;
    }

    if (strcmp(route.provider, "gemini") != 0 && strcmp(route.provider, "google") != 0) {
        aigate_write_gemini_error(rc, 400, "INVALID_ARGUMENT",
            "Gemini endpoint requires a gemini provider route");
        key_rec_free(&krec);
        return 0;
    }

    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int n_candidates = 0;
    if (model_router_select_candidates(ac->cb, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_gemini_error(rc, PIPE_MODEL, "UNAVAILABLE", "no upstream targets available for model");
        key_rec_free(&krec);
        return 0;
    }

    bool is_streaming = (strstr(rq->path, ":streamGenerateContent") != NULL);
    uint64_t total_lat = 0;
    const char* last_provider = route.provider;

    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t* target = &candidates[ci];
        model_rec_t cur_route = route;
        snprintf(cur_route.provider, sizeof cur_route.provider, "%.*s", (int)sizeof cur_route.provider - 1, target->provider);
        snprintf(cur_route.endpoint, sizeof cur_route.endpoint, "%.*s", (int)sizeof cur_route.endpoint - 1, target->endpoint);
        snprintf(cur_route.upstream_key, sizeof cur_route.upstream_key, "%.*s", (int)sizeof cur_route.upstream_key - 1, target->upstream_key);
        last_provider = target->provider;

        char url[1024];
        build_gemini_url(cur_route.endpoint, rq->path, cur_route.upstream_key, url, sizeof url);

        const char* extra_hdrs[4][2] = {{NULL, NULL}};
        int n_extra_hdrs = 0;

        if (is_streaming) {
            gemini_stream_ctx_t sctx;
            memset(&sctx, 0, sizeof sctx);
            sctx.rc = rc;
            gemini_sniff_init(&sctx.sniffer);

            int status = 0;
            char* sbody = NULL;
            size_t slen = 0;
            uint64_t t0 = mono_ns();
            long silence_timeout_ms = ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int urc = upstream_stream_call(url, "", extra_hdrs, n_extra_hdrs,
                                           (const char*)rq->body, rq->body_len, silence_timeout_ms,
                                           gemini_stream_chunk_cb, &sctx, &status, &sbody, &slen);
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
            if (!sctx.headers_sent) {
                cb_record_failure(ac->cb, model, target->endpoint, status);
                if (!is_failover && status >= 400) {
                    if (sbody != NULL && urc == 0) {
                        record_usage_and_event(ac, krec.key_id, model, status, 0, 0, 0, 0, total_lat, target->provider, NULL, 0.0);
                        int rv = aigate_write_json(rc, status, sbody, slen);
                        free(sbody);
                        key_rec_free(&krec);
                        return rv;
                    }
                    free(sbody);
                    break;
                }
                free(sbody);
                if (ci + 1 < n_candidates) {
                    AIGATE_LOG_WARN("native gemini streaming failover for model %s to %s", model, candidates[ci + 1].provider);
                    metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                    continue;
                }
                break;
            }

            long ptok = 0, ctok = 0;
            gemini_sniff_get_tokens(&sctx.sniffer, &ptok, &ctok);
            double req_cost = calc_req_cost(&route, ptok, ctok, 0);

            if (urc != 0) {
                const char* err_sse = "data: {\"error\": {\"code\": 500, \"message\": \"stream interrupted\", \"status\": \"INTERNAL\"}}\n\n";
                if (rc->write != NULL) rc->write(rc->impl, err_sse, strlen(err_sse), true);
                record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, ptok, ctok, 0, 0, total_lat, target->provider, NULL, req_cost);
                if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
                if (ptok + ctok > 0) rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            if (rc->write != NULL) rc->write(rc->impl, "", 0, true);
            record_usage_and_event(ac, krec.key_id, model, status > 0 ? status : 200, ptok, ctok, 0, 0, total_lat, target->provider, NULL, req_cost);
            if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
            key_rec_free(&krec);
            return 0;
        }

        /* Non-streaming */
        int status = 0;
        char* ubody = NULL;
        size_t ulen = 0;
        uint64_t t0 = mono_ns();
        int urc = upstream_call_ext(url, "", extra_hdrs, n_extra_hdrs,
                                    (const char*)rq->body, rq->body_len, ac->default_timeout_ms,
                                    &status, &ubody, &ulen);
        if (n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000};
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url, "", extra_hdrs, n_extra_hdrs,
                                    (const char*)rq->body, rq->body_len, ac->default_timeout_ms,
                                    &status, &ubody, &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            long ptok = 0, ctok = 0;
            gemini_sniff_json_tokens(ubody ? ubody : "", &ptok, &ctok);
            double req_cost = calc_req_cost(&route, ptok, ctok, 0);
            record_usage_and_event(ac, krec.key_id, model, status, ptok, ctok, 0, 0, total_lat, target->provider, NULL, req_cost);
            if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
            int rv = aigate_write_json(rc, status, ubody ? ubody : "", ulen);
            free(ubody);
            key_rec_free(&krec);
            return rv;
        }

        cb_record_failure(ac->cb, model, target->endpoint, status);
        if (!is_failover && status >= 400) {
            if (ubody != NULL && urc == 0) {
                record_usage_and_event(ac, krec.key_id, model, status, 0, 0, 0, 0, total_lat, target->provider, NULL, 0.0);
                int rv = aigate_write_json(rc, status, ubody, ulen);
                free(ubody);
                key_rec_free(&krec);
                return rv;
            }
            free(ubody);
            break;
        }
        free(ubody);
        if (ci + 1 < n_candidates) {
            AIGATE_LOG_WARN("native gemini failover for model %s to %s", model, candidates[ci + 1].provider);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    aigate_write_gemini_error(rc, PIPE_UPSTREAM, "UNAVAILABLE", "upstream request failed");
    record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
    key_rec_free(&krec);
    return 0;
}
```

- [x] **Step 2: Remove native Anthropic and Gemini functions from `src/core/aigate_core.c`**

Delete lines ~1651 to 2770 from `src/core/aigate_core.c`.

- [x] **Step 3: Compile and run test suite**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed.

- [x] **Step 4: Commit Task 3**

```bash
git add src/core/pipeline_native.c src/core/aigate_core.c
git commit -m "refactor(core): ♻️ extract pipeline_native.c"
```

---

### Task 4: Extract `pipeline_responses.c` (Responses API)

**Files:**
- Create: `src/core/pipeline_responses.c`
- Modify: `src/core/aigate_core.c`

- [x] **Step 1: Create `src/core/pipeline_responses.c`**

Extract `handle_responses` from `src/core/aigate_core.c` into `src/core/pipeline_responses.c`:

```c
/** @file pipeline_responses.c
 *  @brief /v1/responses pipeline handling (OpenAI Responses API).
 */
#include "aigate_core_internal.h"
#include "metrics.h"
#include "provider_adapter.h"
#include "provider_openai.h"
#include "response_cache.h"
#include "upstream_client.h"

#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* Auth */
    key_rec_t krec;
    int arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_error(rc, PIPE_AUTH, "auth_error", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* Rate limit */
    long retry_ms = 0;
    int rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            aigate_write_error(rc, 503, "server_error", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) ra_s = 1;
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        if (rc->set_header != NULL) rc->set_header(rc->impl, "Retry-After", ra);
        aigate_write_error(rc, PIPE_RATE, "rate_limit", "rate limit exceeded");
        key_rec_free(&krec);
        return 0;
    }

    /* Daily quota */
    if (krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
        if (rem == LONG_MIN) {
            aigate_write_error(rc, 503, "server_error", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        if (rem <= 0) {
            time_t now = time(NULL);
            time_t next = (time_t)(now - (now % 86400)) + 86400;
            char ra[32];
            snprintf(ra, sizeof ra, "%ld", (long)(next - now));
            if (rc->set_header != NULL) rc->set_header(rc->impl, "Retry-After", ra);
            aigate_write_error(rc, PIPE_RATE, "daily_quota_exceeded", "daily token quota exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* Monthly budget */
    if (ac->be != NULL) {
        char b_err[256] = {0};
        if (budget_enforce_check(ac->be, krec.key_id, krec.group_id,
                                 krec.monthly_cost_budget, krec.monthly_token_budget,
                                 0.0, b_err, sizeof b_err) != 0) {
            aigate_write_error(rc, PIPE_RATE, "budget_exceeded",
                               b_err[0] ? b_err : "monthly budget limit exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* Model parse */
    const char* model = "";
    json_t* jbody = NULL;
    if (rq->body != NULL && rq->body_len > 0) {
        jbody = json_loads((const char*)rq->body, 0, NULL);
    }
    if (jbody != NULL) {
        json_t* jm = json_object_get(jbody, "model");
        if (jm != NULL && json_is_string(jm)) {
            model = json_string_value(jm);
        }
    }
    if (model[0] == '\0') {
        aigate_write_error(rc, 400, "model_not_found", "model field is required");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }
    if (!key_allows_model(&krec, model)) {
        aigate_write_error(rc, PIPE_FORBIDDEN, "auth_error", "model not allowed for this key");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_error(rc, PIPE_MODEL, "model_not_found", "model not found");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    if (strcmp(route.provider, "openai") != 0) {
        aigate_write_error(rc, 400, "unsupported_endpoint",
            "/v1/responses requires an openai-compatible provider");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int n_candidates = 0;
    if (model_router_select_candidates(ac->cb, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_error(rc, PIPE_MODEL, "no_healthy_upstream", "no upstream targets available for model");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    bool is_streaming = false;
    if (jbody != NULL) {
        json_t* js = json_object_get(jbody, "stream");
        if (js != NULL && json_is_true(js)) {
            is_streaming = true;
        }
    }

    bool bypass_cache = false;
    bool no_store = false;
    if (rq->cache_control != NULL) {
        if (strstr(rq->cache_control, "no-cache") != NULL ||
            strstr(rq->cache_control, "max-age=0") != NULL ||
            strcmp(rq->cache_control, "true") == 0 ||
            strcmp(rq->cache_control, "1") == 0) {
            bypass_cache = true;
        }
        if (strstr(rq->cache_control, "no-store") != NULL) {
            bypass_cache = true;
            no_store = true;
        }
    }

    char cache_key[65] = {0};
    if (ac->rc != NULL && !bypass_cache) {
        response_cache_fingerprint(model, (const char*)rq->body, rq->body_len, cache_key);
    }
    if (rc->set_header != NULL && ac->rc != NULL && bypass_cache) {
        rc->set_header(rc->impl, "X-Cache", "MISS");
    }
    if (ac->rc != NULL && cache_key[0] != '\0') {
        cache_entry_t* ce = response_cache_get(ac->rc, cache_key);
        if (ce != NULL) {
            if (!is_streaming) {
                if (rc->set_header != NULL) {
                    rc->set_header(rc->impl, "X-Cache", "HIT");
                    rc->set_header(rc->impl, "X-Cache-Lookup-Time", "0.10ms");
                    char age_str[32];
                    snprintf(age_str, sizeof(age_str), "%ld", (long)(time(NULL) - ce->created_at));
                    rc->set_header(rc->impl, "Age", age_str);
                }
                record_usage_and_event(ac, krec.key_id, model, 200, ce->prompt_tokens, ce->completion_tokens, 0, 0, 100000ULL, "cache", NULL, ce->cost_usd);
                if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, ce->cost_usd, ce->prompt_tokens + ce->completion_tokens);
                rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ce->prompt_tokens + ce->completion_tokens);
                int rv = aigate_write_json(rc, 200, ce->response_body, ce->response_len);
                response_cache_release_entry(ce);
                json_decref(jbody);
                key_rec_free(&krec);
                return rv;
            } else {
                int rv = cache_stream_replay(ac, rc, ce, model, &krec, NULL);
                json_decref(jbody);
                key_rec_free(&krec);
                return rv;
            }
        } else {
            if (rc->set_header != NULL) rc->set_header(rc->impl, "X-Cache", "MISS");
        }
    }

    uint64_t total_lat = 0;
    const char* last_provider = route.provider;

    if (is_streaming) {
        for (int ci = 0; ci < n_candidates; ci++) {
            upstream_target_t* target = &candidates[ci];
            const provider_adapter_t* adapter = provider_find(target->provider);
            if (adapter == NULL || adapter->build_responses == NULL) continue;

            model_rec_t cur_route = route;
            snprintf(cur_route.provider, sizeof cur_route.provider, "%.*s", (int)sizeof cur_route.provider - 1, target->provider);
            snprintf(cur_route.endpoint, sizeof cur_route.endpoint, "%.*s", (int)sizeof cur_route.endpoint - 1, target->endpoint);
            snprintf(cur_route.upstream_key, sizeof cur_route.upstream_key, "%.*s", (int)sizeof cur_route.upstream_key - 1, target->upstream_key);
            last_provider = target->provider;

            char url[1024];
            char* out_body = NULL;
            size_t out_body_len = 0;
            const char* extra_hdrs[4][2] = {{0}};
            int n_extra_hdrs = 0;

            if (adapter->build_responses(&cur_route, rq->body != NULL ? (const char*)rq->body : "", url, sizeof url, extra_hdrs, &n_extra_hdrs, &out_body, &out_body_len) != 0) {
                free(out_body);
                continue;
            }

            stream_cache_acc_t acc;
            memset(&acc, 0, sizeof acc);
            acc.orig_rc = rc;

            aigate_response_ctx proxy_rc = *rc;
            proxy_rc.impl = &acc;
            proxy_rc.set_header = stream_cache_acc_set_header;
            proxy_rc.write = stream_cache_acc_write;

            stream_bridge_t* bridge = adapter->stream_bridge_new != NULL ? adapter->stream_bridge_new(&proxy_rc, model) : NULL;
            if (bridge == NULL) {
                free(out_body);
                continue;
            }

            int status = 0;
            char* sbody = NULL;
            size_t slen = 0;
            uint64_t t0 = mono_ns();
            long silence_timeout_ms = ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int urc = upstream_stream_call(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                           out_body, out_body_len, silence_timeout_ms,
                                           (upstream_chunk_fn)adapter->stream_bridge_feed, bridge,
                                           &status, &sbody, &slen);
            bool headers_sent = adapter->stream_bridge_headers_sent(bridge);

            if (n_candidates == 1 && !headers_sent && (urc != 0 || status >= 500)) {
                struct timespec sl = {0, 200 * 1000000};
                nanosleep(&sl, NULL);
                free(sbody);
                sbody = NULL;
                urc = upstream_stream_call(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                           out_body, out_body_len, silence_timeout_ms,
                                           (upstream_chunk_fn)adapter->stream_bridge_feed, bridge,
                                           &status, &sbody, &slen);
                headers_sent = adapter->stream_bridge_headers_sent(bridge);
            }
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;
            free(out_body);

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
            if (!headers_sent) {
                cb_record_failure(ac->cb, model, target->endpoint, status);
                adapter->stream_bridge_free(bridge);
                if (acc.accum_content != NULL) free(acc.accum_content);

                if (!is_failover && status >= 400) {
                    if (sbody != NULL && urc == 0) {
                        record_usage_and_event(ac, krec.key_id, model, status, 0, 0, 0, 0, total_lat, target->provider, NULL, 0.0);
                        int rv = aigate_write_json(rc, status, sbody, slen);
                        free(sbody);
                        json_decref(jbody);
                        key_rec_free(&krec);
                        return rv;
                    }
                    free(sbody);
                    break;
                }
                if (ci + 1 < n_candidates) {
                    AIGATE_LOG_WARN("responses streaming failover for model %s to %s", model, candidates[ci + 1].provider);
                    metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                    free(sbody);
                    continue;
                }
                free(sbody);
                break;
            }

            long ptok = 0, ctok = 0, cached_tok = 0, reasoning_tok = 0;
            provider_openai_bridge_get_tokens(bridge, &ptok, &ctok, &cached_tok, &reasoning_tok);

            if (urc != 0) {
                const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout" : "stream interrupted: transport error";
                char sse_err[256];
                snprintf(sse_err, sizeof sse_err, "data: {\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\",\"code\":502}}\n\ndata: [DONE]\n\n", err_msg);
                if (rc->write != NULL) rc->write(rc->impl, sse_err, strlen(sse_err), true);
                double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
                record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, ptok, ctok, cached_tok, reasoning_tok, total_lat, target->provider, NULL, req_cost);
                if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
                if (ptok + ctok > 0) rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
                adapter->stream_bridge_free(bridge);
                if (acc.accum_content != NULL) free(acc.accum_content);
                json_decref(jbody);
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            adapter->stream_bridge_finish(bridge);
            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac, krec.key_id, model, status > 0 ? status : 200, ptok, ctok, cached_tok, reasoning_tok, total_lat, target->provider, NULL, req_cost);
            if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            if (ac->rc != NULL && cache_key[0] != '\0' && !no_store && !acc.overflow &&
                (status == 0 || status == 200) && acc.accum_content != NULL && acc.accum_len > 0) {
                json_t* full_resp = json_pack("{s:s, s:s, s:I, s:s, s:[{s:i, s:{s:s, s:s}, s:s}], s:{s:i, s:i, s:i}}",
                                              "id", acc.id[0] ? acc.id : "chatcmpl-stream",
                                              "object", "chat.completion",
                                              "created", (json_int_t)(acc.created > 0 ? acc.created : time(NULL)),
                                              "model", model,
                                              "choices", "index", 0, "message", "role", "assistant", "content", acc.accum_content, "finish_reason", "stop",
                                              "usage", "prompt_tokens", (int)ptok, "completion_tokens", (int)ctok, "total_tokens", (int)(ptok + ctok));
                if (full_resp != NULL) {
                    char* full_json = json_dumps(full_resp, JSON_COMPACT);
                    if (full_json != NULL) {
                        response_cache_set(ac->rc, cache_key, model, full_json, strlen(full_json), ptok, ctok, req_cost, 0);
                        free(full_json);
                    }
                    json_decref(full_resp);
                }
            }
            if (acc.accum_content != NULL) free(acc.accum_content);
            adapter->stream_bridge_free(bridge);
            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        if (rc->set_header != NULL) rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
        aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
        record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* Non-streaming */
    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t* target = &candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL || adapter->build_responses == NULL || adapter->parse_responses_response == NULL) continue;

        model_rec_t cur_route = route;
        snprintf(cur_route.provider, sizeof cur_route.provider, "%.*s", (int)sizeof cur_route.provider - 1, target->provider);
        snprintf(cur_route.endpoint, sizeof cur_route.endpoint, "%.*s", (int)sizeof cur_route.endpoint - 1, target->endpoint);
        snprintf(cur_route.upstream_key, sizeof cur_route.upstream_key, "%.*s", (int)sizeof cur_route.upstream_key - 1, target->upstream_key);
        last_provider = target->provider;

        char url[1024];
        char* out_body = NULL;
        size_t out_body_len = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int n_extra_hdrs = 0;

        if (adapter->build_responses(&cur_route, rq->body != NULL ? (const char*)rq->body : "", url, sizeof url, extra_hdrs, &n_extra_hdrs, &out_body, &out_body_len) != 0) {
            free(out_body);
            continue;
        }

        int status = 0;
        char* ubody = NULL;
        size_t ulen = 0;
        uint64_t t0 = mono_ns();
        int urc = upstream_call_ext(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                    out_body, out_body_len, ac->default_timeout_ms,
                                    &status, &ubody, &ulen);
        if (n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000};
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url, cur_route.upstream_key, extra_hdrs, n_extra_hdrs,
                                    out_body, out_body_len, ac->default_timeout_ms,
                                    &status, &ubody, &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        free(out_body);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            long ptok = 0, ctok = 0, cached_tok = 0, reasoning_tok = 0;
            adapter->parse_responses_response(ubody ? ubody : "", ulen, &ptok, &ctok, &cached_tok, &reasoning_tok);
            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac, krec.key_id, model, status, ptok, ctok, cached_tok, reasoning_tok, total_lat, target->provider, NULL, req_cost);
            if (ac->be != NULL) budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            if (ac->rc != NULL && cache_key[0] != '\0' && status == 200 && ubody != NULL && !no_store && ulen <= 1048576) {
                response_cache_set(ac->rc, cache_key, model, ubody, ulen, ptok, ctok, req_cost, 0);
            }

            int rv = aigate_write_json(rc, status, ubody ? ubody : "", ulen);
            free(ubody);
            json_decref(jbody);
            key_rec_free(&krec);
            return rv;
        }

        cb_record_failure(ac->cb, model, target->endpoint, status);
        if (!is_failover && status >= 400) {
            if (ubody != NULL && urc == 0) {
                record_usage_and_event(ac, krec.key_id, model, status, 0, 0, 0, 0, total_lat, target->provider, NULL, 0.0);
                int rv = aigate_write_json(rc, status, ubody, ulen);
                free(ubody);
                json_decref(jbody);
                key_rec_free(&krec);
                return rv;
            }
            free(ubody);
            break;
        }
        free(ubody);
        if (ci + 1 < n_candidates) {
            AIGATE_LOG_WARN("responses failover for model %s to %s", model, candidates[ci + 1].provider);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    record_usage_and_event(ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
    json_decref(jbody);
    key_rec_free(&krec);
    return 0;
}
```

- [x] **Step 2: Remove `handle_responses` from `src/core/aigate_core.c`**

Delete lines ~982 to 1650 from `src/core/aigate_core.c`.

- [x] **Step 3: Compile and run test suite**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed.

- [x] **Step 4: Commit Task 4**

```bash
git add src/core/pipeline_responses.c src/core/aigate_core.c
git commit -m "refactor(core): ♻️ extract pipeline_responses.c"
```

---

### Task 5: Extract `pipeline_chat.c` and Slim Down `aigate_core.c`

**Files:**
- Create: `src/core/pipeline_chat.c`
- Modify: `src/core/aigate_core.c`

- [x] **Step 1: Create `src/core/pipeline_chat.c`**

Extract `handle_models_list`, `prepare_chat_cache`, `handle_stream_preheaders`, `handle_chat_sync`, and `handle_chat_stream` into `src/core/pipeline_chat.c`:

```c
/** @file pipeline_chat.c
 *  @brief /v1/chat/completions and /v1/models pipeline handling.
 */
#include "aigate_core_internal.h"
#include "metrics.h"
#include "provider_adapter.h"
#include "response_cache.h"
#include "upstream_client.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
handle_models_list(chat_req_t* q)
{
    if (q->rq->path == NULL || strcmp(q->rq->path, "/v1/models") != 0) {
        return 0;
    }
    json_t* root = json_object();
    json_object_set_new(root, "object", json_string("list"));
    json_t* data = json_array();
    const char* models[128];
    int n_models = model_router_list_models(q->ac->router, models, 128);
    for (int i = 0; i < n_models; i++) {
        if (!key_allows_model(&q->krec, models[i])) {
            continue;
        }
        json_t* item = json_object();
        json_object_set_new(item, "id", json_string(models[i]));
        json_object_set_new(item, "object", json_string("model"));
        json_object_set_new(item, "created", json_integer(1700000000));
        json_object_set_new(item, "owned_by", json_string("aigate"));
        json_array_append_new(data, item);
    }
    json_object_set_new(root, "data", data);
    char* packed = json_dumps(root, 0);
    json_decref(root);
    if (!packed) {
        aigate_write_error(q->rc, 500, "internal_error", "failed to serialize models list");
        return -1;
    }
    int rv = aigate_write_json(q->rc, 200, packed, strlen(packed));
    free(packed);
    return rv == 0 ? 1 : -1;
}

int
prepare_chat_cache(chat_req_t* q, bool* is_streaming)
{
    *is_streaming = false;
    if (q->jbody != NULL) {
        json_t* js = json_object_get(q->jbody, "stream");
        if (js != NULL && json_is_true(js)) {
            *is_streaming = true;
        }
    }

    if (q->rq->cache_control != NULL) {
        if (strstr(q->rq->cache_control, "no-cache") != NULL ||
            strstr(q->rq->cache_control, "max-age=0") != NULL ||
            strcmp(q->rq->cache_control, "true") == 0 ||
            strcmp(q->rq->cache_control, "1") == 0) {
            q->bypass_cache = true;
        }
        if (strstr(q->rq->cache_control, "no-store") != NULL) {
            q->bypass_cache = true;
            q->no_store = true;
        }
    }

    if (q->ac->rc != NULL && !q->bypass_cache) {
        response_cache_fingerprint(q->model, (const char*)q->eff_body, q->eff_len, q->cache_key);
    }

    if (q->rc->set_header != NULL && q->ac->rc != NULL && q->bypass_cache) {
        q->rc->set_header(q->rc->impl, "X-Cache", "MISS");
    }

    if (q->ac->rc != NULL && q->cache_key[0] != '\0') {
        cache_entry_t* ce = response_cache_get(q->ac->rc, q->cache_key);
        if (ce != NULL) {
            if (!*is_streaming) {
                if (q->rc->set_header != NULL) {
                    q->rc->set_header(q->rc->impl, "X-Cache", "HIT");
                    q->rc->set_header(q->rc->impl, "X-Cache-Lookup-Time", "0.10ms");
                    char age_str[32];
                    snprintf(age_str, sizeof(age_str), "%ld", (long)(time(NULL) - ce->created_at));
                    q->rc->set_header(q->rc->impl, "Age", age_str);
                }
                record_usage_and_event(q->ac,
                                       q->krec.key_id,
                                       q->model,
                                       200,
                                       ce->prompt_tokens,
                                       ce->completion_tokens,
                                       0,
                                       0,
                                       100000ULL,
                                       "cache",
                                       q->guardrail_act,
                                       ce->cost_usd);
                if (q->ac->be != NULL) {
                    budget_enforce_record(q->ac->be,
                                          q->krec.key_id,
                                          q->krec.group_id,
                                          ce->cost_usd,
                                          ce->prompt_tokens + ce->completion_tokens);
                }
                rl_reserve_tokens(q->ac->rl,
                                  q->krec.key_id,
                                  q->krec.daily_token_quota,
                                  ce->prompt_tokens + ce->completion_tokens);
                aigate_write_json(q->rc, 200, ce->response_body, ce->response_len);
                response_cache_release_entry(ce);
                return 1;
            } else {
                int srv = cache_stream_replay(q->ac, q->rc, ce, q->model, &q->krec, q);
                return (srv == 0) ? 1 : -1;
            }
        } else {
            if (q->rc->set_header != NULL) {
                q->rc->set_header(q->rc->impl, "X-Cache", "MISS");
            }
        }
    }
    return 0;
}

static stream_bridge_t*
handle_stream_preheaders(chat_req_t*               q,
                         stream_cache_acc_t*       acc,
                         const provider_adapter_t* adapter,
                         aigate_response_ctx*      proxy_rc)
{
    memset(acc, 0, sizeof(*acc));
    acc->orig_rc = q->rc;
    *proxy_rc = *q->rc;
    proxy_rc->impl = acc;
    proxy_rc->set_header = stream_cache_acc_set_header;
    proxy_rc->write = stream_cache_acc_write;

    return adapter->stream_bridge_new != NULL ? adapter->stream_bridge_new(proxy_rc, q->model)
                                              : NULL;
}

int
handle_chat_sync(chat_req_t* q)
{
    uint64_t    total_lat = 0;
    const char* last_provider = q->route.provider;

    for (int ci = 0; ci < q->n_candidates; ci++) {
        upstream_target_t* target = &q->candidates[ci];
        model_rec_t cur_route;
        fill_cur_route(&q->route, target, &cur_route);
        last_provider = target->provider;

        char        url[1024];
        char*       out_body = NULL;
        size_t      out_body_len = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;

        int brc = provider_build(&cur_route,
                                 q->eff_body != NULL ? (const char*)q->eff_body : "",
                                 url,
                                 sizeof url,
                                 extra_hdrs,
                                 &n_extra_hdrs,
                                 &out_body,
                                 &out_body_len);
        if (brc != 0) {
            free(out_body);
            continue;
        }

        int      status = 0;
        char*    ubody = NULL;
        size_t   ulen = 0;
        uint64_t t0 = mono_ns();
        int      urc = upstream_call_ext(url,
                                    cur_route.upstream_key,
                                    extra_hdrs,
                                    n_extra_hdrs,
                                    out_body,
                                    out_body_len,
                                    q->ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);

        if (q->n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000};
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url,
                                    cur_route.upstream_key,
                                    extra_hdrs,
                                    n_extra_hdrs,
                                    out_body,
                                    out_body_len,
                                    q->ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        free(out_body);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

        if (!is_failover && status < 400) {
            cb_record_success(q->ac->cb, q->model, target->endpoint);
            long   ptok = 0, ctok = 0, cached_tok = 0, reasoning_tok = 0;
            char*  std_resp = NULL;
            size_t std_resp_len = 0;
            provider_parse_response(&cur_route,
                                    ubody ? ubody : "",
                                    ulen,
                                    q->model,
                                    status,
                                    &std_resp,
                                    &std_resp_len,
                                    &ptok,
                                    &ctok,
                                    &cached_tok,
                                    &reasoning_tok);
            free(ubody);
            if (!std_resp) {
                aigate_write_error(
                    q->rc, PIPE_UPSTREAM, "upstream_error", "failed to parse upstream response");
                record_usage_and_event(q->ac,
                                       q->krec.key_id,
                                       q->model,
                                       PIPE_UPSTREAM,
                                       0,
                                       0,
                                       0,
                                       0,
                                       total_lat,
                                       target->provider,
                                       q->guardrail_act,
                                       0.0);
                chat_req_cleanup(q);
                return 0;
            }

            double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
            settle_success(q, target, status, ptok, ctok, cached_tok, reasoning_tok, total_lat, req_cost);

            if (q->ac->rc != NULL && q->cache_key[0] != '\0' && status == 200 && !q->no_store &&
                std_resp_len <= 1048576) {
                response_cache_set(q->ac->rc,
                                   q->cache_key,
                                   q->model,
                                   std_resp,
                                   std_resp_len,
                                   ptok,
                                   ctok,
                                   req_cost,
                                   0);
            }

            int rv = aigate_write_json(q->rc, status, std_resp, std_resp_len);
            free(std_resp);
            chat_req_cleanup(q);
            return rv;
        }

        cb_record_failure(q->ac->cb, q->model, target->endpoint, status);
        if (!is_failover && status >= 400) {
            if (ubody != NULL && urc == 0) {
                record_usage_and_event(q->ac,
                                       q->krec.key_id,
                                       q->model,
                                       status,
                                       0,
                                       0,
                                       0,
                                       0,
                                       total_lat,
                                       target->provider,
                                       q->guardrail_act,
                                       0.0);
                int rv = aigate_write_json(q->rc, status, ubody, ulen);
                free(ubody);
                chat_req_cleanup(q);
                return rv;
            }
            free(ubody);
            break;
        }
        free(ubody);
        if (ci + 1 < q->n_candidates) {
            failover_warn("sync", q->model, target->provider, q->candidates[ci + 1].provider);
            metrics_inc_failover(q->model, target->provider, q->candidates[ci + 1].provider);
            continue;
        }
    }

    if (q->rc->set_header != NULL) {
        q->rc->set_header(q->rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(q->rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    record_usage_and_event(q->ac,
                           q->krec.key_id,
                           q->model,
                           PIPE_UPSTREAM,
                           0,
                           0,
                           0,
                           0,
                           total_lat,
                           last_provider,
                           q->guardrail_act,
                           0.0);
    chat_req_cleanup(q);
    return 0;
}

int
handle_chat_stream(chat_req_t* q)
{
    uint64_t    total_lat = 0;
    const char* last_provider = q->route.provider;

    for (int ci = 0; ci < q->n_candidates; ci++) {
        upstream_target_t* target = &q->candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL) continue;

        model_rec_t cur_route;
        fill_cur_route(&q->route, target, &cur_route);
        last_provider = target->provider;

        char        url[1024];
        char*       out_body = NULL;
        size_t      out_body_len = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;

        int brc = adapter->build_chat(&cur_route,
                                      q->eff_body != NULL ? (const char*)q->eff_body : "",
                                      url,
                                      sizeof url,
                                      extra_hdrs,
                                      &n_extra_hdrs,
                                      &out_body,
                                      &out_body_len);
        if (brc != 0) {
            free(out_body);
            continue;
        }

        stream_cache_acc_t  acc;
        aigate_response_ctx proxy_rc;
        stream_bridge_t*    bridge =
            handle_stream_preheaders(q, &acc, adapter, &proxy_rc);
        if (bridge == NULL) {
            free(out_body);
            continue;
        }

        int      status = 0;
        char*    sbody = NULL;
        size_t   slen = 0;
        uint64_t t0 = mono_ns();
        long     silence_timeout_ms =
            q->ac->default_timeout_ms > 0 ? (long)q->ac->default_timeout_ms : 30000L;
        int urc = upstream_stream_call(url,
                                       cur_route.upstream_key,
                                       extra_hdrs,
                                       n_extra_hdrs,
                                       out_body,
                                       out_body_len,
                                       silence_timeout_ms,
                                       (upstream_chunk_fn)adapter->stream_bridge_feed,
                                       bridge,
                                       &status,
                                       &sbody,
                                       &slen);
        bool headers_sent = adapter->stream_bridge_headers_sent(bridge);

        if (q->n_candidates == 1 && !headers_sent && (urc != 0 || status >= 500)) {
            struct timespec sl = {0, 200 * 1000000};
            nanosleep(&sl, NULL);
            free(sbody);
            sbody = NULL;
            urc = upstream_stream_call(url,
                                       cur_route.upstream_key,
                                       extra_hdrs,
                                       n_extra_hdrs,
                                       out_body,
                                       out_body_len,
                                       silence_timeout_ms,
                                       (upstream_chunk_fn)adapter->stream_bridge_feed,
                                       bridge,
                                       &status,
                                       &sbody,
                                       &slen);
            headers_sent = adapter->stream_bridge_headers_sent(bridge);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        free(out_body);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

        if (!headers_sent) {
            cb_record_failure(q->ac->cb, q->model, target->endpoint, status);
            adapter->stream_bridge_free(bridge);
            if (acc.accum_content != NULL) free(acc.accum_content);

            if (!is_failover && status >= 400) {
                if (sbody != NULL && urc == 0) {
                    record_usage_and_event(q->ac,
                                           q->krec.key_id,
                                           q->model,
                                           status,
                                           0,
                                           0,
                                           0,
                                           0,
                                           total_lat,
                                           target->provider,
                                           q->guardrail_act,
                                           0.0);
                    int rv = aigate_write_json(q->rc, status, sbody, slen);
                    free(sbody);
                    chat_req_cleanup(q);
                    return rv;
                }
                free(sbody);
                break;
            }

            if (ci + 1 < q->n_candidates) {
                failover_warn("streaming",
                              q->model,
                              target->provider,
                              q->candidates[ci + 1].provider);
                metrics_inc_failover(q->model, target->provider, q->candidates[ci + 1].provider);
                free(sbody);
                continue;
            }
            free(sbody);
            break;
        }

        long ptok = 0, ctok = 0, cached_tok = 0, reasoning_tok = 0;
        adapter->stream_bridge_get_tokens(bridge, &ptok, &ctok, &cached_tok, &reasoning_tok);

        if (urc != 0) {
            const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                : "stream interrupted: transport error";
            char        sse_err[256];
            snprintf(sse_err,
                     sizeof sse_err,
                     "data: {\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\","
                     "\"code\":502}}\n\ndata: [DONE]\n\n",
                     err_msg);
            if (q->rc->write != NULL) {
                q->rc->write(q->rc->impl, sse_err, strlen(sse_err), true);
            }
            double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
            record_usage_and_event(q->ac,
                                   q->krec.key_id,
                                   q->model,
                                   PIPE_UPSTREAM,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   reasoning_tok,
                                   total_lat,
                                   target->provider,
                                   q->guardrail_act,
                                   req_cost);
            if (q->ac->be != NULL) {
                budget_enforce_record(
                    q->ac->be, q->krec.key_id, q->krec.group_id, req_cost, ptok + ctok);
            }
            if (ptok + ctok > 0) {
                rl_reserve_tokens(
                    q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, ptok + ctok);
            }
            adapter->stream_bridge_free(bridge);
            if (acc.accum_content != NULL) free(acc.accum_content);
            chat_req_cleanup(q);
            return 0;
        }

        cb_record_success(q->ac->cb, q->model, target->endpoint);
        adapter->stream_bridge_finish(bridge);
        double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
        settle_success(
            q, target, status, ptok, ctok, cached_tok, reasoning_tok, total_lat, req_cost);

        cache_store_stream(q, &acc, status, ptok, ctok, req_cost);
        if (acc.accum_content != NULL) free(acc.accum_content);
        adapter->stream_bridge_free(bridge);
        chat_req_cleanup(q);
        return 0;
    }

    if (q->rc->set_header != NULL) {
        q->rc->set_header(q->rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(q->rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    record_usage_and_event(q->ac,
                           q->krec.key_id,
                           q->model,
                           PIPE_UPSTREAM,
                           0,
                           0,
                           0,
                           0,
                           total_lat,
                           last_provider,
                           q->guardrail_act,
                           0.0);
    chat_req_cleanup(q);
    return 0;
}
```

- [x] **Step 2: Clean up `src/core/aigate_core.c` into concise controller**

In `src/core/aigate_core.c`:
1. Remove `handle_models_list`, `prepare_chat_cache`, `handle_stream_preheaders`, `handle_chat_sync`, and `handle_chat_stream`.
2. Keep:
   - `mono_ns`, `aigate_core_reload_guardrails`, `calc_req_cost`, `record_usage_and_event`
   - `aigate_core_init`, `aigate_core_shutdown`
   - `aigate_write_json`, `aigate_write_error`, `aigate_write_anthropic_error`, `aigate_write_gemini_error`
   - `chat_req_cleanup`, `gate_request`, `resolve_chat_target`
   - `stream_cache_acc_*`, `cache_stream_replay`, `cache_store_stream`
   - `fill_cur_route`, `settle_success`, `failover_warn`
   - `aigate_handle_request`
3. Verify that `aigate_handle_request` dispatches cleanly:
```c
int
aigate_handle_request(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    if (rq == NULL || rc == NULL || ac == NULL) {
        return -1;
    }

    if (rq->path != NULL && strcmp(rq->path, "/v1/responses") == 0) {
        return handle_responses(ac, rq, rc);
    }

    if (rq->path != NULL && strcmp(rq->path, "/v1/messages") == 0) {
        return handle_anthropic_messages(ac, rq, rc);
    }

    if (rq->path != NULL && (strstr(rq->path, ":generateContent") != NULL ||
                             strstr(rq->path, ":streamGenerateContent") != NULL)) {
        return handle_gemini_generate(ac, rq, rc);
    }

    chat_req_t chatq;
    memset(&chatq, 0, sizeof(chatq));
    chatq.ac = ac;
    chatq.rq = rq;
    chatq.rc = rc;
    if (gate_request(&chatq) != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }

    {
        int handled = handle_models_list(&chatq);
        if (handled != 0) {
            chat_req_cleanup(&chatq);
            return handled > 0 ? 0 : handled;
        }
    }

    if (resolve_chat_target(&chatq) != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }

    if (rq->path != NULL && strcmp(rq->path, "/v1/embeddings") == 0) {
        return handle_embeddings(&chatq);
    }

    bool is_streaming = false;
    int  cache_handled = prepare_chat_cache(&chatq, &is_streaming);
    if (cache_handled != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }

    if (is_streaming) {
        return handle_chat_stream(&chatq);
    }

    return handle_chat_sync(&chatq);
}
```

- [x] **Step 3: Compile and run test suite**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed.

- [x] **Step 4: Check line count of `src/core/aigate_core.c`**

Run:
```bash
wc -l src/core/aigate_core.c src/core/pipeline_*.c
```
Expected: `src/core/aigate_core.c` has ~600 lines or fewer (down from 3674 lines).

- [x] **Step 5: Commit Task 5**

```bash
git add src/core/pipeline_chat.c src/core/aigate_core.c
git commit -m "refactor(core): ♻️ extract pipeline_chat.c and slim down aigate_core.c"
```

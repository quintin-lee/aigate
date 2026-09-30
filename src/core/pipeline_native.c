/** @file pipeline_native.c
 *  @brief Native inbound protocol pipelines: Anthropic /v1/messages and Gemini /v1beta/models.
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

/* -------------------------------------------------------------------------
 * Native Anthropic Messages Pipeline (POST /v1/messages)
 * ------------------------------------------------------------------------- */

/** @brief Build Anthropic upstream URL: empty endpoint defaults to api.anthropic.com; strips trailing slash; if already has /v1 suffix appends /messages else appends /v1/messages. */
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

/** @brief Anthropic streaming callback context: first chunk writes SSE headers and passes through chunks, feeds usage sniffer. */
typedef struct {
    aigate_response_ctx* rc;           /**< Real response context (borrowed). */
    bool                 headers_sent; /**< Whether SSE headers written. */
    anthropic_sniffer_t  sniffer;      /**< Usage accumulation sniffer. */
} anthropic_stream_ctx_t;

/** @brief Anthropic streaming upstream callback: first chunk writes SSE headers, then passes through chunks and feeds usage sniffer.
 *  @return 0 to continue; -1 on downstream write failure aborts upstream. */
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
    anthropic_sniffer_feed(&ctx->sniffer, chunk, len);
    return 0;
}

/** @brief Anthropic /v1/messages end-to-end pipeline (stages same as handle_responses, error body is Anthropic-shaped).
 *  @return Always 0; errors written directly to @p rc. */
int
handle_anthropic_messages(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* --- auth --- */
    key_rec_t krec;
    int       arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_anthropic_error(rc, 401, "authentication_error", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* --- rate limit --- */
    long retry_ms = 0;
    int  rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            aigate_write_anthropic_error(rc, 503, "api_error", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) {
            ra_s = 1;
        }
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "Retry-After", ra);
        }
        aigate_write_anthropic_error(rc, 429, "rate_limit_error", "rate limit exceeded");
        key_rec_free(&krec);
        return 0;
    }

    /* --- daily token quota gate --- */
    if (krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
        if (rem == LONG_MIN) {
            aigate_write_anthropic_error(rc, 503, "api_error", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        if (rem <= 0) {
            time_t now = time(NULL);
            time_t next = (time_t)(now - (now % 86400)) + 86400; /* next UTC midnight */
            char   ra[32];
            snprintf(ra, sizeof ra, "%ld", (long)(next - now));
            if (rc->set_header != NULL) {
                rc->set_header(rc->impl, "Retry-After", ra);
            }
            aigate_write_anthropic_error(rc, 429, "rate_limit_error", "daily token quota exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* --- monthly budget limit gate --- */
    if (ac->be != NULL) {
        char b_err[256] = {0};
        if (budget_enforce_check(ac->be,
                                 krec.key_id,
                                 krec.group_id,
                                 krec.monthly_cost_budget,
                                 krec.monthly_token_budget,
                                 0.0,
                                 b_err,
                                 sizeof b_err) != 0) {
            aigate_write_anthropic_error(
                rc, 429, "rate_limit_error", b_err[0] ? b_err : "monthly budget limit exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* --- model + allowlist (parsed from request body) --- */
    const char* model = "";
    json_t*     jbody = NULL;
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
        aigate_write_anthropic_error(
            rc, 403, "authentication_error", "model not allowed for this key");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- route --- */
    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_anthropic_error(rc, 404, "not_found_error", "model not found");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- strict provider check --- */
    if (strcmp(route.provider, "anthropic") != 0) {
        aigate_write_anthropic_error(
            rc,
            400,
            "invalid_request_error",
            "/v1/messages requires an anthropic provider (unsupported_endpoint)");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- candidate targets selection --- */
    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int               n_candidates = 0;
    if (model_router_select_candidates(
            ac->cb, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_anthropic_error(
            rc, 503, "api_error", "no upstream targets available for model");
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

    uint64_t    total_lat = 0;
    const char* last_provider = route.provider;
    const char* raw_body = rq->body != NULL ? (const char*)rq->body : "";
    size_t      raw_len = rq->body_len;

    if (is_streaming) {
        for (int ci = 0; ci < n_candidates; ci++) {
            upstream_target_t* target = &candidates[ci];
            model_rec_t        cur_route = route;
            snprintf(cur_route.provider,
                     sizeof cur_route.provider,
                     "%.*s",
                     (int)sizeof cur_route.provider - 1,
                     target->provider);
            snprintf(cur_route.endpoint,
                     sizeof cur_route.endpoint,
                     "%.*s",
                     (int)sizeof cur_route.endpoint - 1,
                     target->endpoint);
            snprintf(cur_route.upstream_key,
                     sizeof cur_route.upstream_key,
                     "%.*s",
                     (int)sizeof cur_route.upstream_key - 1,
                     target->upstream_key);
            last_provider = target->provider;

            char url[1024];
            build_anthropic_url(cur_route.endpoint, url, sizeof url);

            const char* extra_hdrs[4][2] = {{0}};
            int         n_extra_hdrs = 0;
            if (cur_route.upstream_key[0] != '\0') {
                extra_hdrs[n_extra_hdrs][0] = "x-api-key";
                extra_hdrs[n_extra_hdrs][1] = cur_route.upstream_key;
                n_extra_hdrs++;
            }
            extra_hdrs[n_extra_hdrs][0] = "anthropic-version";
            extra_hdrs[n_extra_hdrs][1] = "2023-06-01";
            n_extra_hdrs++;

            anthropic_stream_ctx_t sctx;
            memset(&sctx, 0, sizeof sctx);
            sctx.rc = rc;
            anthropic_sniffer_init(&sctx.sniffer);

            int      status = 0;
            char*    sbody = NULL;
            size_t   slen = 0;
            uint64_t t0 = mono_ns();
            long     silence_timeout_ms =
                ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int urc = upstream_stream_call(url,
                                           cur_route.upstream_key,
                                           extra_hdrs,
                                           n_extra_hdrs,
                                           raw_body,
                                           raw_len,
                                           silence_timeout_ms,
                                           anthropic_stream_chunk_cb,
                                           &sctx,
                                           &status,
                                           &sbody,
                                           &slen);

            if (n_candidates == 1 && !sctx.headers_sent && (urc != 0 || status >= 500)) {
                struct timespec sl = {0, 200 * 1000000}; /* 200ms */
                nanosleep(&sl, NULL);
                free(sbody);
                sbody = NULL;
                urc = upstream_stream_call(url,
                                           cur_route.upstream_key,
                                           extra_hdrs,
                                           n_extra_hdrs,
                                           raw_body,
                                           raw_len,
                                           silence_timeout_ms,
                                           anthropic_stream_chunk_cb,
                                           &sctx,
                                           &status,
                                           &sbody,
                                           &slen);
            }
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

            if (!sctx.headers_sent) {
                cb_record_failure(ac->cb, model, target->endpoint, status);

                if (!is_failover && status >= 400) {
                    if (sbody != NULL && urc == 0) {
                        record_usage_and_event(ac,
                                               krec.key_id,
                                               model,
                                               status,
                                               0,
                                               0,
                                               0,
                                               0,
                                               total_lat,
                                               target->provider,
                                               NULL,
                                               0.0);
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
                    AIGATE_LOG_WARN("anthropic stream failover for model %s to %s",
                                    model,
                                    candidates[ci + 1].provider);
                    metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                    free(sbody);
                    continue;
                }
                free(sbody);
                break;
            }

            long ptok = 0, ctok = 0, cached_tok = 0;
            anthropic_sniffer_get_tokens(&sctx.sniffer, &ptok, &ctok, &cached_tok);

            if (urc != 0) {
                const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                    : "stream interrupted: transport error";
                char        sse_err[256];
                snprintf(sse_err,
                         sizeof sse_err,
                         "event: error\r\ndata: "
                         "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\"%s\"}"
                         "}\r\n\r\n",
                         err_msg);
                if (rc->write != NULL) {
                    rc->write(rc->impl, sse_err, strlen(sse_err), true);
                }
                double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
                record_usage_and_event(ac,
                                       krec.key_id,
                                       model,
                                       PIPE_UPSTREAM,
                                       ptok,
                                       ctok,
                                       cached_tok,
                                       0,
                                       total_lat,
                                       target->provider,
                                       NULL,
                                       req_cost);
                if (ac->be != NULL) {
                    budget_enforce_record(
                        ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
                }
                if (ptok + ctok > 0) {
                    rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
                }
                json_decref(jbody);
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            if (rc->write != NULL) {
                rc->write(rc->impl, "", 0, true);
            }
            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac,
                                   krec.key_id,
                                   model,
                                   status > 0 ? status : 200,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   0,
                                   total_lat,
                                   target->provider,
                                   NULL,
                                   req_cost);
            if (ac->be != NULL) {
                budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
        }
        aigate_write_anthropic_error(rc, 502, "api_error", "upstream request failed");
        record_usage_and_event(
            ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* non-streaming */
    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t* target = &candidates[ci];
        model_rec_t        cur_route = route;
        snprintf(cur_route.provider,
                 sizeof cur_route.provider,
                 "%.*s",
                 (int)sizeof cur_route.provider - 1,
                 target->provider);
        snprintf(cur_route.endpoint,
                 sizeof cur_route.endpoint,
                 "%.*s",
                 (int)sizeof cur_route.endpoint - 1,
                 target->endpoint);
        snprintf(cur_route.upstream_key,
                 sizeof cur_route.upstream_key,
                 "%.*s",
                 (int)sizeof cur_route.upstream_key - 1,
                 target->upstream_key);
        last_provider = target->provider;

        char url[1024];
        build_anthropic_url(cur_route.endpoint, url, sizeof url);

        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;
        if (cur_route.upstream_key[0] != '\0') {
            extra_hdrs[n_extra_hdrs][0] = "x-api-key";
            extra_hdrs[n_extra_hdrs][1] = cur_route.upstream_key;
            n_extra_hdrs++;
        }
        extra_hdrs[n_extra_hdrs][0] = "anthropic-version";
        extra_hdrs[n_extra_hdrs][1] = "2023-06-01";
        n_extra_hdrs++;

        int      status = 0;
        char*    ubody = NULL;
        size_t   ulen = 0;
        uint64_t t0 = mono_ns();
        int      urc = upstream_call_ext(url,
                                         cur_route.upstream_key,
                                         extra_hdrs,
                                         n_extra_hdrs,
                                         raw_body,
                                         raw_len,
                                         ac->default_timeout_ms,
                                         &status,
                                         &ubody,
                                         &ulen);

        if (n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000}; /* 200ms */
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url,
                                    cur_route.upstream_key,
                                    extra_hdrs,
                                    n_extra_hdrs,
                                    raw_body,
                                    raw_len,
                                    ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            long ptok = 0, ctok = 0, cached_tok = 0;
            anthropic_sniff_usage_json(ubody ? ubody : "", &ptok, &ctok, &cached_tok);

            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac,
                                   krec.key_id,
                                   model,
                                   status,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   0,
                                   total_lat,
                                   target->provider,
                                   NULL,
                                   req_cost);
            if (ac->be != NULL) {
                budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            if (rc->set_header != NULL) {
                rc->set_header(rc->impl, "X-Upstream-Provider", target->provider);
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
                record_usage_and_event(ac,
                                       krec.key_id,
                                       model,
                                       status,
                                       0,
                                       0,
                                       0,
                                       0,
                                       total_lat,
                                       target->provider,
                                       NULL,
                                       0.0);
                if (rc->set_header != NULL) {
                    rc->set_header(rc->impl, "X-Upstream-Provider", target->provider);
                }
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
            AIGATE_LOG_WARN(
                "anthropic failover for model %s to %s", model, candidates[ci + 1].provider);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) {
        rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_anthropic_error(rc, 502, "api_error", "upstream request failed");
    record_usage_and_event(
        ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
    json_decref(jbody);
    key_rec_free(&krec);
    return 0;
}

/* -------------------------------------------------------------------------
 * Native Google Gemini Pipeline (POST /v1beta/models/{model}:generateContent etc)
 * ------------------------------------------------------------------------- */

/** @brief Extract Gemini model name from request path segment `/models/<name>[:action][?..]`.
 *  @return 0 on success with NUL-terminated name in @p model_buf; -1 if path lacks /models/, name empty, or too long. */
static int
extract_gemini_model(const char* path, char* model_buf, size_t cap)
{
    if (path == NULL) {
        return -1;
    }
    const char* m = strstr(path, "/models/");
    if (m == NULL) {
        return -1;
    }
    const char* start = m + strlen("/models/");
    const char* end = strchr(start, ':');
    if (end == NULL) {
        end = strchr(start, '?');
    }
    size_t len = (end != NULL) ? (size_t)(end - start) : strlen(start);
    if (len == 0 || len >= cap) {
        return -1;
    }
    memcpy(model_buf, start, len);
    model_buf[len] = '\0';
    return 0;
}

/** @brief Build Gemini upstream URL: empty endpoint defaults to generativelanguage; strips trailing /v1beta|/v1; streaming uses `:streamGenerateContent?alt=sse`, else `:generateContent`. */
static void
build_gemini_url(
    const char* endpoint, const char* model, bool is_streaming, char* url_out, size_t url_cap)
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
    if (elen >= 7 && strcmp(base_ep + elen - 7, "/v1beta") == 0) {
        base_ep[elen - 7] = '\0';
    } else if (elen >= 3 && strcmp(base_ep + elen - 3, "/v1") == 0) {
        base_ep[elen - 3] = '\0';
    }

    if (is_streaming) {
        snprintf(
            url_out, url_cap, "%s/v1beta/models/%s:streamGenerateContent?alt=sse", base_ep, model);
    } else {
        snprintf(url_out, url_cap, "%s/v1beta/models/%s:generateContent", base_ep, model);
    }
}

/** @brief Gemini streaming callback context: first chunk writes SSE headers and passes through chunks, feeds usage sniffer. */
typedef struct {
    aigate_response_ctx* rc;           /**< Real response context (borrowed). */
    bool                 headers_sent; /**< Whether SSE headers written. */
    gemini_sniffer_t     sniffer;      /**< Usage accumulation sniffer. */
} gemini_stream_ctx_t;

/** @brief Gemini streaming upstream callback: first chunk writes SSE headers, then passes through chunks and feeds usage sniffer.
 *  @return 0 to continue; -1 on downstream write failure aborts upstream. */
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
    gemini_sniffer_feed(&ctx->sniffer, chunk, len);
    return 0;
}

/** @brief Gemini generate end-to-end pipeline (stages same as handle_responses, errors are Google RPC-shaped status).
 *  @return Always 0; errors written directly to @p rc. */
int
handle_gemini_generate(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* --- auth --- */
    key_rec_t krec;
    int       arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_gemini_error(rc, 401, "UNAUTHENTICATED", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* --- rate limit --- */
    long retry_ms = 0;
    int  rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            aigate_write_gemini_error(rc, 503, "UNAVAILABLE", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) {
            ra_s = 1;
        }
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "Retry-After", ra);
        }
        aigate_write_gemini_error(rc, 429, "RESOURCE_EXHAUSTED", "rate limit exceeded");
        key_rec_free(&krec);
        return 0;
    }

    /* --- daily token quota gate --- */
    if (krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
        if (rem == LONG_MIN) {
            aigate_write_gemini_error(rc, 503, "UNAVAILABLE", "distributed_state_unavailable");
            key_rec_free(&krec);
            return 0;
        }
        if (rem <= 0) {
            time_t now = time(NULL);
            time_t next = (time_t)(now - (now % 86400)) + 86400; /* next UTC midnight */
            char   ra[32];
            snprintf(ra, sizeof ra, "%ld", (long)(next - now));
            if (rc->set_header != NULL) {
                rc->set_header(rc->impl, "Retry-After", ra);
            }
            aigate_write_gemini_error(rc, 429, "RESOURCE_EXHAUSTED", "daily token quota exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* --- monthly budget limit gate --- */
    if (ac->be != NULL) {
        char b_err[256] = {0};
        if (budget_enforce_check(ac->be,
                                 krec.key_id,
                                 krec.group_id,
                                 krec.monthly_cost_budget,
                                 krec.monthly_token_budget,
                                 0.0,
                                 b_err,
                                 sizeof b_err) != 0) {
            aigate_write_gemini_error(
                rc, 429, "RESOURCE_EXHAUSTED", b_err[0] ? b_err : "monthly budget limit exceeded");
            key_rec_free(&krec);
            return 0;
        }
    }

    /* --- model from URL path --- */
    char model[128] = {0};
    if (extract_gemini_model(rq->path, model, sizeof model) != 0) {
        aigate_write_gemini_error(
            rc, 400, "INVALID_ARGUMENT", "failed to extract model from URL path");
        key_rec_free(&krec);
        return 0;
    }

    if (!key_allows_model(&krec, model)) {
        aigate_write_gemini_error(rc, 403, "PERMISSION_DENIED", "model not allowed for this key");
        key_rec_free(&krec);
        return 0;
    }

    /* --- route --- */
    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_gemini_error(rc, 404, "NOT_FOUND", "model not found");
        key_rec_free(&krec);
        return 0;
    }

    /* --- strict provider check --- */
    if (strcmp(route.provider, "gemini") != 0 && strcmp(route.provider, "google") != 0) {
        aigate_write_gemini_error(
            rc,
            400,
            "INVALID_ARGUMENT",
            "Gemini endpoints require a gemini or google provider (unsupported_endpoint)");
        key_rec_free(&krec);
        return 0;
    }

    /* --- candidate targets selection --- */
    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int               n_candidates = 0;
    if (model_router_select_candidates(
            ac->cb, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_gemini_error(
            rc, 503, "UNAVAILABLE", "no upstream targets available for model");
        key_rec_free(&krec);
        return 0;
    }

    bool is_streaming =
        (strstr(rq->path, ":streamGenerateContent") != NULL || strstr(rq->path, "alt=sse") != NULL);

    uint64_t    total_lat = 0;
    const char* last_provider = route.provider;
    const char* raw_body = rq->body != NULL ? (const char*)rq->body : "";
    size_t      raw_len = rq->body_len;

    if (is_streaming) {
        for (int ci = 0; ci < n_candidates; ci++) {
            upstream_target_t* target = &candidates[ci];
            model_rec_t        cur_route = route;
            snprintf(cur_route.provider,
                     sizeof cur_route.provider,
                     "%.*s",
                     (int)sizeof cur_route.provider - 1,
                     target->provider);
            snprintf(cur_route.endpoint,
                     sizeof cur_route.endpoint,
                     "%.*s",
                     (int)sizeof cur_route.endpoint - 1,
                     target->endpoint);
            snprintf(cur_route.upstream_key,
                     sizeof cur_route.upstream_key,
                     "%.*s",
                     (int)sizeof cur_route.upstream_key - 1,
                     target->upstream_key);
            last_provider = target->provider;

            char url[1024];
            build_gemini_url(cur_route.endpoint, model, true, url, sizeof url);

            const char* extra_hdrs[4][2] = {{0}};
            int         n_extra_hdrs = 0;
            if (cur_route.upstream_key[0] != '\0') {
                extra_hdrs[n_extra_hdrs][0] = "x-goog-api-key";
                extra_hdrs[n_extra_hdrs][1] = cur_route.upstream_key;
                n_extra_hdrs++;
            }

            gemini_stream_ctx_t sctx;
            memset(&sctx, 0, sizeof sctx);
            sctx.rc = rc;
            gemini_sniffer_init(&sctx.sniffer);

            int      status = 0;
            char*    sbody = NULL;
            size_t   slen = 0;
            uint64_t t0 = mono_ns();
            long     silence_timeout_ms =
                ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int urc = upstream_stream_call(url,
                                           cur_route.upstream_key,
                                           extra_hdrs,
                                           n_extra_hdrs,
                                           raw_body,
                                           raw_len,
                                           silence_timeout_ms,
                                           gemini_stream_chunk_cb,
                                           &sctx,
                                           &status,
                                           &sbody,
                                           &slen);

            if (n_candidates == 1 && !sctx.headers_sent && (urc != 0 || status >= 500)) {
                struct timespec sl = {0, 200 * 1000000}; /* 200ms */
                nanosleep(&sl, NULL);
                free(sbody);
                sbody = NULL;
                urc = upstream_stream_call(url,
                                           cur_route.upstream_key,
                                           extra_hdrs,
                                           n_extra_hdrs,
                                           raw_body,
                                           raw_len,
                                           silence_timeout_ms,
                                           gemini_stream_chunk_cb,
                                           &sctx,
                                           &status,
                                           &sbody,
                                           &slen);
            }
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

            if (!sctx.headers_sent) {
                cb_record_failure(ac->cb, model, target->endpoint, status);

                if (!is_failover && status >= 400) {
                    if (sbody != NULL && urc == 0) {
                        record_usage_and_event(ac,
                                               krec.key_id,
                                               model,
                                               status,
                                               0,
                                               0,
                                               0,
                                               0,
                                               total_lat,
                                               target->provider,
                                               NULL,
                                               0.0);
                        int rv = aigate_write_json(rc, status, sbody, slen);
                        free(sbody);
                        key_rec_free(&krec);
                        return rv;
                    }
                    free(sbody);
                    break;
                }

                if (ci + 1 < n_candidates) {
                    AIGATE_LOG_WARN("gemini stream failover for model %s to %s",
                                    model,
                                    candidates[ci + 1].provider);
                    metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                    free(sbody);
                    continue;
                }
                free(sbody);
                break;
            }

            long ptok = 0, ctok = 0, cached_tok = 0;
            gemini_sniffer_get_tokens(&sctx.sniffer, &ptok, &ctok, &cached_tok);

            if (urc != 0) {
                const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                    : "stream interrupted: transport error";
                char        sse_err[256];
                snprintf(
                    sse_err,
                    sizeof sse_err,
                    "data: "
                    "{\"error\":{\"code\":502,\"message\":\"%s\",\"status\":\"UNAVAILABLE\"}}\n\n",
                    err_msg);
                if (rc->write != NULL) {
                    rc->write(rc->impl, sse_err, strlen(sse_err), true);
                }
                double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
                record_usage_and_event(ac,
                                       krec.key_id,
                                       model,
                                       PIPE_UPSTREAM,
                                       ptok,
                                       ctok,
                                       cached_tok,
                                       0,
                                       total_lat,
                                       target->provider,
                                       NULL,
                                       req_cost);
                if (ac->be != NULL) {
                    budget_enforce_record(
                        ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
                }
                if (ptok + ctok > 0) {
                    rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
                }
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            if (rc->write != NULL) {
                rc->write(rc->impl, "", 0, true);
            }
            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac,
                                   krec.key_id,
                                   model,
                                   status > 0 ? status : 200,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   0,
                                   total_lat,
                                   target->provider,
                                   NULL,
                                   req_cost);
            if (ac->be != NULL) {
                budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            key_rec_free(&krec);
            return 0;
        }

        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
        }
        aigate_write_gemini_error(rc, 502, "UNAVAILABLE", "upstream request failed");
        record_usage_and_event(
            ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
        key_rec_free(&krec);
        return 0;
    }

    /* non-streaming */
    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t* target = &candidates[ci];
        model_rec_t        cur_route = route;
        snprintf(cur_route.provider,
                 sizeof cur_route.provider,
                 "%.*s",
                 (int)sizeof cur_route.provider - 1,
                 target->provider);
        snprintf(cur_route.endpoint,
                 sizeof cur_route.endpoint,
                 "%.*s",
                 (int)sizeof cur_route.endpoint - 1,
                 target->endpoint);
        snprintf(cur_route.upstream_key,
                 sizeof cur_route.upstream_key,
                 "%.*s",
                 (int)sizeof cur_route.upstream_key - 1,
                 target->upstream_key);
        last_provider = target->provider;

        char url[1024];
        build_gemini_url(cur_route.endpoint, model, false, url, sizeof url);

        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;
        if (cur_route.upstream_key[0] != '\0') {
            extra_hdrs[n_extra_hdrs][0] = "x-goog-api-key";
            extra_hdrs[n_extra_hdrs][1] = cur_route.upstream_key;
            n_extra_hdrs++;
        }

        int      status = 0;
        char*    ubody = NULL;
        size_t   ulen = 0;
        uint64_t t0 = mono_ns();
        int      urc = upstream_call_ext(url,
                                         cur_route.upstream_key,
                                         extra_hdrs,
                                         n_extra_hdrs,
                                         raw_body,
                                         raw_len,
                                         ac->default_timeout_ms,
                                         &status,
                                         &ubody,
                                         &ulen);

        if (n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000}; /* 200ms */
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url,
                                    cur_route.upstream_key,
                                    extra_hdrs,
                                    n_extra_hdrs,
                                    raw_body,
                                    raw_len,
                                    ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            long ptok = 0, ctok = 0, cached_tok = 0;
            gemini_sniff_usage_json(ubody ? ubody : "", &ptok, &ctok, &cached_tok);

            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac,
                                   krec.key_id,
                                   model,
                                   status,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   0,
                                   total_lat,
                                   target->provider,
                                   NULL,
                                   req_cost);
            if (ac->be != NULL) {
                budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            if (rc->set_header != NULL) {
                rc->set_header(rc->impl, "X-Upstream-Provider", target->provider);
            }
            int rv = aigate_write_json(rc, status, ubody ? ubody : "", ulen);
            free(ubody);
            key_rec_free(&krec);
            return rv;
        }

        cb_record_failure(ac->cb, model, target->endpoint, status);

        if (!is_failover && status >= 400) {
            if (ubody != NULL && urc == 0) {
                record_usage_and_event(ac,
                                       krec.key_id,
                                       model,
                                       status,
                                       0,
                                       0,
                                       0,
                                       0,
                                       total_lat,
                                       target->provider,
                                       NULL,
                                       0.0);
                if (rc->set_header != NULL) {
                    rc->set_header(rc->impl, "X-Upstream-Provider", target->provider);
                }
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
            AIGATE_LOG_WARN(
                "gemini failover for model %s to %s", model, candidates[ci + 1].provider);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) {
        rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_gemini_error(rc, 502, "UNAVAILABLE", "upstream request failed");
    record_usage_and_event(
        ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
    key_rec_free(&krec);
    return 0;
}

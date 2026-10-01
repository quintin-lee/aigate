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

/** @brief OpenAI /responses end-to-end pipeline: auth → rate limit → daily quota → guardrails → cache → upstream → write back.
 *  @return Always 0; all errors written directly to @p rc (OpenAI-shaped error body). */
int
handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* --- auth --- */
    key_rec_t krec;
    int       arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_error(rc, PIPE_AUTH, "auth_error", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* --- rate limit --- */
    long retry_ms = 0;
    int  rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            aigate_write_error(rc, 503, "server_error", "distributed_state_unavailable");
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
        aigate_write_error(rc, PIPE_RATE, "rate_limit", "rate limit exceeded");
        key_rec_free(&krec);
        return 0;
    }

    /* --- daily token quota gate --- */
    if (krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota);
        if (rem == LONG_MIN) {
            aigate_write_error(rc, 503, "server_error", "distributed_state_unavailable");
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
            aigate_write_error(rc, PIPE_RATE, "daily_quota_exceeded", "daily token quota exceeded");
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
            aigate_write_error(rc,
                               PIPE_RATE,
                               "budget_exceeded",
                               b_err[0] ? b_err : "monthly budget limit exceeded");
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

    /* --- route --- */
    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_error(rc, PIPE_MODEL, "model_not_found", "model not found");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- strict provider check --- */
    if (strcmp(route.provider, "openai") != 0) {
        aigate_write_error(rc,
                           400,
                           "unsupported_endpoint",
                           "/v1/responses requires an openai-compatible provider");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- candidate targets selection --- */
    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int               n_candidates = 0;
    if (model_router_select_candidates(
            ac->cb, NULL, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_error(
            rc, PIPE_MODEL, "no_healthy_upstream", "no upstream targets available for model");
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
            strcmp(rq->cache_control, "true") == 0 || strcmp(rq->cache_control, "1") == 0) {
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
                record_usage_and_event(ac,
                                       krec.key_id,
                                       model,
                                       200,
                                       ce->prompt_tokens,
                                       ce->completion_tokens,
                                       0,
                                       0,
                                       100000ULL,
                                       "cache",
                                       NULL,
                                       ce->cost_usd);
                if (ac->be != NULL) {
                    budget_enforce_record(ac->be,
                                          krec.key_id,
                                          krec.group_id,
                                          ce->cost_usd,
                                          ce->prompt_tokens + ce->completion_tokens);
                }
                rl_reserve_tokens(ac->rl,
                                  krec.key_id,
                                  krec.daily_token_quota,
                                  ce->prompt_tokens + ce->completion_tokens);
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
            if (rc->set_header != NULL) {
                rc->set_header(rc->impl, "X-Cache", "MISS");
            }
        }
    }

    uint64_t    total_lat = 0;
    const char* last_provider = route.provider;

    if (is_streaming) {
        for (int ci = 0; ci < n_candidates; ci++) {
            upstream_target_t*        target = &candidates[ci];
            const provider_adapter_t* adapter = provider_find(target->provider);
            if (adapter == NULL || adapter->build_responses == NULL) {
                continue;
            }

            model_rec_t cur_route = route;
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

            char        url[1024];
            char*       out_body = NULL;
            size_t      out_body_len = 0;
            const char* extra_hdrs[4][2] = {{0}};
            int         n_extra_hdrs = 0;

            if (adapter->build_responses(&cur_route,
                                         rq->body != NULL ? (const char*)rq->body : "",
                                         url,
                                         sizeof url,
                                         extra_hdrs,
                                         &n_extra_hdrs,
                                         &out_body,
                                         &out_body_len) != 0) {
                free(out_body);
                continue;
            }

            stream_cache_acc_t acc;
            memset(&acc, 0, sizeof(acc));
            acc.orig_rc = rc;

            aigate_response_ctx proxy_rc = *rc;
            proxy_rc.impl = &acc;
            proxy_rc.set_header = stream_cache_acc_set_header;
            proxy_rc.write = stream_cache_acc_write;

            stream_bridge_t* bridge = adapter->stream_bridge_new != NULL
                                          ? adapter->stream_bridge_new(&proxy_rc, model)
                                          : NULL;
            if (bridge == NULL) {
                free(out_body);
                continue;
            }

            int      status = 0;
            char*    sbody = NULL;
            size_t   slen = 0;
            uint64_t t0 = mono_ns();
            long     silence_timeout_ms =
                ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int  urc = upstream_stream_call(url,
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

            if (n_candidates == 1 && !headers_sent && (urc != 0 || status >= 500)) {
                struct timespec sl = {0, 200 * 1000000}; /* 200ms */
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
                cb_record_failure(ac->cb, model, target->endpoint, status);
                adapter->stream_bridge_free(bridge);
                if (acc.accum_content != NULL) {
                    free(acc.accum_content);
                }
                free(acc.line_buf);
                acc.line_buf = NULL;

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
                    AIGATE_LOG_WARN("responses streaming failover for model %s to %s",
                                    model,
                                    candidates[ci + 1].provider);
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
                const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                    : "stream interrupted: transport error";
                char        sse_err[256];
                snprintf(
                    sse_err,
                    sizeof sse_err,
                    "data: "
                    "{\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\",\"code\":502}}\n\n"
                    "data: [DONE]\n\n",
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
                                       reasoning_tok,
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
                adapter->stream_bridge_free(bridge);
                if (acc.accum_content != NULL) {
                    free(acc.accum_content);
                }
                free(acc.line_buf);
                acc.line_buf = NULL;
                json_decref(jbody);
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            adapter->stream_bridge_finish(bridge);
            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac,
                                   krec.key_id,
                                   model,
                                   status > 0 ? status : 200,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   reasoning_tok,
                                   total_lat,
                                   target->provider,
                                   NULL,
                                   req_cost);
            if (ac->be != NULL) {
                budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            if (ac->rc != NULL && cache_key[0] != '\0' && !no_store && !acc.overflow &&
                (status == 0 || status == 200) && acc.accum_content != NULL && acc.accum_len > 0) {
                json_t* full_resp = json_pack(
                    "{s:s, s:s, s:I, s:s, s:[{s:i, s:{s:s, s:s}, s:s}], s:{s:i, s:i, s:i}}",
                    "id",
                    acc.id[0] ? acc.id : "chatcmpl-stream",
                    "object",
                    "chat.completion",
                    "created",
                    (json_int_t)(acc.created > 0 ? acc.created : time(NULL)),
                    "model",
                    model,
                    "choices",
                    "index",
                    0,
                    "message",
                    "role",
                    "assistant",
                    "content",
                    acc.accum_content,
                    "finish_reason",
                    "stop",
                    "usage",
                    "prompt_tokens",
                    (int)ptok,
                    "completion_tokens",
                    (int)ctok,
                    "total_tokens",
                    (int)(ptok + ctok));
                if (full_resp != NULL) {
                    char* full_json = json_dumps(full_resp, JSON_COMPACT);
                    if (full_json != NULL) {
                        response_cache_set(ac->rc,
                                           cache_key,
                                           model,
                                           full_json,
                                           strlen(full_json),
                                           ptok,
                                           ctok,
                                           req_cost,
                                           0);
                        free(full_json);
                    }
                    json_decref(full_resp);
                }
            }
            if (acc.accum_content != NULL) {
                free(acc.accum_content);
            }
            free(acc.line_buf);
            acc.line_buf = NULL;
            adapter->stream_bridge_free(bridge);

            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
        }
        aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
        record_usage_and_event(
            ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* Non-streaming */
    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t*        target = &candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL || adapter->build_responses == NULL ||
            adapter->parse_responses_response == NULL) {
            continue;
        }

        model_rec_t cur_route = route;
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

        char        url[1024];
        char*       out_body = NULL;
        size_t      out_body_len = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;

        if (adapter->build_responses(&cur_route,
                                     rq->body != NULL ? (const char*)rq->body : "",
                                     url,
                                     sizeof url,
                                     extra_hdrs,
                                     &n_extra_hdrs,
                                     &out_body,
                                     &out_body_len) != 0) {
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
                                    out_body,
                                    out_body_len,
                                    ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        free(out_body);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            long ptok = 0, ctok = 0, cached_tok = 0, reasoning_tok = 0;
            adapter->parse_responses_response(
                ubody ? ubody : "", ulen, &ptok, &ctok, &cached_tok, &reasoning_tok);

            double req_cost = calc_req_cost(&route, ptok, ctok, cached_tok);
            record_usage_and_event(ac,
                                   krec.key_id,
                                   model,
                                   status,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   reasoning_tok,
                                   total_lat,
                                   target->provider,
                                   NULL,
                                   req_cost);
            if (ac->be != NULL) {
                budget_enforce_record(ac->be, krec.key_id, krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            if (ac->rc != NULL && cache_key[0] != '\0' && status == 200 && ubody != NULL &&
                !no_store && ulen <= 1048576) {
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
                "responses failover for model %s to %s", model, candidates[ci + 1].provider);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) {
        rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    record_usage_and_event(
        ac, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, 0, total_lat, last_provider, NULL, 0.0);
    json_decref(jbody);
    key_rec_free(&krec);
    return 0;
}
